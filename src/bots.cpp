// bots.cpp - 봇 5종 구현
#include "bots.h"

#include <algorithm>
#include <array>
#include <cmath>
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
// 공통 도우미
// ---------------------------------------------------------------------------

Action Bot::checkOrFold(const GameState& s) const {
    return s.toCall == 0 ? Action::check() : Action::fold();
}

Action Bot::raiseByPot(const GameState& s, double fraction) const {
    if (!s.canRaise) return s.toCall == 0 ? Action::check() : Action::call();
    // "콜한 뒤의 팟"의 fraction 배만큼 더 올린다.
    const int potAfterCall = s.pot + s.toCall;
    int target = s.currentBet + static_cast<int>(std::lround(potAfterCall * fraction));
    target = std::max(target, s.minRaiseTo);
    if (target >= s.maxRaiseTo) return Action::allIn();
    return Action::raiseTo(target);
}

// 이미 누가 레이즈해서 "큰 베팅"을 마주한 상황인가? (콜 비용이 빅블라인드의 n배 초과)
static bool facingRaise(const GameState& s, double bigBlinds = 1.0) {
    return s.toCall > s.bigBlind * bigBlinds;
}

// ---------------------------------------------------------------------------
// 1) 타이트
// ---------------------------------------------------------------------------
Action TightBot::decide(const GameState& s) {
    const double strength = handStrength(s);
    const double potOdds = s.toCall > 0 ? static_cast<double>(s.toCall) / (s.pot + s.toCall) : 0.0;
    const double bbs = static_cast<double>(s.toCall) / s.bigBlind;  // 콜 비용이 빅블라인드 몇 개인가

    if (s.board.empty()) {  // 프리플랍
        if (strength >= 0.65) return raiseByPot(s, 0.8);  // QQ+, AKs 등: 재레이즈도 한다
        if (strength >= 0.5) {                            // TT, AKo, AQs 등
            if (!facingRaise(s)) return raiseByPot(s, 0.7);  // 아무도 안 올렸으면 먼저 레이즈
            return bbs <= 12 ? Action::call() : Action::fold();
        }
        if (strength >= 0.38) {                           // 77~99, AJo, 수티드 커넥터 등
            if (s.toCall == 0) return Action::check();
            return bbs <= 4 ? Action::call() : Action::fold();
        }
        return checkOrFold(s);
    }
    // 플랍 이후
    if (strength >= 0.9) return raiseByPot(s, 0.75);
    if (strength >= 0.75) return s.toCall == 0 ? raiseByPot(s, 0.65) : Action::call();
    if (strength >= 0.55) {
        if (s.toCall == 0) return raiseByPot(s, 0.5);
        return potOdds < 0.35 ? Action::call() : Action::fold();
    }
    return checkOrFold(s);
}

// ---------------------------------------------------------------------------
// 2) 루즈
// ---------------------------------------------------------------------------
Action LooseBot::decide(const GameState& s) {
    const double strength = handStrength(s);
    const bool preflop = s.board.empty();

    // 강한 핸드는 가끔 레이즈. 큰 레이즈를 마주했을 때는 아주 강해야만 재레이즈.
    if (strength >= (preflop ? 0.6 : 0.75) && rand01() < 0.7) {
        if (!facingRaise(s, 3) || strength >= 0.85) return raiseByPot(s, 0.6);
    }
    // 가끔 의미 없는 오픈 레이즈
    if (!facingRaise(s) && rand01() < 0.08) return raiseByPot(s, 0.5);

    // 팟 대비 아주 큰 베팅이 아니면 웬만하면 따라간다.
    const double minStrength = preflop ? 0.12 : 0.15;
    const bool tooExpensive = s.toCall > s.pot * 0.9 + s.bigBlind;
    if (strength >= minStrength && (!tooExpensive || strength >= 0.5)) {
        return s.toCall == 0 ? Action::check() : Action::call();
    }
    if (rand01() < 0.25 && s.toCall <= s.bigBlind * 2) return s.toCall == 0 ? Action::check() : Action::call();
    return checkOrFold(s);
}

// ---------------------------------------------------------------------------
// 3) 공격형
// ---------------------------------------------------------------------------
Action AggroBot::decide(const GameState& s) {
    const double strength = handStrength(s);
    const bool preflop = s.board.empty();

    // 강한 핸드: 항상 크게 벳/레이즈
    if (strength >= (preflop ? 0.5 : 0.65)) return raiseByPot(s, 0.9 + 0.6 * rand01());
    // 블러프: 아직 큰 베팅이 없을 때만 40% 확률로 약한 핸드로 벳/레이즈
    if (!facingRaise(s) && rand01() < 0.40) return raiseByPot(s, 0.6 + 0.4 * rand01());
    // 큰 베팅 앞에서는 중간 이상의 핸드로만 콜
    if (strength >= (preflop ? 0.3 : 0.35) && s.toCall <= s.pot * 0.75) {
        return s.toCall == 0 ? Action::check() : Action::call();
    }
    return checkOrFold(s);
}

// ---------------------------------------------------------------------------
// 4) 콜링 스테이션
// ---------------------------------------------------------------------------
Action CallingStationBot::decide(const GameState& s) {
    const double strength = handStrength(s);
    // 아주 강할 때만 가끔 레이즈 (큰 베팅을 마주한 상황에서는 그냥 콜)
    if (strength >= 0.9 && !facingRaise(s, 3) && rand01() < 0.5) return raiseByPot(s, 0.5);
    // 내 칩의 절반 넘게 내야 하는데 쓰레기 핸드면 그때만 접는다.
    if (s.toCall > 0 && strength < 0.12 && s.toCall > s.myChips * 0.5) return Action::fold();
    return s.toCall == 0 ? Action::check() : Action::call();
}

// ---------------------------------------------------------------------------
// 5) 확률 계산형
// ---------------------------------------------------------------------------
Action OddsBot::decide(const GameState& s) {
    // 아직 폴드하지 않은 상대 수
    const int opponents = std::max(1, s.playersLeft() - 1);
    const double equity = estimateEquity(s.myHole, s.board, opponents, trials_, rng_);

    // 팟 오즈: 콜하려고 내는 칩 / (콜 후 팟). 승률이 이보다 높으면 콜이 이득이다.
    const double potOdds = s.toCall > 0 ? static_cast<double>(s.toCall) / (s.pot + s.toCall) : 0.0;
    // 상대가 많을수록 승률 기준을 높인다 (내 승률이 상대 수에 맞춰 이미 반영돼 있지만,
    // 레이즈에는 "평균 승률 1/(n+1)"보다 확실히 높아야 한다는 기준을 둔다).
    const double fair = 1.0 / (opponents + 1);

    if (equity > std::max(0.65, fair + 0.3)) return raiseByPot(s, 0.75);   // 밸류 벳
    if (equity > std::max(0.5, fair + 0.18) && s.toCall == 0) return raiseByPot(s, 0.5);
    if (s.toCall == 0) return Action::check();
    // 콜하는 경우: 승률이 팟 오즈를 넘고 약간의 여유(안전 마진)가 있을 때
    if (equity > potOdds + 0.03) return Action::call();
    return Action::fold();
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
