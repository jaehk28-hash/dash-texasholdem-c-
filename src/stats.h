// stats.h - 익명 사용 통계 (숫자만 쌓는다)
//
// 저장하는 것: 횟수, 날짜, 시각(동시 접속 최고치), 브라우저가 만든 랜덤 ID의 "해시".
// 날짜는 서버 설정과 상관없이 한국 시간이다. 지난 날짜의 집계는 ledger(ledger.h)에 덧붙인다.
// 저장하지 않는 것: 닉네임, IP, 카드, 방 코드 등 사람을 알아볼 수 있는 정보.
//
// 파일 쓰기는 별도 스레드가 한다. 실패해도 게임에는 영향이 없고 로그만 남긴다.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 브라우저 종류와 기기 구간. User-Agent 원문은 저장하지 않고 아래 번호만 남긴다.
constexpr int kBrowserKinds = 9;
const char* browserKindName(int kind);  // Chrome, Safari, Edge, Firefox, 삼성 인터넷, 웨일, 카카오톡 인앱, 인스타그램 인앱, 기타
int browserKind(const std::string& userAgent);
constexpr int kDevices = 3;  // 0 모바일, 1 태블릿, 2 데스크톱
const char* deviceName(int device);
// touchMac: iPad는 Mac처럼 알려 오므로, 화면이 터치되는 Mac은 태블릿으로 본다
int deviceKind(const std::string& userAgent, bool touchMac);

constexpr int kTimeBuckets = 5;  // 1분 미만, 1~5분, 5~15분, 15~30분, 30분 이상
int timeBucket(long long seconds);

// 브라우저가 30초마다 보내는 신호 하나
struct Beat {
    std::string id;          // 랜덤 ID의 해시
    bool game = true;        // 게임 화면이 보이는 중 ("지금 접속 중"에 센다)
    long long playSec = 0;   // 지난 신호 이후 게임 화면을 보며 조작이 있었던 시간
    long long staySec = 0;   // 그 밖의 화면(시작, 설정, 규칙, 족보 등)의 시간
    bool final = false;      // 화면을 떠나면서 보낸 마지막 신호
    int kind = kBrowserKinds - 1;
    int device = 2;
};

struct KindStats {
    long long visits = 0;    // 방문 수 (같은 브라우저는 하루 한 번)
    long long browsers = 0;  // 처음 본 브라우저 수
    long long done = 0;      // 그중 한 판 이상 완료한 브라우저
    long long playSec = 0, totalSec = 0, sessions = 0;
};

// 화면에 보이는 한 브라우저의 집계. 식별값은 담지 않는다.
struct VisitorStats {
    std::string firstDate, lastDate;
    long long playSec = 0, sessions = 0, hands = 0;
    int device = 2;
};

struct DayStats {
    std::string date;  // "2026-11-28"
    long long hands = 0;
    long long visitors = 0;  // 그날의 고유 방문자 수
    long long active = 0;    // 그날 한 판 이상 끝낸 고유 브라우저 수 ("활성 플레이어")
    long long peak = 0;      // 동시 접속 최고치
    long long peakMinute = 0;  // 그 시각 (한국 시간 0시부터 몇 분)
    long long playSec = 0;       // 그날의 플레이 시간 합
    long long playBrowsers = 0;  // 플레이 시간이 있었던 브라우저 수
    long long longestSec = 0;    // 가장 긴 세션 (플레이 + 체류)
};

struct StatsSummary {
    long long totalVisits = 0;  // 같은 ID는 하루에 한 번만 센 합계
    long long todayVisits = 0;
    long long uniqueVisitors = 0;
    long long soloStarts = 0;
    long long roomStarts = 0;
    long long publicRooms = 0;  // 공개 방 생성 수
    long long publicJoins = 0;  // 공개 방 입장 수
    long long chatRooms = 0;    // 채팅(이모지, 한마디, 글)을 쓴 방 수
    long long chatSends = 0;    // 사람이 보낸 횟수 (내용은 저장하지 않는다)
    long long online = 0;            // 지금 접속 중 (최근 60초 안에 신호를 보낸 브라우저)
    long long todayActive = 0;       // 오늘의 활성 플레이어
    long long returningPlayers = 0;  // 서로 다른 날 이틀 이상 한 판을 끝낸 브라우저
    long long botHits = 0;           // 봇이 첫 화면을 가져간 횟수 (방문에는 세지 않는다)
    long long totalHands = 0;
    long long todayHands = 0;
    double handsPerGame = 0;
    int topBots = -1;  // 가장 많이 쓰인 봇 수 설정. 아직 없으면 -1
    bool ledgerBroken = false;  // ledger 파일이 바뀌었거나 줄이 사라졌다
    long long todayAvgPlaySec = 0;  // 오늘 플레이한 브라우저 하나의 평균
    long long todayLongestSec = 0;
    long long playHist[kTimeBuckets] = {};  // 세션 수 (플레이 시간 구간별)
    long long stayHist[kTimeBuckets] = {};  // 세션 수 (체류 시간 구간별)
    KindStats kinds[kBrowserKinds];
    std::vector<VisitorStats> top;  // 플레이 시간이 긴 순서로 최대 20
    std::vector<DayStats> days;  // 오래된 날부터, 마지막이 오늘
};

class Stats {
public:
    using Clock = std::function<std::time_t()>;
    static constexpr std::size_t kMaxVisitorIds = 20000;  // 파일이 끝없이 커지지 않게
    static constexpr int kOnlineSeconds = 60;  // 이 시간 안에 신호가 있으면 "접속 중"
    static constexpr int kBeatMinSeconds = 15;      // 이보다 자주 온 신호는 버린다 (마지막 신호는 예외)
    static constexpr int kBeatGapSeconds = 90;      // 이보다 벌어진 구간은 시간에 넣지 않는다
    static constexpr int kSessionGapSeconds = 1800; // 이만큼 조용하면 다음 신호부터 새 세션
    static constexpr int kTopVisitors = 20;

    // path의 파일이 있으면 이어서 센다. clock은 테스트에서 날짜를 바꾸기 위한 것.
    explicit Stats(std::string path, Clock clock = {});
    ~Stats();  // 남은 변경을 저장하고 끝낸다
    Stats(const Stats&) = delete;
    Stats& operator=(const Stats&) = delete;

    // 아래 함수들은 여러 스레드에서 불러도 되고, 예외를 던지지 않는다.
    // 같은 ID는 하루에 한 번만. src는 유입 경로(?src=)이고, 정해 둔 이름이 아니면 "other"로 센다.
    // kind(브라우저 종류)를 주면 브라우저별 집계에도 반영한다.
    void visit(const std::string& idHash, const std::string& src = "", int kind = -1, int device = 2);
    void beat(const Beat& beat);
    // 운영자 표시가 있는 브라우저: 브라우저별 목록에서 감춘다 (저장된 숫자는 지우지 않는다)
    void exclude(const std::string& idHash);
    void gameStarted(bool solo, int bots);
    void handFinished();
    void handPlayed(const std::string& idHash);  // 그 브라우저가 한 판을 끝까지 함께했다
    // 게임 화면이 열려 있다는 신호. 메모리에서만 세고, 파일에는 그날의 최고치와 시각만 남긴다.
    void heartbeat(const std::string& idHash);
    void botHit();
    void publicRoomCreated();
    void publicJoined();
    void chatSent(bool firstInRoom);
    StatsSummary summary(int days = 14) const;

    bool flush();  // 지금 바로 파일에 쓴다. 실패하면 false

    // ledger를 검증하고, 어제까지 빠진 날짜를 덧붙인다. 시작할 때와 날짜가 바뀔 때 저절로 불린다.
    // 체인이 깨졌으면 아무것도 쓰지 않고 false.
    bool updateLedger();
    const std::string& ledgerPath() const { return ledgerPath_; }  // stats.json과 같은 폴더

    // 해시로 받을 수 있는 모양인가 (16진수 8~64자)
    static bool validIdHash(const std::string& text);

private:
    void load();
    std::string today() const;
    std::string serializeLocked() const;
    void changedLocked();
    void bump(const char* key);  // 숫자 하나를 늘린다
    void writerLoop();

    const std::string path_;
    const std::string ledgerPath_;
    const Clock clock_;

    mutable std::mutex mutex_;  // 아래 숫자들을 보호한다
    std::map<std::string, long long> counters_;  // "visits", "hands", "day.<날짜>.hands" ...
    std::map<std::string, std::string> seen_;    // ID 해시 -> 마지막으로 센 날짜
    // ID 해시 -> 마지막으로 판을 끝낸 날짜. "다시 한 사람"으로 이미 셌으면 뒤에 '+'
    std::map<std::string, std::string> played_;
    std::map<std::string, std::time_t> online_;  // ID 해시 -> 마지막 신호 (파일에 쓰지 않는다)
    long long onlineLocked(std::time_t now) const;
    struct Browser {
        std::string first, last, playDate;
        long long play = 0, stay = 0, sessions = 0, hands = 0;
        long long device[kDevices] = {};
        int kind = kBrowserKinds - 1;
        bool excluded = false;
    };
    struct Session {  // 메모리에만 둔다
        std::time_t last = 0;
        long long play = 0, stay = 0;
    };
    Browser* browserLocked(const std::string& idHash, int kind, const std::string& date);  // 꽉 차면 nullptr
    void onlineBeatLocked(const std::string& idHash, std::time_t now);
    std::map<std::string, Browser> browsers_;  // ID 해시 -> 집계값
    std::map<std::string, Session> sessions_;  // ID 해시 -> 지금 세션
    std::string ledgerFrom_;     // ledger를 시작하는 날짜
    std::string ledgerHead_;     // 마지막으로 덧붙인 줄의 hash (줄 수는 counters_["ledger.lines"])
    std::string ledgerChecked_;  // ledger를 마지막으로 확인한 날짜
    bool ledgerBroken_ = false;
    bool dirty_ = false;
    bool stopping_ = false;
    std::condition_variable cv_;

    std::mutex writeMutex_;   // 파일 쓰기는 한 번에 하나
    std::mutex ledgerMutex_;  // ledger 갱신도 한 번에 하나
    std::thread writer_;
};

// 링크 미리보기 봇이나 크롤러의 User-Agent인가 (문자열은 저장하지 않는다)
bool isBotAgent(const std::string& userAgent);

// /stats?format=json 의 내용. 식별값은 들어가지 않는다.
std::string statsJson(const StatsSummary& summary, int playingGames = 0, int openRooms = 0);

// /stats 화면 (CSS만으로 그린 막대그래프). 10초마다 저절로 새로 고친다.
std::string statsPageHtml(const StatsSummary& summary, int playingGames = 0, int openRooms = 0);
