// evaluator.h - 족보 판정
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "card.h"

// 족보 종류. 숫자가 클수록 강하다. (로열 플러시는 A 하이 스트레이트 플러시)
enum class HandCategory : int {
    HighCard = 0,
    OnePair,
    TwoPair,
    ThreeOfAKind,
    Straight,
    Flush,
    FullHouse,
    FourOfAKind,
    StraightFlush
};

// 핸드 점수. 정수 하나로 족보 + 키커까지 담는다.
// 값이 클수록 강한 핸드이고, 같으면 완전히 동점이다.
// 구조: [족보 4비트][가장 중요한 카드 4비트]...[가장 덜 중요한 카드 4비트]
using HandScore = std::uint32_t;

// 정확히 5장의 점수를 계산한다.
HandScore evaluate5(const Card* cards);

// 5~7장 중 가장 좋은 5장 조합의 점수를 계산한다. (빠른 방식: 숫자/무늬별 개수를 세어 직접 판정)
HandScore evaluateBest(const Card* cards, int count);
HandScore evaluateBest(const std::vector<Card>& cards);

// 같은 결과를 느리지만 단순하게 구하는 방식: 가능한 모든 5장 조합을 evaluate5로 검사한다.
// 빠른 방식이 맞는지 검증하는 기준(레퍼런스)으로 쓴다.
HandScore evaluateBestBruteForce(const std::vector<Card>& cards);

// 점수에서 족보 종류를 꺼낸다.
HandCategory categoryOf(HandScore score);

// 족보 한국어 이름 (예: "원 페어").
std::string categoryNameKo(HandCategory category);
