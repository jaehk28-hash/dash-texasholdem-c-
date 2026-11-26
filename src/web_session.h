// web_session.h - 게임 방(Room)과 방 모음(RoomManager)
//
// "혼자 봇과 하기"도 사람 1명짜리 방이다. 친구들과 할 때는 6자리 코드로 같은 방에 들어온다.
// 접속자는 쿠키의 토큰으로 구분한다.
//
// 스레드 구조
//  - 게임이 시작된 방마다 "게임 스레드"가 하나 있어서 Game::playHand()를 계속 돌린다.
//  - HTTP 요청 스레드는 Game 객체를 직접 만지지 않는다.
//    게임 스레드가 mutex를 잡고 복사해 둔 정보만 읽고,
//    행동은 mutex + 조건변수(condition_variable)로 게임 스레드에 전달한다.
//  - 잠금 순서는 항상 RoomManager -> Room 이다 (반대로 잡지 않으므로 교착이 생기지 않는다).
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "game.h"

constexpr int kSeats = 6;  // 한 테이블의 최대 인원 (사람 + 봇)

// 서버 전체 설정 (명령줄 옵션에서 온다)
struct WebOptions {
    GameConfig config;       // 방 설정의 기본값
    bool showHint = true;    // false면 어떤 방에서도 힌트를 보여 주지 않는다 (--no-hint)
    int hintTrials = 10000;
    std::optional<unsigned> seed;  // 없으면 게임마다 무작위
    std::chrono::milliseconds botDelay{500};        // 봇이 행동하기 전에 쉬는 시간
    std::chrono::milliseconds nextHandDelay{5000};  // 판이 끝나고 다음 판까지 결과를 보여 주는 시간
};

// 방을 만들 때 고르는 설정
struct RoomSettings {
    GameConfig config;
    int bots = 5;         // 원하는 봇 수. 사람이 많으면 남는 자리만큼만 앉는다.
    bool hint = true;     // 승률 힌트
    int turnSeconds = 0;  // 한 차례의 제한 시간. 0이면 무제한 (혼자 할 때)
};

// 설정값이 허용 범위인지 검사한다. solo는 혼자 하기.
bool validateSettings(const RoomSettings& settings, bool solo, std::string& error);

// 닉네임 검사: 한글, 영문, 숫자, 공백, '_', '-' 만 1~10자. 앞뒤 공백은 잘라서 name에 다시 넣는다.
// (로그 줄에 이름이 그대로 들어가므로 특수 기호는 받지 않는다)
bool cleanNickname(std::string& name, std::string& error);

enum class RoomPhase { Lobby, Playing, Over };

struct MemberInfo {
    std::string name;
    bool isHost = false;
    bool isMe = false;
};

// 한 사람의 시점에서 본 방. 이 구조체에는 그 사람이 봐도 되는 정보만 담긴다.
struct RoomView {
    RoomPhase phase = RoomPhase::Lobby;
    std::string code;
    bool solo = false;
    bool isHost = false;
    RoomSettings settings;
    std::vector<MemberInfo> members;  // 방에 있는 사람들 (봇 제외)

    // ---- 게임이 시작된 뒤 ----
    TableSnapshot table;  // 공개 정보 + 내 홀 카드. table.state.mySeat이 내 자리
    // 좌석별로 "공개해도 되는" 카드. 쇼다운에서 폴드하지 않은 좌석만 값이 들어간다.
    std::vector<std::optional<std::array<Card, 2>>> revealed;
    bool myTurn = false;
    int turnSeat = -1;  // 지금 결정 중인 사람의 좌석 (봇 차례면 -1)
    int turnId = 0;     // 사람 차례가 올 때마다 1씩 증가
    int secondsLeft = 0;  // 제한 시간이 있을 때 남은 초
    std::optional<double> equity;
    int opponents = 0;
    bool handOver = false;
    std::string message;
    std::deque<std::string> log;
};

// 웹에서 온 행동 이름("fold", "check", "call", "raise", "allin")을 검사해서 Action으로 바꾼다.
// 지금 상황에서 허용되지 않으면 false와 함께 error에 안내 문구를 채운다.
bool buildWebAction(const std::string& name, std::optional<long long> amount, const GameState& state,
                    Action& out, std::string& error);

// RoomView를 GET /api/state 응답 JSON으로 바꾼다.
std::string viewToJson(const RoomView& view);

// 게임 방 하나. 모든 public 함수는 여러 스레드에서 동시에 불러도 안전하다.
class Room {
public:
    Room(const WebOptions& options, const RoomSettings& settings, std::string code, bool solo,
         unsigned seed);
    ~Room();
    Room(const Room&) = delete;
    Room& operator=(const Room&) = delete;

    // 로비에 있는 방에 들어온다. 첫 번째로 들어온 사람이 방장.
    bool join(const std::string& token, std::string name, std::string& error);
    // 방을 나간다. 게임 중이면 남은 칩은 자동으로 체크/폴드하며 사라진다.
    void leave(const std::string& token);
    // 방장만. 로비에서 시작하거나, 끝난 게임을 다시 시작한다.
    bool start(const std::string& token, std::string& error);
    // 내 행동. turnId를 주면 그 차례가 아직 유효할 때만 받는다.
    bool submit(const std::string& token, const std::string& action, std::optional<long long> amount,
                std::optional<long long> turnId, std::string& error);

    // token의 시점에서 본 방. 방 사람이 아니면 빈 값.
    std::optional<RoomView> view(const std::string& token) const;
    bool empty() const;  // 남아 있는 사람이 없는가

    void stop();  // 게임 스레드에 "그만하라"고 알린다 (기다리지 않는다)
    const std::string& code() const { return code_; }
    bool solo() const { return solo_; }

private:
    friend class WebHumanPlayer;
    friend class PacedPlayer;

    struct Member {
        std::string token;
        std::string name;
        int seat = -1;      // 게임 시작 전에는 -1
        bool left = false;  // 게임 중에 나간 사람
    };

    // 아래 두 함수는 mutex_를 잡은 상태에서 부른다.
    const Member* findMember(const std::string& token) const;
    void addLog(const std::string& line);

    void run(std::vector<std::string> humanNames, int botCount, unsigned seed);  // 게임 스레드
    Action waitForAction(int seat, const GameState& state);
    void pause(std::chrono::milliseconds time);  // stop()이 오면 즉시 깨어나는 잠
    bool seatLeft(int seat) const;               // mutex_를 잡은 상태에서

    const WebOptions options_;
    const RoomSettings settings_;
    const std::string code_;
    const bool solo_;

    std::mutex startMutex_;  // start()가 동시에 두 번 실행되지 않게 한다

    mutable std::mutex mutex_;  // 아래 변수들을 보호한다
    std::condition_variable cv_;
    std::vector<Member> members_;  // 첫 번째 사람(나가지 않은)이 방장
    RoomPhase phase_ = RoomPhase::Lobby;
    unsigned nextSeed_;
    bool stopping_ = false;

    std::vector<TableSnapshot> tables_;  // 좌석별 시점의 테이블 (사람 좌석만 사용)
    std::vector<std::optional<std::array<Card, 2>>> revealed_;
    bool handOver_ = false;
    std::deque<std::string> log_;

    int turnSeat_ = -1;  // 입력을 기다리는 좌석
    int turnId_ = 0;
    GameState turnState_;
    std::optional<Action> pending_;
    std::optional<double> equity_;
    std::chrono::steady_clock::time_point deadline_;

    std::mt19937 hintRng_;  // 게임 스레드만 사용
    std::thread thread_;    // startMutex_로 보호
};

// 토큰 -> 방. 모든 함수는 여러 스레드에서 동시에 불러도 안전하다.
class RoomManager {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kMaxRooms = 20;
    static constexpr std::chrono::minutes kIdleLimit{30};

    explicit RoomManager(WebOptions options) : options_(std::move(options)) {}

    // 방을 만들고 들어간다. 성공하면 접속 토큰, 실패하면 빈 문자열.
    // 이미 다른 방에 있던 토큰이면 그 방에서는 나간다. full은 "자리가 없어서" 실패했는지.
    std::string create(const std::string& token, const std::string& name, const RoomSettings& settings,
                       bool solo, std::string& error, bool& full, Clock::time_point now = Clock::now());

    // 코드로 방에 들어간다. 성공하면 접속 토큰. unknownCode는 "그런 방이 없어서" 실패했는지.
    std::string join(const std::string& token, const std::string& code, const std::string& name,
                     std::string& error, bool& unknownCode, Clock::time_point now = Clock::now());

    void leave(const std::string& token);

    // token이 들어가 있는 방. 없으면 nullptr
    std::shared_ptr<Room> find(const std::string& token) const;

    // "입력이 있었다"고 기록한다. 화면 갱신용 조회(GET)는 입력으로 치지 않는다.
    void touch(const std::string& token, Clock::time_point now = Clock::now());

    // kIdleLimit 이상 입력이 없는 방을 지우고, 지운 개수를 돌려준다.
    std::size_t cleanupIdle(Clock::time_point now = Clock::now());

    std::size_t size() const;
    const WebOptions& options() const { return options_; }

private:
    struct Entry {
        std::shared_ptr<Room> room;
        Clock::time_point lastInput;
    };

    // mutex_를 잡은 상태에서 부른다. 방이 비면 removed로 옮긴다
    // (방은 mutex를 푼 뒤에 없어져야 한다. 소멸자가 게임 스레드를 기다리기 때문).
    void leaveLocked(const std::string& token, std::vector<std::shared_ptr<Room>>& removed);

    const WebOptions options_;
    mutable std::mutex mutex_;  // 아래 변수들을 보호한다
    std::map<std::string, Entry> rooms_;         // 방 코드 -> 방
    std::map<std::string, std::string> tokens_;  // 접속 토큰 -> 방 코드
    unsigned roomsCreated_ = 0;
};
