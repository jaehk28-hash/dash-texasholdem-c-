// test_game.cpp - 한 판 진행 로직 테스트 (블라인드, 베팅 규칙, 사이드 팟, 봇끼리 대량 시뮬레이션)
#include <map>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "bots.h"
#include "check.h"
#include "equity.h"
#include "evaluator.h"
#include "game.h"
#include "human_player.h"

// ---------------------------------------------------------------------------
// 테스트 도우미
// ---------------------------------------------------------------------------

// 정해진 행동을 순서대로 하는 플레이어. 대본이 끝나면 체크/콜만 한다.
class ScriptedPlayer : public Player {
public:
    ScriptedPlayer(std::string name, std::vector<std::pair<int, Street>>* record)
        : name_(std::move(name)), record_(record) {}
    std::string name() const override { return name_; }
    Action decide(const GameState& s) override {
        states.push_back(s);
        if (record_) record_->push_back({s.mySeat, s.street});
        if (next_ < script.size()) return script[next_++];
        return Action::call();
    }
    std::vector<Action> script;
    std::vector<GameState> states;  // 결정할 때마다 받은 상황을 기록

private:
    std::string name_;
    std::vector<std::pair<int, Street>>* record_;
    std::size_t next_ = 0;
};

struct Table {
    std::unique_ptr<Game> game;
    std::vector<ScriptedPlayer*> p;
    std::vector<std::pair<int, Street>> record;  // (좌석, 라운드) 행동 순서 기록
};

static std::unique_ptr<Table> makeTable(int n, unsigned seed = 1) {
    auto t = std::make_unique<Table>();
    std::vector<std::unique_ptr<Player>> players;
    for (int i = 0; i < n; ++i) {
        auto sp = std::make_unique<ScriptedPlayer>("P" + std::to_string(i), &t->record);
        t->p.push_back(sp.get());
        players.push_back(std::move(sp));
    }
    t->game = std::make_unique<Game>(GameConfig{}, std::move(players), seed);
    return t;
}

static Card C1(const std::string& s) {
    Card c;
    parseCard(s, c);
    return c;
}
static std::vector<Card> cardsOf(const std::string& text) {
    std::vector<Card> v;
    std::istringstream in(text);
    std::string tok;
    while (in >> tok) v.push_back(C1(tok));
    return v;
}

// 덱 순서를 만든다. holes는 "SB부터 시계 방향" 순서로 나열한 "As Ad" 형태의 홀 카드들,
// board는 "플랍3장 턴 리버" 5장. 번 카드는 아직 안 쓴 카드로 채운다.
static std::vector<Card> stackedDeck(const std::vector<std::string>& holes, const std::string& board) {
    std::vector<Card> order;
    std::vector<std::vector<Card>> h;
    for (auto& s : holes) h.push_back(cardsOf(s));
    for (int round = 0; round < 2; ++round)
        for (auto& cards : h) order.push_back(cards[round]);
    std::vector<Card> b = cardsOf(board);
    std::vector<Card> used = order;
    used.insert(used.end(), b.begin(), b.end());
    std::vector<Card> dummies;
    Deck all;
    for (const Card& c : all.cards()) {
        if (std::find(used.begin(), used.end(), c) == used.end()) dummies.push_back(c);
        if (dummies.size() == 3) break;
    }
    // 번, 플랍 3장, 번, 턴, 번, 리버
    order.push_back(dummies[0]);
    for (int i = 0; i < 3; ++i) order.push_back(b[i]);
    order.push_back(dummies[1]);
    order.push_back(b[3]);
    order.push_back(dummies[2]);
    order.push_back(b[4]);
    return order;
}

static bool historyHas(const HandSummary& h, const std::string& text) {
    for (const auto& line : h.history) {
        if (line.find(text) != std::string::npos) return true;
    }
    return false;
}

static int sum(const std::vector<int>& v) {
    int s = 0;
    for (int x : v) s += x;
    return s;
}

// ---------------------------------------------------------------------------
// 테스트들
// ---------------------------------------------------------------------------

// 버튼이 매 판 한 칸씩 시계 방향으로 이동하고, SB/BB가 그 다음 두 자리인지
static void testButtonRotation() {
    auto t = makeTable(6, 5);
    int prev = -1;
    for (int hand = 0; hand < 12; ++hand) {
        HandSummary h = t->game->playHand();
        if (prev >= 0) CHECK(h.button == (prev + 1) % 6);
        CHECK(h.smallBlindSeat == (h.button + 1) % 6);
        CHECK(h.bigBlindSeat == (h.button + 2) % 6);
        CHECK(sum(h.endChips) == 60000);
        prev = h.button;
    }
}

// 행동 순서: 프리플랍은 UTG부터, 이후는 SB부터
static void testActionOrderSixHanded() {
    auto t = makeTable(6);
    t->game->setNextButton(0);
    t->game->playHand();
    std::vector<std::pair<int, Street>> expected;
    for (int s : {3, 4, 5, 0, 1, 2}) expected.push_back({s, Street::Preflop});
    for (Street st : {Street::Flop, Street::Turn, Street::River})
        for (int s : {1, 2, 3, 4, 5, 0}) expected.push_back({s, st});
    CHECK(t->record == expected);
}

// 헤즈업: 버튼이 SB, 프리플랍은 버튼 먼저, 이후는 BB 먼저
static void testHeadsUp() {
    auto t = makeTable(2);
    t->game->setNextButton(0);
    HandSummary h = t->game->playHand();
    CHECK(h.smallBlindSeat == h.button && h.button == 0 && h.bigBlindSeat == 1);
    std::vector<std::pair<int, Street>> expected = {
        {0, Street::Preflop}, {1, Street::Preflop}, {1, Street::Flop}, {0, Street::Flop},
        {1, Street::Turn},    {0, Street::Turn},    {1, Street::River}, {0, Street::River}};
    CHECK(t->record == expected);
    // 헤즈업 포지션 이름
    CHECK(positionName(t->p[0]->states[0], 0) == "BTN/SB");
    CHECK(positionName(t->p[0]->states[0], 1) == "BB");
    // 두 번째 판은 버튼이 옮겨 간다 (3명 이상일 때와 같은 규칙)
    HandSummary h2 = t->game->playHand();
    CHECK(h2.button == 1 && h2.smallBlindSeat == 1 && h2.bigBlindSeat == 0);
}

static void testPositionNames() {
    auto t = makeTable(6);
    t->game->setNextButton(0);
    t->game->playHand();
    const GameState& st = t->p[3]->states[0];
    const char* names[6] = {"BTN", "SB", "BB", "UTG", "HJ", "CO"};
    for (int i = 0; i < 6; ++i) CHECK(positionName(st, i) == names[i]);

    auto t5 = makeTable(5);
    t5->game->setNextButton(0);
    t5->game->playHand();
    const GameState& s5 = t5->p[3]->states[0];
    const char* n5[5] = {"BTN", "SB", "BB", "UTG", "CO"};
    for (int i = 0; i < 5; ++i) CHECK(positionName(s5, i) == n5[i]);
}

// 블라인드가 걸리고 프리플랍 상태가 정확한지
static void testBlindsAndInitialState() {
    auto t = makeTable(6);
    t->game->setNextButton(0);
    t->game->playHand();
    const GameState& s = t->p[3]->states[0];  // UTG의 첫 결정
    CHECK(s.pot == 150);
    CHECK(s.currentBet == 100 && s.toCall == 100);
    CHECK(s.minRaiseTo == 200 && s.maxRaiseTo == 10000);
    CHECK(s.canRaise);
    CHECK(s.players[1].bet == 50 && s.players[2].bet == 100);
    CHECK(s.board.empty());
    CHECK(s.myHole[0] != s.myHole[1]);
}

// 딜 순서: SB부터 한 장씩 두 바퀴, 번 카드 뒤에 보드
static void testDealOrderAndBurn() {
    auto t = makeTable(3);
    t->game->setNextButton(0);  // SB = 1, BB = 2, UTG = 0
    t->game->setNextDeck(stackedDeck({"As Ad", "Ks Kd", "Qs Qd"}, "2c 7d 9h Js 3c"));
    HandSummary h = t->game->playHand();
    CHECK(h.holes[1][0] == C1("As") && h.holes[1][1] == C1("Ad"));  // SB가 첫 장을 받는다
    CHECK(h.holes[2][0] == C1("Ks") && h.holes[2][1] == C1("Kd"));
    CHECK(h.holes[0][0] == C1("Qs") && h.holes[0][1] == C1("Qd"));
    CHECK(h.board == cardsOf("2c 7d 9h Js 3c"));
}

// 최소 레이즈: 직전 증가분 이상이어야 하고, 작으면 최소 금액으로 올려 준다
static void testMinRaise() {
    auto t = makeTable(6);
    t->game->setNextButton(0);  // UTG = 3
    t->p[3]->script = {Action::raiseTo(300), Action::fold()};
    t->p[4]->script = {Action::raiseTo(400)};  // 최소는 500 -> 500으로 보정
    for (int s : {5, 0, 1, 2}) t->p[s]->script = {Action::fold()};
    HandSummary h = t->game->playHand();
    CHECK(t->p[4]->states[0].minRaiseTo == 500);   // 300 + 증가분 200
    CHECK(t->p[5]->states[0].currentBet == 500);
    CHECK(t->p[5]->states[0].minRaiseTo == 700);   // 500 + 증가분 200
    CHECK(historyHas(h, "P4: 레이즈 500"));
    CHECK(!h.showdown);
    CHECK(h.endChips[4] == 10000 + 100 + 50 + 300 - 0 - 0);  // 블라인드 150 + P3의 300 획득
}

// 큰 레이즈 뒤에는 최소 레이즈 폭도 커진다
static void testMinRaiseGrows() {
    auto t = makeTable(6);
    t->game->setNextButton(0);
    t->p[3]->script = {Action::raiseTo(300)};   // 증가분 200
    t->p[4]->script = {Action::raiseTo(800)};   // 증가분 500
    t->game->playHand();
    CHECK(t->p[5]->states[0].minRaiseTo == 1300);  // 800 + 500
}

// 포스트플랍 최소 벳은 빅블라인드
static void testPostflopMinBet() {
    auto t = makeTable(3);
    t->game->setNextButton(0);  // SB=1, BB=2, UTG=0
    t->p[1]->script = {Action::call(), Action::raiseTo(30)};  // 플랍 첫 행동: 30 벳 -> 100으로 보정
    HandSummary h = t->game->playHand();
    CHECK(t->p[1]->states[1].street == Street::Flop);
    CHECK(t->p[1]->states[1].minRaiseTo == 100);
    CHECK(historyHas(h, "P1: 벳 100"));
}

// 작은 올인은 이미 행동한 사람의 레이즈 권리를 다시 열지 않는다
static void testShortAllInDoesNotReopen() {
    auto t = makeTable(3);
    t->game->setNextButton(0);  // UTG = 0, SB = 1, BB = 2
    t->game->setChips(1, 350);  // SB가 350까지 올인 (300+200=500보다 작은 숏 올인)
    t->p[0]->script = {Action::raiseTo(300), Action::raiseTo(2000)};
    t->p[1]->script = {Action::allIn()};
    HandSummary h = t->game->playHand();
    const GameState& again = t->p[0]->states[1];  // P0의 두 번째 결정
    CHECK(again.toCall == 50);
    CHECK(!again.canRaise);
    CHECK(historyHas(h, "P0: 콜 50"));  // 레이즈 시도는 콜로 처리된다
    // 아직 행동 안 한 BB는 레이즈할 수 있었다
    CHECK(t->p[2]->states[0].canRaise);
    CHECK(t->p[2]->states[0].minRaiseTo == 350 + 200);
}

// 풀 레이즈 크기 이상의 올인은 권리를 다시 연다
static void testFullAllInReopens() {
    auto t = makeTable(3);
    t->game->setNextButton(0);
    t->game->setChips(1, 600);  // 300 -> 600 (증가분 300 >= 200: 풀 레이즈)
    t->p[0]->script = {Action::raiseTo(300), Action::fold()};
    t->p[1]->script = {Action::allIn()};
    t->game->playHand();
    const GameState& again = t->p[0]->states[1];
    CHECK(again.canRaise);
    CHECK(again.minRaiseTo == 600 + 300);
}

// 숏 올인이 여러 번 쌓여 풀 레이즈 폭 이상이 되면 권리가 다시 열린다
static void testCumulativeShortAllIns() {
    // 4명: 버튼=0, SB=1, BB=2, UTG=3. (P0이 끝까지 남아 있어야 레이즈할 상대가 있다)
    auto t = makeTable(4);
    t->game->setNextButton(0);
    t->game->setChips(1, 350);  // +50 (숏)
    t->game->setChips(2, 520);  // +170 (누적 220 >= 200)
    t->p[3]->script = {Action::raiseTo(300), Action::fold()};
    t->p[1]->script = {Action::allIn()};
    t->p[2]->script = {Action::allIn()};
    t->game->playHand();
    CHECK(t->p[3]->states[1].canRaise);

    // 숏 올인 하나만 있었다면 닫혀 있다
    auto t2 = makeTable(4);
    t2->game->setNextButton(0);
    t2->game->setChips(1, 350);
    t2->p[3]->script = {Action::raiseTo(300), Action::fold()};
    t2->p[1]->script = {Action::allIn()};
    t2->game->playHand();
    CHECK(!t2->p[3]->states[1].canRaise);
}

// 모두 폴드: 쇼다운 없이 승리, 콜 받지 못한 베팅은 반환
static void testEveryoneFolds() {
    auto t = makeTable(6);
    t->game->setNextButton(0);  // SB=1, BB=2
    for (int s : {3, 4, 5, 0, 1}) t->p[s]->script = {Action::fold()};
    HandSummary h = t->game->playHand();
    CHECK(!h.showdown);
    CHECK(h.endChips[2] == 10050);  // BB는 SB의 50을 얻는다
    CHECK(h.endChips[1] == 9950);
    CHECK(sum(h.endChips) == 60000);
}

// 올인이 여러 명: 메인 팟 + 사이드 팟이 각각 최고 핸드에게 간다
static void testSidePotsAtShowdown() {
    // 스택: P0 1000, P1 3000, P2 5000. 버튼=2 -> SB=P0, BB=P1, UTG=P2
    const std::string board = "2c 7d 9h Js 3c";
    struct Case { std::vector<std::string> holes; std::vector<int> expected; };
    // 홀 카드는 SB(P0), P1, P2 순서
    std::vector<Case> cases = {
        // P0 최강: 메인 3000은 P0, 사이드 4000은 P1(2등), P2는 환불 2000만
        {{"As Ad", "Ks Kd", "Qs Qd"}, {3000, 4000, 2000}},
        // P2 최강: 모든 팟 + 환불
        {{"Qs Qd", "Ks Kd", "As Ad"}, {0, 0, 9000}},
        // P1 최강: 메인 + 사이드 전부 (P0는 메인에서도 짐)
        {{"Qs Qd", "As Ad", "Ks Kd"}, {0, 7000, 2000}},
    };
    for (const Case& c : cases) {
        auto t = makeTable(3);
        t->game->setChips(0, 1000);
        t->game->setChips(1, 3000);
        t->game->setChips(2, 5000);
        t->game->setNextButton(2);
        t->game->setNextDeck(stackedDeck(c.holes, board));
        t->p[2]->script = {Action::allIn()};
        t->p[0]->script = {Action::allIn()};
        t->p[1]->script = {Action::allIn()};
        HandSummary h = t->game->playHand();
        CHECK(h.showdown);
        CHECK(h.endChips == c.expected);
        CHECK(historyHas(h, "콜 받지 못한 베팅 2000 반환"));
    }
}

// 동점 분배와 홀수 칩: 버튼 왼쪽 첫 승자부터 한 칩씩
static void testSplitPotOddChip() {
    auto t = makeTable(3);
    for (int i = 0; i < 3; ++i) t->game->setChips(i, 1001);
    t->game->setNextButton(0);  // SB=P1, BB=P2, UTG=P0
    // 보드 로열 플러시(스페이드)로 P1, P2는 보드 플레이로 동점, P0는 약한 핸드지만 같이 보드 플레이... 구분 위해 P0에게 2c 3d
    t->game->setNextDeck(stackedDeck({"4h 5h", "6h 7h", "2c 3d"}, "Ts Js Qs Ks As"));
    for (int i = 0; i < 3; ++i) t->p[i]->script = {Action::allIn()};
    HandSummary h = t->game->playHand();
    // 세 명 모두 보드 로열 플러시로 동점: 3003 / 3 = 1001씩, 홀수 없음
    CHECK(h.endChips == (std::vector<int>{1001, 1001, 1001}));

    // 두 명만 동점: P0은 약한 핸드를 가지지만 보드가 로열이 아니면 패배.
    auto t2 = makeTable(3);
    for (int i = 0; i < 3; ++i) t2->game->setChips(i, 1001);
    t2->game->setNextButton(0);
    // P1,P2는 에이스 페어(보드에 맞는 같은 숫자), P0은 낮은 페어
    t2->game->setNextDeck(stackedDeck({"Ah 5d", "Ad 5h", "2c 3d"}, "Kc 8s 9h Tc 4d"));
    for (int i = 0; i < 3; ++i) t2->p[i]->script = {Action::allIn()};
    HandSummary h2 = t2->game->playHand();
    // 총 3003, P1(SB, 버튼 왼쪽 첫 승자)이 홀수 칩 1개를 더 받는다
    CHECK(h2.endChips == (std::vector<int>{0, 1502, 1501}));
}

// 탈락과 게임 종료
static void testEliminationAndGameOver() {
    auto t = makeTable(2);
    t->game->setChips(0, 100);
    t->game->setNextButton(0);  // P0이 SB(헤즈업 버튼)
    t->game->setNextDeck(stackedDeck({"Ks Kd", "As Ad"}, "2c 7d 9h Js 3c"));
    t->p[0]->script = {Action::allIn()};
    HandSummary h = t->game->playHand();
    CHECK(h.endChips == (std::vector<int>{0, 10100}));
    CHECK(t->game->activePlayers() == 1 && t->game->isOver());
    CHECK(historyHas(h, "탈락"));
}

// 탈락자는 다음 판부터 빠지고, 버튼과 블라인드는 남은 사람 사이에서 돈다
static void testEliminatedSeatSkipped() {
    auto t = makeTable(4);
    t->game->setChips(2, 0);
    t->game->setNextButton(1);
    HandSummary h = t->game->playHand();
    CHECK(!h.dealtIn[2]);
    CHECK(h.smallBlindSeat == 3 && h.bigBlindSeat == 0);  // 2번 좌석 건너뜀
    HandSummary h2 = t->game->playHand();
    CHECK(h2.button == 3);
}

// 칩이 블라인드보다 적으면 가진 만큼만 내고 올인
static void testShortBlind() {
    auto t = makeTable(3);
    t->game->setChips(2, 60);  // BB가 60밖에 없다
    t->game->setNextButton(0);  // SB=1, BB=2
    HandSummary h = t->game->playHand();
    CHECK(historyHas(h, "P2: 빅 블라인드 60 (올인)"));
    CHECK(sum(h.endChips) == 20060);
    CHECK(t->p[0]->states[0].toCall == 100);  // 다른 사람은 여전히 100을 맞춰야 한다
}

// 무작위로 행동하는 플레이어: 이상한 금액도 섞어서 규칙 보정과 칩 보존을 시험한다
class RandomPlayer : public Player {
public:
    RandomPlayer(int id, unsigned seed) : id_(id), rng_(seed) {}
    std::string name() const override { return "R" + std::to_string(id_); }
    Action decide(const GameState& s) override {
        // 상황 자체가 일관적인지도 같이 확인한다
        CHECK(s.toCall >= 0 && s.toCall <= s.myChips);
        CHECK(s.minRaiseTo > s.currentBet);
        CHECK(s.maxRaiseTo == s.myBet + s.myChips);
        CHECK(s.pot >= 0);
        CHECK(s.board.size() == 0 || s.board.size() >= 3);
        switch (rng_() % 8) {
            case 0: return Action::fold();
            case 1: return Action::check();
            case 2:
            case 3: return Action::call();
            case 4:
            case 5: return Action::raiseTo(static_cast<int>(rng_() % 3000) - 200);
            case 6: return Action::raiseTo(static_cast<int>(rng_() % 40000));
            default: return Action::allIn();
        }
    }

private:
    int id_;
    std::mt19937 rng_;
};

static void testRandomGames() {
    int hands = 0, showdowns = 0;
    for (unsigned game = 0; game < 150; ++game) {
        for (int n : {2, 3, 6}) {
            std::vector<std::unique_ptr<Player>> players;
            for (int i = 0; i < n; ++i) players.push_back(std::make_unique<RandomPlayer>(i, game * 10 + i));
            Game g(GameConfig{}, std::move(players), game);
            const int total = n * 10000;
            while (!g.isOver() && hands < 1000000) {
                HandSummary h = g.playHand();
                ++hands;
                if (h.showdown) ++showdowns;
                if (sum(h.endChips) != total) {
                    CHECK(false);
                    return;
                }
            }
        }
    }
    std::printf("무작위 시뮬레이션: %d판 진행 (쇼다운 %d판), 칩 보존 검사는 매 판 수행\n", hands, showdowns);
    CHECK(hands > 1000);
}

// 같은 시드면 같은 결과
static void testReproducible() {
    auto run = [](unsigned seed) {
        auto t = makeTable(6, seed);
        std::vector<int> result;
        for (int i = 0; i < 5; ++i) {
            HandSummary h = t->game->playHand();
            result.insert(result.end(), h.endChips.begin(), h.endChips.end());
        }
        return result;
    };
    CHECK(run(99) == run(99));
    CHECK(run(99) != run(100));
}

// ---------------------------------------------------------------------------
// 4단계: 봇과 승률 계산
// ---------------------------------------------------------------------------

static std::array<Card, 2> holeOf(const std::string& text) {
    auto v = cardsOf(text);
    return {v[0], v[1]};
}

static void testHandStrength() {
    CHECK(preflopStrength(holeOf("As Ad")) > preflopStrength(holeOf("Ks Kd")));
    CHECK(preflopStrength(holeOf("Ks Kd")) > preflopStrength(holeOf("As Ks")));
    CHECK(preflopStrength(holeOf("As Ks")) > preflopStrength(holeOf("As Kd")));
    CHECK(preflopStrength(holeOf("9s 8s")) > preflopStrength(holeOf("9s 2d")));
    CHECK(preflopStrength(holeOf("As Ad")) > 0.95);
    CHECK(preflopStrength(holeOf("7s 2d")) < 0.05);

    auto flop = cardsOf("Kc 8d 3h");
    double trips = postflopStrength(holeOf("Ks Kd"), flop);
    double topPair = postflopStrength(holeOf("Kh Qd"), flop);
    double bottomPair = postflopStrength(holeOf("3d 4d"), flop);
    double nothing = postflopStrength(holeOf("7s 2d"), flop);
    CHECK(trips > topPair && topPair > bottomPair && bottomPair > nothing);
    // 보드에 깔린 페어는 내 핸드가 아니다
    double boardPair = postflopStrength(holeOf("Qs 2d"), cardsOf("Kc Kd 3h"));
    CHECK(boardPair < topPair);
}

// 알려진 승률과 비교 (헤즈업 AA ≈ 85.2%, AKs ≈ 67%, 72o ≈ 34.6%, 6인 AA ≈ 49%)
static void testEquityKnownValues() {
    std::mt19937 rng(123);
    std::vector<Card> none;
    double aa = estimateEquity(holeOf("As Ad"), none, 1, 40000, rng);
    double aks = estimateEquity(holeOf("As Ks"), none, 1, 40000, rng);
    double bad = estimateEquity(holeOf("7s 2d"), none, 1, 40000, rng);
    double aa5 = estimateEquity(holeOf("As Ad"), none, 5, 40000, rng);
    std::printf("승률: AA %.3f, AKs %.3f, 72o %.3f, AA vs 5명 %.3f\n", aa, aks, bad, aa5);
    CHECK(std::fabs(aa - 0.852) < 0.012);
    CHECK(std::fabs(aks - 0.670) < 0.012);
    CHECK(std::fabs(bad - 0.346) < 0.012);
    CHECK(std::fabs(aa5 - 0.49) < 0.015);

    // 이미 나온 카드가 반영되는지: 리버에서 로열 플러시를 쥐었으면 100%
    double royal = estimateEquity(holeOf("As Ks"), cardsOf("Qs Js Ts 2c 3d"), 3, 500, rng);
    CHECK(royal == 1.0);
    // 상대가 없으면 100%
    CHECK(estimateEquity(holeOf("7s 2d"), none, 0, 10, rng) == 1.0);
    // 플랍 이후: 넛 플러시 드로우 + 오버카드는 상대 하나에게 꽤 높다
    double draw = estimateEquity(holeOf("As Ks"), cardsOf("Qs 7s 2d"), 1, 20000, rng);
    CHECK(draw > 0.55 && draw < 0.75);
}

// 직접 만든 상황에서 봇이 성격대로 행동하는지
static GameState makeState(const std::string& hole, const std::string& board, int pot, int toCall,
                           int currentBet) {
    GameState s;
    s.numSeats = 6;
    s.mySeat = 0;
    s.smallBlind = 50;
    s.bigBlind = 100;
    s.myHole = holeOf(hole);
    s.board = cardsOf(board);
    s.street = s.board.empty() ? Street::Preflop : (s.board.size() == 3 ? Street::Flop : s.board.size() == 4 ? Street::Turn : Street::River);
    s.players.resize(6);
    for (auto& p : s.players) { p.inHand = true; p.chips = 10000; }
    s.pot = pot;
    s.toCall = toCall;
    s.currentBet = currentBet;
    s.myChips = 10000;
    s.minRaiseTo = currentBet + std::max(100, currentBet);
    s.maxRaiseTo = 10000;
    s.canRaise = true;
    return s;
}

static void testBotPersonalities() {
    // 타이트: 쓰레기 핸드는 레이즈 앞에서 폴드, 최상위 핸드는 레이즈
    for (unsigned seed = 1; seed <= 20; ++seed) {
        TightBot tight(seed);
        CHECK(tight.decide(makeState("7s 2d", "", 250, 300, 300)).type == ActionType::Fold);
        CHECK(tight.decide(makeState("As Ad", "", 250, 300, 300)).type == ActionType::Raise);
        // 콜링 스테이션: 쓰레기 핸드로도 콜
        CallingStationBot station(seed);
        CHECK(station.decide(makeState("7s 2d", "", 250, 300, 300)).type == ActionType::Call);
        CHECK(station.decide(makeState("7s 2d", "", 0, 0, 0)).type == ActionType::Check);
    }
    // 확률 계산형: 승률이 낮으면 큰 베팅 앞에서 폴드, 높으면 콜 이상
    OddsBot odds(7, 800);
    CHECK(odds.decide(makeState("7s 2d", "Ks Qd Jc", 600, 600, 600)).type == ActionType::Fold);
    ActionType t = odds.decide(makeState("As Ad", "Ks Ad Jc", 600, 300, 300)).type;
    CHECK(t == ActionType::Call || t == ActionType::Raise || t == ActionType::AllIn);
    // 공짜로 볼 수 있을 때 폴드하지 않는다
    for (unsigned seed = 1; seed <= 20; ++seed) {
        AggroBot a(seed); LooseBot l(seed); TightBot ti(seed);
        CHECK(a.decide(makeState("7s 2d", "", 150, 0, 100)).type != ActionType::Fold);
        CHECK(l.decide(makeState("7s 2d", "", 150, 0, 100)).type != ActionType::Fold);
        CHECK(ti.decide(makeState("7s 2d", "", 150, 0, 100)).type != ActionType::Fold);
    }
}

// 성격 차이가 통계로 드러나는지: 무작위 프리플랍 핸드에 대한 반응 비율
static void testBotStatistics() {
    std::mt19937 rng(5);
    auto rate = [&](Player& bot, bool raiseOnly) {
        int hits = 0, n = 3000;
        for (int i = 0; i < n; ++i) {
            Deck d;
            d.shuffle(rng);
            std::array<Card, 2> hole = {d.draw(), d.draw()};
            GameState s = makeState(cardToString(hole[0]) + " " + cardToString(hole[1]), "", 150, 100, 100);
            Action a = bot.decide(s);
            bool raised = a.type == ActionType::Raise || a.type == ActionType::AllIn;
            bool played = a.type != ActionType::Fold;
            if (raiseOnly ? raised : played) ++hits;
        }
        return static_cast<double>(hits) / n;
    };
    TightBot tight(1); LooseBot loose(2); AggroBot aggro(3); CallingStationBot station(4);
    double tightPlay = rate(tight, false), loosePlay = rate(loose, false), stationPlay = rate(station, false);
    double aggroRaise = rate(aggro, true), stationRaise = rate(station, true), tightRaise = rate(tight, true);
    std::printf("프리플랍 참여율: 타이트 %.2f 루즈 %.2f 콜링 %.2f / 레이즈율: 공격형 %.2f 타이트 %.2f 콜링 %.2f\n",
                tightPlay, loosePlay, stationPlay, aggroRaise, tightRaise, stationRaise);
    CHECK(tightPlay < 0.4);
    CHECK(loosePlay > tightPlay + 0.2);
    CHECK(stationPlay > 0.95);
    CHECK(aggroRaise > tightRaise);
    CHECK(aggroRaise > stationRaise + 0.2);
}

// ---------------------------------------------------------------------------
// 봇의 공정성: 봇의 입력에는 남의 홀 카드가 없고, 사람의 카드가 달라도 결정이 같다
// ---------------------------------------------------------------------------
static std::string describe(const GameState& s) {
    std::ostringstream out;
    out << s.handNumber << ' ' << static_cast<int>(s.street) << ' ' << s.mySeat << ' ' << s.button << ' ' << s.pot << ' '
        << s.currentBet << ' ' << s.toCall << ' ' << s.minRaiseTo << ' ' << s.maxRaiseTo << ' ' << s.canRaise << ' '
        << s.myChips << ' ' << s.myBet << ' ' << cardToString(s.myHole[0]) << cardToString(s.myHole[1]) << ' ';
    for (const Card& c : s.board) out << cardToString(c);
    for (const PlayerView& p : s.players) {
        out << '|' << p.name << ',' << p.chips << ',' << p.bet << ',' << p.contributed << ',' << p.inHand << p.folded << p.allIn;
    }
    return out.str();
}

// 항상 콜(또는 체크)만 하는 "사람"
class CallerSpy : public Player {
public:
    std::string name() const override { return "사람"; }
    Action decide(const GameState& s) override { return s.toCall == 0 ? Action::check() : Action::call(); }
};

// 봇을 감싸서 받은 입력, 고른 행동, 생각 시간을 적어 둔다
class RecordingBot : public Player {
public:
    RecordingBot(std::unique_ptr<Player> bot, std::vector<std::string>& log, std::vector<GameState>& states)
        : bot_(std::move(bot)), log_(log), states_(states) {}
    std::string name() const override { return bot_->name(); }
    Action decide(const GameState& s) override {
        const Action a = bot_->decide(s);
        std::mt19937 rng(11);
        log_.push_back(describe(s) + " => " + std::to_string(static_cast<int>(a.type)) + ":" + std::to_string(a.amount) +
                       " t" + std::to_string(thinkTimeMs(s, a, 1.0, rng)));
        states_.push_back(s);
        return a;
    }
    void observe(const PublicAction& a) override { bot_->observe(a); }

private:
    std::unique_ptr<Player> bot_;
    std::vector<std::string>& log_;
    std::vector<GameState>& states_;
};

static void testBotsIgnoreOthersCards() {
    int compared = 0;
    for (unsigned seed = 1; seed <= 30; ++seed) {
        std::mt19937 shuffle(seed);
        std::vector<Card> deck;
        {
            Deck d;
            d.shuffle(shuffle);
            for (int i = 0; i < 52; ++i) deck.push_back(d.draw());
        }
        // 6명, 버튼 0 -> 홀 카드는 1번 좌석부터 돌아가므로 0번(사람)은 여섯 번째와 열두 번째 장을 받는다
        std::vector<Card> other = deck;
        std::swap(other[5], other[40]);
        std::swap(other[11], other[41]);

        std::vector<std::string> logs[2];
        std::vector<GameState> states[2];
        for (int run = 0; run < 2; ++run) {
            std::vector<std::unique_ptr<Player>> players;
            players.push_back(std::make_unique<CallerSpy>());
            for (auto& bot : makeAllBots(seed)) {
                dynamic_cast<Bot&>(*bot).setHumanSeats(1);
                players.push_back(std::make_unique<RecordingBot>(std::move(bot), logs[run], states[run]));
            }
            Game game(GameConfig{}, std::move(players), seed);
            game.setNextButton(0);
            game.setNextDeck(run == 0 ? deck : other);
            const HandSummary hand = game.playHand();
            CHECK(hand.holes[0][0] == (run == 0 ? deck : other)[5] && hand.holes[0][1] == (run == 0 ? deck : other)[11]);
        }
        // 사람의 카드만 달라졌는데, 봇이 본 것과 고른 것, 생각 시간이 모두 같다
        CHECK(!logs[0].empty() && logs[0] == logs[1]);
        compared += static_cast<int>(logs[0].size());

        // 결정의 확률 분포도 같다 (같은 입력을 여러 시드의 봇에게 물어본다)
        if (seed <= 3 && !states[0].empty()) {
            for (std::size_t at = 0; at < states[0].size(); at += 4) {
                std::map<int, int> a, b;
                for (unsigned k = 1; k <= 100; ++k) {
                    AggroBot first(k), second(k);
                    ++a[static_cast<int>(first.decide(states[0][at]).type)];
                    ++b[static_cast<int>(second.decide(states[1][at]).type)];
                }
                CHECK(a == b);
            }
        }
    }
    CHECK(compared > 200);
}

// 성격, 틸트, 난이도, 크기의 다양성, 강한 패는 접지 않기, 생각 시간
static void testBotHumanLikeness() {
    TightBot tight(1);
    AggroBot aggro(1);
    CallingStationBot station(1);
    LooseBot loose(1);
    CHECK(tight.profile().vpip < loose.profile().vpip && loose.profile().vpip < station.profile().vpip);
    CHECK(aggro.profile().pfr > tight.profile().pfr && aggro.profile().bluff > tight.profile().bluff);
    CHECK(station.profile().foldPressure < 0.1);

    // 틸트: 큰 팟을 잃으면 3판 동안. 설명 문구("신중함", "세게 베팅", "잘 안 접음")와 같은 방향
    const BotProfile t0 = tight.profile(), a0 = aggro.profile(), s0 = station.profile();
    tight.handEnded(-2500, 100);
    aggro.handEnded(-2500, 100);
    station.handEnded(-2500, 100);
    CHECK(tight.tilted() && tight.profile().aggression < t0.aggression && tight.profile().vpip < t0.vpip);
    CHECK(aggro.profile().aggression > a0.aggression && aggro.profile().bluff > a0.bluff);
    CHECK(station.profile().aggression == s0.aggression && station.profile().foldPressure == s0.foldPressure);
    for (int i = 0; i < 3; ++i) tight.handEnded(0, 100);
    CHECK(!tight.tilted() && tight.profile().vpip == t0.vpip);
    tight.handEnded(-500, 100);  // 작은 손실에는 변하지 않는다
    CHECK(!tight.tilted());

    // 난이도는 실수와 블러프만 바꾼다
    LooseBot easy(1), hard(1);
    easy.setDifficulty(Difficulty::Easy);
    hard.setDifficulty(Difficulty::Hard);
    CHECK(easy.profile().bluff < hard.profile().bluff && easy.profile().vpip == hard.profile().vpip &&
          easy.profile().aggression == hard.profile().aggression);

    // 사람에 대한 기억은 작게만
    AggroBot watcher(1);
    watcher.setHumanSeats(1);
    for (int i = 0; i < 50; ++i) watcher.observe(PublicAction{0, ActionType::Raise, Street::Preflop});
    CHECK(a0.foldPressure - watcher.profile().foldPressure <= 0.0501 && watcher.profile().foldPressure < a0.foldPressure);

    // 같은 상황에서도 크기가 여러 가지: 프리플랍 오픈은 2.2~3.5BB
    std::set<int> opens, bets;
    GameState pre = makeState("As Ad", "", 150, 100, 100);
    pre.currentBet = 100;
    pre.minRaiseTo = 200;
    GameState flop = makeState("As Ad", "Ac 7d 2h", 600, 0, 0);
    flop.minRaiseTo = 100;
    for (unsigned seed = 1; seed <= 300; ++seed) {
        AggroBot bot(seed);
        const Action open = bot.decide(pre);
        if (open.type == ActionType::Raise) opens.insert(open.amount);
        const Action next = bot.decide(flop);
        if (next.type == ActionType::Raise) bets.insert(next.amount);
    }
    CHECK(opens.size() > 20 && *opens.begin() >= 220 && *opens.rbegin() <= 350);
    CHECK(bets.size() > 20 && *bets.begin() >= 150 && *bets.rbegin() > 600);  // 팟의 1/3부터 오버벳까지

    // 아주 강한 패는 어떤 난이도에서도 접지 않는다
    const GameState strong = makeState("As Ad", "Ac 7d 2h", 1200, 800, 800);
    int folds = 0;
    for (unsigned seed = 1; seed <= 300; ++seed) {
        for (auto& bot : makeAllBots(seed)) {
            dynamic_cast<Bot&>(*bot).setDifficulty(Difficulty::Easy);
            if (bot->decide(strong).type == ActionType::Fold) ++folds;
        }
    }
    CHECK(folds == 0);

    // 생각 시간: 상황별 범위(±20%)이고, 성격 속도를 곱하며, 카드와는 무관하다
    std::mt19937 rng(3);
    GameState call = makeState("7s 2d", "", 300, 100, 100);
    GameState big = makeState("7s 2d", "Ks Qd Jc", 600, 900, 900);
    for (int i = 0; i < 500; ++i) {
        const int fold = thinkTimeMs(call, Action::fold(), 1.0, rng);
        const int normal = thinkTimeMs(call, Action::call(), 1.0, rng);
        const int heavy = thinkTimeMs(big, Action::call(), 1.0, rng);
        CHECK(fold >= 800 && fold <= 2400);
        CHECK(normal >= 1200 && normal <= 4200);
        CHECK(heavy >= 2800 && heavy <= 8400);
    }
    GameState aces = call;
    aces.myHole = strong.myHole;
    for (unsigned seed = 1; seed <= 50; ++seed) {
        std::mt19937 x(seed), y(seed);
        CHECK(thinkTimeMs(call, Action::call(), 1.0, x) == thinkTimeMs(aces, Action::call(), 1.0, y));
    }
    CHECK(aggro.profile().thinkSpeed < tight.profile().thinkSpeed);
    OddsBot odds(1);
    CHECK(odds.profile().thinkSpeed > loose.profile().thinkSpeed);
}

// 봇 5종 + 타이트 1명이 6인 테이블에서 대량으로 플레이: 칩 보존, 크래시 없음
static void testBotsPlayManyHands() {
    int hands = 0;
    for (unsigned seed = 1; seed <= 12; ++seed) {
        auto bots = makeAllBots(seed);
        bots.push_back(std::make_unique<TightBot>(seed + 100));
        Game g(GameConfig{}, std::move(bots), seed);
        for (int i = 0; i < 200 && !g.isOver(); ++i) {
            HandSummary h = g.playHand();
            ++hands;
            CHECK(sum(h.endChips) == 60000);
        }
    }
    CHECK(hands > 1000);
}

// ---------------------------------------------------------------------------
// 5단계: 사람 플레이어 입력 처리
// ---------------------------------------------------------------------------

// 프리플랍 UTG 상황: 콜 100, 최소 레이즈 200, 최대 10000
static GameState humanState(bool canRaise = true) {
    GameState s = makeState("As Kd", "", 150, 100, 100);
    s.canRaise = canRaise;
    s.minRaiseTo = 200;
    return s;
}

static void testHumanParse() {
    GameState s = humanState();
    Action a;
    std::string err;
    CHECK(parseHumanCommand("f", s, a, err) && a.type == ActionType::Fold);
    CHECK(parseHumanCommand("  FOLD ", s, a, err) && a.type == ActionType::Fold);
    CHECK(parseHumanCommand("c", s, a, err) && a.type == ActionType::Call);
    CHECK(parseHumanCommand("r 300", s, a, err) && a.type == ActionType::Raise && a.amount == 300);
    CHECK(parseHumanCommand("raise 200", s, a, err) && a.amount == 200);  // 정확히 최소
    CHECK(parseHumanCommand("a", s, a, err) && a.type == ActionType::AllIn);
    CHECK(parseHumanCommand("r 10000", s, a, err) && a.type == ActionType::AllIn);  // 전 재산 = 올인
    // pot: 콜 후 팟(150+100=250)만큼 더 -> 100 + 250 = 350
    CHECK(parseHumanCommand("r pot", s, a, err) && a.type == ActionType::Raise && a.amount == 350);
    CHECK(parseHumanCommand("r half", s, a, err) && a.amount == 225);

    // 잘못된 입력은 모두 거부하고 안내 문구를 남긴다
    const char* bad[] = {"", "   ", "xyz", "r", "r abc", "r 12abc", "r -300", "r 3.5", "r 199",
                         "r 10001", "r 99999999999999", "raise", "ㅁㅁ", "call 5 6 extra?"};
    for (const char* line : bad) {
        err.clear();
        bool ok = parseHumanCommand(line, s, a, err);
        // "call 5 6 extra?"는 첫 단어가 call이라 통과한다 (뒤 내용은 무시)
        if (std::string(line) == "call 5 6 extra?") continue;
        CHECK(!ok);
        CHECK(!err.empty());
    }

    // 체크할 수 있을 때 폴드는 거부, 레이즈 권리가 없으면 레이즈/올인 거부
    GameState free = makeState("As Kd", "Qc 7d 2h", 300, 0, 0);
    CHECK(!parseHumanCommand("f", free, a, err));
    CHECK(parseHumanCommand("c", free, a, err) && a.type == ActionType::Check);
    GameState closed = humanState(false);
    CHECK(!parseHumanCommand("r 400", closed, a, err));
    CHECK(!parseHumanCommand("a", closed, a, err));
    CHECK(parseHumanCommand("c", closed, a, err) && a.type == ActionType::Call);
}

static void testHumanPlayerReprompts() {
    // 잘못된 입력 세 번 뒤에 올바른 입력: 계속 다시 묻고 마지막 입력만 채택한다
    std::istringstream in("hello\nr abc\nr 50\nr 400\n");
    std::ostringstream out;
    HumanPlayer human("나", HumanOptions{false, 100}, in, out, 1);
    Action a = human.decide(humanState());
    CHECK(a.type == ActionType::Raise && a.amount == 400);
    std::string text = out.str();
    int errors = 0;
    for (std::size_t pos = 0; (pos = text.find("  ! ", pos)) != std::string::npos; ++pos) ++errors;
    CHECK(errors == 3);

    // 입력이 끝나면(EOF) 종료 예외
    std::istringstream empty("");
    std::ostringstream out2;
    HumanPlayer h2("나", HumanOptions{false, 100}, empty, out2, 1);
    bool quit = false;
    try { h2.decide(humanState()); } catch (const QuitRequested&) { quit = true; }
    CHECK(quit);

    // q 입력도 종료
    std::istringstream q("q\n");
    std::ostringstream out3;
    HumanPlayer h3("나", HumanOptions{false, 100}, q, out3, 1);
    quit = false;
    try { h3.decide(humanState()); } catch (const QuitRequested&) { quit = true; }
    CHECK(quit);
}

// 사람 + 봇 5명이 실제 게임에서 입력 대본대로 끝까지 진행되는지
static void testHumanInGame() {
    // 콜, 폴드, 잘못된 입력(zz)을 번갈아 넣는다.
    std::string script;
    for (int i = 0; i < 3000; ++i) script += (i % 3 == 0) ? "c\n" : (i % 3 == 1 ? "f\n" : "zz\n");
    std::istringstream in(script);
    std::ostringstream out;
    std::vector<std::unique_ptr<Player>> players;
    players.push_back(std::make_unique<HumanPlayer>("나", HumanOptions{false, 100}, in, out, 1));
    for (auto& b : makeAllBots(3)) players.push_back(std::move(b));
    Game g(GameConfig{}, std::move(players), 3);
    int hands = 0;
    try {
        while (!g.isOver() && g.chips(0) > 0 && hands < 30) {
            HandSummary h = g.playHand();
            CHECK(sum(h.endChips) == 60000);
            ++hands;
        }
    } catch (const QuitRequested&) {
    }
    CHECK(hands > 5);
    CHECK(out.str().find("내 카드") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 6단계: 힌트 (팟 오즈, 승률 표시, --no-hint)
// ---------------------------------------------------------------------------
static void testHints() {
    // 팟 300(이미 베팅 포함), 콜 100 -> 100 / 400 = 25%
    GameState s = makeState("As Ad", "", 300, 100, 100);
    CHECK(std::fabs(potOdds(s) - 0.25) < 1e-9);
    CHECK(potOdds(makeState("As Ad", "", 300, 0, 0)) == 0.0);
    // 팟과 같은 크기를 콜 -> 50%... 팟 200, 콜 200 -> 200/400
    CHECK(std::fabs(potOdds(makeState("As Ad", "", 200, 200, 200)) - 0.5) < 1e-9);

    // 힌트 켬: 포지션, 팟, 콜 비용, 팟 오즈, 승률이 출력된다
    {
        std::istringstream in("c\n");
        std::ostringstream out;
        HumanPlayer h("나", HumanOptions{true, 2000}, in, out, 1);
        GameState st = makeState("As Ad", "", 300, 100, 100);
        st.button = 0;
        st.smallBlindSeat = 1;
        st.bigBlindSeat = 2;
        h.decide(st);
        std::string text = out.str();
        CHECK(text.find("[힌트]") != std::string::npos);
        CHECK(text.find("포지션 BTN") != std::string::npos);
        CHECK(text.find("팟 오즈 25.0%") != std::string::npos);
        CHECK(text.find("현재 승률") != std::string::npos);
        CHECK(text.find("콜 비용 100") != std::string::npos);
        // AA는 5명 상대로도 팟 오즈 25%를 크게 넘으므로 콜이 이득이라고 알려 준다
        CHECK(text.find("이득") != std::string::npos);
    }
    // 힌트 끔: 힌트 문구가 전혀 나오지 않는다
    {
        std::istringstream in("c\n");
        std::ostringstream out;
        HumanPlayer h("나", HumanOptions{false, 2000}, in, out, 1);
        h.decide(makeState("As Ad", "", 300, 100, 100));
        CHECK(out.str().find("[힌트]") == std::string::npos);
        CHECK(out.str().find("승률") == std::string::npos);
    }
    // 승률이 낮은 핸드로 큰 베팅을 마주하면 손해라고 알려 준다
    {
        std::istringstream in("f\n");
        std::ostringstream out;
        HumanPlayer h("나", HumanOptions{true, 2000}, in, out, 1);
        h.decide(makeState("7s 2d", "Ks Qd Jc", 1000, 1000, 1000));
        CHECK(out.str().find("손해") != std::string::npos);
    }
}

int main() {
    testButtonRotation();
    testActionOrderSixHanded();
    testHeadsUp();
    testPositionNames();
    testBlindsAndInitialState();
    testDealOrderAndBurn();
    testMinRaise();
    testMinRaiseGrows();
    testPostflopMinBet();
    testShortAllInDoesNotReopen();
    testFullAllInReopens();
    testCumulativeShortAllIns();
    testEveryoneFolds();
    testSidePotsAtShowdown();
    testSplitPotOddChip();
    testEliminationAndGameOver();
    testEliminatedSeatSkipped();
    testShortBlind();
    testRandomGames();
    testReproducible();
    testHandStrength();
    testEquityKnownValues();
    testBotPersonalities();
    testBotStatistics();
    testBotsIgnoreOthersCards();
    testBotHumanLikeness();
    testBotsPlayManyHands();
    testHumanParse();
    testHumanPlayerReprompts();
    testHumanInGame();
    testHints();
    std::printf("%d개 검사 중 %d개 실패\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
