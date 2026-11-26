// test_web.cpp - 웹 모드 테스트 (방 정리, 잘못된 액션 거절, 남의 카드 비노출, 친구 방)
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

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
    bool full = false;
    return manager.create(token, name, settings, solo, error, full, now);
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
    for (std::size_t i = 0; i < RoomManager::kMaxRooms; ++i) {
        tokens.push_back(makeRoom(manager, "", "나", soloSettings(), true));
        CHECK(!tokens.back().empty());
    }
    CHECK(makeRoom(manager, "", "나", soloSettings(), true).empty());  // 21번째는 거절
    // 이미 방이 있는 사람은 자기 방을 새로 만들 수 있다
    CHECK(makeRoom(manager, tokens[0], "나", soloSettings(), true) == tokens[0]);
    CHECK(manager.size() == RoomManager::kMaxRooms);
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
    s = friendSettings();
    s.turnSeconds = 0;  // 친구 방은 제한 시간이 있어야 한다
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

int main() {
    testJsonParser();
    testActionValidation();
    testRoomCleanup();
    testRoomLimit();
    testSettingsAndNickname();
    testJoinAndLeave();
    testNoOtherCardsBeforeShowdown();
    testTurnTimeoutAndStaleTurn();
    testLeaveDuringGame();
    testHttpApi();
    std::printf("%d개 검사 중 %d개 실패\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
