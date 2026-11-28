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

struct DayStats {
    std::string date;  // "2026-11-28"
    long long hands = 0;
    long long visitors = 0;  // 그날의 고유 방문자 수
    long long active = 0;    // 그날 한 판 이상 끝낸 고유 브라우저 수 ("활성 플레이어")
    long long peak = 0;      // 동시 접속 최고치
    long long peakMinute = 0;  // 그 시각 (한국 시간 0시부터 몇 분)
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
    std::vector<DayStats> days;  // 오래된 날부터, 마지막이 오늘
};

class Stats {
public:
    using Clock = std::function<std::time_t()>;
    static constexpr std::size_t kMaxVisitorIds = 20000;  // 파일이 끝없이 커지지 않게
    static constexpr int kOnlineSeconds = 60;  // 이 시간 안에 신호가 있으면 "접속 중"

    // path의 파일이 있으면 이어서 센다. clock은 테스트에서 날짜를 바꾸기 위한 것.
    explicit Stats(std::string path, Clock clock = {});
    ~Stats();  // 남은 변경을 저장하고 끝낸다
    Stats(const Stats&) = delete;
    Stats& operator=(const Stats&) = delete;

    // 아래 함수들은 여러 스레드에서 불러도 되고, 예외를 던지지 않는다.
    // 같은 ID는 하루에 한 번만. src는 유입 경로(?src=)이고, 정해 둔 이름이 아니면 "other"로 센다.
    void visit(const std::string& idHash, const std::string& src = "");
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

// /stats 화면 (CSS만으로 그린 막대그래프). 10초마다 저절로 새로 고친다.
std::string statsPageHtml(const StatsSummary& summary, int playingGames = 0, int openRooms = 0);
