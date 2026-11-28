// bot_talk.h - 봇의 말투: 어떤 일에 어떤 이모지나 한마디로 반응하는가
//
// 순수한 연출이다. 봇의 베팅 판단에는 아무 영향이 없다.
// 입력은 봇 이름, 일어난 일(모두가 본 것), 사람이 보낸 문구뿐이고 카드는 들어오지 않는다.
#pragma once

#include <random>
#include <string>

enum class BotEvent { WonBig, LostBig, BluffWorked, FacedAllIn };

struct BotRemark {
    std::string kind;  // "emoji" 또는 "phrase" (chat.h의 목록에 있는 것만)
    std::string text;
};

// 성격별로 말이 많은 정도 (0~1)
double botTalkRate(const std::string& bot);

// 일어난 일에 대한 한마디
BotRemark botEventRemark(const std::string& bot, BotEvent event, std::mt19937& rng);

// 사람이 보낸 이모지나 빠른 말에 대한 답장 (매핑 표)
BotRemark botReplyRemark(const std::string& bot, const std::string& humanText, std::mt19937& rng);
