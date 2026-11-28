// stats.cpp - 익명 사용 통계 구현
#include "stats.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
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
#include <vector>

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

const char* browserKindName(int kind) {
    static const char* names[kBrowserKinds] = {"Chrome", "Safari", "Edge", "Firefox", "삼성 인터넷", "웨일",
                                               "카카오톡 인앱", "인스타그램 인앱", "기타"};
    return names[kind >= 0 && kind < kBrowserKinds ? kind : kBrowserKinds - 1];
}

int browserKind(const std::string& ua) {
    auto has = [&ua](const char* part) { return ua.find(part) != std::string::npos; };
    // 순서가 중요하다: 대부분의 브라우저가 "Chrome"과 "Safari"를 함께 적는다.
    if (has("KAKAOTALK")) return 6;
    if (has("Instagram")) return 7;
    if (has("Whale/")) return 5;
    if (has("SamsungBrowser/")) return 4;
    if (has("Edg/") || has("EdgA/") || has("EdgiOS/")) return 2;
    if (has("Firefox/") || has("FxiOS/")) return 3;
    // 다른 앱 안의 화면과 다른 브라우저는 기타
    if (has("FBAN") || has("FBAV") || has("NAVER") || has("DaumApps") || has("Line/") || has("; wv)") || has("OPR/")) return 8;
    if (has("Chrome/") || has("CriOS/")) return 0;
    if (has("Safari/") && has("Version/")) return 1;
    return 8;
}

const char* deviceName(int device) { return device == 0 ? "모바일" : device == 1 ? "태블릿" : "데스크톱"; }

int deviceKind(const std::string& ua, bool touchMac) {
    auto has = [&ua](const char* part) { return ua.find(part) != std::string::npos; };
    if (has("iPad") || has("Tablet") || (has("Android") && !has("Mobile")) || (touchMac && has("Macintosh"))) return 1;
    if (has("Mobi") || has("iPhone") || has("Android")) return 0;
    return 2;
}

int timeBucket(long long seconds) {
    return seconds < 60 ? 0 : seconds < 300 ? 1 : seconds < 900 ? 2 : seconds < 1800 ? 3 : 4;
}

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
        } else if (key.rfind("br.", 0) == 0 && value.type == JsonValue::Type::String) {
            std::vector<std::string> parts;
            std::size_t from = 0;
            for (std::size_t bar = value.text.find('|'); ; bar = value.text.find('|', from)) {
                parts.push_back(value.text.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
                if (bar == std::string::npos) break;
                from = bar + 1;
            }
            if (parts.size() < 12) continue;
            auto number = [&parts](std::size_t i) { return std::atoll(parts[i].c_str()); };
            Browser b;
            b.first = parts[0];
            b.last = parts[1];
            b.playDate = parts[2];
            b.play = number(3);
            b.stay = number(4);
            b.sessions = number(5);
            b.hands = number(6);
            for (int d = 0; d < kDevices; ++d) b.device[d] = number(7 + static_cast<std::size_t>(d));
            b.kind = static_cast<int>(number(10));
            if (b.kind < 0 || b.kind >= kBrowserKinds) b.kind = kBrowserKinds - 1;
            b.excluded = number(11) != 0;
            browsers_[key.substr(3)] = b;
        } else if (key.rfind("play.", 0) == 0 && value.type == JsonValue::Type::String) {
            played_[key.substr(5)] = value.text;
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

void Stats::visit(const std::string& idHash, const std::string& src, int kind, int device) {
    if (!validIdHash(idHash)) return;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string date = today();  // 잠근 뒤에 읽어야 ledger에 쓴 날짜가 바뀌지 않는다
        if (kind >= 0) {
            if (Browser* b = browserLocked(idHash, kind, date)) {
                if (b->excluded) b->excluded = false;  // ?owner=0 으로 다시 세기 시작
                if (b->device[0] + b->device[1] + b->device[2] == 0 && device >= 0 && device < kDevices) ++b->device[device];
                changedLocked();
            }
        }
        const auto found = seen_.find(idHash);
        if (found != seen_.end() && found->second == date) return;  // 오늘 이미 셌다
        if (found == seen_.end()) {
            if (seen_.size() >= kMaxVisitorIds) return;
            ++counters_["unique"];
            ++counters_["day." + date + ".new"];
        }
        seen_[idHash] = date;
        ++counters_["visits"];
        if (kind >= 0 && kind < kBrowserKinds) ++counters_["kind." + std::to_string(kind) + ".visits"];
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

void Stats::handPlayed(const std::string& idHash) {
    if (!validIdHash(idHash)) return;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string date = today();
        const auto mine = browsers_.find(idHash);
        if (mine != browsers_.end()) {
            if (mine->second.hands++ == 0) ++counters_["kind." + std::to_string(mine->second.kind) + ".done"];
            changedLocked();
        }
        auto found = played_.find(idHash);
        if (found == played_.end()) {
            if (played_.size() >= kMaxVisitorIds) return;
            played_[idHash] = date;
        } else {
            const bool counted = !found->second.empty() && found->second.back() == '+';
            if (found->second.compare(0, date.size(), date) == 0) return;  // 오늘 이미 셌다
            if (!counted) ++counters_["returning"];  // 처음으로 다른 날에 다시 했다
            found->second = date + "+";
        }
        ++counters_["day." + date + ".active"];
        changedLocked();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "통계 기록 실패: %s\n", e.what());
    }
}

long long Stats::onlineLocked(std::time_t now) const {
    long long count = 0;
    for (const auto& [id, last] : online_) {
        if (now - last <= kOnlineSeconds) ++count;
    }
    return count;
}

Stats::Browser* Stats::browserLocked(const std::string& idHash, int kind, const std::string& date) {
    auto found = browsers_.find(idHash);
    if (found == browsers_.end()) {
        if (browsers_.size() >= kMaxVisitorIds) return nullptr;
        Browser fresh;
        fresh.first = date;
        fresh.kind = kind >= 0 && kind < kBrowserKinds ? kind : kBrowserKinds - 1;
        found = browsers_.emplace(idHash, fresh).first;
        ++counters_["kind." + std::to_string(fresh.kind) + ".browsers"];
    }
    found->second.last = date;
    return &found->second;
}

void Stats::onlineBeatLocked(const std::string& idHash, std::time_t now) {
    for (auto it = online_.begin(); it != online_.end();) {  // 끊긴 접속은 지운다
        it = now - it->second > kOnlineSeconds ? online_.erase(it) : std::next(it);
    }
    if (!online_.count(idHash) && online_.size() >= kMaxVisitorIds) return;
    online_[idHash] = now;
    const std::string base = "day." + dateText(now) + ".peak";
    const long long count = static_cast<long long>(online_.size());
    if (count > counters_[base]) {
        counters_[base] = count;
        counters_[base + "_at"] = (now + kKoreaOffset) % kDaySeconds / 60;
        changedLocked();
    }
}

void Stats::heartbeat(const std::string& idHash) {
    if (!validIdHash(idHash)) return;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        onlineBeatLocked(idHash, clock_());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "통계 기록 실패: %s\n", e.what());
    }
}

void Stats::beat(const Beat& beat) {
    if (!validIdHash(beat.id)) return;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::time_t now = clock_();
        const std::string date = dateText(now);
        if (beat.game && !beat.final) onlineBeatLocked(beat.id, now);
        if (beat.final) online_.erase(beat.id);

        auto found = sessions_.find(beat.id);
        const bool fresh = found == sessions_.end() || now - found->second.last > kSessionGapSeconds;
        // 너무 자주 온 신호는 버린다. 마지막 신호는 받되, 아래에서 실제 흐른 시간까지만 인정한다.
        if (!fresh && !beat.final && now - found->second.last < kBeatMinSeconds) return;
        if (fresh && beat.final) return;  // 시작을 모르는 마지막 신호

        Browser* browser = browserLocked(beat.id, beat.kind, date);
        if (!browser || browser->excluded) return;
        const std::string kind = "kind." + std::to_string(browser->kind) + ".";
        const std::string day = "day." + date + ".";

        if (fresh) {
            if (sessions_.size() >= kMaxVisitorIds) sessions_.clear();
            found = sessions_.insert_or_assign(beat.id, Session{now, 0, 0}).first;
            ++browser->sessions;
            if (beat.device >= 0 && beat.device < kDevices) ++browser->device[beat.device];
            ++counters_[kind + "sessions"];
            ++counters_["hist.play.0"];
            ++counters_["hist.stay.0"];
            changedLocked();
            return;  // 첫 신호는 기준점만 잡는다
        }

        Session& session = found->second;
        const long long elapsed = static_cast<long long>(now - session.last);
        session.last = now;
        if (elapsed > kBeatGapSeconds) return;  // 벌어진 구간은 세지 않는다
        // 보낸 값을 그대로 믿지 않는다: 두 시간의 합은 실제로 흐른 시간을 넘지 못한다.
        const long long play = std::clamp<long long>(beat.playSec, 0, std::max<long long>(0, elapsed));
        const long long stay = std::clamp<long long>(beat.staySec, 0, std::max<long long>(0, elapsed - play));
        if (play + stay == 0) return;

        auto move = [this](const char* name, long long before, long long after) {
            if (timeBucket(before) == timeBucket(after)) return;
            --counters_[std::string("hist.") + name + "." + std::to_string(timeBucket(before))];
            ++counters_[std::string("hist.") + name + "." + std::to_string(timeBucket(after))];
        };
        move("play", session.play, session.play + play);
        move("stay", session.stay, session.stay + stay);
        session.play += play;
        session.stay += stay;

        browser->play += play;
        browser->stay += stay;
        counters_[kind + "psec"] += play;
        counters_[kind + "sec"] += play + stay;
        if (play > 0) {
            if (browser->playDate != date) {
                browser->playDate = date;
                ++counters_[day + "pbrowsers"];
            }
            counters_[day + "psec"] += play;
        }
        counters_[day + "longest"] = std::max(counters_[day + "longest"], session.play + session.stay);
        changedLocked();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "통계 기록 실패: %s\n", e.what());
    }
}

void Stats::exclude(const std::string& idHash) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(idHash);
        online_.erase(idHash);
        const auto found = browsers_.find(idHash);
        if (found != browsers_.end() && !found->second.excluded) {
            found->second.excluded = true;
            changedLocked();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "통계 기록 실패: %s\n", e.what());
    }
}

void Stats::botHit() { bump("bot_hits"); }

bool isBotAgent(const std::string& userAgent) {
    std::string lower;
    for (const char c : userAgent) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* part : {"bot", "crawler", "spider", "preview", "facebookexternalhit", "kakaotalk-scrap", "slurp", "scrapy"}) {
        if (lower.find(part) != std::string::npos) return true;
    }
    return false;
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
    s.online = onlineLocked(now);
    s.returningPlayers = get("returning");
    s.botHits = get("bot_hits");
    s.todayActive = get("day." + dateText(now) + ".active");
    s.totalHands = get("hands");
    for (int i = days - 1; i >= 0; --i) {
        DayStats day;
        day.date = dateText(now - i * kDaySeconds);
        day.hands = get("day." + day.date + ".hands");
        day.visitors = get("day." + day.date + ".visitors");
        day.active = get("day." + day.date + ".active");
        day.peak = get("day." + day.date + ".peak");
        day.peakMinute = get("day." + day.date + ".peak_at");
        day.playSec = get("day." + day.date + ".psec");
        day.playBrowsers = get("day." + day.date + ".pbrowsers");
        day.longestSec = get("day." + day.date + ".longest");
        s.days.push_back(day);
    }
    if (!s.days.empty()) {
        const DayStats& t = s.days.back();
        s.todayAvgPlaySec = t.playBrowsers > 0 ? t.playSec / t.playBrowsers : 0;
        s.todayLongestSec = t.longestSec;
    }
    for (int i = 0; i < kTimeBuckets; ++i) {
        s.playHist[i] = get("hist.play." + std::to_string(i));
        s.stayHist[i] = get("hist.stay." + std::to_string(i));
    }
    for (int k = 0; k < kBrowserKinds; ++k) {
        const std::string base = "kind." + std::to_string(k) + ".";
        s.kinds[k] = KindStats{get(base + "visits"), get(base + "browsers"), get(base + "done"),
                               get(base + "psec"), get(base + "sec"), get(base + "sessions")};
    }
    std::vector<const Browser*> list;
    for (const auto& [id, b] : browsers_) {
        if (!b.excluded) list.push_back(&b);
    }
    std::sort(list.begin(), list.end(), [](const Browser* a, const Browser* b) {
        if (a->play != b->play) return a->play > b->play;
        if (a->hands != b->hands) return a->hands > b->hands;
        return a->first < b->first;
    });
    for (std::size_t i = 0; i < list.size() && i < static_cast<std::size_t>(kTopVisitors); ++i) {
        const Browser& b = *list[i];
        const int device = static_cast<int>(std::max_element(b.device, b.device + kDevices) - b.device);
        s.top.push_back(VisitorStats{b.first, b.last, b.play, b.sessions, b.hands, device});
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
    for (const auto& [id, date] : played_) {
        out += comma + jsonString("play." + id) + ":" + jsonString(date);
        comma = ",\n";
    }
    for (const auto& [id, b] : browsers_) {
        std::string line = b.first + "|" + b.last + "|" + b.playDate;
        for (long long n : {b.play, b.stay, b.sessions, b.hands, b.device[0], b.device[1], b.device[2],
                            static_cast<long long>(b.kind), static_cast<long long>(b.excluded)}) {
            line += "|" + std::to_string(n);
        }
        out += comma + jsonString("br." + id) + ":" + jsonString(line);
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
            cv_.wait_for(lock, std::chrono::seconds(2), [this] { return stopping_; });
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

namespace {

// 초 -> "12.5" (분)
std::string minutes(long long seconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(seconds) / 60.0);
    return text;
}

long long meanOf(long long total, long long count) { return count > 0 ? total / count : 0; }

std::string percent(long long part, long long whole) {
    return whole > 0 ? std::to_string((part * 100 + whole / 2) / whole) + "%" : "-";
}

const char* const kBucketNames[kTimeBuckets] = {"1분 미만", "1~5분", "5~15분", "15~30분", "30분 이상"};

}  // namespace

std::string statsJson(const StatsSummary& s, int playingGames, int openRooms) {
    auto number = [](long long n) { return std::to_string(n); };
    std::string j = "{\"now\":{\"online\":" + number(s.online) + ",\"playing\":" + number(playingGames) +
                    ",\"rooms\":" + number(openRooms) + "}";
    j += ",\"totals\":{\"visits\":" + number(s.totalVisits) + ",\"unique\":" + number(s.uniqueVisitors) +
         ",\"hands\":" + number(s.totalHands) + ",\"solo_starts\":" + number(s.soloStarts) +
         ",\"room_starts\":" + number(s.roomStarts) + ",\"public_rooms\":" + number(s.publicRooms) +
         ",\"public_joins\":" + number(s.publicJoins) + ",\"chat_rooms\":" + number(s.chatRooms) +
         ",\"chat_sends\":" + number(s.chatSends) + ",\"returning\":" + number(s.returningPlayers) +
         ",\"bot_hits\":" + number(s.botHits) + "}";
    j += ",\"today\":{\"visits\":" + number(s.todayVisits) + ",\"hands\":" + number(s.todayHands) +
         ",\"active\":" + number(s.todayActive) + ",\"avg_play_sec\":" + number(s.todayAvgPlaySec) +
         ",\"longest_session_sec\":" + number(s.todayLongestSec) + "}";
    j += ",\"days\":[";
    for (std::size_t i = 0; i < s.days.size(); ++i) {
        const DayStats& d = s.days[i];
        j += std::string(i ? "," : "") + "{\"date\":" + jsonString(d.date) + ",\"visitors\":" + number(d.visitors) +
             ",\"active\":" + number(d.active) + ",\"hands\":" + number(d.hands) + ",\"peak\":" + number(d.peak) +
             ",\"peak_minute\":" + number(d.peakMinute) + ",\"play_sec\":" + number(d.playSec) +
             ",\"play_browsers\":" + number(d.playBrowsers) + ",\"longest_session_sec\":" + number(d.longestSec) + "}";
    }
    j += "],\"session_buckets\":[";
    for (int i = 0; i < kTimeBuckets; ++i) {
        j += std::string(i ? "," : "") + "{\"range\":" + jsonString(kBucketNames[i]) + ",\"play\":" + number(s.playHist[i]) +
             ",\"stay\":" + number(s.stayHist[i]) + "}";
    }
    j += "],\"browsers\":[";
    for (int k = 0; k < kBrowserKinds; ++k) {
        const KindStats& b = s.kinds[k];
        j += std::string(k ? "," : "") + "{\"kind\":" + jsonString(browserKindName(k)) + ",\"visits\":" + number(b.visits) +
             ",\"browsers\":" + number(b.browsers) + ",\"finished_a_hand\":" + number(b.done) +
             ",\"avg_play_sec\":" + number(meanOf(b.playSec, b.browsers)) +
             ",\"avg_session_sec\":" + number(meanOf(b.totalSec, b.sessions)) + "}";
    }
    j += "],\"top_visitors\":[";
    for (std::size_t i = 0; i < s.top.size(); ++i) {
        const VisitorStats& v = s.top[i];
        j += std::string(i ? "," : "") + "{\"label\":" + jsonString("방문자 " + number(static_cast<long long>(i) + 1)) +
             ",\"first\":" + jsonString(v.firstDate) + ",\"last\":" + jsonString(v.lastDate) +
             ",\"play_sec\":" + number(v.playSec) + ",\"sessions\":" + number(v.sessions) + ",\"hands\":" + number(v.hands) +
             ",\"device\":" + jsonString(deviceName(v.device)) + "}";
    }
    return j + "]}";
}

std::string statsPageHtml(const StatsSummary& s, int playingGames, int openRooms) {
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
        "<meta name=\"robots\" content=\"noindex, nofollow\"><meta http-equiv=\"refresh\" content=\"10\">"
        "<title>사용 통계</title><style>"
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
        "font-size:15px;color:#ffd48a}"
        ".now{border:2px solid #c9a24b;border-radius:10px;padding:12px;font-size:17px;text-align:center;color:#ece6d6}"
        ".pair{display:flex;gap:1px;align-items:flex-end;justify-content:center;width:100%;flex:1;min-height:0}"
        ".pair i{width:42%}.pair i.b,.key.b{background:#8fae9b}.key{display:inline-block;width:10px;height:10px;"
        "background:#c9a24b;margin:0 4px 0 10px}table{width:100%;border-collapse:collapse;font-size:13px}"
        "td,th{border-bottom:1px solid #332f22;padding:4px;text-align:right}th:first-child,td:first-child{text-align:left}"
        ".wide{overflow-x:auto}.wide table{white-space:nowrap}"
        ".rate{display:inline-block;width:44px;height:6px;margin-left:6px;background:#332f22;vertical-align:middle}"
        ".rate span{display:block;height:100%;background:#c9a24b}"
        "</style></head><body><main><h1>사용 통계</h1>";
    html += "<p class=\"now\">지금 접속 중 <strong>" + std::to_string(s.online) + "명</strong> / 진행 중인 게임 <strong>" +
            std::to_string(playingGames) + "개</strong> / 열린 방 <strong>" + std::to_string(openRooms) + "개</strong></p>";
    if (s.ledgerBroken) {
        html += "<p class=\"warn\" role=\"alert\"><strong>ledger 불일치</strong>: data/stats-ledger.jsonl의 내용이 바뀌었거나 "
                "줄이 사라졌습니다. 공개 저장소의 기록과 비교하세요. 해결될 때까지 새 줄을 덧붙이지 않습니다.</p>";
    }
    html += "<div class=\"cards\">";
    html += card("오늘 평균 플레이 시간(분)", minutes(s.todayAvgPlaySec));
    html += card("오늘 가장 긴 세션(분)", minutes(s.todayLongestSec));
    html += card("오늘 활성 플레이어", std::to_string(s.todayActive));
    html += card("다시 한 사람", std::to_string(s.returningPlayers));
    html += card("봇 접속", std::to_string(s.botHits));
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
    // 방문자와 활성 플레이어를 날짜별로 나란히
    long long top = 1;
    for (const DayStats& d : s.days) top = std::max({top, d.visitors, d.active});
    html += "<h2>최근 14일 방문자와 활성 플레이어</h2><p><span class=\"key\"></span>방문자"
            "<span class=\"key b\"></span>활성 플레이어 (한 판 이상 끝냄)</p><div class=\"chart\">";
    auto bar = [top](long long value, const char* cls) {
        return std::string("<i class=\"") + cls + "\" style=\"height:" + std::to_string(value * 100 / top) + "%\"></i>";
    };
    for (const DayStats& d : s.days) {
        html += "<div class=\"col\" title=\"" + d.date + ": 방문자 " + std::to_string(d.visitors) + ", 활성 " +
                std::to_string(d.active) + "\"><em>" + std::to_string(d.visitors) + "/" + std::to_string(d.active) +
                "</em><div class=\"pair\">" + bar(d.visitors, "a") + bar(d.active, "b") + "</div><small>" +
                d.date.substr(8) + "</small></div>";
    }
    html += "</div><h2>일별 동시 접속 최고치와 플레이 시간</h2><div class=\"wide\"><table><tr><th>날짜</th><th>최고</th>"
            "<th>시각(한국)</th><th>평균 플레이(분)</th><th>가장 긴 세션(분)</th></tr>";
    for (auto d = s.days.rbegin(); d != s.days.rend(); ++d) {
        char at[16] = "-";
        if (d->peak > 0) {
            std::snprintf(at, sizeof(at), "%02d:%02d", static_cast<int>(d->peakMinute / 60),
                          static_cast<int>(d->peakMinute % 60));
        }
        html += "<tr><td>" + d->date + "</td><td>" + std::to_string(d->peak) + "</td><td>" + at + "</td><td>" +
                minutes(meanOf(d->playSec, d->playBrowsers)) + "</td><td>" + minutes(d->longestSec) + "</td></tr>";
    }
    html += "</table></div>";

    // 세션 길이 분포: 플레이와 체류를 나란히
    long long most = 1;
    for (int i = 0; i < kTimeBuckets; ++i) most = std::max({most, s.playHist[i], s.stayHist[i]});
    html += "<h2>세션 길이 분포 (세션 수)</h2><p><span class=\"key\"></span>플레이 시간<span class=\"key b\"></span>"
            "체류 시간</p><div class=\"chart\">";
    for (int i = 0; i < kTimeBuckets; ++i) {
        auto column = [most](long long value, const char* cls) {
            return std::string("<i class=\"") + cls + "\" style=\"height:" + std::to_string(value * 100 / most) + "%\"></i>";
        };
        html += "<div class=\"col\"><em>" + std::to_string(s.playHist[i]) + "/" + std::to_string(s.stayHist[i]) +
                "</em><div class=\"pair\">" + column(s.playHist[i], "a") + column(s.stayHist[i], "b") + "</div><small>" +
                kBucketNames[i] + "</small></div>";
    }
    html += "</div><h2>브라우저 종류별</h2><div class=\"wide\"><table><tr><th>종류</th><th>방문</th><th>브라우저</th>"
            "<th>한 판 완료</th><th>첫 판 완료율</th><th>평균 플레이(분)</th><th>평균 세션(분)</th></tr>";
    for (int k = 0; k < kBrowserKinds; ++k) {
        const KindStats& b = s.kinds[k];
        const long long rate = b.browsers > 0 ? b.done * 100 / b.browsers : 0;
        html += std::string("<tr><td>") + browserKindName(k) + "</td><td>" + std::to_string(b.visits) + "</td><td>" +
                std::to_string(b.browsers) + "</td><td>" + std::to_string(b.done) + "</td><td>" + percent(b.done, b.browsers) +
                "<span class=\"rate\"><span style=\"width:" + std::to_string(rate) + "%\"></span></span></td><td>" +
                minutes(meanOf(b.playSec, b.browsers)) + "</td><td>" + minutes(meanOf(b.totalSec, b.sessions)) + "</td></tr>";
    }
    html += "</table></div><h2>플레이 시간이 긴 방문자 (상위 20)</h2><div class=\"wide\"><table><tr><th>방문자</th>"
            "<th>첫 방문</th><th>마지막</th><th>플레이(분)</th><th>세션</th><th>완료 판</th><th>기기</th></tr>";
    for (std::size_t i = 0; i < s.top.size(); ++i) {
        const VisitorStats& v = s.top[i];
        html += "<tr><td>방문자 " + std::to_string(i + 1) + "</td><td>" + v.firstDate + "</td><td>" + v.lastDate + "</td><td>" +
                minutes(v.playSec) + "</td><td>" + std::to_string(v.sessions) + "</td><td>" + std::to_string(v.hands) +
                "</td><td>" + deviceName(v.device) + "</td></tr>";
    }
    html += "</table></div>";
    html += "<p>집계 숫자와 날짜, 브라우저를 구분하는 무작위 ID의 해시만 저장합니다. 닉네임, IP, 카드·채팅 내용은 저장하지 않습니다. "
            "날짜는 한국 시간 기준입니다.</p>"
            "<p>사람 수가 아니라 브라우저 수의 추정치입니다. 한 사람이 여러 기기로 들어오면 따로 세어집니다.</p>"
            "<p>플레이 시간은 화면이 켜져 있고 조작이 있었던 시간만 센 추정치입니다.</p>";
    return html + "</main></body></html>";
}
