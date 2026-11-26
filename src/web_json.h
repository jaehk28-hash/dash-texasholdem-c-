// web_json.h - 웹 API에 필요한 만큼만 만든 아주 작은 JSON 도구 (외부 라이브러리 없음)
#pragma once

#include <map>
#include <string>

// 문자열을 JSON 문자열 리터럴로 바꾼다. 앞뒤 따옴표까지 붙여서 돌려준다.
std::string jsonString(const std::string& text);

// 파싱한 값 하나. 숫자는 정수만 받는다.
struct JsonValue {
    enum class Type { String, Integer, Boolean, Null };
    Type type = Type::Null;
    std::string text;      // type == String일 때
    long long number = 0;  // type == Integer일 때
    bool flag = false;     // type == Boolean일 때
};

// {"action":"raise","amount":300} 처럼 "한 겹짜리" 객체만 해석한다.
// 중첩 객체/배열, 소수, 중복 키, 뒤에 붙은 쓰레기 문자는 모두 실패(false)로 처리한다.
// 입력이 어떤 모양이든 예외를 던지지 않는다.
bool parseFlatJsonObject(const std::string& body, std::map<std::string, JsonValue>& out);
