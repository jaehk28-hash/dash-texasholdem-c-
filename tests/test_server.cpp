// test_server.cpp - 방 제한, 자동 정리, 익명 사용 통계 테스트
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
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
#include "ledger.h"
#include "sha256.h"
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
    return timegm(&t) - 9 * 60 * 60;  // 한국 시간 (시험하는 컴퓨터의 시간대와 무관)
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

void testSha256() {
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

void testLedger() {
    const std::string folder = tempFolder();
    const std::string path = folder + "/data/stats.json";
    const std::string ledger = folder + "/data/stats-ledger.jsonl";
    std::time_t now = at(10, 23, 59, 0);
    const auto clock = [&now] { return now; };
    {
        Stats stats(path, clock);
        CHECK(stats.ledgerPath() == ledger);
        stats.visit("1111111111111111", "instagram");
        stats.visit("2222222222222222", "홍길동 010-1234-5678");  // 모르는 이름은 other
        stats.visit("3333333333333333");
        stats.gameStarted(true, 3);
        stats.gameStarted(false, 2);
        stats.handFinished();
        CHECK(stats.updateLedger() && !std::ifstream(ledger));  // 오늘은 아직 쓰지 않는다
        now = at(11, 0, 0, 1);  // 한국 시간 자정이 지남
        CHECK(stats.updateLedger() && stats.updateLedger());  // 두 번 불러도 한 줄
    }
    const std::string first = readAll(ledger);
    CHECK(std::count(first.begin(), first.end(), '\n') == 1);
    CHECK(first.find("{\"date\":\"2026-11-10\",\"counts\":{\"visits\":3,\"unique_visitors\":3,\"solo_starts\":1,"
                     "\"room_starts\":1,\"hands_finished\":1,\"src\":{\"instagram\":1,\"other\":1}},"
                     "\"prev\":\"genesis\",\"hash\":\"") == 0);
    CHECK(first.find("010-1234") == std::string::npos && first.find("1111111111111111") == std::string::npos);

    now = at(13, 9, 0, 0);  // 이틀 넘게 꺼져 있었다
    {
        Stats stats(path, clock);  // 켜면서 11일, 12일을 덧붙인다
        CHECK(!stats.summary().ledgerBroken);
    }
    const std::string three = readAll(ledger);
    CHECK(three.compare(0, first.size(), first) == 0);  // 앞줄은 그대로
    const LedgerState chain = checkLedger(three);
    CHECK(chain.ok && chain.hashes.size() == 3 && chain.lastDate == "2026-11-12");

    CHECK(!checkLedger(three.substr(0, three.size() - 1)).ok);  // 잘린 줄
    std::string edited = three;
    edited.replace(edited.find("\"visits\":3"), 10, "\"visits\":9");
    CHECK(!checkLedger(edited).ok);
    CHECK(!checkLedger(three.substr(first.size())).ok);  // 앞줄 삭제

    // 고친 파일, 뒷줄 삭제, 파일 삭제는 모두 경고하고 덧붙이지 않는다
    now = at(14, 9, 0, 0);
    for (const std::string& bad : {edited, first, std::string()}) {
        std::ofstream(ledger, std::ios::trunc) << bad;
        Stats stats(path, clock);
        CHECK(stats.summary().ledgerBroken && !stats.updateLedger());
        CHECK(statsPageHtml(stats.summary()).find("ledger 불일치") != std::string::npos);
        CHECK(readAll(ledger) == bad);
    }
    std::ofstream(ledger, std::ios::trunc) << three;  // 원래대로 돌리면 이어서 쓴다
    Stats stats(path, clock);
    CHECK(!stats.summary().ledgerBroken && checkLedger(readAll(ledger)).hashes.size() == 4);
    CHECK(statsPageHtml(stats.summary()).find("ledger 불일치") == std::string::npos);
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

// ---------------------------------------------------------------------------
// 공개 방
// ---------------------------------------------------------------------------
RoomSettings roomSettings(bool open, int bots = 0) {
    RoomSettings s;
    s.bots = bots;
    s.turnSeconds = 45;
    s.isPublic = open;
    return s;
}

std::string makeFor(RoomManager& rooms, const std::string& name, const RoomSettings& settings) {
    std::string error;
    CreateFail fail = CreateFail::None;
    return rooms.create("", name, settings, false, error, fail);
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void testNicknameFilter() {
    std::string error;
    for (const char* bad : {"a@b.com", "x.kr", "http", "www abc", "0101234567", "010-123-45", "씨발", "시 1 발", "FuCk", "병_신"}) {
        std::string name = bad;
        CHECK(!cleanNickname(name, error) && error == BAD_NICKNAME_MESSAGE);
    }
    for (const char* good : {"철수", "Kim_1", "player 07", "나"}) {
        std::string name = good;
        CHECK(cleanNickname(name, error));
    }
}

void testPublicListHidesPrivateRooms() {
    WebOptions options = fastOptions();
    options.maxRooms = 10;
    options.maxPlayers = 2;
    RoomManager rooms(options);
    const std::string secret = makeFor(rooms, "비밀방장", roomSettings(false));
    const std::string open = makeFor(rooms, "열린방장", roomSettings(true, 3));
    const std::string secretCode = rooms.find(secret)->code(), openCode = rooms.find(open)->code();
    const std::string secretId = rooms.find(secret)->id();

    std::vector<PublicRoom> list = rooms.publicRooms();
    CHECK(list.size() == 1 && list[0].name == "열린방장의 방" && list[0].players == 1 && list[0].maxPlayers == 2);
    CHECK(!list[0].playing && list[0].turnSeconds == 45 && list[0].chips == 10000 && list[0].bots == 3);
    std::string json = publicRoomsJson(list);
    CHECK(!has(json, "비밀") && !has(json, secretCode) && !has(json, openCode) && !has(json, secretId) && !has(json, "code"));
    CHECK(list[0].id != openCode && list[0].id.size() == 16);

    // 비공개 방은 식별자를 알아도 들어갈 수 없다
    std::string error;
    CHECK(rooms.joinPublic("", secretId, "손님", error).empty());
    // 가득 찬 방은 못 들어간다
    CHECK(!rooms.joinPublic("", list[0].id, "손님", error).empty());
    CHECK(rooms.publicRooms()[0].players == 2);
    CHECK(rooms.joinPublic("", list[0].id, "또손님", error).empty() && error == "이 방은 인원이 가득 찼어요.");

    // 대기실에서 방장만 바꿀 수 있고, 목록에 바로 반영된다
    CHECK(rooms.setPublic(secret, true, error) && rooms.publicRooms().size() == 2);
    CHECK(rooms.setPublic(secret, false, error) && rooms.publicRooms().size() == 1);
    CHECK(!rooms.setPublic("모르는토큰", true, error));

    // 게임이 시작되면 "게임 중"으로 남고 입장은 안 되며, 설정도 바꿀 수 없다
    CHECK(rooms.find(open)->start(open, error));
    list = rooms.publicRooms();
    CHECK(list.size() == 1 && list[0].playing);
    CHECK(!rooms.setPublic(open, false, error));
    rooms.leave(open);
}

void testKickAndHostHandover() {
    RoomManager rooms(fastOptions());
    const std::string host = makeFor(rooms, "방장", roomSettings(true));
    const std::string code = rooms.find(host)->code();
    const std::string id = rooms.find(host)->id();
    std::string error;
    bool unknown = false;
    const auto now = RoomManager::Clock::now();
    const std::string guest = rooms.join("", code, "손님", error, unknown, now, "bbbbbbbbbbbbbbbb");
    const std::string third = rooms.join("", code, "셋째", error, unknown, now, "cccccccccccccccc");
    CHECK(!guest.empty() && !third.empty());
    CHECK(!rooms.kick(guest, "셋째", error));  // 방장이 아니면 안 된다
    CHECK(!rooms.kick(host, "방장", error));   // 자기 자신은 안 된다
    CHECK(rooms.kick(host, "손님", error));
    CHECK(!rooms.find(guest) && rooms.closedMessage(guest) == KICKED_MESSAGE);
    // 같은 브라우저는 이름을 바꿔도, 코드로도 목록으로도 못 들어온다
    CHECK(rooms.join(guest, code, "새이름", error, unknown, now, "bbbbbbbbbbbbbbbb").empty() && error == BANNED_MESSAGE);
    CHECK(rooms.joinPublic("", id, "새이름", error, "bbbbbbbbbbbbbbbb").empty() && error == BANNED_MESSAGE);
    CHECK(rooms.join("", code, "새이름", error, unknown, now, "").empty());  // 식별값을 빼도 안 된다
    CHECK(!rooms.join("", code, "넷째", error, unknown, now, "dddddddddddddddd").empty());

    // 방장이 나가면 다음 사람에게 넘어간다
    rooms.leave(host);
    CHECK(rooms.find(third)->view(third)->isHost);
    CHECK(rooms.publicRooms()[0].name == "셋째의 방");
}

void testAbandonedPublicRoomCloses() {
    RoomManager rooms(fastOptions());
    const auto start = RoomManager::Clock::now();
    const std::string open = makeFor(rooms, "공개", roomSettings(true));
    const std::string secret = makeFor(rooms, "비공개", roomSettings(false));
    const std::string watched = makeFor(rooms, "지켜봄", roomSettings(true));
    rooms.seen(watched, start + 4min);
    CHECK(rooms.cleanupIdle(start + 4min) == 0);
    CHECK(rooms.cleanupIdle(start + 6min) == 1);  // 접속이 없는 공개 방만
    CHECK(!rooms.find(open) && rooms.find(secret) && rooms.find(watched));
    CHECK(rooms.closedMessage(open) == ROOM_CLOSED_MESSAGE);
}

void testPublicHttpAndLimits() {
    const std::string folder = tempFolder();
    WebOptions options = fastOptions();
    options.maxRooms = 2;
    options.listPerMinute = 4;
    options.joinPerMinute = 12;
    options.wrongCodeLimit = 3;
    options.stats = std::make_shared<Stats>(folder + "/stats.json");
    TestServer s(options);
    httplib::Client client("127.0.0.1", s.port);
    const httplib::Headers a{{"X-Client-Id", "aaaaaaaaaaaaaaaa"}}, b{{"X-Client-Id", "bbbbbbbbbbbbbbbb"}},
        c{{"X-Client-Id", "cccccccccccccccc"}};
    auto cookie = [](const httplib::Result& r) {
        const std::string v = r->get_header_value("Set-Cookie");
        return v.substr(0, v.find(';'));
    };
    auto hidden = client.Post("/api/room/create", a, R"({"name":"숨은방장"})", "application/json");
    auto shown = client.Post("/api/room/create", b, R"({"name":"열린방장","public":true})", "application/json");
    CHECK(hidden && hidden->status == 200 && shown && shown->status == 200);
    const std::string hiddenCode = s.rooms.find(cookie(hidden).substr(4))->code();
    const std::string shownCode = s.rooms.find(cookie(shown).substr(4))->code();

    // 방 제한은 공개 방에도 그대로
    const auto full = client.Post("/api/room/create", c, R"({"name":"셋째","public":true})", "application/json");
    CHECK(full && full->status == 429 && has(full->body, "ROOMS_FULL") && has(full->body, "방이 가득 찼어요"));

    const auto list = client.Get("/api/rooms/public", c);
    CHECK(list && list->status == 200 && has(list->body, "열린방장의 방"));
    CHECK(!has(list->body, "숨은") && !has(list->body, hiddenCode) && !has(list->body, shownCode));
    // 다른 API로도 비공개 방의 코드는 새지 않는다
    CHECK(!has(client.Get("/api/state", c)->body, hiddenCode));
    CHECK(!has(client.Get("/api/state", httplib::Headers{{"Cookie", cookie(shown)}})->body, hiddenCode));

    for (int i = 0; i < 3; ++i) CHECK(client.Get("/api/rooms/public", c)->status == 200);
    CHECK(client.Get("/api/rooms/public", c)->status == 429);
    CHECK(client.Get("/api/rooms/public", a)->status == 200);  // 다른 브라우저는 영향 없음

    // 목록의 식별자로 입장
    const std::size_t at = list->body.find("\"id\":\"") + 6;
    const std::string id = list->body.substr(at, 16);
    const auto joined = client.Post("/api/room/join", c, "{\"id\":\"" + id + "\",\"name\":\"손님\"}", "application/json");
    CHECK(joined && joined->status == 200);
    CHECK(options.stats->summary().publicRooms == 1 && options.stats->summary().publicJoins == 1);
    CHECK(has(statsPageHtml(options.stats->summary()), "<b>1</b><span>공개 방 입장</span>"));

    // 틀린 코드를 연속 3번 넣으면, 맞는 코드도 잠시 못 넣는다
    const httplib::Headers d{{"X-Client-Id", "dddddddddddddddd"}};
    for (int i = 0; i < 3; ++i) {
        CHECK(client.Post("/api/room/join", d, R"({"code":"x","name":"추측"})", "application/json")->status == 400);
    }
    const std::string right = "{\"code\":\"" + shownCode + "\",\"name\":\"추측\"}";
    CHECK(client.Post("/api/room/join", d, right, "application/json")->status == 429);

    // 입장 시도 횟수 제한
    const httplib::Headers e{{"X-Client-Id", "eeeeeeeeeeeeeeee"}};
    int last = 0;
    for (int i = 0; i < 13; ++i) {
        last = client.Post("/api/room/join", e, R"({"id":"0000000000000000","name":"반복"})", "application/json")->status;
    }
    CHECK(last == 429);
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
    testSha256();
    testLedger();
    testWriteFailureDoesNotStopGame();
    testStatsPage();
    testAssets();
    testNicknameFilter();
    testPublicListHidesPrivateRooms();
    testKickAndHostHandover();
    testAbandonedPublicRoomCloses();
    testPublicHttpAndLimits();
    std::printf("%d개 검사 중 %d개 실패\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
