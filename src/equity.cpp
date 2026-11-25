// equity.cpp - 몬테카를로 승률 계산 구현
#include "equity.h"

#include <algorithm>

#include "evaluator.h"

double estimateEquity(const std::array<Card, 2>& hole, const std::vector<Card>& board,
                      int numOpponents, int trials, std::mt19937& rng) {
    if (numOpponents <= 0) return 1.0;
    if (trials <= 0) return 0.0;

    // 아직 보이지 않는 카드들
    std::vector<Card> unseen;
    {
        Deck deck;
        for (const Card& c : deck.cards()) {
            if (c == hole[0] || c == hole[1]) continue;
            if (std::find(board.begin(), board.end(), c) != board.end()) continue;
            unseen.push_back(c);
        }
    }

    const int boardMissing = 5 - static_cast<int>(board.size());
    const int needed = boardMissing + 2 * numOpponents;  // 이번 시행에 뽑을 카드 수
    const int unseenCount = static_cast<int>(unseen.size());
    if (needed > unseenCount) return 0.0;  // 상대가 너무 많은 비정상 입력

    std::vector<Card> full = board;  // 시행마다 공용 카드를 5장으로 채운 것
    full.resize(5);
    std::vector<Card> mine(7);
    mine[0] = hole[0];
    mine[1] = hole[1];
    std::vector<Card> theirs(7);

    double score = 0.0;
    for (int t = 0; t < trials; ++t) {
        // 부분 셔플: unseen의 앞쪽 needed장만 무작위로 확정한다.
        for (int i = 0; i < needed; ++i) {
            std::uniform_int_distribution<int> dist(i, unseenCount - 1);
            std::swap(unseen[i], unseen[dist(rng)]);
        }
        for (int i = 0; i < boardMissing; ++i) full[board.size() + i] = unseen[i];
        for (int i = 0; i < 5; ++i) {
            mine[2 + i] = full[i];
            theirs[2 + i] = full[i];
        }
        const HandScore myScore = evaluateBest(mine);

        bool lost = false;
        int ties = 0;
        for (int o = 0; o < numOpponents && !lost; ++o) {
            theirs[0] = unseen[boardMissing + 2 * o];
            theirs[1] = unseen[boardMissing + 2 * o + 1];
            const HandScore s = evaluateBest(theirs);
            if (s > myScore) lost = true;
            else if (s == myScore) ++ties;
        }
        if (!lost) score += 1.0 / (1 + ties);
    }
    return score / trials;
}
