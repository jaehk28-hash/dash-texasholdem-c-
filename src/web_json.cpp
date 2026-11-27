// web_json.cpp - 작은 JSON 도구 구현
#include "web_json.h"

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>

std::string jsonString(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (u < 0x20) {  // 그 밖의 제어 문자는 \u00XX 형태로
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", u);
            out += buf;
        } else {
            out += c;  // 한글(UTF-8)은 그대로 넣어도 된다
        }
    }
    out += '"';
    return out;
}

namespace {

// 문자열을 앞에서부터 한 글자씩 읽어 가는 도우미
struct Reader {
    const std::string& s;
    std::size_t pos = 0;

    void skipSpaces() {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    }
    // 공백을 건너뛴 다음 글자가 c이면 먹고 true
    bool eat(char c) {
        skipSpaces();
        if (pos < s.size() && s[pos] == c) {
            ++pos;
            return true;
        }
        return false;
    }
    bool readString(std::string& out) {
        if (!eat('"')) return false;
        out.clear();
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c == '\\') {  // 우리 API에 필요한 이스케이프만 허용
                if (pos >= s.size()) return false;
                c = s[pos++];
                if (c != '"' && c != '\\' && c != '/') return false;
            }
            out += c;
        }
        return false;  // 닫는 따옴표가 없다
    }
    bool readValue(JsonValue& v) {
        skipSpaces();
        if (pos >= s.size()) return false;
        if (s[pos] == '"') {
            v.type = JsonValue::Type::String;
            return readString(v.text);
        }
        for (const char* word : {"true", "false", "null"}) {
            const std::string w = word;
            if (s.compare(pos, w.size(), w) == 0) {
                v.type = w == "null" ? JsonValue::Type::Null : JsonValue::Type::Boolean;
                v.flag = w == "true";
                pos += w.size();
                return true;
            }
        }
        // 정수: -?[0-9]+  (너무 긴 숫자는 넘침을 막기 위해 거부)
        const bool negative = s[pos] == '-';
        if (negative) ++pos;
        long long n = 0;
        int digits = 0;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
            if (++digits > 12) return false;
            n = n * 10 + (s[pos++] - '0');
        }
        if (digits == 0) return false;
        v.type = JsonValue::Type::Integer;
        v.number = negative ? -n : n;
        return true;  // 뒤에 ".5" 같은 것이 있으면 바깥에서 ,나 }가 아니라서 실패한다
    }
};

}  // namespace

bool parseFlatJsonObject(const std::string& body, std::map<std::string, JsonValue>& out) {
    out.clear();
    Reader r{body};
    if (!r.eat('{')) return false;
    if (!r.eat('}')) {
        while (true) {
            std::string key;
            JsonValue value;
            if (!r.readString(key) || !r.eat(':') || !r.readValue(value)) return false;
            if (!out.emplace(key, value).second) return false;  // 중복 키
            if (r.eat(',')) continue;
            if (r.eat('}')) break;
            return false;
        }
    }
    r.skipSpaces();
    return r.pos == body.size();
}
