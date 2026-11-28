// test_web.cpp - 웹 모드 테스트 (방 정리, 잘못된 액션 거절, 남의 카드 비노출, 친구 방)
#include <random>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "chat.h"
#include "check.h"
#include "httplib.h"
#include "web_json.h"
#include "web_server.h"
#include "web_session.h"

using namespace std::chrono_literals;

// 테스트에서는 쉬는 시간 없이 빠르게 진행한다.
static WebOptions fastOptions(unsigned seed = 1) {
    WebOptions o;
    o.seed = seed;
    o.hintTrials = 50;
    o.aiAction = 0ms;
    o.autoContinue = true;
    o.nextHandDelay = 30ms;
    return o;
}

static RoomSettings soloSettings() { return RoomSettings{}; }  // 봇 5, 무제한
static RoomSettings friendSettings(int bots = 0) {
    RoomSettings s;
    s.bots = bots;
    s.turnSeconds = 60;
    return s;
}

// 조건이 참이 될 때까지 최대 5초 기다린다.
template <class Predicate>
static bool waitFor(Predicate done) {
    for (int i = 0; i < 2500; ++i) {
        if (done()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

// ---------------------------------------------------------------------------
// JSON과 행동 검사
// ---------------------------------------------------------------------------
static void testJsonParser() {
    std::map<std::string, JsonValue> m;
    CHECK(parseFlatJsonObject("{\"action\":\"raise\",\"amount\":300}", m));
    CHECK(m["action"].text == "raise" && m["amount"].number == 300);
    CHECK(parseFlatJsonObject(" { } ", m) && m.empty());
    for (const char* bad : {"", "{", "[]", "{\"a\":}", "{\"a\":1,}", "{\"a\":1.5}", "{\"a\":{}}",
                            "{\"a\":1}x", "{\"a\":1,\"a\":2}", "{\"a\":99999999999999999999}",
                            "{\"a\":\"x}", "{a:1}"}) {
        CHECK(!parseFlatJsonObject(bad, m));
    }
    CHECK(jsonString("a\"b\\c\n") == "\"a\\\"b\\\\c\\n\"");
}

static void testActionValidation() {
    GameState s;  // 팟 300, 콜 100, 레이즈 200 ~ 5000인 상황
    s.pot = 300;
    s.currentBet = 100;
    s.toCall = 100;
    s.minRaiseTo = 200;
    s.maxRaiseTo = 5000;
    s.canRaise = true;
    Action a;
    std::string error;
    CHECK(buildWebAction("fold", {}, s, a, error) && a.type == ActionType::Fold);
    CHECK(buildWebAction("call", {}, s, a, error) && a.type == ActionType::Call);
    CHECK(buildWebAction("raise", 200, s, a, error) && a.amount == 200);
    CHECK(buildWebAction("raise", 5000, s, a, error) && a.type == ActionType::AllIn);
    CHECK(buildWebAction("allin", {}, s, a, error));
    CHECK(!buildWebAction("check", {}, s, a, error));
    CHECK(!buildWebAction("raise", {}, s, a, error));
    CHECK(!buildWebAction("raise", 199, s, a, error));
    CHECK(!buildWebAction("raise", 5001, s, a, error));
    CHECK(!buildWebAction("raise", -1, s, a, error));
    CHECK(!buildWebAction("raise", 100000000000LL, s, a, error));
    CHECK(!buildWebAction("dance", {}, s, a, error));

    s.toCall = 0;
    CHECK(buildWebAction("check", {}, s, a, error));
    CHECK(!buildWebAction("fold", {}, s, a, error));
    CHECK(!buildWebAction("call", {}, s, a, error));
    s.canRaise = false;
    CHECK(!buildWebAction("raise", 300, s, a, error));
}

// ---------------------------------------------------------------------------
// 방 관리
// ---------------------------------------------------------------------------
static std::string makeRoom(RoomManager& manager, const std::string& token, const std::string& name,
                            const RoomSettings& settings, bool solo,
                            RoomManager::Clock::time_point now = RoomManager::Clock::now()) {
    std::string error;
    CreateFail fail = CreateFail::None;
    return manager.create(token, name, settings, solo, error, fail, "", now);
}

static std::string joinRoom(RoomManager& manager, const std::string& code, const std::string& name,
                            const std::string& token = "") {
    std::string error;
    bool unknown = false;
    return manager.join(token, code, name, error, unknown);
}

static void testRoomCleanup() {
    RoomManager manager(fastOptions());
    const auto start = RoomManager::Clock::now();
    const std::string a = makeRoom(manager, "", "나", soloSettings(), true, start);
    const std::string b = makeRoom(manager, "", "나", soloSettings(), true, start);
    CHECK(a.size() == 32 && b.size() == 32 && a != b);

    CHECK(manager.cleanupIdle(start + 29min) == 0);
    manager.touch(b, start + 20min);  // b만 입력이 있었다
    CHECK(manager.cleanupIdle(start + 30min) == 1);
    CHECK(!manager.find(a) && manager.find(b));
    CHECK(manager.cleanupIdle(start + 50min) == 1);
    CHECK(manager.size() == 0);
}

static void testRoomLimit() {
    RoomManager manager(fastOptions());
    std::vector<std::string> tokens;
    for (std::size_t i = 0; i < static_cast<std::size_t>(MAX_ROOMS); ++i) {
        tokens.push_back(makeRoom(manager, "", "나", soloSettings(), true));
        CHECK(!tokens.back().empty());
    }
    CHECK(makeRoom(manager, "", "나", soloSettings(), true).empty());  // 21번째는 거절
    // 이미 방이 있는 사람은 자기 방을 새로 만들 수 있다
    CHECK(makeRoom(manager, tokens[0], "나", soloSettings(), true) == tokens[0]);
    CHECK(manager.size() == static_cast<std::size_t>(MAX_ROOMS));
}

static void testSettingsAndNickname() {
    std::string error;
    CHECK(validateSettings(soloSettings(), true, error));
    CHECK(validateSettings(friendSettings(0), false, error));
    RoomSettings s = soloSettings();
    s.bots = 0;
    CHECK(!validateSettings(s, true, error));  // 혼자 하기는 봇이 있어야 한다
    s = soloSettings();
    s.config.smallBlind = 200;  // SB > BB
    CHECK(!validateSettings(s, true, error));
    s = soloSettings();
    s.config.startingChips = 2000000;
    CHECK(!validateSettings(s, true, error));
    for (int seconds : {0, 15, 30, 45, 60}) {  // 친구 방에서 고를 수 있는 값
        s = friendSettings();
        s.turnSeconds = seconds;
        CHECK(validateSettings(s, false, error));
    }
    s = friendSettings();
    s.turnSeconds = 20;
    CHECK(!validateSettings(s, false, error));
    s = friendSettings();
    s.extensions = 4;
    CHECK(!validateSettings(s, false, error));
    s.extensions = -1;
    CHECK(!validateSettings(s, false, error));

    for (const char* good : {"철수", "Kim_1", "  영희  ", "ㅋㅋ"}) {
        std::string name = good;
        CHECK(cleanNickname(name, error));
    }
    for (const char* bad : {"", "   ", "=====", "a:b", "<b>", "열한글자짜리긴닉네임", "x\n", "\xff\xfe", "😀"}) {
        std::string name = bad;
        if (std::string(bad) == "열한글자짜리긴닉네임") name += "임";  // 11자
        CHECK(!cleanNickname(name, error));
    }
}

static void testJoinAndLeave() {
    RoomManager manager(fastOptions());
    const std::string host = makeRoom(manager, "", "방장", friendSettings(), false);
    auto room = manager.find(host);
    CHECK(room && room->code().size() == 6);
    if (!room) return;

    std::string error;
    CHECK(!room->start(host, error));  // 혼자이고 봇도 없으면 시작 불가
    CHECK(joinRoom(manager, "abcdef", "손님").empty());
    CHECK(joinRoom(manager, room->code(), "방장").empty());    // 같은 닉네임
    CHECK(joinRoom(manager, room->code(), "타이트").empty());  // 봇 이름
    std::vector<std::string> guests;
    for (int i = 1; i < kSeats; ++i) {
        guests.push_back(joinRoom(manager, room->code(), "손님" + std::to_string(i)));
        CHECK(!guests.back().empty());
    }
    CHECK(joinRoom(manager, room->code(), "일곱째").empty());  // 6명까지
    CHECK(!room->start(guests[0], error));                     // 방장만 시작

    manager.leave(host);  // 방장이 나가면 다음 사람이 방장
    CHECK(!manager.find(host));
    CHECK(room->view(guests[0]) && room->view(guests[0])->isHost);
    CHECK(room->start(guests[0], error));
    CHECK(joinRoom(manager, room->code(), "늦음").empty());  // 시작한 방에는 못 들어온다
    for (const std::string& g : guests) manager.leave(g);
    CHECK(manager.size() == 0);  // 모두 나가면 방이 없어진다

    // 혼자 하기 방은 코드를 알아도 못 들어간다
    const std::string solo = makeRoom(manager, "", "나", soloSettings(), true);
    auto soloRoom = manager.find(solo);
    CHECK(soloRoom && joinRoom(manager, soloRoom->code(), "손님").empty());
}

// ---------------------------------------------------------------------------
// 남의 카드 비노출: 사람 2명 + 봇 3명으로 여러 판을 진행하며 각자의 응답을 검사한다
// ---------------------------------------------------------------------------
static void testNoOtherCardsBeforeShowdown() {
    int showdownChecks = 0;
    for (unsigned seed = 1; seed <= 3; ++seed) {
        WebOptions options = fastOptions(seed);
        options.nextHandDelay = 60ms;
        Room room(options, friendSettings(3), "000000", false, seed);
        std::string error;
        CHECK(room.join("a", "철수", error) && room.join("b", "영희", error));
        CHECK(room.start("a", error));
        const std::string tokens[2] = {"a", "b"};

        int hand = 0;
        std::vector<std::string> earlier[2];  // 이번 판에서 쇼다운 전에 각자 받은 응답
        for (int step = 0; step < 4000 && hand <= 6; ++step) {
            bool over = false;
            for (int who = 0; who < 2; ++who) {
                const auto v = room.view(tokens[who]);
                CHECK(v.has_value());
                if (!v || v->table.state.players.empty()) continue;
                over = v->phase == RoomPhase::Over;
                CHECK(v->table.state.mySeat == who);
                if (v->table.state.handNumber != hand) {
                    hand = v->table.state.handNumber;
                    earlier[0].clear();
                    earlier[1].clear();
                }
                const std::string json = viewToJson(*v);
                if (!v->handOver) {
                    for (const auto& r : v->revealed) CHECK(!r.has_value());
                    earlier[who].push_back(json.substr(0, json.find("\"log\":")));
                } else {
                    // 공개된 남의 카드는 쇼다운 전 응답에는 없었어야 한다
                    for (std::size_t i = 0; i < v->revealed.size(); ++i) {
                        if (!v->revealed[i] || static_cast<int>(i) == who) continue;
                        CHECK(!v->table.state.players[i].folded);
                        for (const Card& c : *v->revealed[i]) {
                            const std::string text = "\"" + cardToString(c) + "\"";
                            for (const std::string& body : earlier[who]) {
                                CHECK(body.find(text) == std::string::npos);
                                ++showdownChecks;
                            }
                        }
                    }
                    earlier[who].clear();
                }
                if (v->myTurn) {
                    CHECK(!room.submit(tokens[1 - who], "call", {}, {}, error));  // 남의 차례에는 거절
                    room.submit(tokens[who], v->table.state.toCall > 0 ? "call" : "check", {},
                                v->turnId, error);
                }
            }
            if (over) break;
            std::this_thread::sleep_for(2ms);
        }
    }
    CHECK(showdownChecks > 0);
}

// 제한 시간이 지나면 자동으로 넘어가고, 지난 차례 번호는 거절된다
static void testTurnTimeoutAndStaleTurn() {
    RoomSettings settings = friendSettings(1);
    settings.turnSeconds = 1;  // 검사 함수를 거치지 않으므로 짧게 줄 수 있다
    settings.extensions = 0;
    Room room(fastOptions(), settings, "000000", false, 2);
    std::string error;
    CHECK(room.join("a", "철수", error) && room.start("a", error));
    CHECK(waitFor([&] { return room.view("a")->myTurn; }));
    const int first = room.view("a")->turnId;
    CHECK(!room.submit("a", "allin", {}, first + 1, error));
    // 아무것도 하지 않으면 1초 뒤 다음 차례로 넘어간다
    CHECK(waitFor([&] { return room.view("a")->turnId > first; }));
    CHECK(!room.submit("a", "allin", {}, first, error));
    bool logged = false;
    // view()는 복사본을 돌려주므로, 변수에 담아 둔 뒤에 순회해야 한다.
    const RoomView after = *room.view("a");
    for (const std::string& line : after.log) {
        if (line.find("시간 초과") != std::string::npos) logged = true;
    }
    CHECK(logged);
}

// 시간 제한 시험용: 1초 제한, 연장은 300ms, 여유 50ms
static WebOptions timerOptions(unsigned seed) {
    WebOptions o = fastOptions(seed);
    o.turnGrace = 50ms;
    o.extensionTime = 300ms;
    return o;
}

static int countLines(const RoomView& v, const std::string& part) {
    int n = 0;
    for (const std::string& line : v.log) {
        if (line.find(part) != std::string::npos) ++n;
    }
    return n;
}

// 사람 수 2, 4명: 시간이 다 되면 연장이 먼저 쓰이고, 그 뒤에 낼 칩이 있으면 폴드, 없으면 체크.
// 연속 3번 자동 처리되면 자리 비움이 되고, 그 뒤로는 기다리지 않는다.
static void testExtensionsAutoActionAndAway(int humans) {
    RoomSettings settings = friendSettings(0);
    settings.turnSeconds = 1;
    settings.extensions = 1;
    Room room(timerOptions(3), settings, "000000", false, 3);
    const std::vector<std::string> tokens = {"a", "b", "c", "d"};
    const std::vector<std::string> names = {"하나", "둘", "셋", "넷"};
    std::string error;
    for (int i = 0; i < humans; ++i) CHECK(room.join(tokens[i], names[i], error));
    CHECK(room.start("a", error));

    // 첫 차례: 낼 칩이 있다 (빅 블라인드 뒤)
    CHECK(waitFor([&] { return room.view("a")->turnSeat >= 0; }));
    const RoomView before = *room.view("a");
    const int seat = before.turnSeat;
    const std::string who = names[seat];
    CHECK(before.extensionsLeft.size() == static_cast<std::size_t>(humans) && before.extensionsLeft[seat] == 1);
    CHECK(before.timeTotalMs == 1000 && before.timeLeftMs > 0 && !before.timeExtended);
    CHECK(waitFor([&] { return room.view("a")->timeExtended; }));
    const RoomView extended = *room.view("a");
    CHECK(extended.turnSeat == seat && extended.extensionsLeft[seat] == 0 && extended.timeTotalMs == 300);
    CHECK(countLines(extended, who + ": 시간 연장") == 1);
    CHECK(waitFor([&] { return countLines(*room.view("a"), who + ": 폴드") == 1; }));
    CHECK(countLines(*room.view("a"), who + ": 시간 초과") == 1);

    // 아무도 누르지 않으면 모두 자리 비움이 되고 게임은 계속 간다
    bool everyoneAway = false;
    for (int i = 0; i < 6000 && !everyoneAway; ++i) {
        const RoomView v = *room.view("a");
        everyoneAway = std::all_of(v.away.begin(), v.away.end(), [](bool b) { return b; }) || v.phase == RoomPhase::Over;
        std::this_thread::sleep_for(5ms);
    }
    CHECK(everyoneAway);
    const RoomView idle = *room.view("a");
    for (int i = 0; i < humans; ++i) CHECK(idle.extensionsLeft[i] == 0);  // 판이 바뀌어도 다시 차지 않는다
    // 자리 비움이면 1초를 기다리지 않고 판이 빠르게 넘어간다
    const int hand = idle.table.state.handNumber;
    CHECK(waitFor([&] { return room.view("a")->table.state.handNumber >= hand + 3 || room.view("a")->phase == RoomPhase::Over; }));
}

// 베팅이 없으면 폴드가 아니라 체크
static void testTimeoutChecksWhenNothingToCall() {
    RoomSettings settings = friendSettings(0);
    settings.turnSeconds = 1;
    settings.extensions = 0;
    Room room(timerOptions(5), settings, "000000", false, 5);
    std::string error;
    CHECK(room.join("a", "하나", error) && room.join("b", "둘", error) && room.start("a", error));
    CHECK(waitFor([&] { return room.view("a")->turnSeat >= 0; }));
    const int seat = room.view("a")->turnSeat;
    const std::string first = seat == 0 ? "a" : "b", second = seat == 0 ? "b" : "a";
    CHECK(room.submit(first, "call", {}, room.view(first)->turnId, error));
    // 빅 블라인드는 낼 칩이 없다: 시간이 지나면 체크
    CHECK(waitFor([&] { return countLines(*room.view("a"), std::string(seat == 0 ? "둘" : "하나") + ": 체크") >= 1; }));
    CHECK(countLines(*room.view(second), std::string(seat == 0 ? "둘" : "하나") + ": 폴드") == 0);
}

// 자리 비움은 직접 돌아오면 풀리고, 직접 행동하면 연속 횟수가 지워진다
static void testComeBack() {
    RoomSettings settings = friendSettings(0);
    settings.turnSeconds = 1;
    settings.extensions = 0;
    WebOptions options = timerOptions(3);
    options.nextHandDelay = 100ms;
    Room room(options, settings, "000000", false, 3);
    std::string error;
    CHECK(room.join("a", "하나", error) && room.join("b", "둘", error) && room.start("a", error));
    // b는 바로 행동하고, a는 아무것도 하지 않는다
    bool away = false;
    for (int i = 0; i < 3000 && !away; ++i) {
        const RoomView b = *room.view("b");
        if (b.myTurn) room.submit("b", b.table.state.toCall > 0 ? "call" : "check", {}, b.turnId, error);
        away = room.view("a")->meAway;
        std::this_thread::sleep_for(5ms);
    }
    CHECK(away && !room.view("b")->meAway);
    CHECK(countLines(*room.view("a"), "하나: 시간 초과") == AWAY_AFTER);
    CHECK(viewToJson(*room.view("b")).find("\"away\":true") != std::string::npos);  // 다른 사람에게도 보인다
    CHECK(room.comeBack("a", error));
    CHECK(!room.view("a")->meAway && countLines(*room.view("a"), "하나: 자리로 돌아왔습니다") == 1);
    CHECK(waitFor([&] {
        const RoomView b = *room.view("b");
        if (b.myTurn) room.submit("b", b.table.state.toCall > 0 ? "call" : "check", {}, b.turnId, error);
        return room.view("a")->myTurn;  // 돌아온 뒤에는 다시 기다려 준다
    }));
}

// 시간 제한이 있는 친구 방은 방장이 누르지 않아도 다음 단계로 간다
static void testGateAutoAdvance() {
    RoomSettings settings = friendSettings(0);
    settings.turnSeconds = 1;
    WebOptions options = timerOptions(5);
    options.autoContinue = false;
    Room room(options, settings, "000000", false, 5);
    std::string error;
    CHECK(room.join("a", "하나", error) && room.join("b", "둘", error) && room.start("a", error));
    CHECK(waitFor([&] {
        for (const char* who : {"a", "b"}) {
            const RoomView v = *room.view(who);
            if (v.myTurn) room.submit(who, v.table.state.toCall > 0 ? "call" : "check", {}, v.turnId, error);
        }
        return room.view("a")->waiting == Waiting::Flop;
    }));
    CHECK(room.view("b")->gateSeconds >= 1);
    CHECK(waitFor([&] { return room.view("a")->table.state.board.size() == 3; }));

    settings.turnSeconds = 0;  // 제한 없음: 계속 기다린다
    Room open(options, settings, "000001", false, 5);
    CHECK(open.join("a", "하나", error) && open.join("b", "둘", error) && open.start("a", error));
    CHECK(waitFor([&] {
        for (const char* who : {"a", "b"}) {
            const RoomView v = *open.view(who);
            if (v.myTurn) open.submit(who, v.table.state.toCall > 0 ? "call" : "check", {}, v.turnId, error);
        }
        return open.view("a")->waiting == Waiting::Flop;
    }));
    std::this_thread::sleep_for(1300ms);
    CHECK(open.view("a")->waiting == Waiting::Flop && open.view("a")->gateSeconds == 0 && open.view("a")->timeTotalMs == 0);
}

// 친구 방은 힌트를 요청해도 승률을 계산하지도 보내지도 않는다
static void testNoHintInFriendRoom() {
    RoomSettings settings = friendSettings(1);
    settings.hint = true;
    Room room(fastOptions(2), settings, "000000", false, 2);
    std::string error;
    CHECK(room.join("a", "철수", error) && room.start("a", error));
    CHECK(waitFor([&] { return room.view("a")->myTurn; }));
    const RoomView v = *room.view("a");
    CHECK(!v.settings.hint && !v.equity);
    const std::string json = viewToJson(v);
    CHECK(json.find("\"hint\":null") != std::string::npos);
    CHECK(json.find("\"hint\":true") == std::string::npos && json.find("equity") == std::string::npos);
}

// 게임 중에 나가면 그 사람의 차례를 기다리지 않는다
static void testLeaveDuringGame() {
    Room room(fastOptions(), friendSettings(1), "000000", false, 4);
    std::string error;
    CHECK(room.join("a", "철수", error) && room.join("b", "영희", error));
    CHECK(room.start("a", error));
    CHECK(waitFor([&] { return room.view("a")->turnSeat >= 0; }));
    const int seat = room.view("a")->turnSeat;
    const std::string leaver = seat == 0 ? "a" : "b";
    const std::string stayer = seat == 0 ? "b" : "a";
    room.leave(leaver);
    CHECK(!room.view(leaver));
    CHECK(waitFor([&] { return room.view(stayer)->myTurn; }));  // 60초를 기다리지 않는다
}

// ---------------------------------------------------------------------------
// 실제 HTTP 서버: 400 거절, 방 만들기/들어가기, 코드 대입 제한
// ---------------------------------------------------------------------------
static void testHttpApi() {
    WebOptions options = fastOptions(7);
    options.nextHandDelay = 60s;  // 다음 판이 저절로 시작되지 않게 한다 (종료 시에는 바로 깨어난다)
    RoomManager manager(options);
    httplib::Server server;
    setupRoutes(server, manager, "<html>ok</html>");
    const int port = server.bind_to_any_port("127.0.0.1");
    CHECK(port > 0);
    std::thread thread([&] { server.listen_after_bind(); });
    server.wait_until_ready();

    {
        httplib::Client client("127.0.0.1", port);
        auto post = [&](const std::string& path, const std::string& body, const httplib::Headers& headers) {
            auto res = client.Post(path, headers, body, "application/json");
            return res ? res->status : -1;
        };
        auto cookieOf = [](const httplib::Result& res) {
            const std::string c = res ? res->get_header_value("Set-Cookie") : "";
            return httplib::Headers{{"Cookie", c.substr(0, c.find(';'))}};
        };

        auto page = client.Get("/");
        CHECK(page && page->status == 200 && page->body == "<html>ok</html>");
        auto none = client.Get("/api/state");
        CHECK(none && none->body.find("\"hasGame\":false") != std::string::npos);
        CHECK(post("/api/action", "{\"action\":\"call\"}", {}) == 400);  // 게임 없음

        CHECK(post("/api/new", "{\"bots\":9}", {}) == 400);
        CHECK(post("/api/new", "{\"chips\":\"many\"}", {}) == 400);
        CHECK(post("/api/room/create", "{}", {}) == 400);  // 닉네임 없음

        auto created = client.Post("/api/new", "{\"chips\":5000,\"bots\":2}", "application/json");
        CHECK(created && created->status == 200);
        CHECK(created && created->get_header_value("Set-Cookie").find("HttpOnly") != std::string::npos);
        const httplib::Headers headers = cookieOf(created);
        auto session = manager.find(headers.begin()->second.substr(4));
        CHECK(session != nullptr);

        if (session) {
            const std::string token = headers.begin()->second.substr(4);
            CHECK(waitFor([&] { return session->view(token)->myTurn; }));
            CHECK(session->view(token)->table.state.players.size() == 3);  // 나 + 봇 2
            const int turn = session->view(token)->turnId;
            for (const char* bad : {"", "not json", "{}", "{\"action\":5}", "{\"action\":\"dance\"}",
                                    "{\"action\":\"raise\"}", "{\"action\":\"raise\",\"amount\":1}",
                                    "{\"action\":\"raise\",\"amount\":99999999}",
                                    "{\"action\":\"raise\",\"amount\":-5}",
                                    "{\"action\":\"raise\",\"amount\":\"300\"}"}) {
                CHECK(post("/api/action", bad, headers) == 400);
            }
            CHECK(post("/api/action", std::string(10000, 'x'), headers) == 413);
            CHECK(session->view(token)->myTurn && session->view(token)->turnId == turn);
            // 올인하면 이번 판에는 내 차례가 다시 오지 않으므로 다음 요청은 400
            CHECK(post("/api/action", "{\"action\":\"allin\"}", headers) == 200);
            CHECK(post("/api/action", "{\"action\":\"allin\"}", headers) == 400);
        }

        // 방 만들기 -> 코드로 들어가기 -> 시작
        auto host = client.Post("/api/room/create", "{\"name\":\"철수\",\"bots\":0}", "application/json");
        CHECK(host && host->status == 200);
        const httplib::Headers hostCookie = cookieOf(host);
        auto room = manager.find(hostCookie.begin()->second.substr(4));
        CHECK(room != nullptr);
        if (room) {
            const std::string join = "{\"code\":\"" + room->code() + "\",\"name\":\"영희\"}";
            auto guest = client.Post("/api/room/join", join, "application/json");
            CHECK(guest && guest->status == 200);
            CHECK(post("/api/room/start", "{}", cookieOf(guest)) == 400);  // 방장 아님
            CHECK(post("/api/room/start", "{}", hostCookie) == 200);
            auto state = client.Get("/api/state", cookieOf(guest));
            CHECK(state && state->body.find("\"phase\":\"playing\"") != std::string::npos);
            CHECK(post("/api/room/leave", "{}", cookieOf(guest)) == 200);
        }

        // 없는 코드를 10번 넣으면 429
        for (int i = 0; i < 10; ++i) {
            CHECK(post("/api/room/join", "{\"code\":\"x\",\"name\":\"손님\"}", {}) == 400);
        }
        CHECK(post("/api/room/join", "{\"code\":\"x\",\"name\":\"손님\"}", {}) == 429);
    }

    server.stop();
    thread.join();
}

// 첫 상대가 아직 생각 중이어도, 나눠 받은 내 카드는 바로 보여야 한다
static void testMyCardsVisibleRightAfterDeal() {
    WebOptions options = fastOptions();
    options.aiAction = 30000ms;  // 첫 봇이 한참 생각한다
    Room room(options, soloSettings(), "000000", true, 1);
    std::string error;
    CHECK(room.join("a", "나", error) && room.start("a", error));
    CHECK(waitFor([&] { return room.view("a")->actionPhase == "thinking"; }));
    const RoomView v = *room.view("a");
    CHECK(!v.myTurn && v.actingSeat != v.table.state.mySeat);  // 봇이 먼저인 판
    CHECK(v.table.holeDealt);
}

// ---------------------------------------------------------------------------
// 채팅: 속도 제한, 필터, 방 권한
// ---------------------------------------------------------------------------
static void testChatLimiter() {
    const ChatRules rules;
    const auto t0 = std::chrono::steady_clock::now();
    ChatLimiter react;
    CHECK(react.react(t0, rules) == 0);
    CHECK(react.react(t0 + 2999ms, rules) > 0);
    CHECK(react.react(t0 + 3000ms, rules) == 0);

    ChatLimiter text;
    CHECK(text.text(t0, rules) == 0);
    CHECK(text.text(t0 + 1s, rules) > 0 && text.text(t0 + 1s, rules) <= 2001);
    CHECK(text.text(t0 + 3s, rules) == 0);
    CHECK(text.text(t0 + 4s, rules) > 0);
    // 10초 안에 6번째 시도: 30초 잠금
    CHECK(text.text(t0 + 5s, rules) > 29000);
    CHECK(text.text(t0 + 20s, rules) > 0 && text.textWait(t0 + 20s, rules) > 0);
    CHECK(text.text(t0 + 36s, rules) == 0);
    // 3초 간격을 지키면 계속 보낼 수 있다
    ChatLimiter calm;
    for (int i = 0; i < 20; ++i) CHECK(calm.text(t0 + std::chrono::seconds(3 * i), rules) == 0);
}

static void testChatFilter() {
    std::string error;
    for (const char* bad : {"http://a", "www.abc", "abc.com 와", "a . com", "naver.kr", "놀러와 닷컴", "me@x.com", "@insta",
                            "010-1234-5678", "01012345678", "010 1234 5678", "1588-1234", "줄\n바꿈", "탭\t", "a\xe2\x80\xae" "b",
                            "\xff", "", "   "}) {
        std::string text = bad;
        CHECK(!cleanChatText(text, error) && !error.empty());
    }
    for (const char* good : {"나이스!", "10000 걸었어?", "1000 2000", "콜 3번 했다", "안녕 :)", "ㅋㅋ.ㅋㅋ"}) {
        std::string text = good;
        CHECK(cleanChatText(text, error) && text == good);
    }
    std::string forty, fortyOne;
    for (int i = 0; i < 40; ++i) forty += "가";
    fortyOne = forty + "나";
    CHECK(cleanChatText(forty, error));
    CHECK(!cleanChatText(fortyOne, error));
    std::string text = "아 씨발 진짜";
    CHECK(cleanChatText(text, error) && text == "아 *** 진짜");
    text = "병 신 아님 FuCk";
    CHECK(cleanChatText(text, error) && text == "*** 아님 ***");
    text = "<b>굵게</b>";  // HTML은 글자 그대로 전달하고, 화면은 textContent로만 넣는다
    CHECK(cleanChatText(text, error) && text == "<b>굵게</b>");
}

static void testChatRoomRules() {
    WebOptions options = fastOptions();
    options.aiAction = 60000ms;  // 게임을 멈춰 두고 채팅만 본다
    options.chat.cooldown = 0ms;
    options.chat.burst = 1000;
    Room room(options, friendSettings(4), "000000", false, 1);
    std::string error;
    int wait = 0;
    CHECK(room.join("a", "방장", error) && room.join("b", "손님", error));
    CHECK(!room.react("a", 0, {}, wait, error));  // 대기실에서는 보내지 않는다
    CHECK(room.start("a", error));

    CHECK(!room.react("모르는사람", 0, {}, wait, error) && !room.say("모르는사람", "안녕", wait, error));
    CHECK(!room.view("모르는사람"));  // 방 사람이 아니면 메시지를 받지 못한다
    CHECK(room.react("a", 0, {}, wait, error) && room.react("b", {}, 2, wait, error));
    CHECK(!room.react("a", 6, {}, wait, error) && !room.react("a", {}, -1, wait, error));
    CHECK(!room.react("a", 0, 0, wait, error) && !room.react("a", {}, {}, wait, error));
    CHECK(room.say("b", "안녕하세요", wait, error));
    RoomView v = *room.view("a");
    CHECK(v.chat.free && v.chat.lines.size() == 3);
    CHECK(v.chat.lines[0].entry.name == "방장" && v.chat.lines[0].entry.text == chatEmojis()[0]);
    CHECK(v.chat.lines[1].entry.text == chatPhrases()[2] && v.chat.lines[2].entry.kind == "text");

    // 방장만 켜고 끄고, 금지한다
    CHECK(!room.setChatOn("b", false, error) && !room.mute("b", "방장", true, error));
    CHECK(room.setChatOn("a", false, error));
    CHECK(!room.say("b", "안녕", wait, error) && room.react("b", 1, {}, wait, error));
    CHECK(room.setChatOn("a", true, error) && room.mute("a", "손님", true, error));
    CHECK(!room.say("b", "안녕", wait, error) && error == "방장이 채팅을 금지했어요");
    CHECK(room.view("b")->chat.muted && room.view("a")->members[1].muted);
    CHECK(room.react("b", 1, {}, wait, error));  // 채팅 금지여도 이모지는 된다
    CHECK(room.mute("a", "손님", false, error) && room.say("b", "다시", wait, error));
    CHECK(!room.mute("a", "방장", true, error));

    for (int i = 0; i < CHAT_KEEP + 10; ++i) CHECK(room.say("a", "줄 " + std::to_string(i), wait, error));
    CHECK(room.view("a")->chat.lines.size() == static_cast<std::size_t>(CHAT_KEEP));
    CHECK(waitFor([&] { return !room.view("a")->log.empty(); }));
    for (const std::string& line : room.view("a")->log) CHECK(line.find("줄 ") == std::string::npos);  // 기록에 남기지 않는다

    room.leave("b");
    CHECK(!room.say("b", "나간 뒤", wait, error));
}

static void testChatNotInSoloOrPublic() {
    WebOptions options = fastOptions();
    options.aiAction = 60000ms;
    std::string error;
    int wait = 0;
    Room solo(options, soloSettings(), "000000", true, 1);
    CHECK(solo.join("a", "나", error) && solo.start("a", error));
    CHECK(!solo.view("a")->chat.free && !solo.say("a", "안녕", wait, error));
    CHECK(solo.react("a", 0, {}, wait, error));
    CHECK(!solo.react("a", 1, {}, wait, error) && wait > 0);  // 3초에 한 번 (이모지와 한마디가 함께)
    CHECK(!solo.react("a", {}, 1, wait, error) && wait > 0);

    RoomSettings open = friendSettings(1);
    open.isPublic = true;
    Room room(options, open, "000001", false, 1);
    CHECK(room.join("a", "방장", error) && room.start("a", error));
    CHECK(!room.view("a")->chat.free && !room.say("a", "안녕", wait, error));
    CHECK(!room.setChatOn("a", true, error));
    CHECK(room.react("a", {}, 0, wait, error));
}

static void testBotReaction() {
    for (const bool quiet : {false, true}) {
        WebOptions options = fastOptions(5);
        options.botsAlwaysTalk = true;
        options.botGap = 0ms;
        options.replyMin = 0ms;
        RoomSettings settings = soloSettings();
        settings.config.startingChips = 5000;
        settings.bots = 3;
        Room room(options, settings, "000000", true, 5);
        std::string error;
        CHECK(room.join("a", "나", error));
        room.setQuiet("a", quiet);
        CHECK(room.start("a", error));
        std::size_t most = 0;
        for (int i = 0; i < 2000; ++i) {
            const RoomView v = *room.view("a");
            if (v.myTurn) room.submit("a", "allin", {}, v.turnId, error);
            most = v.chat.lines.size();
            // 봇 한 명은 한 판에 많아야 한 번
            CHECK(static_cast<int>(most) <= v.table.state.handNumber * 3);
            if (v.phase == RoomPhase::Over || (!quiet && most > 0)) break;
            std::this_thread::sleep_for(2ms);
        }
        if (quiet) CHECK(most == 0);
        else CHECK(most >= 1);
    }
}

// ---------------------------------------------------------------------------
// 봇의 반응: 매핑 표, 답장, 도배 방지, 효과 줄이기
// ---------------------------------------------------------------------------
static void testBotReplyTable() {
    std::mt19937 rng(1);
    auto known = [](const BotRemark& r) {
        const auto& list = r.kind == "emoji" ? chatEmojis() : chatPhrases();
        return std::find(list.begin(), list.end(), r.text) != list.end();
    };
    std::vector<std::string> all = chatEmojis();
    all.insert(all.end(), chatPhrases().begin(), chatPhrases().end());
    for (const std::string& said : all) {
        for (const char* bot : {"타이트", "루즈", "공격형", "콜링스테이션", "확률계산"}) {
            for (int i = 0; i < 20; ++i) CHECK(known(botReplyRemark(bot, said, rng)));
        }
    }
    for (int i = 0; i < 100; ++i) {  // 요청한 대응
        const std::string laugh = botReplyRemark("루즈", "😂", rng).text;
        CHECK(laugh == "😎" || laugh == "ㅋㅋㅋ 인정");
        CHECK(botReplyRemark("루즈", "나이스 핸드!", rng).text != "실화냐");
        const std::string bluff = botReplyRemark("확률계산", "블러프 아님. 진짜임", rng).text;
        CHECK(bluff == "과연?" || bluff == "믿어볼까?");
    }
    for (const char* bot : {"타이트", "콜링스테이션"}) CHECK(botTalkRate("공격형") > botTalkRate(bot));
    for (BotEvent e : {BotEvent::WonBig, BotEvent::LostBig, BotEvent::BluffWorked, BotEvent::FacedAllIn}) {
        for (int i = 0; i < 50; ++i) CHECK(known(botEventRemark("공격형", e, rng)));
    }
}

static int botLines(const RoomView& v) {
    return static_cast<int>(std::count_if(v.chat.lines.begin(), v.chat.lines.end(),
                                          [](const ChatView::Line& l) { return l.entry.bot; }));
}

static void testBotRepliesWithoutSpam() {
    WebOptions options = fastOptions(4);
    options.aiAction = 60000ms;  // 첫 봇이 생각하는 동안 채팅만 본다
    options.chat.cooldown = 0ms;
    options.replyMin = 30ms;
    options.replyMax = 40ms;
    options.botGap = 300ms;
    RoomSettings settings = soloSettings();
    settings.bots = 2;
    Room room(options, settings, "000000", true, 4);
    std::string error;
    int wait = 0;
    CHECK(room.join("a", "나", error) && room.start("a", error));
    CHECK(waitFor([&] { return room.view("a")->actionPhase == "thinking"; }));

    for (int i = 0; i < 8; ++i) CHECK(room.react("a", 1, {}, wait, error));  // 연타
    CHECK(botLines(*room.view("a")) == 0);  // 바로 답하지 않는다
    std::this_thread::sleep_for(80ms);
    CHECK(botLines(*room.view("a")) == 1);  // 답장은 한 번
    CHECK(room.react("a", 0, {}, wait, error));  // 간격(300ms) 안
    std::this_thread::sleep_for(80ms);
    CHECK(botLines(*room.view("a")) == 1);
    std::this_thread::sleep_for(300ms);
    CHECK(room.react("a", {}, 0, wait, error));
    std::this_thread::sleep_for(80ms);
    RoomView v = *room.view("a");
    CHECK(botLines(v) == 2);  // 다른 봇이 답한다
    std::set<std::string> speakers;
    for (const auto& line : v.chat.lines) {
        if (line.entry.bot) speakers.insert(line.entry.name);
    }
    CHECK(speakers.size() == 2);
    std::this_thread::sleep_for(300ms);
    CHECK(room.react("a", 4, {}, wait, error));  // 두 봇 모두 이번 판에 이미 말했다
    std::this_thread::sleep_for(80ms);
    CHECK(botLines(*room.view("a")) == 2);

    // 효과 줄이기: 봇의 말을 보내지 않는다
    Room quiet(options, settings, "000001", true, 4);
    CHECK(quiet.join("a", "나", error));
    quiet.setQuiet("a", true);
    CHECK(quiet.start("a", error));
    CHECK(waitFor([&] { return quiet.view("a")->actionPhase == "thinking"; }));
    CHECK(quiet.react("a", 0, {}, wait, error));
    std::this_thread::sleep_for(80ms);
    CHECK(botLines(*quiet.view("a")) == 0);

    // 친구 방: 끈 사람에게만 안 보인다
    Room friends(options, friendSettings(2), "000002", false, 4);
    CHECK(friends.join("a", "하나", error) && friends.join("b", "둘", error));
    friends.setQuiet("b", true);
    CHECK(friends.start("a", error));
    CHECK(waitFor([&] { return !friends.view("a")->table.state.players.empty(); }));
    CHECK(friends.react("a", 0, {}, wait, error));
    std::this_thread::sleep_for(80ms);
    CHECK(botLines(*friends.view("a")) == 1 && botLines(*friends.view("b")) == 0);
}

int main() {
    testJsonParser();
    testActionValidation();
    testRoomCleanup();
    testRoomLimit();
    testSettingsAndNickname();
    testJoinAndLeave();
    testNoOtherCardsBeforeShowdown();
    testTurnTimeoutAndStaleTurn();
    testExtensionsAutoActionAndAway(2);
    testExtensionsAutoActionAndAway(4);
    testTimeoutChecksWhenNothingToCall();
    testComeBack();
    testGateAutoAdvance();
    testNoHintInFriendRoom();
    testMyCardsVisibleRightAfterDeal();
    testLeaveDuringGame();
    testHttpApi();
    testChatLimiter();
    testChatFilter();
    testChatRoomRules();
    testChatNotInSoloOrPublic();
    testBotReaction();
    testBotReplyTable();
    testBotRepliesWithoutSpam();
    std::printf("%d개 검사 중 %d개 실패\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
