// web_server.cpp - HTTP 서버 구현
#include "web_server.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "httplib.h"
#include "web_json.h"

namespace {

const char* const kJson = "application/json; charset=utf-8";

void sendError(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_content("{\"error\":" + jsonString(message) + "}", kJson);
}

// 한도를 넘었을 때: {"error":"ROOMS_FULL","message":"..."} 와 429
void sendLimit(httplib::Response& res, const std::string& code, const std::string& message) {
    res.status = 429;
    res.set_content("{\"error\":" + jsonString(code) + ",\"message\":" + jsonString(message) + "}", kJson);
}

// 브라우저가 보낸 식별값 (랜덤 ID의 해시). 모양이 이상하면 없는 것으로 본다.
std::string clientId(const httplib::Request& req) {
    const std::string id = req.get_header_value("X-Client-Id");
    return Stats::validIdHash(id) ? id : "";
}

// 통계에서 빼는 요청: 운영자가 표시한 브라우저(?owner=1)와 봇. 값은 저장하지 않는다.
bool counted(const httplib::Request& req) {
    return req.get_header_value("X-No-Stats") != "1" && !isBotAgent(req.get_header_value("User-Agent"));
}

// 토큰 비교에 걸리는 시간으로 값을 짐작할 수 없게, 끝까지 비교한다.
bool sameSecret(const std::string& given, const std::string& expected) {
    unsigned char diff = given.size() == expected.size() ? 0 : 1;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        diff |= static_cast<unsigned char>(expected[i] ^ (i < given.size() ? given[i] : 0));
    }
    return diff == 0;
}

// "Cookie: a=1; sid=abcd" 헤더에서 sid 값을 꺼낸다. 없으면 빈 문자열.
std::string sessionToken(const httplib::Request& req) {
    const std::string cookies = req.get_header_value("Cookie");
    std::istringstream in(cookies);
    std::string part;
    while (std::getline(in, part, ';')) {
        const std::size_t start = part.find_first_not_of(' ');
        if (start == std::string::npos) continue;
        if (part.compare(start, 4, "sid=") == 0) return part.substr(start + 4);
    }
    return "";
}

// web/index.html을 읽는다. 빌드할 때의 소스 폴더를 먼저, 없으면 현재 폴더 기준으로 찾는다.
std::string loadIndexHtml() {
    const char* candidates[] = {
#ifdef HOLDEM_WEB_DIR
        HOLDEM_WEB_DIR "/index.html",
#endif
        "web/index.html",
    };
    for (const char* path : candidates) {
        std::ifstream file(path, std::ios::binary);
        if (!file) continue;
        std::ostringstream content;
        content << file.rdbuf();
        return content.str();
    }
    return "";
}

// 파일 전체를 읽는다. 없으면 빈 문자열.
std::string readBinary(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

// 방 코드를 마구 대입해 보는 것을 막는다: 같은 IP에서 1분에 10번 틀리면 잠시 거절.
class JoinLimiter {
public:
    bool blocked(const std::string& ip) {
        std::lock_guard<std::mutex> lock(mutex_);
        forgetOld();
        const auto it = failures_.find(ip);
        return it != failures_.end() && it->second.count >= kMaxFailures;
    }
    void fail(const std::string& ip) {
        std::lock_guard<std::mutex> lock(mutex_);
        forgetOld();
        auto& f = failures_[ip];
        if (f.count++ == 0) f.since = std::chrono::steady_clock::now();
    }

private:
    static constexpr int kMaxFailures = 10;
    struct Failures {
        int count = 0;
        std::chrono::steady_clock::time_point since;
    };
    void forgetOld() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = failures_.begin(); it != failures_.end();) {
            it = now - it->second.since >= std::chrono::minutes(1) ? failures_.erase(it) : std::next(it);
        }
    }
    std::mutex mutex_;
    std::map<std::string, Failures> failures_;
};

// 1분 동안 key마다 limit번까지만 허용한다.
class RateLimiter {
public:
    bool allow(const std::string& key, int limit) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (now - since_ >= std::chrono::minutes(1) || counts_.size() > kMaxKeys) {
            counts_.clear();
            since_ = now;
        }
        return ++counts_[key] <= limit;
    }

private:
    static constexpr std::size_t kMaxKeys = 10000;  // 식별값을 마구 바꿔 보내도 메모리가 늘지 않게
    std::mutex mutex_;
    std::map<std::string, int> counts_;
    std::chrono::steady_clock::time_point since_ = std::chrono::steady_clock::now();
};

// 브라우저별로, 틀린 코드를 연속으로 limit번 넣으면 1분 동안 막는다. 맞히면 처음부터 다시 센다.
class WrongCodeGuard {
public:
    bool blocked(const std::string& key, int limit) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = fails_.find(key);
        if (it == fails_.end()) return false;
        if (std::chrono::steady_clock::now() - it->second.last >= std::chrono::minutes(1)) {
            fails_.erase(it);
            return false;
        }
        return it->second.count >= limit;
    }
    void fail(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fails_.size() > 10000) fails_.clear();
        Fails& f = fails_[key];
        ++f.count;
        f.last = std::chrono::steady_clock::now();
    }
    void success(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        fails_.erase(key);
    }

private:
    struct Fails {
        int count = 0;
        std::chrono::steady_clock::time_point last;
    };
    std::mutex mutex_;
    std::map<std::string, Fails> fails_;
};

using Body = std::map<std::string, JsonValue>;

// 요청 본문을 JSON으로 읽는다. 본문이 비어 있으면 빈 객체로 본다.
bool readBody(const httplib::Request& req, httplib::Response& res, Body& body) {
    if (req.body.empty()) return true;
    if (parseFlatJsonObject(req.body, body)) return true;
    sendError(res, 400, "요청 형식(JSON)이 잘못되었습니다.");
    return false;
}

// 아래 get* 함수들: 키가 없으면 value를 그대로 두고, 타입이 틀리면 false.
bool getString(const Body& body, const char* key, std::string& value) {
    const auto it = body.find(key);
    if (it == body.end()) return true;
    if (it->second.type != JsonValue::Type::String) return false;
    value = it->second.text;
    return true;
}

bool getInt(const Body& body, const char* key, int& value) {
    const auto it = body.find(key);
    if (it == body.end()) return true;
    const long long n = it->second.number;
    if (it->second.type != JsonValue::Type::Integer || n < -1000000000 || n > 1000000000) return false;
    value = static_cast<int>(n);
    return true;
}

bool getBool(const Body& body, const char* key, bool& value) {
    const auto it = body.find(key);
    if (it == body.end()) return true;
    if (it->second.type != JsonValue::Type::Boolean) return false;
    value = it->second.flag;
    return true;
}

void setSessionCookie(httplib::Response& res, const std::string& token) {
    // HttpOnly: 자바스크립트가 못 읽음, SameSite=Strict: 다른 사이트의 요청에는 안 실림
    res.set_header("Set-Cookie", "sid=" + token + "; Path=/; HttpOnly; SameSite=Strict");
}

// 방 만들기 공통 처리. solo면 바로 게임을 시작한다.
void createRoom(const httplib::Request& req, httplib::Response& res, RoomManager& rooms, bool solo) {
    Body body;
    if (!readBody(req, res, body)) return;

    const WebOptions& options = rooms.options();
    RoomSettings settings;
    settings.config = options.config;
    settings.turnSeconds = solo ? 0 : TURN_SECONDS_DEFAULT;
    settings.hint = solo;  // 친구 방에는 승률 힌트가 없다
    std::string name = solo ? "나" : "";
    std::string pace = "slow";
    std::string difficulty = "normal";
    if (!getString(body, "name", name) || !getInt(body, "chips", settings.config.startingChips) ||
        !getInt(body, "sb", settings.config.smallBlind) ||
        !getInt(body, "bb", settings.config.bigBlind) || !getInt(body, "bots", settings.bots) ||
        (solo && !getBool(body, "hint", settings.hint)) || !getString(body, "pace", pace) ||
        !parsePace(pace, settings.pace) ||
        (!solo && (!getInt(body, "turnSeconds", settings.turnSeconds) || !getInt(body, "extensions", settings.extensions) ||
                   !getBool(body, "public", settings.isPublic)))) {
        sendError(res, 400, "설정값의 형식이 잘못되었습니다.");
        return;
    }

    if (solo) {
        if (!getString(body, "difficulty", difficulty) ||
            (difficulty != "easy" && difficulty != "normal" && difficulty != "hard")) {
            sendError(res, 400, "difficulty는 easy, normal, hard 중 하나여야 합니다.");
            return;
        }
        settings.difficulty = difficulty == "easy"   ? Difficulty::Easy
                              : difficulty == "hard" ? Difficulty::Hard
                                                     : Difficulty::Normal;
    }

    rooms.cleanupIdle();  // 자리를 만들기 위해 오래된 방을 먼저 정리
    std::string error;
    CreateFail fail = CreateFail::None;
    const std::string token =
        rooms.create(sessionToken(req), name, settings, solo, error, fail, clientId(req), RoomManager::Clock::now(),
                     counted(req));
    if (token.empty()) {
        if (fail == CreateFail::RoomsFull) sendLimit(res, "ROOMS_FULL", error);
        else if (fail == CreateFail::ClientFull) sendLimit(res, "CLIENT_ROOMS_FULL", error);
        else sendError(res, 400, error);
        return;
    }
    setSessionCookie(res, token);
    res.set_content("{\"ok\":true}", kJson);
}

}  // namespace

void setupRoutes(httplib::Server& server, RoomManager& rooms, const std::string& indexHtml) {
    // 안전 설정: 큰 요청과 느린 연결을 제한한다.
    server.set_payload_max_length(4096);
    server.set_read_timeout(5, 0);
    server.set_write_timeout(5, 0);
    // 여러 사람이 1초마다 폴링하므로, 연결 하나가 작업 스레드를 붙잡고 있지 않게
    // 응답 후 바로 연결을 닫고 작업 스레드는 넉넉히 둔다.
    server.set_keep_alive_max_count(1);
    server.new_task_queue = [] { return new httplib::ThreadPool(32, 64); };

    // 처리 중 예외가 나도 서버는 계속 돌고, 그 요청만 500으로 응답한다.
    server.set_exception_handler(
        [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
            try {
                if (ep) std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "요청 처리 오류: %s\n", e.what());
            } catch (...) {
                std::fprintf(stderr, "요청 처리 중 알 수 없는 오류\n");
            }
            sendError(res, 500, "서버 내부 오류");
        });

    server.Get("/", [indexHtml, &rooms](const httplib::Request& req, httplib::Response& res) {
        // 링크 미리보기 봇은 스크립트를 실행하지 않으므로 첫 화면 요청에서 센다 (방문에는 넣지 않는다).
        if (rooms.options().stats && isBotAgent(req.get_header_value("User-Agent"))) rooms.options().stats->botHit();
        res.set_content(indexHtml, "text/html; charset=utf-8");
    });

    // 혼자 봇과 하기
    server.Post("/api/new", [&rooms](const httplib::Request& req, httplib::Response& res) {
        createRoom(req, res, rooms, true);
    });

    // 친구와 할 방 만들기
    server.Post("/api/room/create", [&rooms](const httplib::Request& req, httplib::Response& res) {
        createRoom(req, res, rooms, false);
    });

    auto limiter = std::make_shared<JoinLimiter>();
    auto guard = std::make_shared<WrongCodeGuard>();
    auto joins = std::make_shared<RateLimiter>();
    server.Post("/api/room/join", [&rooms, limiter, guard, joins](const httplib::Request& req, httplib::Response& res) {
        const WebOptions& options = rooms.options();
        const std::string client = clientId(req);
        const std::string who = client.empty() ? req.remote_addr : client;
        if (!joins->allow(who, options.joinPerMinute)) {
            sendLimit(res, "RATE_LIMIT", "입장을 너무 자주 시도했어요. 1분 뒤에 다시 해 주세요.");
            return;
        }
        Body body;
        if (!readBody(req, res, body)) return;
        std::string code, id, name;
        if (!getString(body, "code", code) || !getString(body, "id", id) || !getString(body, "name", name)) {
            sendError(res, 400, "code, id, name은 문자열이어야 합니다.");
            return;
        }
        std::string error;
        std::string token;
        if (!id.empty()) {  // 공개 방 목록에서 고른 방
            token = rooms.joinPublic(sessionToken(req), id, name, error, client, RoomManager::Clock::now(), counted(req));
        } else {
            // 코드를 마구 대입하지 못하게: IP별 합계와 브라우저별 연속 실패를 함께 본다.
            if (limiter->blocked(req.remote_addr) || (!client.empty() && guard->blocked(client, options.wrongCodeLimit))) {
                sendError(res, 429, "코드를 너무 많이 틀렸습니다. 1분 뒤에 다시 시도해 주세요.");
                return;
            }
            bool unknownCode = false;
            token = rooms.join(sessionToken(req), code, name, error, unknownCode, RoomManager::Clock::now(), client,
                               counted(req));
            if (unknownCode) {
                limiter->fail(req.remote_addr);
                if (!client.empty()) guard->fail(client);
            } else if (!token.empty() && !client.empty()) {
                guard->success(client);
            }
        }
        if (token.empty()) {
            sendError(res, 400, error);
            return;
        }
        setSessionCookie(res, token);
        res.set_content("{\"ok\":true}", kJson);
    });

    // 공개 방 목록. 비공개 방과 방 코드는 응답에 들어가지 않는다.
    auto lists = std::make_shared<RateLimiter>();
    server.Get("/api/rooms/public", [&rooms, lists](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        const std::string client = clientId(req);
        if (!lists->allow(client.empty() ? req.remote_addr : client, rooms.options().listPerMinute)) {
            sendLimit(res, "RATE_LIMIT", "목록을 너무 자주 불러왔어요. 잠시 뒤에 다시 해 주세요.");
            return;
        }
        rooms.cleanupIdle();
        res.set_content(publicRoomsJson(rooms.publicRooms()), kJson);
    });

    // ---- 채팅 (속도 제한과 검사는 모두 Room이 한다. 내용은 어디에도 기록하지 않는다) ----
    auto chatResult = [&rooms](httplib::Response& res, const std::string& token, bool ok, int waitMs,
                               const std::string& error) {
        if (ok) {
            rooms.touch(token);
            res.set_content("{\"ok\":true}", kJson);
            return;
        }
        res.status = waitMs > 0 ? 429 : 400;
        res.set_content("{\"error\":" + jsonString(error) + ",\"waitMs\":" + std::to_string(waitMs) + "}", kJson);
    };
    server.Post("/api/chat/react", [&rooms, chatResult](const httplib::Request& req, httplib::Response& res) {
        const std::string token = sessionToken(req);
        Body body;
        if (!readBody(req, res, body)) return;
        int emoji = 0, phrase = 0;
        if (!getInt(body, "emoji", emoji) || !getInt(body, "phrase", phrase)) {
            sendError(res, 400, "번호만 보낼 수 있습니다.");
            return;
        }
        const auto room = rooms.find(token);
        int waitMs = 0;
        std::string error = "들어가 있는 방이 없습니다.";
        const bool ok = room && room->react(token, body.count("emoji") ? std::optional<int>(emoji) : std::nullopt,
                                            body.count("phrase") ? std::optional<int>(phrase) : std::nullopt,
                                            waitMs, error);
        chatResult(res, token, ok, waitMs, error);
    });
    server.Post("/api/chat/say", [&rooms, chatResult](const httplib::Request& req, httplib::Response& res) {
        const std::string token = sessionToken(req);
        Body body;
        std::string text;
        // 줄바꿈 같은 제어 문자가 든 요청은 JSON을 읽는 단계에서 거절된다.
        if (!parseFlatJsonObject(req.body, body) || !getString(body, "text", text)) {
            sendError(res, 400, CHAT_BLOCKED_MESSAGE);
            return;
        }
        const auto room = rooms.find(token);
        int waitMs = 0;
        std::string error = "들어가 있는 방이 없습니다.";
        const bool ok = room && room->say(token, text, waitMs, error);
        chatResult(res, token, ok, waitMs, error);
    });
    server.Post("/api/chat/enable", [&rooms, chatResult](const httplib::Request& req, httplib::Response& res) {
        const std::string token = sessionToken(req);
        Body body;
        if (!readBody(req, res, body)) return;
        bool on = true;
        const auto room = rooms.find(token);
        std::string error = "요청이 잘못되었습니다.";
        const bool ok = room && body.count("on") && getBool(body, "on", on) && room->setChatOn(token, on, error);
        chatResult(res, token, ok, 0, error);
    });
    server.Post("/api/chat/mute", [&rooms, chatResult](const httplib::Request& req, httplib::Response& res) {
        const std::string token = sessionToken(req);
        Body body;
        if (!readBody(req, res, body)) return;
        std::string name;
        bool on = true;
        const auto room = rooms.find(token);
        std::string error = "요청이 잘못되었습니다.";
        const bool ok = room && getString(body, "name", name) && getBool(body, "on", on) && room->mute(token, name, on, error);
        chatResult(res, token, ok, 0, error);
    });
    // 봇 반응 받지 않기 ("효과 줄이기"를 켠 사람)
    server.Post("/api/chat/quiet", [&rooms](const httplib::Request& req, httplib::Response& res) {
        const std::string token = sessionToken(req);
        Body body;
        if (!readBody(req, res, body)) return;
        bool quiet = false;
        const auto room = rooms.find(token);
        if (!room || !getBool(body, "quiet", quiet)) {
            sendError(res, 400, "요청이 잘못되었습니다.");
            return;
        }
        room->setQuiet(token, quiet);
        res.set_content("{\"ok\":true}", kJson);
    });

    // 방장만, 대기실에서: 참가자 내보내기
    server.Post("/api/room/kick", [&rooms](const httplib::Request& req, httplib::Response& res) {
        const std::string token = sessionToken(req);
        Body body;
        if (!readBody(req, res, body)) return;
        std::string name, error;
        if (!getString(body, "name", name) || !rooms.kick(token, name, error)) {
            sendError(res, 400, error.empty() ? "name은 문자열이어야 합니다." : error);
            return;
        }
        rooms.touch(token);
        res.set_content("{\"ok\":true}", kJson);
    });

    // 방장만, 게임 시작 전: 공개/비공개 바꾸기
    server.Post("/api/room/visibility", [&rooms](const httplib::Request& req, httplib::Response& res) {
        const std::string token = sessionToken(req);
        Body body;
        if (!readBody(req, res, body)) return;
        bool open = false;
        std::string error;
        if (!body.count("public") || !getBool(body, "public", open)) {
            sendError(res, 400, "public은 true 또는 false여야 합니다.");
            return;
        }
        if (!rooms.setPublic(token, open, error)) {
            sendError(res, 400, error);
            return;
        }
        rooms.touch(token);
        res.set_content("{\"ok\":true}", kJson);
    });

    server.Post("/api/room/start", [&rooms](const httplib::Request& req, httplib::Response& res) {
        const std::string token = sessionToken(req);
        auto room = rooms.find(token);
        std::string error = "들어가 있는 방이 없습니다.";
        if (!room || !room->start(token, error)) {
            sendError(res, 400, error);
            return;
        }
        rooms.touch(token);
        res.set_content("{\"ok\":true}", kJson);
    });

    // 다음 단계 / 다음 판으로
    server.Post("/api/continue", [&rooms](const httplib::Request& req, httplib::Response& res) {
        const std::string token = sessionToken(req);
        auto room = rooms.find(token);
        Body body;
        if (!readBody(req, res, body)) return;
        std::optional<long long> gateId;
        const auto found = body.find("gateId");
        if (found != body.end()) {
            if (found->second.type != JsonValue::Type::Integer) {
                sendError(res, 400, "gateId는 정수여야 합니다.");
                return;
            }
            gateId = found->second.number;
        }
        std::string error = "들어가 있는 방이 없습니다.";
        if (!room || !room->proceed(token, gateId, error)) {
            sendError(res, 400, error);
            return;
        }
        rooms.touch(token);
        res.set_content("{\"ok\":true}", kJson);
    });

    server.Post("/api/skip", [&rooms](const httplib::Request& req, httplib::Response& res) {
        const std::string token = sessionToken(req);
        auto room = rooms.find(token);
        std::string error = "들어가 있는 방이 없습니다.";
        if (!room || !room->skipHand(token, error)) {
            sendError(res, 400, error);
            return;
        }
        rooms.touch(token);
        res.set_content("{\"ok\":true}", kJson);
    });

    // "자리 비움"에서 돌아오기
    server.Post("/api/back", [&rooms](const httplib::Request& req, httplib::Response& res) {
        const std::string token = sessionToken(req);
        auto room = rooms.find(token);
        std::string error = "들어가 있는 방이 없습니다.";
        if (!room || !room->comeBack(token, error)) {
            sendError(res, 400, error);
            return;
        }
        rooms.touch(token);
        res.set_content("{\"ok\":true}", kJson);
    });

    server.Post("/api/pace", [&rooms](const httplib::Request& req, httplib::Response& res) {
        const std::string token = sessionToken(req);
        auto room = rooms.find(token);
        Body body;
        if (!readBody(req, res, body)) return;
        std::string text;
        Pace pace = Pace::Slow;
        if (!getString(body, "pace", text) || !parsePace(text, pace)) {
            sendError(res, 400, "pace는 slow, normal, fast 중 하나여야 합니다.");
            return;
        }
        std::string error = "들어가 있는 방이 없습니다.";
        if (!room || !room->setPace(token, pace, error)) {
            sendError(res, 400, error);
            return;
        }
        rooms.touch(token);
        res.set_content("{\"ok\":true}", kJson);
    });

    // 방문 집계: 브라우저가 처음 열릴 때 한 번 부른다. 저장하는 것은 ID의 해시뿐이다.
    server.Post("/api/visit", [&rooms](const httplib::Request& req, httplib::Response& res) {
        const auto& stats = rooms.options().stats;
        std::map<std::string, JsonValue> body;
        std::string src;  // 유입 경로. 정해 둔 이름만 Stats가 센다.
        if (parseFlatJsonObject(req.body, body)) {
            const auto found = body.find("src");
            if (found != body.end() && found->second.type == JsonValue::Type::String) src = found->second.text;
        }
        if (stats && counted(req)) stats->visit(clientId(req), src);
        res.set_content("{\"ok\":true}", kJson);
    });

    // "게임 화면이 열려 있다"는 신호 (30초마다). 방 활동으로는 치지 않는다.
    server.Post("/api/beat", [&rooms](const httplib::Request& req, httplib::Response& res) {
        const auto& stats = rooms.options().stats;
        if (stats && counted(req)) stats->heartbeat(clientId(req));
        res.set_content("{\"ok\":true}", kJson);
    });

    // 사용 통계 화면. STATS_TOKEN이 없거나 틀리면 없는 주소처럼 404.
    server.Get("/stats", [&rooms](const httplib::Request& req, httplib::Response& res) {
        const WebOptions& options = rooms.options();
        if (options.statsToken.empty() || !options.stats ||
            !sameSecret(req.get_param_value("token"), options.statsToken)) {
            res.status = 404;
            return;
        }
        res.set_header("X-Robots-Tag", "noindex, nofollow");
        res.set_header("Cache-Control", "no-store");
        res.set_header("Referrer-Policy", "no-referrer");
        const auto [playing, open] = rooms.activity();
        res.set_content(statsPageHtml(options.stats->summary(), playing, open), "text/html; charset=utf-8");
    });

    server.Post("/api/room/leave", [&rooms](const httplib::Request& req, httplib::Response& res) {
        rooms.leave(sessionToken(req));
        res.set_content("{\"ok\":true}", kJson);
    });

    server.Get("/api/state", [&rooms](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        const std::string token = sessionToken(req);
        auto room = rooms.find(token);
        std::optional<RoomView> view;
        if (room) view = room->view(token);
        if (view) rooms.seen(token);
        if (!view) {  // 방이 없으면 설정 화면에 쓸 기본값을 알려 준다
            const std::string message = rooms.closedMessage(token);
            const std::string closed = message.empty() ? "" : ",\"closed\":" + jsonString(message);
            const GameConfig& c = rooms.options().config;
            res.set_content("{\"hasGame\":false,\"defaults\":{\"chips\":" +
                                std::to_string(c.startingChips) + ",\"sb\":" +
                                std::to_string(c.smallBlind) + ",\"bb\":" +
                                std::to_string(c.bigBlind) + "}" + closed + "}",
                            kJson);
            return;
        }
        res.set_content(viewToJson(*view), kJson);
    });

    server.Post("/api/action", [&rooms](const httplib::Request& req, httplib::Response& res) {
        const std::string token = sessionToken(req);
        auto room = rooms.find(token);
        if (!room) {
            sendError(res, 400, "게임이 없습니다. 새 게임을 시작해 주세요.");
            return;
        }

        Body body;
        if (!readBody(req, res, body)) return;
        const auto action = body.find("action");
        if (action == body.end() || action->second.type != JsonValue::Type::String) {
            sendError(res, 400, "action 문자열이 필요합니다.");
            return;
        }
        std::optional<long long> amount, turnId;
        for (auto [key, target] : {std::pair{"amount", &amount}, std::pair{"turnId", &turnId}}) {
            const auto found = body.find(key);
            if (found == body.end()) continue;
            if (found->second.type != JsonValue::Type::Integer) {
                sendError(res, 400, std::string(key) + "는 정수여야 합니다.");
                return;
            }
            *target = found->second.number;
        }

        std::string error;
        if (!room->submit(token, action->second.text, amount, turnId, error)) {
            sendError(res, 400, error);
            return;
        }
        rooms.touch(token);  // 올바른 입력만 "활동"으로 친다
        res.set_content("{\"ok\":true}", kJson);
    });
}

std::size_t setupAssets(httplib::Server& server, const std::string& folder) {
    const std::pair<const char*, const char*> files[] = {
        {"logo.svg", "image/svg+xml"},      {"favicon.svg", "image/svg+xml"},
        {"favicon.ico", "image/x-icon"},    {"apple-touch-icon.png", "image/png"},
        {"og-image.png", "image/png"},    {"fonts/coupe-brand.woff2", "font/woff2"},
    };
    std::size_t count = 0;
    for (const auto& [name, type] : files) {
        const std::string content = readBinary(folder + "/" + name);
        if (content.empty()) {
            std::fprintf(stderr, "그림 파일을 찾을 수 없습니다: %s/%s\n", folder.c_str(), name);
            continue;
        }
        server.Get(std::string("/") + name, [content, type = std::string(type)](const httplib::Request&, httplib::Response& res) {
            res.set_header("Cache-Control", "public, max-age=86400");
            res.set_content(content, type);
        });
        ++count;
    }
    return count;
}

int runWebServer(int port, const WebOptions& options) {
    const std::string indexHtml = loadIndexHtml();
    if (indexHtml.empty()) {
        std::cerr << "web/index.html을 찾을 수 없습니다. 프로젝트 폴더에서 실행해 주세요.\n";
        return 1;
    }

    RoomManager rooms(options);
    httplib::Server server;
    setupRoutes(server, rooms, indexHtml);
#ifdef HOLDEM_WEB_DIR
    setupAssets(server, std::ifstream(HOLDEM_WEB_DIR "/index.html") ? HOLDEM_WEB_DIR : "web");
#else
    setupAssets(server, "web");
#endif
    // 기본 설정은 같은 포트에 서버 두 개가 뜰 수 있어서, 주소 재사용만 허용하도록 바꾼다.
    server.set_socket_options([](auto sock) {
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
    });

    if (!server.bind_to_port("0.0.0.0", port)) {
        std::cerr << "포트 " << port << "를 열 수 없습니다. 이미 사용 중인지 확인해 주세요.\n";
        return 1;
    }

    // 1분마다 오래된 방을 정리하는 스레드
    std::mutex cleanupMutex;
    std::condition_variable cleanupCv;
    bool done = false;
    std::thread cleaner([&] {
        std::unique_lock<std::mutex> lock(cleanupMutex);
        while (!cleanupCv.wait_for(lock, std::chrono::minutes(1), [&] { return done; })) {
            rooms.cleanupIdle();
        }
    });

    std::cout << "웹 모드 시작: http://localhost:" << port << "  (외부에서는 http://서버IP:" << port
              << ")\n종료하려면 Ctrl+C" << std::endl;
    const bool ok = server.listen_after_bind();

    {
        std::lock_guard<std::mutex> lock(cleanupMutex);
        done = true;
    }
    cleanupCv.notify_all();
    cleaner.join();
    return ok ? 0 : 1;
}
