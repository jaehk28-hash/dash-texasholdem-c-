// bot_talk.cpp - 봇의 반응 표
#include "bot_talk.h"

#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "chat.h"

namespace {

BotRemark remarkOf(const std::string& text) {
    const auto& emojis = chatEmojis();
    const bool emoji = std::find(emojis.begin(), emojis.end(), text) != emojis.end();
    return {emoji ? "emoji" : "phrase", text};
}

// 말투: 공격형은 자신 있게(앞쪽), 타이트는 담담하게(뒤쪽), 콜링스테이션은 가끔 엉뚱하게
BotRemark pickFor(const std::string& bot, const std::vector<std::string>& choices, std::mt19937& rng) {
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    if (bot == "콜링스테이션" && uniform(rng) < 0.3) {
        const char* odd[] = {"콜?", "한 판 더?", "ㅋㅋㅋ 인정", "🔥"};
        return remarkOf(odd[std::uniform_int_distribution<int>(0, 3)(rng)]);
    }
    const std::size_t last = choices.size() - 1;
    if (bot == "공격형" && uniform(rng) < 0.7) return remarkOf(choices.front());
    if (bot == "타이트" && uniform(rng) < 0.7) return remarkOf(choices[last]);
    return remarkOf(choices[std::uniform_int_distribution<std::size_t>(0, last)(rng)]);
}

}  // namespace

double botTalkRate(const std::string& bot) {
    if (bot == "공격형") return 0.8;
    if (bot == "루즈") return 0.6;
    if (bot == "콜링스테이션") return 0.45;
    if (bot == "확률계산") return 0.35;
    return 0.25;  // 타이트
}

BotRemark botEventRemark(const std::string& bot, BotEvent event, std::mt19937& rng) {
    // 앞: 자신 있는 말, 뒤: 담담한 말
    switch (event) {
        case BotEvent::WonBig: return pickFor(bot, {"이번 판은 내가 접수", "😎", "나이스 핸드!", "👏"}, rng);
        case BotEvent::LostBig: return pickFor(bot, {"실화냐", "😭", "아깝다...", "어쩔 수 없지"}, rng);
        case BotEvent::BluffWorked: return pickFor(bot, {"블러프 아님. 진짜임", "😎", "😂", "잘 봤어요 ㅎㅎ"}, rng);
        case BotEvent::FacedAllIn: break;
    }
    return pickFor(bot, {"🔥", "그걸 콜해?", "콜?", "오 이거 위험한데"}, rng);
}

BotRemark botReplyRemark(const std::string& bot, const std::string& humanText, std::mt19937& rng) {
    static const std::map<std::string, std::vector<std::string>> table = {
        {"😎", {"😎", "ㅋㅋㅋ 인정"}},
        {"😂", {"😎", "ㅋㅋㅋ 인정"}},
        {"😭", {"👏", "어쩔 수 없지"}},
        {"😡", {"😂", "어쩔 수 없지"}},
        {"🔥", {"🔥", "오 이거 위험한데"}},
        {"👏", {"😎", "잘 봤어요 ㅎㅎ"}},
        {"나이스 핸드!", {"😎", "잘 봤어요 ㅎㅎ"}},
        {"아깝다...", {"😂", "어쩔 수 없지"}},
        {"이번 판은 내가 접수", {"실화냐", "과연?"}},
        {"블러프 아님. 진짜임", {"과연?", "믿어볼까?"}},
        {"오 그 베팅 믿어도 돼?", {"블러프 아님. 진짜임", "과연?"}},
        {"한 판 더?", {"콜?", "👏"}},
        {"잘 봤어요 ㅎㅎ", {"😎", "👏"}},
        {"실화냐", {"😂", "ㅋㅋㅋ 인정"}},
        {"콜?", {"🔥", "오 이거 위험한데"}},
        {"그걸 콜해?", {"😎", "어쩔 수 없지"}},
        {"오 이거 위험한데", {"🔥", "과연?"}},
        {"어쩔 수 없지", {"👏", "ㅋㅋㅋ 인정"}},
        {"ㅋㅋㅋ 인정", {"😂", "😎"}},
        {"과연?", {"😎", "믿어볼까?"}},
        {"믿어볼까?", {"블러프 아님. 진짜임", "👏"}},
    };
    const auto found = table.find(humanText);
    return found == table.end() ? remarkOf("👏") : pickFor(bot, found->second, rng);
}
