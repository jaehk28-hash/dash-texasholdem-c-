// stats.h - 익명 사용 통계 (숫자만 쌓는다)
//
// 저장하는 것: 횟수, 날짜, 브라우저가 만든 랜덤 ID의 "해시".
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
};

struct StatsSummary {
    long long totalVisits = 0;  // 같은 ID는 하루에 한 번만 센 합계
    long long todayVisits = 0;
    long long uniqueVisitors = 0;
    long long soloStarts = 0;
    long long roomStarts = 0;
    long long totalHands = 0;
    long long todayHands = 0;
    double handsPerGame = 0;
    int topBots = -1;  // 가장 많이 쓰인 봇 수 설정. 아직 없으면 -1
    std::vector<DayStats> days;  // 오래된 날부터, 마지막이 오늘
};

class Stats {
public:
    using Clock = std::function<std::time_t()>;
    static constexpr std::size_t kMaxVisitorIds = 20000;  // 파일이 끝없이 커지지 않게

    // path의 파일이 있으면 이어서 센다. clock은 테스트에서 날짜를 바꾸기 위한 것.
    explicit Stats(std::string path, Clock clock = {});
    ~Stats();  // 남은 변경을 저장하고 끝낸다
    Stats(const Stats&) = delete;
    Stats& operator=(const Stats&) = delete;

    // 아래 함수들은 여러 스레드에서 불러도 되고, 예외를 던지지 않는다.
    void visit(const std::string& idHash);  // 같은 ID는 하루에 한 번만
    void gameStarted(bool solo, int bots);
    void handFinished();
    StatsSummary summary(int days = 14) const;

    bool flush();  // 지금 바로 파일에 쓴다. 실패하면 false

    // 해시로 받을 수 있는 모양인가 (16진수 8~64자)
    static bool validIdHash(const std::string& text);

private:
    void load();
    std::string today() const;
    std::string serializeLocked() const;
    void changedLocked();
    void writerLoop();

    const std::string path_;
    const Clock clock_;

    mutable std::mutex mutex_;  // 아래 숫자들을 보호한다
    std::map<std::string, long long> counters_;  // "visits", "hands", "day.<날짜>.hands" ...
    std::map<std::string, std::string> seen_;    // ID 해시 -> 마지막으로 센 날짜
    bool dirty_ = false;
    bool stopping_ = false;
    std::condition_variable cv_;

    std::mutex writeMutex_;  // 파일 쓰기는 한 번에 하나
    std::thread writer_;
};

// /stats 화면 (CSS만으로 그린 막대그래프)
std::string statsPageHtml(const StatsSummary& summary);
