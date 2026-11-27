// test_server.cpp - 방 제한, 자동 정리, 익명 사용 통계 테스트
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "httplib.h"
#include "stats.h"
#include "web_server.h"
#include "web_session.h"

using namespace std::chrono_literals;

namespace {

WebOptions fastOptions() {
    WebOptions o;
    o.seed = 1u;
    o.hintTrials = 20;
    o.aiAction = 0ms;
    o.autoContinue = true;
    o.nextHandDelay = 20ms;
    return o;
}

template <class Predicate>
bool waitFor(Predicate done) {
    for (int i = 0; i < 2500; ++i) {
        if (done()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

std::string tempFolder() {
    const char* base = std::getenv("TMPDIR");
    std::string pattern = std::string(base && *base ? base : "/tmp") + "/holdem-test-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* made = mkdtemp(buffer.data());
    CHECK(made != nullptr);
    return made ? made : ".";
}

std::string readAll(const std::string& path) {
    std::ifstream file(path);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

// 서버를 잠깐 띄워 HTTP로 확인한다.
struct TestServer {
    RoomManager rooms;
    httplib::Server server;
    std::thread thread;
    int port = 0;
    explicit TestServer(const WebOptions& options) : rooms(options) {
        setupRoutes(server, rooms, "<html>ok</html>");
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~TestServer() {
        server.stop();
        thread.join();
    }
};

// ---------------------------------------------------------------------------
// 방 제한
// ---------------------------------------------------------------------------
void testRoomsFullIs429() {
    WebOptions options = fastOptions();
    options.maxRooms = 3;
    TestServer s(options);
    httplib::Client client("127.0.0.1", s.port);
    // 혼자 하기도 방 수에 포함된다 (쿠키 없이 매번 새 접속)
    CHECK(client.Post("/api/new", "{}", "application/json")->status == 200);
    CHECK(client.Post("/api/new", "{}", "application/json")->status == 200);
    CHECK(client.Post("/api/room/create", "{\"name\":\"a\"}", "application/json")->status == 200);
    for (const char* path : {"/api/new", "/api/room/create"}) {
        const auto res = client.Post(path, "{\"name\":\"b\"}", "application/json");
        CHECK(res && res->status == 429);
        CHECK(res && res->body ==
                         "{\"error\":\"ROOMS_FULL\",\"message\":\"지금 방이 가득 찼어요. 잠시 뒤에 다시 시도해 주세요.\"}");
    }
    CHECK(s.rooms.size() == 3);
}

void testPerClientLimit() {
    WebOptions options = fastOptions();
    options.maxRooms = 5;
    TestServer s(options);
    httplib::Client client("127.0.0.1", s.port);
    auto create = [&](const std::string& id) {
        httplib::Headers headers;
        if (!id.empty()) headers.emplace("X-Client-Id", id);
        return client.Post("/api/room/create", headers, "{\"name\":\"a\"}", "application/json");
    };
    CHECK(create("aaaaaaaaaaaaaaaa")->status == 200);
    CHECK(create("aaaaaaaaaaaaaaaa")->status == 200);
    const auto third = create("aaaaaaaaaaaaaaaa");
    CHECK(third->status == 429 && third->body.find("CLIENT_ROOMS_FULL") != std::string::npos);
    CHECK(create("bbbbbbbbbbbbbbbb")->status == 200);  // 다른 브라우저는 따로 센다
    // ID가 없으면 브라우저별 제한은 없지만 전체 한도는 같다
    CHECK(create("")->status == 200);
    CHECK(create("")->status == 200);
    CHECK(create("")->status == 429);
    CHECK(s.rooms.size() == 5);
}

// 같은 접속(쿠키)이 새 방을 만들면 혼자 있던 옛 방은 없어지므로 제한에 걸리지 않는다
void testReplaceOwnRoom() {
    RoomManager manager(fastOptions());
    std::string error;
    CreateFail fail = CreateFail::None;
    std::string token;
    for (int i = 0; i < 5; ++i) {
        token = manager.create(token, "나", RoomSettings{}, true, error, fail, "cccccccccccccccc");
        CHECK(!token.empty() && fail == CreateFail::None);
    }
    CHECK(manager.size() == 1);
}

void testPlayersPerRoom() {
    WebOptions options = fastOptions();
    options.maxPlayers = 3;
    RoomSettings settings;
    settings.bots = 5;
    settings.turnSeconds = 60;
    Room room(options, settings, "000000", false, 1);
    std::string error;
    CHECK(room.join("a", "하나", error) && room.join("b", "둘", error) && room.join("c", "셋", error));
    CHECK(!room.join("d", "넷", error) && error == "이 방은 인원이 가득 찼어요.");
    CHECK(room.start("a", error));
    CHECK(waitFor([&] { return !room.view("a")->table.state.players.empty(); }));
    CHECK(room.view("a")->table.state.players.size() == 3);  // 봇을 더 앉히지 않는다
}

// 오래된 방만 지우고, 최근에 입력이 있던 게임은 계속 진행된다
void testCleanupKeepsActiveGame() {
    RoomManager manager(fastOptions());
    const auto start = RoomManager::Clock::now();
    std::string error;
    CreateFail fail = CreateFail::None;
    const std::string idle = manager.create("", "나", RoomSettings{}, true, error, fail, "", start);
    const std::string active = manager.create("", "나", RoomSettings{}, true, error, fail, "", start);
    CHECK(manager.size() == 2);
    manager.touch(active, start + 25min);
    CHECK(manager.cleanupIdle(start + 29min) == 0);
    CHECK(manager.cleanupIdle(start + 31min) == 1);
    CHECK(manager.size() == 1);
    CHECK(!manager.find(idle) && manager.closedByIdle(idle));
    CHECK(!manager.closedByIdle(active));

    const auto room = manager.find(active);
    CHECK(room != nullptr);
    if (!room) return;
    CHECK(waitFor([&] { return room->view(active)->myTurn; }));
    const int turn = room->view(active)->turnId;
    manager.cleanupIdle(start + 40min);  // 아직 25분 + 30분 전
    CHECK(room->submit(active, "fold", {}, turn, error) || room->submit(active, "check", {}, turn, error));
    CHECK(waitFor([&] { return room->view(active)->turnId > turn; }));  // 게임이 이어진다

    manager.leave(idle);  // 안내를 확인하면 잊는다
    CHECK(!manager.closedByIdle(idle));
}

void testClosedNoticeInState() {
    WebOptions options = fastOptions();
    TestServer s(options);
    httplib::Client client("127.0.0.1", s.port);
    const auto created = client.Post("/api/new", "{}", "application/json");
    const std::string cookie = created->get_header_value("Set-Cookie");
    const httplib::Headers headers{{"Cookie", cookie.substr(0, cookie.find(';'))}};
    CHECK(client.Get("/api/state", headers)->body.find("closed") == std::string::npos);
    CHECK(s.rooms.cleanupIdle(RoomManager::Clock::now() + 31min) == 1);
    const auto state = client.Get("/api/state", headers);
    CHECK(state->body.find("\"hasGame\":false") != std::string::npos);
    CHECK(state->body.find("\"closed\":\"오래 쓰지 않아 방이 닫혔어요\"") != std::string::npos);
    client.Post("/api/room/leave", headers, "{}", "application/json");
    CHECK(client.Get("/api/state", headers)->body.find("closed") == std::string::npos);
}

// ---------------------------------------------------------------------------
// 통계
// ---------------------------------------------------------------------------
std::time_t at(int day, int hour, int minute, int second) {
    std::tm t{};
    t.tm_year = 126;  // 2026
    t.tm_mon = 10;    // 11월
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = second;
    t.tm_isdst = -1;
    return std::mktime(&t);
}

void testCountersAndPersistence() {
    const std::string folder = tempFolder();
    const std::string path = folder + "/data/stats.json";
    {
        Stats stats(path, [] { return at(10, 12, 0, 0); });
        stats.visit("1111111111111111");
        stats.visit("1111111111111111");  // 새로고침
        stats.visit("2222222222222222");
        stats.visit("닉네임");              // 해시 모양이 아니면 버린다
        stats.visit("192.168.0.1");
        stats.gameStarted(true, 3);
        stats.gameStarted(false, 2);
        stats.gameStarted(true, 3);
        for (int i = 0; i < 6; ++i) stats.handFinished();
        const StatsSummary s = stats.summary();
        CHECK(s.totalVisits == 2 && s.todayVisits == 2 && s.uniqueVisitors == 2);
        CHECK(s.soloStarts == 2 && s.roomStarts == 1 && s.totalHands == 6 && s.todayHands == 6);
        CHECK(s.handsPerGame == 2.0 && s.topBots == 3);
        CHECK(s.days.size() == 14 && s.days.back().date == "2026-11-10" && s.days.front().date == "2026-10-28");
        CHECK(stats.flush());
    }
    struct stat info {};
    CHECK(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    const std::string saved = readAll(path);
    CHECK(saved.find("192.168") == std::string::npos && saved.find("닉네임") == std::string::npos);

    Stats again(path, [] { return at(10, 18, 0, 0); });  // 서버 재시작
    const StatsSummary s = again.summary();
    CHECK(s.totalHands == 6 && s.uniqueVisitors == 2 && s.totalVisits == 2 && s.topBots == 3);
    again.visit("1111111111111111");  // 같은 날에는 다시 세지 않는다
    CHECK(again.summary().totalVisits == 2);
}

void testDateBoundary() {
    const std::string folder = tempFolder();
    std::time_t now = at(10, 23, 59, 59);
    Stats stats(folder + "/stats.json", [&now] { return now; });
    stats.visit("1111111111111111");
    stats.handFinished();
    now = at(11, 0, 0, 1);  // 2초 뒤, 다음 날
    stats.visit("1111111111111111");
    stats.handFinished();
    stats.handFinished();
    const StatsSummary s = stats.summary();
    CHECK(s.totalVisits == 2 && s.uniqueVisitors == 1 && s.todayVisits == 1);
    CHECK(s.todayHands == 2 && s.totalHands == 3);
    CHECK(s.days[12].date == "2026-11-10" && s.days[12].hands == 1 && s.days[12].visitors == 1);
    CHECK(s.days[13].date == "2026-11-11" && s.days[13].hands == 2);
}

void testBrokenFileIsKept() {
    const std::string folder = tempFolder();
    const std::string path = folder + "/stats.json";
    std::ofstream(path) << "not json";
    Stats stats(path);
    CHECK(stats.summary().totalHands == 0);
    CHECK(readAll(path + ".bad") == "not json");
}

// 파일을 쓸 수 없어도 게임은 계속되고, 숫자는 메모리에 남는다
void testWriteFailureDoesNotStopGame() {
    const std::string folder = tempFolder();
    std::ofstream(folder + "/blocker") << "파일";  // 폴더 자리에 파일이 있으면 쓸 수 없다
    WebOptions options = fastOptions();
    options.stats = std::make_shared<Stats>(folder + "/blocker/stats.json");
    Room room(options, RoomSettings{}, "000000", true, 1);
    std::string error;
    CHECK(room.join("a", "나", error) && room.start("a", error));
    CHECK(waitFor([&] {
        const RoomView v = *room.view("a");
        if (v.myTurn) room.submit("a", v.table.state.toCall > 0 ? "fold" : "check", {}, v.turnId, error);
        return v.table.state.handNumber >= 3;
    }));
    CHECK(options.stats->summary().totalHands >= 2 && options.stats->summary().soloStarts == 1);
    CHECK(!std::ifstream(folder + "/blocker/stats.json"));
    CHECK(readAll(folder + "/blocker") == "파일");

    Stats blocked(folder + "/blocker/other.json");
    blocked.handFinished();
    CHECK(!blocked.flush());  // 실패를 알려 주지만 예외는 없다
    CHECK(blocked.summary().totalHands == 1);
}

void testStatsPage() {
    const std::string folder = tempFolder();
    WebOptions options = fastOptions();
    options.stats = std::make_shared<Stats>(folder + "/stats.json");
    {  // 토큰이 없으면 존재 자체를 숨긴다
        TestServer s(options);
        httplib::Client client("127.0.0.1", s.port);
        CHECK(client.Get("/stats")->status == 404);
        CHECK(client.Get("/stats?token=")->status == 404);
        CHECK(client.Get("/stats?token=abc")->status == 404);
    }
    options.statsToken = "secret-token";
    TestServer s(options);
    httplib::Client client("127.0.0.1", s.port);
    CHECK(client.Get("/stats")->status == 404);
    CHECK(client.Get("/stats?token=secret-toke")->status == 404);
    CHECK(client.Get("/stats?token=secret-token-x")->status == 404);
    const httplib::Headers id{{"X-Client-Id", "abcdefabcdefabcd"}};
    CHECK(client.Post("/api/visit", id, "{}", "application/json")->status == 200);
    CHECK(client.Post("/api/visit", id, "{}", "application/json")->status == 200);
    const auto page = client.Get("/stats?token=secret-token");
    CHECK(page && page->status == 200);
    CHECK(page->body.find("noindex") != std::string::npos);
    CHECK(page->get_header_value("X-Robots-Tag").find("noindex") != std::string::npos);
    CHECK(page->body.find("<b>1</b><span>고유 방문자 수</span>") != std::string::npos);
    CHECK(page->body.find("<script") == std::string::npos && page->body.find("http://") == std::string::npos);
}

// 로고와 미리보기 그림이 올바른 형식으로 제공되는가
void testAssets() {
    httplib::Server server;
    CHECK(setupAssets(server, HOLDEM_WEB_DIR) == 6);
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread thread([&] { server.listen_after_bind(); });
    server.wait_until_ready();
    {
        httplib::Client client("127.0.0.1", port);
        const auto og = client.Get("/og-image.png");
        CHECK(og && og->status == 200 && og->get_header_value("Content-Type") == "image/png");
        CHECK(og && og->body.size() > 24 && og->body.compare(1, 3, "PNG") == 0);
        // PNG 머리말의 가로, 세로 (1200 x 630)
        auto number = [&](std::size_t at) {
            int n = 0;
            for (int i = 0; i < 4; ++i) n = n * 256 + static_cast<unsigned char>(og->body[at + i]);
            return n;
        };
        CHECK(og && number(16) == 1200 && number(20) == 630);
        const auto logo = client.Get("/logo.svg");
        CHECK(logo && logo->status == 200 && logo->get_header_value("Content-Type") == "image/svg+xml");
        // 파비콘은 로고와 같은 잔 symbol이어야 한다 (어긋나면 node tools/make-icons.mjs를 다시 실행)
        auto symbolOf = [](const std::string& svg) {
            const std::size_t from = svg.find("<symbol id=\"coupe\"");
            const std::size_t to = svg.find("</symbol>");
            return from == std::string::npos || to == std::string::npos ? std::string() : svg.substr(from, to - from);
        };
        const auto favicon = client.Get("/favicon.svg");
        CHECK(logo && favicon && !symbolOf(logo->body).empty() && symbolOf(logo->body) == symbolOf(favicon->body));
        for (const char* path : {"/favicon.svg", "/favicon.ico", "/apple-touch-icon.png"}) {
            CHECK(client.Get(path)->status == 200);
        }
        const auto font = client.Get("/fonts/coupe-brand.woff2");
        CHECK(font && font->status == 200 && font->get_header_value("Content-Type") == "font/woff2");
        CHECK(font && font->body.compare(0, 4, "wOF2") == 0);
        CHECK(client.Get("/index.html")->status == 404);
        CHECK(client.Get("/../README.md")->status != 200);
    }
    server.stop();
    thread.join();
}

}  // namespace

int main() {
    testRoomsFullIs429();
    testPerClientLimit();
    testReplaceOwnRoom();
    testPlayersPerRoom();
    testCleanupKeepsActiveGame();
    testClosedNoticeInState();
    testCountersAndPersistence();
    testDateBoundary();
    testBrokenFileIsKept();
    testWriteFailureDoesNotStopGame();
    testStatsPage();
    testAssets();
    std::printf("%d개 검사 중 %d개 실패\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
