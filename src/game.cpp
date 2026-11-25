// game.cpp - 한 판 진행 구현
#include "game.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "evaluator.h"

// NDEBUG(릴리스 빌드)에서도 꺼지지 않는 검사. 실패하면 메시지를 찍고 즉시 종료한다.
#define HOLDEM_REQUIRE(cond, msg)                                                      \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "치명적 오류: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            std::abort();                                                              \
        }                                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// 포지션 이름
// ---------------------------------------------------------------------------
std::string positionName(const GameState& st, int seat) {
    if (seat < 0 || seat >= st.numSeats || !st.players[seat].inHand) return "-";

    // 버튼부터 시계 방향으로 참여자를 늘어놓는다.
    std::vector<int> order;
    for (int i = 0; i < st.numSeats; ++i) {
        int s = (st.button + i) % st.numSeats;
        if (st.players[s].inHand) order.push_back(s);
    }
    const int n = static_cast<int>(order.size());
    const int idx = static_cast<int>(std::find(order.begin(), order.end(), seat) - order.begin());

    if (n == 2) return idx == 0 ? "BTN/SB" : "BB";  // 헤즈업: 버튼이 SB
    if (idx == 0) return "BTN";
    if (idx == 1) return "SB";
    if (idx == 2) return "BB";
    if (idx == 3) return "UTG";
    if (idx == n - 1) return "CO";
    if (idx == n - 2) return "HJ";
    return "MP";
}

// ---------------------------------------------------------------------------
// 생성, 조회
// ---------------------------------------------------------------------------
Game::Game(const GameConfig& config, std::vector<std::unique_ptr<Player>> players, unsigned seed)
    : config_(config), rng_(seed) {
    HOLDEM_REQUIRE(players.size() >= 2, "플레이어가 2명 이상 필요합니다");
    for (auto& p : players) {
        Seat s;
        s.player = std::move(p);
        s.chips = config.startingChips;
        seats_.push_back(std::move(s));
    }
}

int Game::activePlayers() const {
    int n = 0;
    for (const Seat& s : seats_) {
        if (s.chips > 0) ++n;
    }
    return n;
}

int Game::nextInHand(int seat) const {
    for (int i = 1; i <= numSeats(); ++i) {
        int s = (seat + i) % numSeats();
        if (seats_[s].inHand) return s;
    }
    return seat;
}

int Game::liveCount() const {
    int n = 0;
    for (const Seat& s : seats_) {
        if (s.inHand && !s.folded) ++n;
    }
    return n;
}

int Game::canActCount() const {
    int n = 0;
    for (const Seat& s : seats_) {
        if (s.inHand && !s.folded && !s.allIn) ++n;
    }
    return n;
}

int Game::totalChips() const {
    int sum = pot_;
    for (const Seat& s : seats_) sum += s.chips + s.bet;
    return sum;
}

std::string Game::seatName(int seat) const { return seats_[seat].player->name(); }

void Game::log(const std::string& line) {
    if (history_) history_->push_back(line);
    if (logger_) logger_(line);
}

// ---------------------------------------------------------------------------
// 행동 가능 여부 판정
// ---------------------------------------------------------------------------

// 이 좌석이 지금 행동해야 하는가?
//  - 최고 베팅액에 못 미치면 (콜/폴드/레이즈 중 선택해야 하므로) 행동 필요
//  - 아직 이번 라운드에 행동하지 않았다면 행동 필요. 단, 맞서 베팅할 상대가
//    (나 말고) 아무도 없으면(모두 올인/폴드) 행동할 필요가 없다.
bool Game::needsAction(int seat) const {
    const Seat& s = seats_[seat];
    if (!s.inHand || s.folded || s.allIn) return false;
    if (s.bet < currentBet_) return true;
    return !s.acted && canActCount() >= 2;
}

// 레이즈 권리 판정.
//  - 아직 행동하지 않았다면 항상 레이즈 가능.
//  - 이미 행동했다면, 내 마지막 행동 이후 최고 베팅액이 "풀 레이즈 폭" 이상 올라야만
//    다시 레이즈할 수 있다. (작은 올인 하나는 레이즈 권리를 다시 열지 않는다.
//    작은 올인이 여러 번 쌓여 풀 레이즈 폭 이상이 되면 다시 열린다.)
//  - 콜 금액을 내고도 칩이 남아야 하고, 맞서 베팅할 상대가 있어야 한다.
bool Game::canRaise(int seat) const {
    const Seat& s = seats_[seat];
    if (!s.inHand || s.folded || s.allIn) return false;
    if (s.chips <= currentBet_ - s.bet) return false;
    if (canActCount() < 2) return false;
    if (!s.acted) return true;
    return currentBet_ - s.levelAtAction >= lastRaise_;
}

GameState Game::makeState(int seat, Street street) const {
    const Seat& me = seats_[seat];
    GameState st;
    st.handNumber = handNumber_;
    st.street = street;
    st.numSeats = numSeats();
    st.mySeat = seat;
    st.button = button_;
    st.smallBlindSeat = sbSeat_;
    st.bigBlindSeat = bbSeat_;
    st.smallBlind = config_.smallBlind;
    st.bigBlind = config_.bigBlind;
    st.myHole = me.hole;
    st.board = board_;
    int bets = 0;
    for (const Seat& s : seats_) {
        PlayerView v;
        v.name = s.player->name();
        v.chips = s.chips;
        v.bet = s.bet;
        v.contributed = s.contributed;
        v.inHand = s.inHand;
        v.folded = s.folded;
        v.allIn = s.allIn;
        st.players.push_back(v);
        bets += s.bet;
    }
    st.pot = pot_ + bets;
    st.currentBet = currentBet_;
    st.toCall = std::min(currentBet_ - me.bet, me.chips);
    st.minRaiseTo = currentBet_ + lastRaise_;
    st.maxRaiseTo = me.bet + me.chips;
    st.canRaise = canRaise(seat);
    st.myChips = me.chips;
    st.myBet = me.bet;
    return st;
}

// ---------------------------------------------------------------------------
// 칩 이동
// ---------------------------------------------------------------------------
void Game::putChips(int seat, int amount) {
    Seat& s = seats_[seat];
    amount = std::min(amount, s.chips);
    s.chips -= amount;
    s.bet += amount;
    s.contributed += amount;
    if (s.chips == 0) s.allIn = true;
}

void Game::postBlind(int seat, int amount, const char* label) {
    putChips(seat, amount);
    log(seatName(seat) + ": " + label + " " + std::to_string(seats_[seat].bet) +
        (seats_[seat].allIn ? " (올인)" : ""));
}

// 라운드가 끝날 때, 아무도 콜하지 않은 가장 큰 베팅의 초과분을 주인에게 돌려준다.
void Game::returnUncalledBet() {
    int best = -1, bestBet = 0, second = 0;
    for (int i = 0; i < numSeats(); ++i) {
        if (!seats_[i].inHand) continue;
        int b = seats_[i].bet;
        if (b > bestBet) {
            second = bestBet;
            bestBet = b;
            best = i;
        } else if (b > second) {
            second = b;
        }
    }
    if (best >= 0 && bestBet > second) {
        int diff = bestBet - second;
        seats_[best].chips += diff;
        seats_[best].bet -= diff;
        seats_[best].contributed -= diff;
        log(seatName(best) + ": 콜 받지 못한 베팅 " + std::to_string(diff) + " 반환");
    }
}

// 라운드 베팅을 팟으로 옮긴다.
void Game::collectBets() {
    for (Seat& s : seats_) {
        pot_ += s.bet;
        s.bet = 0;
    }
}

void Game::dealBoard(int count, const char* label) {
    deck_.burn();  // 번 카드는 버린다
    for (int i = 0; i < count; ++i) board_.push_back(deck_.draw());
    std::string line = std::string("[") + label + "]";
    for (const Card& c : board_) line += " " + cardToString(c);
    log(line);
}

// ---------------------------------------------------------------------------
// 베팅 라운드
// ---------------------------------------------------------------------------

// 행동을 규칙에 맞게 고쳐서 적용한다.
//  - 체크할 수 있는데 폴드 -> 체크, 콜할 게 없는데 콜 -> 체크, 콜해야 하는데 체크 -> 폴드
//  - 레이즈 권리가 없는데 레이즈/올인 -> 콜
//  - 레이즈 금액이 최소 레이즈보다 작으면 최소 레이즈로, 칩보다 크면 올인으로
void Game::applyAction(int seat, Action action) {
    Seat& s = seats_[seat];
    const int toCall = currentBet_ - s.bet;
    const int maxTo = s.bet + s.chips;
    ActionType type = action.type;
    int target = 0;  // Raise일 때 목표 금액

    switch (type) {
        case ActionType::Fold:
            if (toCall == 0) type = ActionType::Check;
            break;
        case ActionType::Check:
            if (toCall > 0) type = ActionType::Fold;
            break;
        case ActionType::Call:
            if (toCall == 0) type = ActionType::Check;
            break;
        case ActionType::AllIn:
            if (maxTo <= currentBet_ || !canRaise(seat)) {
                type = ActionType::Call;  // 올인이 사실상 콜이거나, 레이즈 권리가 없는 경우
            } else {
                type = ActionType::Raise;
                target = maxTo;
            }
            break;
        case ActionType::Raise:
            if (!canRaise(seat)) {
                type = ActionType::Call;
            } else {
                target = std::max(action.amount, currentBet_ + lastRaise_);
                if (target >= maxTo) target = maxTo;
            }
            break;
    }
    if (type == ActionType::Call && toCall == 0) type = ActionType::Check;

    const std::string who = seatName(seat) + ": ";
    switch (type) {
        case ActionType::Fold:
            s.folded = true;
            log(who + "폴드");
            break;
        case ActionType::Check:
            log(who + "체크");
            break;
        case ActionType::Call: {
            int amount = std::min(toCall, s.chips);
            putChips(seat, amount);
            log(who + "콜 " + std::to_string(amount) + (s.allIn ? " (올인)" : ""));
            break;
        }
        case ActionType::Raise: {
            const bool isBet = (currentBet_ == 0);
            const int increase = target - currentBet_;
            putChips(seat, target - s.bet);
            if (increase >= lastRaise_) lastRaise_ = increase;  // 풀 레이즈면 최소 레이즈 폭 갱신
            currentBet_ = target;                               // 작은 올인이면 폭은 그대로
            log(who + (isBet ? "벳 " : "레이즈 ") + std::to_string(target) +
                (s.allIn ? " (올인)" : ""));
            break;
        }
        case ActionType::AllIn:  // 위에서 Call/Raise로 바뀌므로 도달하지 않는다
            break;
    }
    s.acted = true;
    s.levelAtAction = currentBet_;
}

// 한 라운드의 베팅을 진행한다. 폴드로 한 명만 남으면 false를 돌려준다.
bool Game::bettingRound(int startSeat, Street street) {
    for (Seat& s : seats_) s.acted = false;
    if (street != Street::Preflop) {  // 프리플랍은 블라인드가 이미 깔려 있다
        currentBet_ = 0;
        lastRaise_ = config_.bigBlind;
    }

    int cursor = startSeat;
    while (liveCount() > 1) {
        // cursor부터 시계 방향으로 행동할 사람을 찾는다.
        int actor = -1;
        for (int i = 0; i < numSeats(); ++i) {
            int s = (cursor + i) % numSeats();
            if (needsAction(s)) {
                actor = s;
                break;
            }
        }
        if (actor < 0) break;  // 모두 행동을 마쳤다

        Action action = seats_[actor].player->decide(makeState(actor, street));
        applyAction(actor, action);
        cursor = next(actor);
    }

    returnUncalledBet();
    collectBets();
    HOLDEM_REQUIRE(totalChips() == expectedTotal_, "라운드 종료 후 칩 총합이 달라졌습니다");
    return liveCount() > 1;
}

// ---------------------------------------------------------------------------
// 한 판 진행
// ---------------------------------------------------------------------------
HandSummary Game::playHand() {
    HandSummary summary;
    history_ = &summary.history;
    ++handNumber_;
    const int n = numSeats();

    // 1) 참가자 정하기: 칩이 있는 사람만 카드를 받는다.
    for (Seat& s : seats_) {
        s.inHand = s.chips > 0;
        s.folded = s.allIn = s.acted = false;
        s.bet = s.contributed = s.levelAtAction = 0;
    }
    HOLDEM_REQUIRE(activePlayers() >= 2, "칩이 있는 플레이어가 2명 이상이어야 합니다");
    pot_ = 0;
    board_.clear();
    showdown_ = false;
    expectedTotal_ = totalChips();
    for (const Seat& s : seats_) summary.startChips.push_back(s.chips);

    // 2) 딜러 버튼: 매 판 칩이 있는 다음 사람(시계 방향)에게 옮긴다.
    if (forcedButton_ >= 0) {
        HOLDEM_REQUIRE(seats_[forcedButton_].inHand, "강제한 버튼 좌석은 참가자여야 합니다");
        button_ = forcedButton_;
        forcedButton_ = -1;
    } else if (button_ < 0) {
        std::vector<int> candidates;
        for (int i = 0; i < n; ++i) {
            if (seats_[i].inHand) candidates.push_back(i);
        }
        button_ = candidates[rng_() % candidates.size()];  // 첫 판은 무작위
    } else {
        button_ = nextInHand(button_);
    }

    // 3) 블라인드 위치. 헤즈업이면 버튼이 SB.
    const bool headsUp = activePlayers() == 2;
    sbSeat_ = headsUp ? button_ : nextInHand(button_);
    bbSeat_ = nextInHand(sbSeat_);

    log("===== 제 " + std::to_string(handNumber_) + " 판 =====");
    log("버튼: " + seatName(button_) + ", SB: " + seatName(sbSeat_) + ", BB: " + seatName(bbSeat_));

    // 4) 덱 준비
    deck_.reset();
    if (hasNextDeck_) {
        deck_.stack(nextDeck_);
        hasNextDeck_ = false;
    } else {
        deck_.shuffle(rng_);
    }

    // 5) 블라인드 지불. 칩이 모자라면 가진 만큼만 낸다(올인).
    postBlind(sbSeat_, config_.smallBlind, "스몰 블라인드");
    postBlind(bbSeat_, config_.bigBlind, "빅 블라인드");
    currentBet_ = config_.bigBlind;
    lastRaise_ = config_.bigBlind;

    // 6) 홀 카드: SB부터 시계 방향으로 한 장씩 두 바퀴
    for (int round = 0; round < 2; ++round) {
        int seat = sbSeat_;
        for (int i = 0; i < n; ++i) {
            if (seats_[seat].inHand) seats_[seat].hole[round] = deck_.draw();
            seat = next(seat);
        }
    }

    // 7) 프리플랍: BB 다음(UTG)부터. 헤즈업이면 버튼(SB)이 먼저.
    bool live = bettingRound(headsUp ? button_ : nextInHand(bbSeat_), Street::Preflop);

    // 8) 플랍, 턴, 리버: 버튼 다음 첫 사람부터. (헤즈업이면 BB가 먼저)
    const Street streets[3] = {Street::Flop, Street::Turn, Street::River};
    const int counts[3] = {3, 1, 1};
    const char* labels[3] = {"플랍", "턴", "리버"};
    for (int i = 0; i < 3 && live; ++i) {
        dealBoard(counts[i], labels[i]);
        live = bettingRound(next(button_), streets[i]);
    }

    // 9) 쇼다운 / 팟 분배 / 검증
    finishHand(summary);
    summary.handNumber = handNumber_;
    summary.button = button_;
    summary.smallBlindSeat = sbSeat_;
    summary.bigBlindSeat = bbSeat_;
    summary.showdown = showdown_;
    summary.board = board_;
    for (const Seat& s : seats_) {
        summary.endChips.push_back(s.chips);
        summary.holes.push_back(s.hole);
        summary.dealtIn.push_back(s.inHand);
    }
    history_ = nullptr;
    return summary;
}

// 쇼다운(또는 폴드 승리) 처리와 팟 분배
void Game::finishHand(HandSummary& /*summary*/) {
    const int n = numSeats();

    std::vector<PotEntry> entries;
    for (int i = 0; i < n; ++i) {
        if (seats_[i].inHand) entries.push_back(PotEntry{i, seats_[i].contributed, seats_[i].folded});
    }
    std::vector<Pot> pots = buildPots(entries);

    // 점수 계산: 둘 이상 남았으면 쇼다운
    std::vector<HandScore> scores(n, 0);
    showdown_ = liveCount() > 1;
    if (showdown_) {
        for (int i = 0; i < n; ++i) {
            const Seat& s = seats_[i];
            if (!s.inHand || s.folded) continue;
            std::vector<Card> cards = {s.hole[0], s.hole[1]};
            cards.insert(cards.end(), board_.begin(), board_.end());
            scores[i] = evaluateBest(cards);
            log(seatName(i) + ": " + cardToString(s.hole[0]) + " " + cardToString(s.hole[1]) +
                " -> " + categoryNameKo(categoryOf(scores[i])));
        }
    }

    // 팟마다 따로 나눠 준다 (홀수 칩 규칙이 팟마다 적용되도록)
    for (std::size_t k = 0; k < pots.size(); ++k) {
        std::vector<int> won = distributePots({pots[k]}, scores, button_, n);
        const std::string potName = (k == 0) ? "메인 팟" : "사이드 팟 " + std::to_string(k);
        for (int i = 0; i < n; ++i) {
            if (won[i] > 0) {
                seats_[i].chips += won[i];
                log(seatName(i) + ": " + potName + "에서 " + std::to_string(won[i]) + " 획득" +
                    (showdown_ ? "" : " (모두 폴드)"));
            }
        }
    }
    pot_ = 0;

    // 칩 총합이 판 전후로 보존되는지 매 판 검증
    HOLDEM_REQUIRE(totalChips() == expectedTotal_, "판 종료 후 칩 총합이 달라졌습니다");

    for (int i = 0; i < n; ++i) {
        if (seats_[i].inHand && seats_[i].chips == 0) log(seatName(i) + ": 칩이 없어 탈락");
    }
}
