// bots.cpp - 봇 5종 구현
#include "bots.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

#include "equity.h"
#include "evaluator.h"

// ---------------------------------------------------------------------------
// 핸드 강도 계산
// ---------------------------------------------------------------------------

double preflopStrength(const std::array<Card, 2>& hole) {
    // Chen 공식: 높은 카드 점수 + 페어/수티드/연결 보너스
    auto cardPoints = [](int rank) {
        switch (rank) {
            case 14: return 10.0;
            case 13: return 8.0;
            case 12: return 7.0;
            case 11: return 6.0;
            default: return rank / 2.0;  // 10 -> 5, 9 -> 4.5, ... 2 -> 1
        }
    };
    int hi = std::max(hole[0].rank, hole[1].rank);
    int lo = std::min(hole[0].rank, hole[1].rank);
    double points = cardPoints(hi);
    if (hi == lo) {
        points = std::max(5.0, points * 2.0);  // 페어는 두 배 (최소 5점)
    } else {
        if (hole[0].suit == hole[1].suit) points += 2.0;  // 수티드
        int gap = hi - lo - 1;                            // 두 카드 사이 빈 숫자 수
        if (gap == 1) points -= 1.0;
        else if (gap == 2) points -= 2.0;
        else if (gap == 3) points -= 4.0;
        else if (gap >= 4) points -= 5.0;
        if (gap <= 1 && hi < 12) points += 1.0;  // 연결되어 있고 Q보다 낮으면 보너스
    }
    // 대략 -1(72o) ~ 20(AA) 범위를 0~1로 바꾼다.
    return std::clamp((points + 1.0) / 21.0, 0.0, 1.0);
}

double preflopPercentile(const std::array<Card, 2>& hole) {
    // 1326가지 시작 핸드의 강도를 한 번만 계산해 둔다.
    static const std::vector<double> all = [] {
        std::vector<Card> deck;
        for (int suit = 0; suit < 4; ++suit) {
            for (int rank = 2; rank <= 14; ++rank) {
                Card c{};
                c.rank = rank;
                c.suit = static_cast<decltype(c.suit)>(suit);
                deck.push_back(c);
            }
        }
        std::vector<double> values;
        for (std::size_t i = 0; i < deck.size(); ++i) {
            for (std::size_t j = i + 1; j < deck.size(); ++j) values.push_back(preflopStrength({deck[i], deck[j]}));
        }
        std::sort(values.begin(), values.end());
        return values;
    }();
    const double mine = preflopStrength(hole);
    const auto range = std::equal_range(all.begin(), all.end(), mine);
    const double weaker = static_cast<double>(range.first - all.begin());
    const double same = static_cast<double>(range.second - range.first);
    return (weaker + same / 2) / static_cast<double>(all.size());
}

double postflopStrength(const std::array<Card, 2>& hole, const std::vector<Card>& board) {
    std::vector<Card> all = {hole[0], hole[1]};
    all.insert(all.end(), board.begin(), board.end());
    const HandScore score = evaluateBest(all);
    const HandCategory cat = categoryOf(score);

    // 보드만으로도 같은 족보가 되면(= 내 홀 카드가 기여하지 않으면) 가치가 낮다.
    // 보드가 5장일 때만 정확히 비교할 수 있다. 3~4장일 때는 페어 여부로 근사한다.
    int boardMax = 0;
    for (const Card& c : board) boardMax = std::max(boardMax, c.rank);
    auto holeMatchesBoard = [&](const Card& h) {
        return std::any_of(board.begin(), board.end(), [&](const Card& b) { return b.rank == h.rank; });
    };
    const bool pocketPair = hole[0].rank == hole[1].rank;

    switch (cat) {
        case HandCategory::HighCard: {
            int hi = std::max(hole[0].rank, hole[1].rank);
            return hi == 14 ? 0.22 : 0.12;
        }
        case HandCategory::OnePair: {
            // 페어가 내 카드로 만들어졌는지, 얼마나 높은지
            const int pairRank = static_cast<int>((score >> 16) & 0xF);
            const bool mine = pocketPair || holeMatchesBoard(hole[0]) || holeMatchesBoard(hole[1]);
            if (!mine) return 0.18;                       // 보드 페어: 사실상 하이 카드
            if (pairRank >= boardMax) return 0.58;        // 탑 페어 또는 오버페어
            return 0.40;                                  // 미들/바텀 페어, 언더페어
        }
        case HandCategory::TwoPair: {
            bool mineBoth = holeMatchesBoard(hole[0]) && holeMatchesBoard(hole[1]);
            return (mineBoth || pocketPair) ? 0.78 : 0.55;
        }
        case HandCategory::ThreeOfAKind: return 0.84;
        case HandCategory::Straight: return 0.88;
        case HandCategory::Flush: return 0.92;
        case HandCategory::FullHouse: return 0.97;
        case HandCategory::FourOfAKind:
        case HandCategory::StraightFlush: return 0.995;
    }
    return 0.0;
}

double handStrength(const GameState& s) {
    if (s.board.empty()) return preflopStrength(s.myHole);
    return postflopStrength(s.myHole, s.board);
}

// ---------------------------------------------------------------------------
// 생각하는 시간: 상황만 본다 (카드 강도는 입력이 아니다)
// ---------------------------------------------------------------------------
int thinkTimeMs(const GameState& s, const Action& action, double speed, std::mt19937& rng) {
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const bool someoneAllIn = std::any_of(s.players.begin(), s.players.end(),
                                          [](const PlayerView& p) { return p.inHand && !p.folded && p.allIn; });
    const double share = s.pot > 0 ? static_cast<double>(s.toCall) / s.pot : 0.0;
    const bool big = action.type == ActionType::AllIn || (s.toCall > 0 && (someoneAllIn || s.toCall >= s.bigBlind * 8 ||
                     share >= 0.45 || (s.myChips > 0 && s.toCall >= s.myChips * 0.35)));
    double low = 1500, high = 3500;  // 보통의 콜, 체크, 베팅
    if (big) low = 3500, high = 7000;
    else if (s.board.empty() && action.type == ActionType::Fold) low = 1000, high = 2000;  // 쉬운 폴드
    // 범위 안에서는 대부분 무작위이고, 상황의 복잡도에 약하게만 기운다.
    const double complexity = std::min(1.0, share) * 0.6 + static_cast<double>(s.board.size()) / 5.0 * 0.4;
    const double at = uniform(rng) * 0.75 + complexity * 0.25;
    const double jitter = 0.8 + uniform(rng) * 0.4;  // ±20%
    return static_cast<int>((low + (high - low) * at) * speed * jitter);
}

// ---------------------------------------------------------------------------
// 공통 도우미
// ---------------------------------------------------------------------------

Action Bot::checkOrFold(const GameState& s) const {
    return s.toCall == 0 ? Action::check() : Action::fold();
}

Action Bot::checkOrCall(const GameState& s) const {
    return s.toCall == 0 ? Action::check() : Action::call();
}

Action Bot::raiseTo(const GameState& s, int target) const {
    if (!s.canRaise) return checkOrCall(s);
    target = std::max(target, s.minRaiseTo);
    if (target >= s.maxRaiseTo) return Action::allIn();
    return Action::raiseTo(target);
}

Action Bot::raiseByPot(const GameState& s, double fraction) const {
    // "콜한 뒤의 팟"의 fraction 배만큼 더 올린다.
    const int potAfterCall = s.pot + s.toCall;
    return raiseTo(s, s.currentBet + static_cast<int>(std::lround(potAfterCall * fraction)));
}

Action Bot::bet(const GameState& s, const BotProfile& p, double strength) {
    // 크기: 1/3, 1/2, 2/3, 1배, 오버벳. 강할수록, 공격적일수록 큰 쪽이 잦다.
    const double push = std::clamp(p.aggression * 0.6 + (strength >= 0.75 ? 0.3 : 0.0), 0.0, 1.0);
    const double weights[5] = {0.35 - 0.25 * push, 0.30, 0.20 + 0.15 * push, 0.10 + 0.20 * push,
                               strength >= 0.8 || strength < 0.3 ? 0.10 * p.aggression : 0.0};
    const double sizes[5] = {0.33, 0.5, 0.66, 1.0, 1.25 + 0.35 * rand01()};
    double total = 0;
    for (double w : weights) total += w;
    double pick = rand01() * total;
    int i = 0;
    while (i < 4 && pick >= weights[i]) pick -= weights[i++];
    return raiseByPot(s, sizes[i] * (0.92 + 0.16 * rand01()));
}

Action Bot::preflopRaise(const GameState& s) {
    if (s.currentBet <= s.bigBlind) {  // 처음 여는 레이즈
        return raiseTo(s, static_cast<int>(std::lround((2.2 + 1.3 * rand01()) * s.bigBlind)));
    }
    return raiseTo(s, static_cast<int>(std::lround(s.currentBet * (2.5 + rand01()))));
}

double Bot::mistakeRate() const {
    return difficulty_ == Difficulty::Easy ? 0.16 : difficulty_ == Difficulty::Hard ? 0.01 : 0.05;
}

BotProfile Bot::profile() const {
    BotProfile p = base_;
    // 틸트: 공격적인 성격은 더 세게, 신중한 성격은 더 위축된다 (설명 문구와 같은 방향).
    if (tiltHands_ > 0) {
        if (base_.aggression >= 0.8 || base_.vpip >= 0.5) {
            if (base_.foldPressure > 0.1) {  // 콜링 스테이션은 변하지 않는다
                p.aggression = std::min(1.0, p.aggression + 0.12);
                p.bluff += 0.05;
            }
        } else {
            p.aggression -= 0.12;
            p.vpip -= 0.04;
            p.pfr -= 0.03;
            p.foldPressure += 0.08;
        }
    }
    // 사람의 공개된 행동에 대한 약한 기억 (효과는 0.05로 제한)
    if (humanActions_ >= 12) {
        if (humanRaises_ * 5 >= humanActions_ * 2) p.foldPressure = std::max(0.0, p.foldPressure - 0.05);
        if (humanFolds_ * 5 >= humanActions_ * 3) p.bluff += 0.05;
    }
    p.bluff *= difficulty_ == Difficulty::Easy ? 0.6 : difficulty_ == Difficulty::Hard ? 1.15 : 1.0;
    return p;
}

void Bot::observe(const PublicAction& a) {
    if (a.seat >= humanSeats_) return;
    ++humanActions_;
    if (a.type == ActionType::Raise) ++humanRaises_;
    if (a.type == ActionType::Fold) ++humanFolds_;
}

void Bot::handEnded(int chipDelta, int bigBlind) {
    if (tiltHands_ > 0) --tiltHands_;
    if (chipDelta <= -20 * bigBlind) tiltHands_ = 3;  // 큰 팟을 잃으면 몇 판 동안
}

Action Bot::decide(const GameState& s) {
    const Action action = choose(s, profile());
    const bool aggressive = action.type == ActionType::Raise || action.type == ActionType::AllIn;
    lastBluff_ = aggressive && (s.board.empty() ? preflopPercentile(s.myHole) < 0.5 : postflopStrength(s.myHole, s.board) < 0.3);
    return action;
}

// ---------------------------------------------------------------------------
// 기본 판단 (타이트, 루즈, 공격형)
// ---------------------------------------------------------------------------
Action Bot::choose(const GameState& s, const BotProfile& p) {
    if (s.board.empty()) {  // 프리플랍: 핸드의 순위를 VPIP, PFR과 비교한다
        const double rank = preflopPercentile(s.myHole);
        const bool raised = s.currentBet > s.bigBlind;
        const double bbs = static_cast<double>(s.toCall) / s.bigBlind;
        if (rank >= 0.965) return preflopRaise(s);  // 최상위 핸드는 접지도 숨기지도 않는다
        if (rank >= 1.0 - p.pfr) {
            if (!raised) return preflopRaise(s);
            if (rank >= 1.0 - p.pfr * 0.35 && chance(p.aggression * 0.6)) return preflopRaise(s);
            if (bbs <= 6 + (1.0 - p.foldPressure) * 14) return checkOrCall(s);
            return chance(p.foldPressure) ? checkOrFold(s) : checkOrCall(s);
        }
        if (rank >= 1.0 - p.vpip) {
            if (s.toCall == 0) return Action::check();
            if (bbs <= 1.0 + (1.0 - p.foldPressure) * 6) return Action::call();
            return chance(mistakeRate()) && bbs <= 8 ? Action::call() : Action::fold();
        }
        // 범위 밖: 아무도 안 올렸으면 가끔 훔치기
        if (!raised && s.canRaise && chance(p.bluff * 0.3)) return preflopRaise(s);
        if (s.toCall > 0 && !raised && rank >= 0.9 - p.vpip && chance(mistakeRate())) return Action::call();
        return checkOrFold(s);
    }

    const double strength = postflopStrength(s.myHole, s.board);
    const double pressure = s.toCall > 0 ? static_cast<double>(s.toCall) / std::max(1, s.pot - s.toCall) : 0.0;

    if (s.toCall == 0) {
        if (strength >= 0.75) {
            if (chance(p.slowplay)) {  // 숨기고 기다렸다가 레이즈
                trapHand_ = s.handNumber;
                return Action::check();
            }
            return bet(s, p, strength);
        }
        if (strength >= 0.4) return chance(p.aggression * 0.75) ? bet(s, p, strength) : Action::check();
        return chance(p.bluff) ? bet(s, p, strength) : Action::check();
    }

    if (strength >= 0.75) {  // 강한 패는 어떤 경우에도 접지 않는다
        if (trapHand_ == s.handNumber) return bet(s, p, strength);
        return strength >= 0.84 && chance(p.aggression * 0.6) ? bet(s, p, strength) : Action::call();
    }
    if (strength >= 0.55) {
        if (chance(p.foldPressure * std::max(0.0, pressure - 0.6) * 0.6)) return Action::fold();
        return chance(p.aggression * 0.15) ? bet(s, p, strength) : Action::call();
    }
    if (strength >= 0.35) {  // 애매한 패
        double fold = p.foldPressure * std::min(1.5, 0.35 + pressure);
        if (chance(mistakeRate())) fold = 1.0 - fold;
        return chance(fold) ? Action::fold() : Action::call();
    }
    if (pressure <= 0.75 && chance(p.bluff * 0.35)) return bet(s, p, strength);
    if (pressure <= 0.4 && chance((1.0 - p.foldPressure) * 0.35 + mistakeRate())) return Action::call();
    return Action::fold();
}

// ---------------------------------------------------------------------------
// 4) 콜링 스테이션
// ---------------------------------------------------------------------------
Action CallingStationBot::choose(const GameState& s, const BotProfile& p) {
    const double strength = handStrength(s);
    const bool bigRaise = s.toCall > s.bigBlind * 3;
    if (!s.board.empty() && strength >= 0.75 && !bigRaise && chance(1.0 - p.slowplay)) {
        if (strength >= 0.9 || chance(p.aggression)) return raiseByPot(s, chance(0.5) ? 0.33 : 0.5);
    }
    if (s.board.empty() && preflopPercentile(s.myHole) >= 1.0 - p.pfr && !bigRaise && chance(0.5)) return preflopRaise(s);
    // 아무것도 없는데 가끔 엉뚱하게 건다.
    if (s.toCall == 0 && !s.board.empty() && chance(p.bluff)) return raiseByPot(s, chance(0.5) ? 0.33 : 1.0);
    // 내 칩의 절반 넘게 내야 하는데 쓰레기 핸드면 그때만 접는다.
    if (s.toCall > 0 && strength < 0.12 && s.toCall > s.myChips * 0.5) return Action::fold();
    return checkOrCall(s);
}

// ---------------------------------------------------------------------------
// 5) 확률 계산형
// ---------------------------------------------------------------------------
Action OddsBot::choose(const GameState& s, const BotProfile& p) {
    // 아직 폴드하지 않은 상대 수. 상대 카드는 모르므로 무작위로 뽑아 계산한다.
    const int opponents = std::max(1, s.playersLeft() - 1);
    const double equity = estimateEquity(s.myHole, s.board, opponents, trials_, rng_);

    // 팟 오즈: 콜하려고 내는 칩 / (콜 후 팟). 승률이 이보다 높으면 콜이 이득이다.
    const double potOdds = s.toCall > 0 ? static_cast<double>(s.toCall) / (s.pot + s.toCall) : 0.0;
    const double fair = 1.0 / (opponents + 1);

    if (equity > std::max(0.65, fair + 0.3)) {  // 밸류 벳
        if (!s.board.empty() && chance(p.slowplay)) return checkOrCall(s);
        return s.board.empty() ? preflopRaise(s) : bet(s, p, equity);
    }
    if (equity > std::max(0.5, fair + 0.18) && s.toCall == 0) return s.board.empty() ? preflopRaise(s) : bet(s, p, 0.6);
    if (s.toCall == 0) return !s.board.empty() && chance(p.bluff) ? bet(s, p, 0.2) : Action::check();
    // 아슬아슬할 때만 가끔 반대로 고른다.
    bool call = equity > potOdds + 0.03;
    if (std::fabs(equity - potOdds) < 0.08 && chance(mistakeRate() * 2)) call = !call;
    return call ? Action::call() : Action::fold();
}

// ---------------------------------------------------------------------------
std::vector<std::unique_ptr<Player>> makeAllBots(unsigned seed) {
    std::vector<std::unique_ptr<Player>> bots;
    bots.push_back(std::make_unique<TightBot>(seed * 7 + 1));
    bots.push_back(std::make_unique<LooseBot>(seed * 7 + 2));
    bots.push_back(std::make_unique<AggroBot>(seed * 7 + 3));
    bots.push_back(std::make_unique<CallingStationBot>(seed * 7 + 4));
    bots.push_back(std::make_unique<OddsBot>(seed * 7 + 5));
    return bots;
}
