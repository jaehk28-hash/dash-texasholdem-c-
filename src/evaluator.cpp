// evaluator.cpp - 족보 판정 구현
#include "evaluator.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// 다섯 장을 [족보][카드1]..[카드5] 형태의 점수로 포장한다.
HandScore pack(HandCategory category, const std::array<int, 5>& ranks) {
    HandScore score = static_cast<HandScore>(category);
    for (int r : ranks) score = (score << 4) | static_cast<HandScore>(r);
    return score;
}

}  // namespace

HandScore evaluate5(const Card* cards) {
    // 1) 숫자별 개수와 무늬 확인
    std::array<int, 15> count{};  // count[rank] = 그 숫자의 장수
    bool flush = true;
    for (int i = 0; i < 5; ++i) {
        ++count[cards[i].rank];
        if (cards[i].suit != cards[0].suit) flush = false;
    }

    // 2) (장수 많은 순, 숫자 큰 순)으로 숫자를 정렬한다.
    //    예: K K 7 7 2 -> [K, 7, 2] / 장수 [2, 2, 1]
    //    이 순서가 곧 타이브레이크(키커) 비교 순서가 된다.
    std::array<int, 5> ordered{};  // 정렬된 숫자 (중복 없이 앞에서부터 채움)
    std::array<int, 5> groups{};   // 각 숫자의 장수
    int n = 0;
    for (int copies = 4; copies >= 1; --copies) {
        for (int r = 14; r >= 2; --r) {
            if (count[r] == copies) {
                ordered[n] = r;
                groups[n] = copies;
                ++n;
            }
        }
    }

    // 3) 숫자가 겹치는 족보
    if (groups[0] == 4) return pack(HandCategory::FourOfAKind, {ordered[0], ordered[1], 0, 0, 0});
    if (groups[0] == 3 && groups[1] == 2)
        return pack(HandCategory::FullHouse, {ordered[0], ordered[1], 0, 0, 0});
    // 플러시/스트레이트는 5장이 모두 다른 숫자일 때만 가능하다 (n == 5)
    if (n == 5) {
        // 스트레이트: 가장 큰 숫자와 가장 작은 숫자의 차이가 4
        bool straight = (ordered[0] - ordered[4] == 4);
        int high = ordered[0];
        // 휠(A-2-3-4-5): A는 1로 쓰이므로 하이 카드는 5
        if (!straight && ordered[0] == 14 && ordered[1] == 5 && ordered[4] == 2) {
            straight = true;
            high = 5;
        }
        if (straight && flush) return pack(HandCategory::StraightFlush, {high, 0, 0, 0, 0});
        if (flush) return pack(HandCategory::Flush, ordered);
        if (straight) return pack(HandCategory::Straight, {high, 0, 0, 0, 0});
        return pack(HandCategory::HighCard, ordered);
    }
    if (groups[0] == 3) return pack(HandCategory::ThreeOfAKind, {ordered[0], ordered[1], ordered[2], 0, 0});
    if (groups[0] == 2 && groups[1] == 2)
        return pack(HandCategory::TwoPair, {ordered[0], ordered[1], ordered[2], 0, 0});
    return pack(HandCategory::OnePair, {ordered[0], ordered[1], ordered[2], ordered[3], 0});
}

HandScore evaluateBestBruteForce(const std::vector<Card>& cards) {
    const int n = static_cast<int>(cards.size());
    if (n < 5 || n > 7) throw std::invalid_argument("카드는 5~7장이어야 합니다");

    // n장 중 5장을 고르는 모든 조합(최대 21개)을 검사해서 가장 높은 점수를 찾는다.
    // 각 카드를 포함할지 비트마스크로 훑는다 (최대 2^7 = 128번).
    HandScore best = 0;
    Card pick[5];
    for (unsigned mask = 0; mask < (1u << n); ++mask) {
        if (__builtin_popcount(mask) != 5) continue;
        int k = 0;
        for (int i = 0; i < n; ++i) {
            if (mask & (1u << i)) pick[k++] = cards[i];
        }
        best = std::max(best, evaluate5(pick));
    }
    return best;
}

namespace {

// 비트마스크(비트 r = 숫자 r이 있음)에서 가장 높은 스트레이트의 하이 카드를 찾는다. 없으면 0.
// 에이스는 1로도 쓰이므로(휠), 에이스가 있으면 비트 1도 켜서 검사한다.
int highestStraight(unsigned mask) {
    if (mask & (1u << 14)) mask |= 2u;  // A를 1로도 취급
    for (int high = 14; high >= 5; --high) {
        const unsigned window = 0x1Fu << (high - 4);
        if ((mask & window) == window) return high;
    }
    return 0;
}

// 비트마스크에서 높은 숫자부터 최대 count개를 골라 out에 넣고 개수를 돌려준다.
int topRanks(unsigned mask, int count, int* out) {
    int n = 0;
    for (int r = 14; r >= 2 && n < count; --r) {
        if (mask & (1u << r)) out[n++] = r;
    }
    return n;
}

}  // namespace

HandScore evaluateBest(const Card* cards, int count) {
    if (count < 5 || count > 7) throw std::invalid_argument("카드는 5~7장이어야 합니다");

    // 1) 숫자별 개수, 무늬별 개수, 무늬별 숫자 비트마스크
    int rankCount[15] = {};
    int suitCount[4] = {};
    unsigned suitMask[4] = {};
    unsigned present = 0;  // 한 장이라도 있는 숫자
    for (int i = 0; i < count; ++i) {
        const int r = cards[i].rank;
        const int s = static_cast<int>(cards[i].suit);
        ++rankCount[r];
        ++suitCount[s];
        suitMask[s] |= 1u << r;
        present |= 1u << r;
    }

    // 2) 플러시가 있으면 스트레이트 플러시부터 확인
    int flushSuit = -1;
    for (int s = 0; s < 4; ++s) {
        if (suitCount[s] >= 5) flushSuit = s;  // 7장이면 플러시 무늬는 최대 하나
    }
    if (flushSuit >= 0) {
        const int high = highestStraight(suitMask[flushSuit]);
        if (high) return pack(HandCategory::StraightFlush, {high, 0, 0, 0, 0});
    }

    // 3) 숫자가 겹치는 정도 정리 (높은 숫자부터)
    int quad = 0;
    int trips[2] = {0, 0}, nTrips = 0;
    int pairs[3] = {0, 0, 0}, nPairs = 0;
    for (int r = 14; r >= 2; --r) {
        if (rankCount[r] == 4) quad = r;
        else if (rankCount[r] == 3 && nTrips < 2) trips[nTrips++] = r;
        else if (rankCount[r] == 2 && nPairs < 3) pairs[nPairs++] = r;
    }

    int k[5] = {};
    if (quad) {
        // 포카드 + 나머지 중 가장 높은 카드 한 장
        const int n = topRanks(present & ~(1u << quad), 1, k);
        return pack(HandCategory::FourOfAKind, {quad, n ? k[0] : 0, 0, 0, 0});
    }
    if (nTrips >= 1 && (nTrips >= 2 || nPairs >= 1)) {
        // 풀 하우스: 가장 높은 트리플 + (두 번째 트리플 또는 가장 높은 페어) 중 높은 쪽
        int pairRank = nPairs ? pairs[0] : 0;
        if (nTrips >= 2) pairRank = std::max(pairRank, trips[1]);
        return pack(HandCategory::FullHouse, {trips[0], pairRank, 0, 0, 0});
    }
    if (flushSuit >= 0) {
        topRanks(suitMask[flushSuit], 5, k);
        return pack(HandCategory::Flush, {k[0], k[1], k[2], k[3], k[4]});
    }
    if (const int high = highestStraight(present)) {
        return pack(HandCategory::Straight, {high, 0, 0, 0, 0});
    }
    if (nTrips >= 1) {
        topRanks(present & ~(1u << trips[0]), 2, k);
        return pack(HandCategory::ThreeOfAKind, {trips[0], k[0], k[1], 0, 0});
    }
    if (nPairs >= 2) {
        // 투 페어: 높은 두 페어 + 나머지(세 번째 페어 포함) 중 가장 높은 카드
        const unsigned rest = present & ~(1u << pairs[0]) & ~(1u << pairs[1]);
        const int n = topRanks(rest, 1, k);
        return pack(HandCategory::TwoPair, {pairs[0], pairs[1], n ? k[0] : 0, 0, 0});
    }
    if (nPairs == 1) {
        topRanks(present & ~(1u << pairs[0]), 3, k);
        return pack(HandCategory::OnePair, {pairs[0], k[0], k[1], k[2], 0});
    }
    topRanks(present, 5, k);
    return pack(HandCategory::HighCard, {k[0], k[1], k[2], k[3], k[4]});
}

HandScore evaluateBest(const std::vector<Card>& cards) {
    return evaluateBest(cards.data(), static_cast<int>(cards.size()));
}

HandCategory categoryOf(HandScore score) { return static_cast<HandCategory>(score >> 20); }

std::string categoryNameKo(HandCategory category) {
    switch (category) {
        case HandCategory::HighCard: return "하이 카드";
        case HandCategory::OnePair: return "원 페어";
        case HandCategory::TwoPair: return "투 페어";
        case HandCategory::ThreeOfAKind: return "트리플";
        case HandCategory::Straight: return "스트레이트";
        case HandCategory::Flush: return "플러시";
        case HandCategory::FullHouse: return "풀 하우스";
        case HandCategory::FourOfAKind: return "포카드";
        case HandCategory::StraightFlush: return "스트레이트 플러시";
    }
    return "?";
}
