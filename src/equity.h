// equity.h - 몬테카를로 승률 계산
#pragma once

#include <random>
#include <vector>

#include "card.h"

// 내 홀 카드와 지금까지 나온 공용 카드를 알고 있을 때,
// 상대 numOpponents명의 카드를 무작위로 뽑고 남은 공용 카드를 채워서
// trials번 대결시킨 "승률"을 돌려준다. 0.0 ~ 1.0.
//  - 이기면 1점, 동점이면 (1 / 동점자 수)점으로 계산한다 (에퀴티).
//  - 이미 보이는 카드(내 카드, 공용 카드)는 상대에게 나눠지지 않는다.
double estimateEquity(const std::array<Card, 2>& hole, const std::vector<Card>& board,
                      int numOpponents, int trials, std::mt19937& rng);
