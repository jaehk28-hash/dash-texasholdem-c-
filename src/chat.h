// chat.h - 채팅의 규칙: 정해진 문구, 글 검사, 속도 제한
//
// 여기에는 게임 정보가 하나도 들어오지 않는다. 메시지는 정해진 목록이나 사람이 직접 쓴 글로만 만들어진다.
// 메시지는 파일이나 로그에 쓰지 않는다.
#pragma once

#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <vector>

constexpr int CHAT_KEEP = 50;      // 방마다 메모리에 남기는 메시지 수
constexpr int CHAT_MAX_CHARS = 40;  // 자유 채팅 한 줄의 글자 수
constexpr const char* CHAT_BLOCKED_MESSAGE = "보낼 수 없는 내용이에요";

// 속도 제한 값 (테스트에서 바꿀 수 있다)
struct ChatRules {
    std::chrono::milliseconds cooldown{3000};      // 이모지·빠른 말, 자유 채팅 각각 이 간격에 한 번
    std::chrono::milliseconds burstWindow{10000};  // 이 시간 안에
    int burst = 5;                                 // 자유 채팅을 이보다 많이 보내려 하면
    std::chrono::milliseconds lock{30000};         // 이만큼 잠근다
};

// 한 사람의 속도 제한. 모든 함수는 "지금" 시각을 받는다.
class ChatLimiter {
public:
    using Time = std::chrono::steady_clock::time_point;

    // 보내도 되면 0을 돌려주고 기록한다. 아니면 기다려야 하는 시간(ms).
    int react(Time now, const ChatRules& rules);  // 이모지와 빠른 말이 함께 쓴다
    int text(Time now, const ChatRules& rules);   // 거절된 시도도 연속 보내기 횟수에 센다
    // 기록하지 않고 남은 시간만 본다
    int reactWait(Time now, const ChatRules& rules) const;
    int textWait(Time now, const ChatRules& rules) const;

private:
    std::optional<Time> lastReact_, lastText_, lockedUntil_;
    std::deque<Time> attempts_;
};

const std::vector<std::string>& chatEmojis();   // 리액션 6개
const std::vector<std::string>& chatPhrases();  // 빠른 말. 클라이언트는 번호만 보낸다

// 자유 채팅 한 줄을 검사한다. 통과하면 text를 다듬고(욕설은 ***) true.
// 링크, 전화번호, 이메일 모양이나 보이지 않는 글자가 있으면 false.
bool cleanChatText(std::string& text, std::string& error);

// 욕설 목록에 걸리는 부분이 있는가 (닉네임 검사에도 쓴다)
bool hasBadWord(const std::string& text);
