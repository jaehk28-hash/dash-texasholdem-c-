// stats.cpp - 익명 사용 통계 구현
#include "stats.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "ledger.h"
#include "web_json.h"

namespace {

constexpr std::time_t kDaySeconds = 24 * 60 * 60;
constexpr std::time_t kKoreaOffset = 9 * 60 * 60;  // 한국 시간 = UTC+9 (서머타임 없음)
constexpr auto kLedgerCheck = std::chrono::seconds(20);  // 자정이 지났는지 보는 간격

// 한국 시간의 날짜. 서버의 시간대 설정과 상관없다.
std::string dateText(std::time_t time) {
    const std::time_t shifted = time + kKoreaOffset;
    std::tm korea{};
    gmtime_r(&shifted, &korea);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", korea.tm_year + 1900, korea.tm_mon + 1, korea.tm_mday);
    return buf;
}

// "2026-11-30" -> "2026-12-01"
std::string nextDate(const std::string& date) {
    std::tm t{};
    if (std::sscanf(date.c_str(), "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) != 3) return "9999-99-99";
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    t.tm_hour = 12;
    return dateText(timegm(&t) + kDaySeconds);
}

// 세는 유입 경로. 아무 글자나 받으면 공개 기록에 개인정보가 섞일 수 있어 정해 둔 이름만 쓴다.
const char* const kSources[] = {"instagram", "everytime", "kakao", "discord", "friend", "poster", "qr", "github"};

std::string sourceName(const std::string& src) {
    if (src.empty()) return "";
    for (const char* known : kSources) {
        if (src == known) return src;
    }
    return "other";
}

std::string folderOf(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

// ledger 끝에 한 줄을 덧붙인다. 기존 내용은 건드리지 않는다 (O_APPEND).
bool appendLine(const std::string& path, const std::string& line) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0) return false;
    const std::string text = line + "\n";
    bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());  // 한 번에 쓴다
    ok = ::fsync(fd) == 0 && ok;
    return ::close(fd) == 0 && ok;
}

// path의 윗 폴더들을 만든다 (이미 있으면 그대로 둔다)
void makeParentFolders(const std::string& path) {
    for (std::size_t i = path.find('/', 1); i != std::string::npos; i = path.find('/', i + 1)) {
        ::mkdir(path.substr(0, i).c_str(), 0700);
    }
}

// 임시 파일에 다 쓴 뒤 이름을 바꾼다 (쓰다가 꺼져도 원래 파일이 깨지지 않게)
bool writeFile(const std::string& path, const std::string& content) {
    makeParentFolders(path);
    const std::string temp = path + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool ok = ::fchmod(fd, 0600) == 0;
    std::size_t done = 0;
    while (ok && done < content.size()) {
        const ssize_t n = ::write(fd, content.data() + done, content.size() - done);
        if (n <= 0) ok = false;
        else done += static_cast<std::size_t>(n);
    }
    ok = ::close(fd) == 0 && ok;
    ok = ok && std::rename(temp.c_str(), path.c_str()) == 0;
    if (!ok) ::unlink(temp.c_str());
    return ok;
}

}  // namespace

Stats::Stats(std::string path, Clock clock)
    : path_(std::move(path)),
      ledgerPath_(folderOf(path_) + "stats-ledger.jsonl"),
      clock_(clock ? std::move(clock) : [] { return std::time(nullptr); }) {
    load();
    if (ledgerFrom_.empty()) {
        ledgerFrom_ = today();
        dirty_ = true;
    }
    updateLedger();  // 시작할 때 체인 전체를 검증한다
    writer_ = std::thread([this] { writerLoop(); });
}

Stats::~Stats() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    writer_.join();
    flush();
}

void Stats::load() {
    std::ifstream file(path_, std::ios::binary);
    if (!file) return;  // 처음 실행
    std::ostringstream content;
    content << file.rdbuf();
    std::map<std::string, JsonValue> values;
    if (!parseFlatJsonObject(content.str(), values)) {
        // 깨진 파일을 덮어써 버리지 않게 옆에 남겨 둔다.
        std::fprintf(stderr, "통계 파일을 읽을 수 없어 %s.bad로 옮기고 새로 셉니다.\n", path_.c_str());
        std::rename(path_.c_str(), (path_ + ".bad").c_str());
        return;
    }
    for (const auto& [key, value] : values) {
        if (key.rfind("seen.", 0) == 0 && value.type == JsonValue::Type::String) {
            seen_[key.substr(5)] = value.text;
        } else if (key == "ledger.from" && value.type == JsonValue::Type::String) {
            ledgerFrom_ = value.text;
        } else if (key == "ledger.head" && value.type == JsonValue::Type::String) {
            ledgerHead_ = value.text;
        } else if (value.type == JsonValue::Type::Integer) {
            counters_[key] = value.number;
        }
    }
}

std::string Stats::today() const { return dateText(clock_()); }

bool Stats::validIdHash(const std::string& text) {
    return text.size() >= 8 && text.size() <= 64 &&
           text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

void Stats::changedLocked() {
    dirty_ = true;
    cv_.notify_all();
}

void Stats::visit(const std::string& idHash, const std::string& src) {
    if (!validIdHash(idHash)) return;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string date = today();  // 잠근 뒤에 읽어야 ledger에 쓴 날짜가 바뀌지 않는다
        const auto found = seen_.find(idHash);
        if (found != seen_.end() && found->second == date) return;  // 오늘 이미 셌다
        if (found == seen_.end()) {
            if (seen_.size() >= kMaxVisitorIds) return;
            ++counters_["unique"];
            ++counters_["day." + date + ".new"];
        }
        seen_[idHash] = date;
        ++counters_["visits"];
        ++counters_["day." + date + ".visitors"];
        const std::string source = sourceName(src);
        if (!source.empty()) ++counters_["day." + date + ".src." + source];
        changedLocked();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "통계 기록 실패: %s\n", e.what());
    }
}

void Stats::gameStarted(bool solo, int bots) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counters_[solo ? "solo" : "room"];
        ++counters_["day." + today() + (solo ? ".solo" : ".room")];
        ++counters_["bots." + std::to_string(bots)];
        changedLocked();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "통계 기록 실패: %s\n", e.what());
    }
}

void Stats::handFinished() {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string date = today();
        ++counters_["hands"];
        ++counters_["day." + date + ".hands"];
        changedLocked();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "통계 기록 실패: %s\n", e.what());
    }
}

void Stats::bump(const char* key) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counters_[key];
        changedLocked();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "통계 기록 실패: %s\n", e.what());
    }
}

void Stats::publicRoomCreated() { bump("public_rooms"); }
void Stats::publicJoined() { bump("public_joins"); }
void Stats::chatSent(bool firstInRoom) {
    if (firstInRoom) bump("chat_rooms");
    bump("chat_sends");
}

StatsSummary Stats::summary(int days) const {
    const std::time_t now = clock_();
    std::lock_guard<std::mutex> lock(mutex_);
    auto get = [this](const std::string& key) {
        const auto it = counters_.find(key);
        return it == counters_.end() ? 0LL : it->second;
    };
    StatsSummary s;
    s.ledgerBroken = ledgerBroken_;
    s.totalVisits = get("visits");
    s.uniqueVisitors = get("unique");
    s.soloStarts = get("solo");
    s.roomStarts = get("room");
    s.publicRooms = get("public_rooms");
    s.publicJoins = get("public_joins");
    s.chatRooms = get("chat_rooms");
    s.chatSends = get("chat_sends");
    s.totalHands = get("hands");
    for (int i = days - 1; i >= 0; --i) {
        DayStats day;
        day.date = dateText(now - i * kDaySeconds);
        day.hands = get("day." + day.date + ".hands");
        day.visitors = get("day." + day.date + ".visitors");
        s.days.push_back(day);
    }
    s.todayHands = get("day." + dateText(now) + ".hands");
    s.todayVisits = get("day." + dateText(now) + ".visitors");
    const long long games = s.soloStarts + s.roomStarts;
    s.handsPerGame = games > 0 ? static_cast<double>(s.totalHands) / static_cast<double>(games) : 0;
    long long most = 0;
    for (int bots = 0; bots <= 5; ++bots) {
        if (get("bots." + std::to_string(bots)) > most) {
            most = get("bots." + std::to_string(bots));
            s.topBots = bots;
        }
    }
    return s;
}

std::string Stats::serializeLocked() const {
    std::string out = "{";
    const char* comma = "\n";
    for (const auto& [key, value] : counters_) {
        out += comma + jsonString(key) + ":" + std::to_string(value);
        comma = ",\n";
    }
    for (const auto& [id, date] : seen_) {
        out += comma + jsonString("seen." + id) + ":" + jsonString(date);
        comma = ",\n";
    }
    out += comma + jsonString("ledger.from") + ":" + jsonString(ledgerFrom_);
    if (!ledgerHead_.empty()) out += ",\n" + jsonString("ledger.head") + ":" + jsonString(ledgerHead_);
    return out + "\n}\n";
}

bool Stats::flush() {
    std::lock_guard<std::mutex> writing(writeMutex_);
    try {
        std::string content;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!dirty_) return true;
            content = serializeLocked();
            dirty_ = false;
        }
        if (writeFile(path_, content)) return true;
        std::fprintf(stderr, "통계 파일 저장 실패(%s): %s\n", path_.c_str(), std::strerror(errno));
        // 숫자는 메모리에 남아 있으므로 다음 변경 때 다시 시도한다.
    } catch (const std::exception& e) {
        std::fprintf(stderr, "통계 저장 실패: %s\n", e.what());
    }
    return false;
}

bool Stats::updateLedger() {
    std::lock_guard<std::mutex> updating(ledgerMutex_);
    try {
        std::string date, from, head;
        long long recorded = 0;  // 지금까지 덧붙였다고 기억하는 줄 수
        {
            std::lock_guard<std::mutex> lock(mutex_);
            date = today();
            ledgerChecked_ = date;
            from = ledgerFrom_;
            head = ledgerHead_;
            recorded = counters_["ledger.lines"];
        }

        std::string content;
        if (std::ifstream file{ledgerPath_, std::ios::binary}) {
            std::ostringstream all;
            all << file.rdbuf();
            content = all.str();
        }
        LedgerState chain = checkLedger(content);
        // 체인 자체는 맞아도, 뒤쪽 줄을 지우거나 통째로 새로 만들면 기억해 둔 hash와 어긋난다.
        const bool kept = recorded <= 0 || (static_cast<std::size_t>(recorded) <= chain.hashes.size() &&
                                            chain.hashes[static_cast<std::size_t>(recorded) - 1] == head);
        const bool broken = !chain.ok || !kept;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ledgerBroken_ = broken;
        }
        if (broken) {
            std::fprintf(stderr, "ledger 불일치: %s 를 확인하세요. 더 덧붙이지 않습니다.\n", ledgerPath_.c_str());
            return false;
        }

        // 마지막 줄 다음 날부터 어제까지. 아무도 안 온 날도 0으로 남긴다.
        std::string prev = chain.hashes.empty() ? "genesis" : chain.hashes.back();
        std::size_t lines = chain.hashes.size();
        for (std::string day = chain.lastDate.empty() ? from : nextDate(chain.lastDate); day < date;
             day = nextDate(day)) {
            LedgerCounts counts;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const std::string base = "day." + day + ".";
                auto get = [&](const std::string& name) {
                    const auto it = counters_.find(base + name);
                    return it == counters_.end() ? 0LL : it->second;
                };
                counts.visits = get("visitors");
                counts.uniqueVisitors = get("new");
                counts.soloStarts = get("solo");
                counts.roomStarts = get("room");
                counts.handsFinished = get("hands");
                for (const char* source : kSources) {
                    if (get(std::string("src.") + source) > 0) counts.src[source] = get(std::string("src.") + source);
                }
                if (get("src.other") > 0) counts.src["other"] = get("src.other");
            }
            const std::string line = ledgerLine(day, counts, prev);
            makeParentFolders(ledgerPath_);
            if (!appendLine(ledgerPath_, line)) {
                std::fprintf(stderr, "ledger 쓰기 실패(%s): %s\n", ledgerPath_.c_str(), std::strerror(errno));
                return false;
            }
            prev = line.substr(line.size() - 66, 64);  // 줄 끝의 hash
            ++lines;
        }
        if (lines > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (counters_["ledger.lines"] != static_cast<long long>(lines)) {
                counters_["ledger.lines"] = static_cast<long long>(lines);
                ledgerHead_ = prev;
                changedLocked();
            }
        }
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ledger 갱신 실패: %s\n", e.what());
    }
    return false;
}

void Stats::writerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        cv_.wait_for(lock, kLedgerCheck, [this] { return stopping_ || dirty_; });
        if (stopping_) break;
        if (dirty_) {  // 잠깐 모았다가 한 번에 쓴다.
            cv_.wait_for(lock, std::chrono::milliseconds(200), [this] { return stopping_; });
            lock.unlock();
            flush();
            lock.lock();
        }
        if (today() != ledgerChecked_) {  // 자정(한국 시간)이 지났다
            lock.unlock();
            updateLedger();
            lock.lock();
        }
    }
}

std::string statsPageHtml(const StatsSummary& s) {
    auto card = [](const std::string& title, const std::string& value) {
        return "<div class=\"card\"><b>" + value + "</b><span>" + title + "</span></div>";
    };
    auto chart = [&s](const std::string& title, long long DayStats::*field) {
        long long top = 1;
        for (const DayStats& d : s.days) top = std::max(top, d.*field);
        std::string html = "<h2>" + title + "</h2><div class=\"chart\">";
        for (const DayStats& d : s.days) {
            const long long value = d.*field;
            html += "<div class=\"col\" title=\"" + d.date + ": " + std::to_string(value) + "\"><em>" +
                    std::to_string(value) + "</em><i style=\"height:" + std::to_string(value * 100 / top) +
                    "%\"></i><small>" + d.date.substr(8) + "</small></div>";
        }
        return html + "</div>";
    };
    char average[32];
    std::snprintf(average, sizeof(average), "%.1f", s.handsPerGame);

    std::string html =
        "<!doctype html><html lang=\"ko\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<meta name=\"robots\" content=\"noindex, nofollow\"><title>사용 통계</title><style>"
        "body{margin:0;padding:14px;background:#0c100e;color:#ece6d6;font-family:system-ui,sans-serif}"
        "main{max-width:720px;margin:0 auto}h1,h2{color:#e8cf85;font-size:18px}h2{font-size:15px;margin-top:22px}"
        ".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:8px}"
        ".card{border:1px solid #4d4632;border-radius:10px;padding:12px;text-align:center}"
        ".card b{display:block;font-size:28px;color:#e8cf85}.card span{font-size:13px;color:#b9b4a4}"
        ".chart{display:flex;gap:3px;height:150px;border-bottom:1px solid #4d4632}"
        ".col{flex:1;min-width:0;display:flex;flex-direction:column;justify-content:flex-end;align-items:center}"
        ".col i{display:block;width:80%;max-height:100px;background:#c9a24b;border-radius:3px 3px 0 0}"
        ".col em{font-style:normal;font-size:10px}.col small{font-size:10px;color:#b9b4a4;padding:2px 0}"
        "p{font-size:12px;color:#b9b4a4}.warn{border:2px solid #e0a33a;border-radius:10px;padding:10px;"
        "font-size:15px;color:#ffd48a}</style></head><body><main><h1>사용 통계</h1>";
    if (s.ledgerBroken) {
        html += "<p class=\"warn\" role=\"alert\"><strong>ledger 불일치</strong>: data/stats-ledger.jsonl의 내용이 바뀌었거나 "
                "줄이 사라졌습니다. 공개 저장소의 기록과 비교하세요. 해결될 때까지 새 줄을 덧붙이지 않습니다.</p>";
    }
    html += "<div class=\"cards\">";
    html += card("총 판 수", std::to_string(s.totalHands));
    html += card("오늘 판 수", std::to_string(s.todayHands));
    html += card("고유 방문자 수", std::to_string(s.uniqueVisitors));
    html += card("총 방문 수", std::to_string(s.totalVisits));
    html += card("오늘 방문 수", std::to_string(s.todayVisits));
    html += card("혼자 하기 시작", std::to_string(s.soloStarts));
    html += card("친구 방 시작", std::to_string(s.roomStarts));
    html += card("공개 방 생성", std::to_string(s.publicRooms));
    html += card("공개 방 입장", std::to_string(s.publicJoins));
    html += card("채팅 사용 방", std::to_string(s.chatRooms));
    html += card("채팅 전송", std::to_string(s.chatSends));
    html += card("게임당 평균 판 수", average);
    html += card("가장 많은 봇 수", s.topBots < 0 ? "-" : std::to_string(s.topBots));
    html += "</div>";
    html += chart("최근 14일 판 수", &DayStats::hands);
    html += chart("최근 14일 고유 방문자 수", &DayStats::visitors);
    html += "<p>숫자만 저장합니다. 닉네임, IP, 카드 내용은 저장하지 않습니다. 날짜는 한국 시간 기준입니다.</p>";
    return html + "</main></body></html>";
}
