// pot.cpp - 팟과 사이드 팟 계산 구현
#include "pot.h"

#include <algorithm>

std::vector<Pot> buildPots(const std::vector<PotEntry>& entries) {
    // 1) 폴드하지 않은 사람들이 낸 금액을 중복 없이 오름차순으로 모은다 (= 층의 높이).
    std::vector<int> levels;
    for (const PotEntry& e : entries) {
        if (!e.folded && e.contributed > 0) levels.push_back(e.contributed);
    }
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());

    std::vector<Pot> pots;
    int previous = 0;
    for (int level : levels) {
        Pot pot;
        for (const PotEntry& e : entries) {
            // 이 층(previous~level 구간)에 이 사람이 낸 칩
            int slice = std::min(e.contributed, level) - previous;
            if (slice > 0) pot.amount += slice;
            if (!e.folded && e.contributed >= level) pot.eligible.push_back(e.seat);
        }
        pots.push_back(pot);
        previous = level;
    }

    // 살아있는 사람이 모두 0칩인데 폴드한 사람의 칩만 있는 비정상 입력:
    // 칩이 사라지지 않도록 살아있는 사람들이 나눠 갖는 팟 하나로 만든다.
    if (pots.empty()) {
        Pot pot;
        for (const PotEntry& e : entries) {
            pot.amount += e.contributed;
            if (!e.folded) pot.eligible.push_back(e.seat);
        }
        if (pot.amount > 0 && !pot.eligible.empty()) pots.push_back(pot);
        return pots;
    }

    // 2) 살아있는 사람의 최고 금액보다 더 낸 칩(폴드한 사람의 초과분)은 마지막 팟에 합친다.
    for (const PotEntry& e : entries) {
        if (e.contributed > previous) pots.back().amount += e.contributed - previous;
    }
    return pots;
}

std::vector<int> distributePots(const std::vector<Pot>& pots, const std::vector<HandScore>& scores,
                                int buttonSeat, int numSeats) {
    std::vector<int> winnings(numSeats, 0);
    for (const Pot& pot : pots) {
        if (pot.eligible.empty()) continue;

        // 최고 점수를 가진 승자들을 찾는다.
        std::vector<int> winners;
        if (pot.eligible.size() == 1) {
            winners = pot.eligible;  // 혼자면 비교 없이 가져간다
        } else {
            HandScore best = 0;
            for (int seat : pot.eligible) best = std::max(best, scores[seat]);
            for (int seat : pot.eligible) {
                if (scores[seat] == best) winners.push_back(seat);
            }
        }

        // 승자를 "버튼 바로 다음 좌석부터 시계 방향" 순서로 정렬한다.
        std::sort(winners.begin(), winners.end(), [&](int a, int b) {
            int da = (a - buttonSeat - 1 + numSeats) % numSeats;
            int db = (b - buttonSeat - 1 + numSeats) % numSeats;
            return da < db;
        });

        // 똑같이 나누고, 남는 칩은 앞 순서 승자부터 한 칩씩 준다.
        int share = pot.amount / static_cast<int>(winners.size());
        int remainder = pot.amount % static_cast<int>(winners.size());
        for (std::size_t i = 0; i < winners.size(); ++i) {
            winnings[winners[i]] += share + (static_cast<int>(i) < remainder ? 1 : 0);
        }
    }
    return winnings;
}
