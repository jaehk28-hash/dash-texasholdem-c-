// chat.cpp - 채팅 규칙 구현
#include "chat.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {

using Millis = std::chrono::milliseconds;

int remaining(const std::optional<ChatLimiter::Time>& until, ChatLimiter::Time now) {
    if (!until || *until <= now) return 0;
    return std::max(1, static_cast<int>(std::chrono::duration_cast<Millis>(*until - now).count()));
}

int since(const std::optional<ChatLimiter::Time>& last, Millis cooldown, ChatLimiter::Time now) {
    if (!last) return 0;
    return remaining(*last + cooldown, now);
}

// 작은 내장 목록 (외부 서비스 없이). 영어는 소문자로 적는다.
const char* const kBadWords[] = {"씨발", "시발", "씨팔", "시팔", "씨바", "ㅅㅂ", "ㅆㅂ", "병신", "ㅂㅅ", "지랄", "ㅈㄹ",
                                 "개새", "새끼", "좆", "존나", "니미", "느금", "엠창", "썅", "닥쳐", "미친놈", "미친년",
                                 "창녀", "섹스", "fuck", "shit", "bitch", "asshole", "cunt", "nigger", "porn", "sex"};

// UTF-8 한 글자를 읽는다. 잘못된 모양이면 false.
bool nextChar(const std::string& s, std::size_t& i, unsigned& code) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int extra = 0;
    if (c < 0x80) code = c;
    else if ((c & 0xE0) == 0xC0) code = c & 0x1Fu, extra = 1;
    else if ((c & 0xF0) == 0xE0) code = c & 0x0Fu, extra = 2;
    else if ((c & 0xF8) == 0xF0) code = c & 0x07u, extra = 3;
    else return false;
    if (i + static_cast<std::size_t>(extra) >= s.size()) return false;
    for (int k = 1; k <= extra; ++k) {
        const unsigned char next = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
        if ((next & 0xC0) != 0x80) return false;
        code = (code << 6) | (next & 0x3Fu);
    }
    const unsigned smallest[] = {0, 0x80, 0x800, 0x10000};
    if (code < smallest[extra] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
    i += static_cast<std::size_t>(extra) + 1;
    return true;
}

// 욕설이 있는 구간을 [처음, 끝) 바이트 위치로 모은다.
// 띄어쓰기, 기호, 숫자를 끼워 넣어도 찾도록 빼고 비교한다.
std::vector<std::pair<std::size_t, std::size_t>> badRanges(const std::string& text) {
    std::string plain;
    std::vector<std::size_t> at;  // plain의 바이트마다 원래 위치
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80 && !std::isalpha(c)) continue;
        plain += c < 0x80 ? static_cast<char>(std::tolower(c)) : text[i];
        at.push_back(i);
    }
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (const char* word : kBadWords) {
        const std::string w = word;
        for (std::size_t p = plain.find(w); p != std::string::npos; p = plain.find(w, p + 1)) {
            ranges.emplace_back(at[p], at[p + w.size() - 1] + 1);
        }
    }
    std::sort(ranges.begin(), ranges.end());
    return ranges;
}

bool looksLikeContact(const std::string& text) {
    std::string tight;  // 소문자, 띄어쓰기 없이
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (c != ' ') tight += c < 0x80 ? static_cast<char>(std::tolower(c)) : raw;
    }
    if (tight.find('@') != std::string::npos) return true;  // 이메일, 계정 태그
    for (const char* part : {"http", "www", "://", "닷컴", ".com", ".net", ".org", ".kr", ".io", ".co", ".me", ".gg", ".ly",
                             ".app", ".xyz", ".shop", ".link"}) {
        if (tight.find(part) != std::string::npos) return true;
    }
    // 전화번호: 띄어쓰기, -, ., 괄호로만 이어진 숫자가 9개 이상 (대표번호는 8개)
    std::string run;
    auto phone = [&run] {
        return run.size() >= 9 || (run.size() == 8 && (run.compare(0, 2, "15") == 0 || run.compare(0, 2, "16") == 0 ||
                                                       run.compare(0, 2, "18") == 0));
    };
    for (const char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) run += c;
        else if (c != ' ' && c != '-' && c != '.' && c != '(' && c != ')') {
            if (phone()) return true;
            run.clear();
        }
    }
    return phone();
}

}  // namespace

int ChatLimiter::reactWait(Time now, const ChatRules& rules) const { return since(lastReact_, rules.cooldown, now); }

int ChatLimiter::textWait(Time now, const ChatRules& rules) const {
    return std::max(remaining(lockedUntil_, now), since(lastText_, rules.cooldown, now));
}

int ChatLimiter::react(Time now, const ChatRules& rules) {
    const int wait = reactWait(now, rules);
    if (wait == 0) lastReact_ = now;
    return wait;
}

int ChatLimiter::text(Time now, const ChatRules& rules) {
    if (const int locked = remaining(lockedUntil_, now)) return locked;
    while (!attempts_.empty() && now - attempts_.front() >= rules.burstWindow) attempts_.pop_front();
    attempts_.push_back(now);
    if (static_cast<int>(attempts_.size()) > rules.burst) {
        lockedUntil_ = now + rules.lock;
        attempts_.clear();
        return remaining(lockedUntil_, now);
    }
    const int wait = since(lastText_, rules.cooldown, now);
    if (wait == 0) lastText_ = now;
    return wait;
}

const std::vector<std::string>& chatEmojis() {
    static const std::vector<std::string> list = {"😎", "😂", "😭", "😡", "🔥", "👏"};
    return list;
}

const std::vector<std::string>& chatPhrases() {
    static const std::vector<std::string> list = {"나이스 핸드!", "아깝다...", "이번 판은 내가 접수",
                                                  "블러프 아님. 진짜임", "오 그 베팅 믿어도 돼?", "한 판 더?",
                                                  "잘 봤어요 ㅎㅎ", "실화냐", "콜?"};
    return list;
}

bool hasBadWord(const std::string& text) { return !badRanges(text).empty(); }

bool cleanChatText(std::string& text, std::string& error) {
    const std::size_t first = text.find_first_not_of(' ');
    const std::size_t last = text.find_last_not_of(' ');
    text = first == std::string::npos ? "" : text.substr(first, last - first + 1);

    error = "메시지는 1~" + std::to_string(CHAT_MAX_CHARS) + "자까지 보낼 수 있어요";
    int length = 0;
    for (std::size_t i = 0; i < text.size(); ++length) {
        unsigned code = 0;
        if (!nextChar(text, i, code)) {
            error = CHAT_BLOCKED_MESSAGE;
            return false;
        }
        // 줄바꿈, 제어 문자, 글 방향을 뒤집거나 보이지 않는 문자
        if (code < 0x20 || (code >= 0x7F && code < 0xA0) || code == 0x200B || (code >= 0x2028 && code <= 0x202E) ||
            (code >= 0x2066 && code <= 0x2069) || code == 0xFEFF) {
            error = CHAT_BLOCKED_MESSAGE;
            return false;
        }
    }
    if (length < 1 || length > CHAT_MAX_CHARS) return false;
    if (looksLikeContact(text)) {
        error = CHAT_BLOCKED_MESSAGE;
        return false;
    }
    // 뒤에서부터 바꿔야 앞쪽 위치가 밀리지 않는다.
    std::vector<std::pair<std::size_t, std::size_t>> merged;
    for (const auto& r : badRanges(text)) {
        if (!merged.empty() && r.first <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, r.second);
        } else {
            merged.push_back(r);
        }
    }
    for (auto it = merged.rbegin(); it != merged.rend(); ++it) text.replace(it->first, it->second - it->first, "***");
    error.clear();
    return true;
}
