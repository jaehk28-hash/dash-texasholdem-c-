// pot.h - 팟과 사이드 팟 계산
#pragma once

#include <vector>

#include "evaluator.h"

// 한 판에서 한 플레이어가 낸 칩 정보.
struct PotEntry {
    int seat = 0;         // 좌석 번호 (0부터, 시계 방향으로 증가)
    int contributed = 0;  // 이번 판에 팟에 넣은 총 칩
    bool folded = false;  // 폴드했으면 낸 칩은 팟에 남지만 받을 자격은 없다
};

// 팟 하나 (메인 팟 또는 사이드 팟).
struct Pot {
    int amount = 0;            // 팟 크기
    std::vector<int> eligible;  // 이 팟을 가져갈 자격이 있는 좌석들 (폴드하지 않은 사람)
};

// 올인 금액 단위로 메인 팟과 사이드 팟을 나눈다. 결과의 [0]이 메인 팟.
//
// 원리: 폴드하지 않은 사람들이 낸 금액을 작은 순서로 "층"으로 쌓는다.
// 각 층의 팟에는 그 층까지 낸 모든 사람(폴드한 사람 포함)의 칩이 들어가고,
// 그 층 이상을 낸 폴드 안 한 사람만 자격을 얻는다.
// (폴드한 사람이 살아있는 모든 사람보다 더 낸 초과분은 마지막 팟에 넣는다.
//  실제 게임에서는 돌려받지 못한 벳을 미리 돌려주므로 보통 생기지 않는다.)
std::vector<Pot> buildPots(const std::vector<PotEntry>& entries);

// 각 팟을 최고 핸드에게 나눠 준다.
//  - scores[좌석]: 각 좌석의 핸드 점수 (자격 있는 사람이 둘 이상인 팟에서만 쓰인다)
//  - buttonSeat: 딜러 버튼 좌석. 나누어떨어지지 않는 칩은
//    버튼 왼쪽(다음 좌석)에서 가까운 승자부터 한 칩씩 준다.
// 반환: 좌석별로 받는 칩 (크기 = numSeats)
std::vector<int> distributePots(const std::vector<Pot>& pots, const std::vector<HandScore>& scores,
                                int buttonSeat, int numSeats);
