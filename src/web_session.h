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
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "chat.h"
#include "game.h"
#include "review.h"
#include "stats.h"

constexpr int kSeats = 6;  // 한 테이블의 최대 인원 (사람 + 봇)

// ===== 진행 속도: 여기 한 값만 바꾸면 된다 =====
// "느리게"일 때 봇 한 명의 행동에 걸리는 시간(ms). 앞 절반은 "생각 중", 뒤 절반은 행동 공개.
// "보통"은 이 값의 3/5(3초), "빠르게"는 1/5(1초)이 된다.
constexpr int AI_ACTION_MS = 5000;
constexpr int AI_SKIP_MS = 200;  // "남은 진행 빠르게 보기"를 눌렀을 때 봇 한 명의 시간

// ===== 친구 방의 행동 제한 시간 =====
constexpr int TURN_SECONDS_DEFAULT = 45;  // 고를 수 있는 값: 15, 30, 45, 60, 0(제한 없음)
constexpr int EXTENSIONS_DEFAULT = 2;     // 사람마다 가지는 연장 횟수 (0~3). 게임을 새로 시작할 때만 채운다
constexpr int EXTENSION_SECONDS = 30;     // 연장 1회에 늘어나는 시간
constexpr int TURN_GRACE_MS = 1000;       // 네트워크 지연을 감안한 여유
constexpr int AWAY_AFTER = 3;             // 연속으로 이만큼 자동 처리되면 "자리 비움"
constexpr int AWAY_GATE_MS = 3000;        // 방장이 자리 비움일 때 "다음 단계"를 넘기는 시간

// ===== 방 제한: 기본값. 서버를 켤 때 같은 이름의 환경변수로 바꿀 수 있다 =====
constexpr int MAX_ROOMS = 20;              // 동시에 열려 있는 방 (혼자 하기 포함)
constexpr int MAX_PLAYERS_PER_ROOM = 6;    // 방 하나의 최대 인원 (봇 포함, 2~6)
constexpr int ROOM_IDLE_MINUTES = 30;      // 이 시간 동안 입력이 없으면 방을 지운다
constexpr int MAX_ROOMS_PER_CLIENT = 2;    // 한 브라우저가 동시에 만들 수 있는 방
constexpr const char* ROOM_CLOSED_MESSAGE = "오래 쓰지 않아 방이 닫혔어요";

// ===== 공개 방 =====
constexpr int EMPTY_PUBLIC_MINUTES = 5;  // 아무도 접속해 있지 않은 공개 방은 이 시간 뒤에 닫는다
constexpr int LIST_PER_MINUTE = 30;      // 브라우저 하나가 1분에 목록을 볼 수 있는 횟수
constexpr int JOIN_PER_MINUTE = 20;      // 브라우저 하나가 1분에 입장을 시도할 수 있는 횟수
constexpr int WRONG_CODE_LIMIT = 5;      // 틀린 코드를 연속으로 이만큼 넣으면 1분 동안 막는다
constexpr const char* KICKED_MESSAGE = "방장이 방에서 내보냈어요";
constexpr const char* BANNED_MESSAGE = "이 방에는 다시 들어올 수 없어요";
constexpr const char* BAD_NICKNAME_MESSAGE = "이 닉네임은 쓸 수 없어요";

enum class Pace { Slow, Normal, Fast };
bool parsePace(const std::string& text, Pace& out);  // "slow", "normal", "fast"
const char* paceName(Pace pace);

// 게임이 사람의 "다음" 입력을 기다리는 지점
enum class Waiting { None, Flop, Turn, River, Showdown, NextHand };

// 서버 전체 설정 (명령줄 옵션에서 온다)
struct WebOptions {
    GameConfig config;       // 방 설정의 기본값
    bool showHint = true;    // false면 어떤 방에서도 힌트를 보여 주지 않는다 (--no-hint)
    int hintTrials = 10000;
    std::optional<unsigned> seed;  // 없으면 게임마다 무작위
    std::chrono::milliseconds aiAction{AI_ACTION_MS};  // "느리게"의 봇 행동 시간 (테스트는 0)
    // 테스트용: true면 "다음" 입력을 기다리지 않는다. 다음 판은 nextHandDelay 뒤에 시작한다.
    bool autoContinue = false;
    std::chrono::milliseconds nextHandDelay{0};
    std::chrono::milliseconds turnGrace{TURN_GRACE_MS};
    std::chrono::milliseconds extensionTime{EXTENSION_SECONDS * 1000};
    std::chrono::milliseconds awayGate{AWAY_GATE_MS};
    int maxRooms = MAX_ROOMS;
    int maxPlayers = MAX_PLAYERS_PER_ROOM;
    int idleMinutes = ROOM_IDLE_MINUTES;
    int maxRoomsPerClient = MAX_ROOMS_PER_CLIENT;
    int emptyPublicMinutes = EMPTY_PUBLIC_MINUTES;
    int listPerMinute = LIST_PER_MINUTE;
    int joinPerMinute = JOIN_PER_MINUTE;
    int wrongCodeLimit = WRONG_CODE_LIMIT;
    ChatRules chat;                 // 채팅 속도 제한
    int botReactionPercent = 60;    // 큰 판이 끝났을 때 봇이 반응할 확률
    std::shared_ptr<Stats> stats;  // 없으면 통계를 세지 않는다
    std::string statsToken;        // 비어 있으면 /stats는 404
};

// 방을 만들 때 고르는 설정
struct RoomSettings {
    GameConfig config;
    int bots = 5;         // 원하는 봇 수. 사람이 많으면 남는 자리만큼만 앉는다.
    bool hint = true;     // 승률 힌트. 혼자 하기에만 있다 (친구 방은 Room이 항상 끈다)
    int turnSeconds = 0;  // 결정 한 번에 주어지는 시간. 0이면 제한 없음 (혼자 할 때는 항상 0)
    int extensions = EXTENSIONS_DEFAULT;  // 사람마다 가지는 시간 연장 횟수
    Pace pace = Pace::Slow;  // 처음 진행 속도 (게임 중에도 방장이 바꿀 수 있다)
    bool isPublic = false;   // 처음 공개 여부 (대기실에서 방장이 바꿀 수 있다)
};

// 공개 방 목록의 한 줄. 방 코드는 담지 않는다.
struct PublicRoom {
    std::string id;    // 입장에만 쓰는 임시 식별자
    std::string name;  // "방장 닉네임의 방" (사용자가 입력하는 글은 없다)
    int players = 0;
    int maxPlayers = 0;
    bool playing = false;  // 게임 중 (중간 입장은 안 된다)
    int turnSeconds = 0;
    int chips = 0;
    int bots = 0;
};
std::string publicRoomsJson(const std::vector<PublicRoom>& rooms);

// 설정값이 허용 범위인지 검사한다. solo는 혼자 하기.
bool validateSettings(const RoomSettings& settings, bool solo, std::string& error);

// 닉네임 검사: 한글, 영문, 숫자, 공백, '_', '-' 만 1~10자. 앞뒤 공백은 잘라서 name에 다시 넣는다.
// (로그 줄에 이름이 그대로 들어가므로 특수 기호는 받지 않는다)
bool cleanNickname(std::string& name, std::string& error);

enum class RoomPhase { Lobby, Playing, Over };

// 채팅 한 줄. 정해진 목록이나 사람이 직접 쓴 글만 담는다 (카드나 승률은 들어올 길이 없다).
struct ChatEntry {
    long long seq = 0;
    int seat = -1;
    std::string name;
    std::string kind;  // "emoji", "phrase", "text"
    std::string text;
    std::chrono::steady_clock::time_point at;
};

struct ChatView {
    struct Line {
        ChatEntry entry;
        int ageMs = 0;
    };
    std::vector<Line> lines;
    bool free = false;     // 자유 입력이 있는 방인가 (비공개 친구 방)
    bool on = true;        // 방장이 자유 입력을 켜 두었는가
    bool muted = false;    // 내가 채팅 금지인가
    bool quiet = false;    // 내가 봇 반응을 끄겠다고 했는가
    int reactWaitMs = 0;
    int textWaitMs = 0;
};

struct MemberInfo {
    std::string name;
    bool muted = false;
    bool isHost = false;
    bool isMe = false;
};

// 방금 공개된 행동 (말풍선용). 문구는 행동 기록 줄에서 이름을 뺀 부분이다.
struct ActionNote {
    int seq = 0;  // 새 행동마다 1씩 증가
    int seat = -1;
    std::string text;  // "콜 100", "폴드", "레이즈 400 (올인)"
};

// 보는 사람의 지금 족보 (플랍 이후)
struct MyHand {
    std::string name;  // "원 페어 A"
    std::string code;  // "ONE_PAIR" (족보 패널에서 내 행을 찾는 값)
    int level = 1;     // 1 ~ 6
    bool boardOnly = false;  // 공용 카드만으로 만들어진 패 (모두가 같이 쓴다)
};

// 한 사람의 시점에서 본 방. 이 구조체에는 그 사람이 봐도 되는 정보만 담긴다.
struct RoomView {
    RoomPhase phase = RoomPhase::Lobby;
    std::string code;
    bool solo = false;
    bool isHost = false;
    bool isPublic = false;
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
    int timeLeftMs = 0;   // 지금 차례의 남은 시간. 시간은 서버만 잰다
    int timeTotalMs = 0;  // 지금 구간의 전체 시간 (0이면 제한 시간이 없거나 사람 차례가 아님)
    bool timeExtended = false;      // 지금 연장 시간을 쓰는 중인가
    std::vector<int> extensionsLeft;  // 사람 좌석별 남은 연장 횟수
    std::vector<bool> away;           // 사람 좌석별 "자리 비움"
    ChatView chat;
    bool meAway = false;
    int gateSeconds = 0;  // "다음"을 안 눌러도 넘어가기까지 남은 초 (0이면 계속 기다림)
    std::optional<double> equity;
    int opponents = 0;
    bool handOver = false;
    int actingSeat = -1;  // 지금 결정 중인 좌석 (봇 포함. turnSeat은 사람만)
    std::string actionPhase;  // "thinking"(생각 중), "revealed"(행동 공개), ""
    std::optional<ActionNote> lastAction;
    std::optional<MyHand> myHand;
    Pace pace = Pace::Slow;
    Waiting waiting = Waiting::None;
    int gateId = 0;  // 기다리는 지점마다 1씩 증가 (두 번 눌러 두 단계를 넘기지 않게)
    bool canContinue = false;  // 내가 "다음"을 누를 수 있는가 (방장)
    bool canSkip = false;      // "남은 진행 빠르게 보기"를 누를 수 있는가
    bool skipping = false;
    // 마지막으로 끝난 판의 리뷰. 다음 판이 진행되는 동안에도 남는다.
    std::shared_ptr<const HandReview> review;
    std::optional<ReviewFlow> reviewFlow;  // 보는 사람 자신의 흐름 (쇼다운에 참여했을 때)
    std::string message;
    std::deque<std::string> log;
};

// 웹에서 온 행동 이름("fold", "check", "call", "raise", "allin")을 검사해서 Action으로 바꾼다.
// 지금 상황에서 허용되지 않으면 false와 함께 error에 안내 문구를 채운다.
bool buildWebAction(const std::string& name, std::optional<long long> amount, const GameState& state,
                    Action& out, std::string& error);

// 리뷰를 JSON 객체로 바꾼다. flow는 없으면 nullptr.
std::string reviewToJson(const HandReview& review, const ReviewFlow* flow);

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
    bool join(const std::string& token, std::string name, std::string& error, const std::string& clientId = "");
    // 방장만, 대기실에서만. 이름의 참가자를 내보내고 그 접속 토큰을 kicked에 담는다.
    bool kick(const std::string& token, const std::string& name, std::string& kicked, std::string& error);
    // 내보낸 적이 있는 접속 토큰이나 브라우저인가
    bool banned(const std::string& token, const std::string& clientId) const;
    // 방장만, 게임 시작 전에만. 이 방이 처음 공개되면 firstTime이 true.
    bool setPublic(const std::string& token, bool open, bool& firstTime, std::string& error);
    bool isPublic() const;
    // 공개 방 목록에 실을 정보. 비공개, 혼자 하기, 끝난 게임이면 빈 값.
    std::optional<PublicRoom> publicInfo() const;
    const std::string& id() const { return id_; }
    // 방을 나간다. 게임 중이면 남은 칩은 자동으로 체크/폴드하며 사라진다.
    void leave(const std::string& token);
    // 방장만. 로비에서 시작하거나, 끝난 게임을 다시 시작한다.
    bool start(const std::string& token, std::string& error);
    // 내 행동. turnId를 주면 그 차례가 아직 유효할 때만 받는다.
    bool submit(const std::string& token, const std::string& action, std::optional<long long> amount,
                std::optional<long long> turnId, std::string& error);

    // 방장만. 기다리는 단계를 넘긴다. gateId를 주면 그 지점이 아직 유효할 때만 받는다.
    bool proceed(const std::string& token, std::optional<long long> gateId, std::string& error);
    // 방장만. 더 행동할 사람이 없을 때, 이번 판의 남은 진행을 빠르게 넘긴다.
    bool skipHand(const std::string& token, std::string& error);
    bool setPace(const std::string& token, Pace pace, std::string& error);
    // "자리 비움"에서 돌아온다.
    bool comeBack(const std::string& token, std::string& error);

    // 채팅. 방 사람만, 게임이 시작된 뒤에만. 너무 빠르면 false와 함께 waitMs에 기다릴 시간.
    // 이모지(emoji)나 빠른 말(phrase)은 목록의 번호만 받는다.
    bool react(const std::string& token, std::optional<int> emoji, std::optional<int> phrase, int& waitMs,
               std::string& error);
    // 자유 채팅. 비공개 친구 방에서만.
    bool say(const std::string& token, std::string text, int& waitMs, std::string& error);
    bool setChatOn(const std::string& token, bool on, std::string& error);  // 방장만
    bool mute(const std::string& token, const std::string& name, bool on, std::string& error);  // 방장만
    void setQuiet(const std::string& token, bool quiet);  // 봇 반응 받지 않기 (효과 줄이기)

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
        std::string client;  // 브라우저 식별값 (내보내기에만 쓰고, 메모리에만 둔다)
    };

    // 아래 두 함수는 mutex_를 잡은 상태에서 부른다.
    const Member* findMember(const std::string& token) const;
    void addLog(const std::string& line);

    void run(std::vector<std::string> humanNames, int botCount, unsigned seed);  // 게임 스레드
    Action waitForAction(int seat, const GameState& state);
    void pause(std::chrono::milliseconds time);  // stop()이 오면 즉시 깨어나는 잠
    bool isHostLocked(const std::string& token) const;
    bool hostAwayLocked() const;
    // 아래 둘은 mutex_를 잡은 상태에서
    bool freeChatLocked() const { return !solo_ && !public_; }
    void addChatLocked(int seat, const std::string& name, const std::string& kind, const std::string& text);
    void countChatLocked();
    // 게임 스레드: 혼자 하기에서 큰 판이 끝나면 봇 하나가 가끔 반응한다
    void botReaction(const HandReview& review, int humans);
    void presentLocked(int seat);  // 직접 누른 사람: 자리 비움을 풀고 연속 횟수를 지운다
    bool canSkipLocked(const Member& viewer) const;
    // ---- 아래는 게임 스레드에서만 ----
    void think(int seat);         // "생각 중" 표시 후 대기
    void finishAction();          // 공개한 행동을 잠시 보여 주고 표시를 지운다
    void paceHalf();              // 봇 행동 시간의 절반만큼 대기
    void waitAt(Waiting point);   // "다음" 입력을 기다린다
    // 끝난 판의 리뷰를 공개하고, 힌트가 켜져 있으면 승률 흐름을 이어서 계산한다.
    void publishReview(const HandSummary& hand, const std::vector<std::string>& names, int humans,
                       std::vector<int>& previousRanks);
    bool seatLeft(int seat) const;               // mutex_를 잡은 상태에서

    const WebOptions options_;
    const RoomSettings settings_;
    const std::string code_;
    const bool solo_;
    const std::string id_;  // 공개 목록에 쓰는 식별자. 방 코드와 상관없는 난수

    std::mutex startMutex_;  // start()가 동시에 두 번 실행되지 않게 한다

    mutable std::mutex mutex_;  // 아래 변수들을 보호한다
    std::condition_variable cv_;
    std::vector<Member> members_;  // 첫 번째 사람(나가지 않은)이 방장
    bool public_ = false;
    bool everPublic_ = false;
    std::deque<ChatEntry> chat_;  // 최근 CHAT_KEEP개. 방과 함께 사라지고 어디에도 저장하지 않는다
    long long chatSeq_ = 0;
    std::map<std::string, ChatLimiter> limits_;  // 접속 토큰별
    std::set<std::string> muted_;                // 채팅 금지 토큰
    std::set<std::string> quiet_;                // 봇 반응을 끈 토큰
    bool chatOn_ = true;
    bool chatCounted_ = false;  // 통계의 "채팅을 쓴 방"에 이미 셌는가
    std::mt19937 chatRng_{std::random_device{}()};  // 게임 스레드만. 게임의 난수와 섞지 않는다
    std::set<std::string> banned_;  // 내보낸 사람의 토큰과 브라우저 식별값
    RoomPhase phase_ = RoomPhase::Lobby;
    unsigned nextSeed_;
    bool stopping_ = false;

    std::vector<TableSnapshot> tables_;  // 좌석별 시점의 테이블 (사람 좌석만 사용)
    std::vector<std::optional<std::array<Card, 2>>> revealed_;
    bool handOver_ = false;
    int actingSeat_ = -1;
    std::string actionPhase_;
    std::optional<ActionNote> lastAction_;
    int actionSeq_ = 0;
    std::vector<std::string> seatNames_;
    Pace pace_;
    Waiting waiting_ = Waiting::None;
    int gateId_ = 0;
    bool continue_ = false;
    bool skip_ = false;
    bool holdAction_ = false;  // 게임 스레드만: 공개한 봇 행동을 아직 보여 주는 중
    std::shared_ptr<const HandReview> review_;
    std::vector<std::optional<ReviewFlow>> flows_;  // 좌석별 (사람 좌석만)
    std::deque<std::string> log_;

    int turnSeat_ = -1;  // 입력을 기다리는 좌석
    int turnId_ = 0;
    GameState turnState_;
    std::optional<Action> pending_;
    std::optional<double> equity_;
    std::chrono::steady_clock::time_point deadline_;
    std::chrono::milliseconds turnTotal_{0};
    bool turnExtended_ = false;
    std::vector<int> extensions_;  // 좌석별 남은 연장 (사람 좌석만)
    std::vector<int> autoStreak_;  // 좌석별 연속 자동 처리 횟수
    std::vector<bool> away_;       // 좌석별 자리 비움
    std::optional<std::chrono::steady_clock::time_point> gateDeadline_;

    std::mt19937 hintRng_;  // 게임 스레드만 사용
    std::thread thread_;    // startMutex_로 보호
};

// 방을 만들지 못한 이유
enum class CreateFail { None, Invalid, RoomsFull, ClientFull };

// 토큰 -> 방. 모든 함수는 여러 스레드에서 동시에 불러도 안전하다.
class RoomManager {
public:
    using Clock = std::chrono::steady_clock;

    explicit RoomManager(WebOptions options) : options_(std::move(options)) {}

    // 방을 만들고 들어간다. 성공하면 접속 토큰, 실패하면 빈 문자열.
    // 이미 다른 방에 있던 토큰이면 그 방에서는 나간다. 실패하면 fail에 이유를 담는다.
    // clientId는 브라우저가 보낸 식별값. 비어 있으면 브라우저별 제한 없이 전체 한도만 적용한다.
    std::string create(const std::string& token, const std::string& name, const RoomSettings& settings,
                       bool solo, std::string& error, CreateFail& fail, const std::string& clientId = "",
                       Clock::time_point now = Clock::now());

    // 코드로 방에 들어간다. 성공하면 접속 토큰. unknownCode는 "그런 방이 없어서" 실패했는지.
    std::string join(const std::string& token, const std::string& code, const std::string& name,
                     std::string& error, bool& unknownCode, Clock::time_point now = Clock::now(),
                     const std::string& clientId = "");
    // 공개 방 목록의 식별자로 들어간다. 비공개가 된 방은 없는 방으로 대한다.
    std::string joinPublic(const std::string& token, const std::string& id, const std::string& name,
                           std::string& error, const std::string& clientId = "",
                           Clock::time_point now = Clock::now());
    // 공개 방만. 들어갈 수 있는 방이 앞에 온다.
    std::vector<PublicRoom> publicRooms() const;
    bool kick(const std::string& token, const std::string& name, std::string& error,
              Clock::time_point now = Clock::now());
    bool setPublic(const std::string& token, bool open, std::string& error);
    // 그 방의 누군가가 화면을 보고 있다 (빈 공개 방을 가리는 데만 쓴다)
    void seen(const std::string& token, Clock::time_point now = Clock::now());
    // 방이 없어진 사람에게 보여 줄 안내. 없으면 빈 문자열.
    std::string closedMessage(const std::string& token) const;

    void leave(const std::string& token);

    // token이 들어가 있는 방. 없으면 nullptr
    std::shared_ptr<Room> find(const std::string& token) const;

    // "입력이 있었다"고 기록한다. 화면 갱신용 조회(GET)는 입력으로 치지 않는다.
    void touch(const std::string& token, Clock::time_point now = Clock::now());

    // idleMinutes 이상 입력이 없는 방을 지우고, 지운 개수를 돌려준다.
    std::size_t cleanupIdle(Clock::time_point now = Clock::now());

    // 이 토큰이 있던 방이 오래 쓰지 않아 지워졌는가 (leave, 새 방 만들기/들어가기를 하면 잊는다)
    bool closedByIdle(const std::string& token) const;

    std::size_t size() const;
    const WebOptions& options() const { return options_; }

private:
    struct Entry {
        std::shared_ptr<Room> room;
        Clock::time_point lastInput;
        std::string creator;  // 만든 브라우저의 식별값 (메모리에만 둔다)
        Clock::time_point lastSeen;
    };

    // mutex_를 잡은 상태에서: 이미 찾은 방에 들어간다. 성공하면 접속 토큰.
    std::string enterLocked(const std::string& token, std::map<std::string, Entry>::iterator it,
                            const std::string& name, std::string& error, const std::string& clientId,
                            Clock::time_point now, std::vector<std::shared_ptr<Room>>& removed);

    // mutex_를 잡은 상태에서 부른다. 방이 비면 removed로 옮긴다
    // (방은 mutex를 푼 뒤에 없어져야 한다. 소멸자가 게임 스레드를 기다리기 때문).
    void leaveLocked(const std::string& token, std::vector<std::shared_ptr<Room>>& removed);

    const WebOptions options_;
    mutable std::mutex mutex_;  // 아래 변수들을 보호한다
    std::map<std::string, Entry> rooms_;         // 방 코드 -> 방
    std::map<std::string, std::string> tokens_;  // 접속 토큰 -> 방 코드
    std::map<std::string, std::pair<Clock::time_point, std::string>> closed_;  // 안내를 보여 줄 토큰
    unsigned roomsCreated_ = 0;
};
