// review.cpp - 핸드 리뷰 구현
#include "review.h"

#include <algorithm>
#include <array>
#include <utility>

namespace {

// 점수의 i번째(0~4) 숫자. 0이 가장 중요하다.
int rankAt(HandScore score, int i) { return static_cast<int>((score >> (4 * (4 - i))) & 0xF); }

// 같은 족보일 때, i번째 숫자가 무엇을 뜻하는지
std::string roleOf(HandCategory category, int i) {
    switch (category) {
        case HandCategory::HighCard: return "highCard";
        case HandCategory::OnePair: return i == 0 ? "pair" : "kicker";
        case HandCategory::TwoPair: return i == 0 ? "highPair" : i == 1 ? "lowPair" : "kicker";
        case HandCategory::ThreeOfAKind: return i == 0 ? "trips" : "kicker";
        case HandCategory::Straight:
        case HandCategory::StraightFlush: return "straight";
        case HandCategory::Flush: return "flushCard";
        case HandCategory::FullHouse: return i == 0 ? "trips" : "fullPair";
        case HandCategory::FourOfAKind: return i == 0 ? "quads" : "kicker";
    }
    return "kicker";
}

// 족보 자체를 이루는 카드 수 (정렬된 5장의 앞에서부터)
int madeCards(HandCategory category) {
    switch (category) {
        case HandCategory::HighCard: return 1;
        case HandCategory::OnePair: return 2;
        case HandCategory::ThreeOfAKind: return 3;
        case HandCategory::TwoPair:
        case HandCategory::FourOfAKind: return 4;
        default: return 5;
    }
}

bool contains(const std::vector<int>& list, int value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

}  // namespace

HandCompare compareScores(HandScore win, HandScore lose) {
    HandCompare c;
    if (win == lose) return c;  // 동점
    if (categoryOf(win) != categoryOf(lose)) {
        c.type = HandCompare::Type::Category;
        return c;
    }
    c.type = HandCompare::Type::Rank;
    for (int i = 0; i < 5; ++i) {
        const int a = rankAt(win, i), b = rankAt(lose, i);
        if (a != b) {
            c.role = roleOf(categoryOf(win), i);
            c.winRank = a;
            c.loseRank = b;
            break;
        }
        c.shared.push_back(a);
    }
    return c;
}

std::array<Card, 5> bestFive(const std::array<Card, 2>& hole, const std::vector<Card>& board) {
    // 공용 카드를 앞에 두면, 점수가 같은 조합 중에서 공용 카드를 많이 쓴 쪽이 먼저 뽑힌다.
    std::vector<Card> cards = board;
    cards.insert(cards.end(), hole.begin(), hole.end());
    const int n = static_cast<int>(cards.size());

    std::array<Card, 5> best{};
    HandScore bestScore = 0;
    for (unsigned mask = 0; mask < (1u << n); ++mask) {
        std::array<Card, 5> pick{};
        int k = 0;
        for (int i = 0; i < n && k <= 5; ++i) {
            if (!(mask & (1u << i))) continue;
            if (k < 5) pick[k] = cards[i];
            ++k;
        }
        if (k != 5) continue;
        const HandScore score = evaluate5(pick.data());
        if (score > bestScore) {
            bestScore = score;
            best = pick;
        }
    }

    // (같은 숫자가 많은 순, 숫자가 큰 순)으로 정렬. A-2-3-4-5에서는 A를 1로 본다.
    std::array<int, 15> count{};
    for (const Card& c : best) ++count[c.rank];
    const HandCategory category = categoryOf(bestScore);
    const bool wheel = (category == HandCategory::Straight || category == HandCategory::StraightFlush) &&
                       rankAt(bestScore, 0) == 5;
    auto value = [wheel](const Card& c) { return wheel && c.rank == 14 ? 1 : c.rank; };
    std::stable_sort(best.begin(), best.end(), [&](const Card& a, const Card& b) {
        if (count[a.rank] != count[b.rank]) return count[a.rank] > count[b.rank];
        return value(a) > value(b);
    });
    return best;
}

std::string handName(HandScore score) {
    if (categoryOf(score) == HandCategory::StraightFlush && rankAt(score, 0) == 14) return "로열 플러시";
    return categoryNameKo(categoryOf(score));
}

HandReview buildHandReview(const HandSummary& hand, const std::vector<std::string>& names) {
    HandReview r;
    r.handNumber = hand.handNumber;
    r.showdown = hand.showdown;
    r.board = hand.board;
    r.names = names;
    const int seats = static_cast<int>(hand.dealtIn.size());

    for (int i = 0; i < seats; ++i) {
        if (!hand.dealtIn[i]) continue;
        if (hand.folded[i]) {
            r.folded.push_back(i);
        } else if (hand.showdown) {  // 카드를 담는 곳은 여기뿐이다
            ReviewEntry e;
            e.seat = i;
            e.hole = hand.holes[i];
            e.best = bestFive(e.hole, hand.board);
            e.score = evaluate5(e.best.data());
            e.name = handName(e.score);
            for (int k = 0; k < madeCards(categoryOf(e.score)); ++k) e.key[k] = true;
            r.entries.push_back(e);
        }
    }

    // 강한 순으로 정렬 (동점은 좌석 순)
    std::stable_sort(r.entries.begin(), r.entries.end(),
                     [](const ReviewEntry& a, const ReviewEntry& b) { return a.score > b.score; });
    for (std::size_t i = 0; i < r.entries.size(); ++i) {
        ReviewEntry& e = r.entries[i];
        e.place = (i > 0 && r.entries[i - 1].score == e.score) ? r.entries[i - 1].place
                                                              : static_cast<int>(i) + 1;
        if (i + 1 >= r.entries.size()) continue;
        e.versusNext = compareScores(e.score, r.entries[i + 1].score);
        e.nextSeat = r.entries[i + 1].seat;
        // 승부를 가른 카드가 키커라면 그것도 강조한다.
        if (e.versusNext->type == HandCompare::Type::Rank) {
            for (int k = 0; k < 5; ++k) {
                if (e.best[k].rank == e.versusNext->winRank) {
                    e.key[k] = true;
                    break;
                }
            }
        }
    }

    auto scoreOf = [&r](int seat) -> HandScore {
        for (const ReviewEntry& e : r.entries) {
            if (e.seat == seat) return e.score;
        }
        return 0;
    };

    // 금액은 게임이 실제로 지급한 값(hand.pots)을 그대로 옮긴다.
    for (std::size_t k = 0; k < hand.pots.size(); ++k) {
        const PotResult& source = hand.pots[k];
        ReviewPot pot;
        pot.amount = source.amount;
        pot.eligible = source.eligible;
        for (int i = 0; i < seats; ++i) {
            if (source.won[i] > 0) pot.winners.push_back({i, source.won[i]});
        }
        for (const ReviewWinner& w : pot.winners) {
            if (w.amount != pot.winners.front().amount) pot.oddChip = true;
        }
        if (k > 0) {  // 앞 팟에는 자격이 있었지만 이 팟에는 없는 사람 = 올인해서 한도가 정해진 사람
            for (int seat : hand.pots[k - 1].eligible) {
                if (!contains(source.eligible, seat)) pot.cappedBy.push_back(seat);
            }
        }
        if (r.showdown && pot.eligible.size() > 1 && !pot.winners.empty()) {
            const HandScore winScore = scoreOf(pot.winners.front().seat);
            HandScore loseScore = 0;
            for (int seat : pot.eligible) {
                const HandScore s = scoreOf(seat);
                if (s < winScore && s > loseScore) {
                    loseScore = s;
                    pot.loserSeat = seat;
                }
            }
            pot.compare = compareScores(winScore, pot.loserSeat >= 0 ? loseScore : winScore);
            if (pot.winners.size() > 1 && r.board.size() == 5) {
                pot.compare->boardPlays = evaluate5(r.board.data()) == winScore;
            }
        }
        r.pots.push_back(pot);
    }
    return r;
}

std::optional<ReviewFlow> buildReviewFlow(const HandReview& review, int seat) {
    const ReviewEntry* mine = nullptr;
    for (const ReviewEntry& e : review.entries) {
        if (e.seat == seat) mine = &e;
    }
    if (!mine || review.board.size() != 5) return std::nullopt;

    ReviewFlow flow;
    for (const ReviewPot& pot : review.pots) {
        for (const ReviewWinner& w : pot.winners) {
            if (w.seat == seat) flow.won = true;
        }
    }
    const std::pair<const char*, int> streets[] = {{"flop", 3}, {"turn", 4}, {"river", 5}};
    for (const auto& [street, count] : streets) {
        auto scoreWith = [&review, count = count](const std::array<Card, 2>& hole) {
            std::vector<Card> cards(review.board.begin(), review.board.begin() + count);
            cards.insert(cards.end(), hole.begin(), hole.end());
            return evaluateBest(cards);
        };
        const HandScore my = scoreWith(mine->hole);
        HandScore bestOther = 0;
        for (const ReviewEntry& e : review.entries) {
            if (e.seat != seat) bestOther = std::max(bestOther, scoreWith(e.hole));
        }
        StreetStanding standing;
        standing.street = street;
        standing.status = my > bestOther ? "ahead" : my == bestOther ? "tied" : "behind";
        flow.streets.push_back(standing);
    }
    return flow;
}
