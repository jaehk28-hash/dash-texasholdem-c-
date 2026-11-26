// test_review.cpp - 핸드 리뷰 생성 테스트 (승부 이유, 사이드 팟, 금액 일치, 카드 비공개)
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bots.h"
#include "check.h"
#include "game.h"
#include "review.h"
#include "web_server.h"
#include "web_session.h"

namespace {

// 정해진 행동을 순서대로 하고, 대본이 끝나면 콜(낼 칩이 없으면 체크)한다.
class Scripted : public Player {
public:
    Scripted(std::string name, std::vector<Action> script) : name_(std::move(name)), script_(std::move(script)) {}
    std::string name() const override { return name_; }
    Action decide(const GameState&) override {
        return next_ < script_.size() ? script_[next_++] : Action::call();
    }

private:
    std::string name_;
    std::vector<Action> script_;
    std::size_t next_ = 0;
};

Card card(const std::string& text) {
    Card c;
    CHECK(parseCard(text, c));
    return c;
}

struct Setup {
    std::vector<std::pair<std::string, std::string>> holes;  // 좌석 순
    std::string board;                                       // "As Kd 7h 4c 2s"
    std::vector<std::vector<Action>> scripts;
    std::vector<int> chips;  // 비우면 모두 1000
};

struct Result {
    HandSummary hand;
    HandReview review;
};

// 버튼은 좌석 0. 원하는 카드가 나오도록 덱을 쌓고 한 판을 진행한다.
Result play(const Setup& setup) {
    const int n = static_cast<int>(setup.holes.size());
    std::vector<std::unique_ptr<Player>> players;
    std::vector<std::string> names;
    for (int i = 0; i < n; ++i) {
        names.push_back("P" + std::to_string(i));
        players.push_back(std::make_unique<Scripted>(
            names.back(), i < static_cast<int>(setup.scripts.size()) ? setup.scripts[i] : std::vector<Action>{}));
    }
    Game game(GameConfig{1000, 5, 10}, std::move(players), 1);
    for (std::size_t i = 0; i < setup.chips.size(); ++i) game.setChips(static_cast<int>(i), setup.chips[i]);
    game.setNextButton(0);

    // 딜 순서: SB부터 한 장씩 두 바퀴, 그다음 번 카드를 섞어 플랍/턴/리버
    const int sb = n == 2 ? 0 : 1;
    std::vector<std::string> order;
    for (int round = 0; round < 2; ++round) {
        for (int i = 0; i < n; ++i) {
            const auto& hole = setup.holes[(sb + i) % n];
            order.push_back(round == 0 ? hole.first : hole.second);
        }
    }
    std::vector<std::string> board;
    for (std::size_t i = 0; i + 1 < setup.board.size(); i += 3) board.push_back(setup.board.substr(i, 2));
    // 번 카드는 어디에도 쓰지 않은 카드로
    std::vector<std::string> burns;
    for (const char* c : {"2d", "3d", "4d", "5d", "6d", "7d", "8d", "9d"}) {
        bool used = setup.board.find(c) != std::string::npos;
        for (const auto& h : setup.holes) used = used || h.first == c || h.second == c;
        if (!used && burns.size() < 3) burns.push_back(c);
    }
    order.insert(order.end(), {burns[0], board[0], board[1], board[2], burns[1], board[3], burns[2], board[4]});
    std::vector<Card> deck;
    for (const std::string& c : order) deck.push_back(card(c));
    game.setNextDeck(deck);

    Result result;
    result.hand = game.playHand();
    result.review = buildHandReview(result.hand, names);
    return result;
}

// 리뷰의 팟 금액이 실제 칩 변화와 일치해야 한다.
void checkMoney(const Result& r) {
    std::vector<int> won(r.hand.dealtIn.size(), 0);
    for (const ReviewPot& pot : r.review.pots) {
        int sum = 0;
        for (const ReviewWinner& w : pot.winners) {
            won[w.seat] += w.amount;
            sum += w.amount;
        }
        CHECK(sum == pot.amount);
    }
    for (std::size_t i = 0; i < won.size(); ++i) {
        CHECK(r.hand.endChips[i] == r.hand.startChips[i] - r.hand.contributed[i] + won[i]);
    }
}

void testByCategory() {
    const Result r = play({{{"9h", "8h"}, {"Ks", "Qs"}}, "7c 6d 5s Kd Qh", {}, {}});
    checkMoney(r);
    CHECK(r.review.showdown && r.review.entries.size() == 2);
    CHECK(r.review.entries[0].seat == 0 && r.review.entries[0].name == "스트레이트");
    CHECK(r.review.entries[1].name == "투 페어");
    CHECK(r.review.pots.size() == 1 && r.review.pots[0].compare);
    CHECK(r.review.pots[0].compare->type == HandCompare::Type::Category);
    CHECK(r.review.pots[0].winners[0].seat == 0 && r.review.pots[0].loserSeat == 1);
}

void testByHighRank() {
    const Result r = play({{{"Ks", "3c"}, {"Qs", "3h"}}, "Kd Qd 7h 7c 2s", {}, {}});
    checkMoney(r);
    const HandCompare c = *r.review.pots[0].compare;
    CHECK(c.type == HandCompare::Type::Rank && c.role == "highPair");
    CHECK(c.winRank == 13 && c.loseRank == 12 && c.shared.empty());
}

void testByKicker() {
    const Result r = play({{{"As", "3c"}, {"Qs", "3h"}}, "Kd Ks 6h 6c 2s", {}, {}});
    checkMoney(r);
    const HandCompare c = *r.review.pots[0].compare;
    CHECK(c.type == HandCompare::Type::Rank && c.role == "kicker");
    CHECK(c.winRank == 14 && c.loseRank == 12);
    CHECK(c.shared.size() == 2 && c.shared[0] == 13 && c.shared[1] == 6);
    CHECK(r.review.entries[0].key[4]);  // 승부를 가른 A도 강조
}

void testSplitWithOddChip() {
    // SB(좌석 1)가 5를 내고 폴드 -> 팟 25를 두 사람이 13, 12로 나눈다.
    const Result r = play({{{"2c", "3h"}, {"7c", "8c"}, {"2h", "3s"}}, "As Ks Qh Jd Tc",
                           {{}, {Action::fold()}, {}}, {}});
    checkMoney(r);
    CHECK(r.review.pots.size() == 1);
    const ReviewPot& pot = r.review.pots[0];
    CHECK(pot.amount == 25 && pot.winners.size() == 2 && pot.oddChip);
    CHECK(pot.compare && pot.compare->type == HandCompare::Type::Tie && pot.compare->boardPlays);
    CHECK(pot.loserSeat == -1);
    CHECK(r.review.entries[0].place == 1 && r.review.entries[1].place == 1);
    CHECK(r.review.folded.size() == 1 && r.review.folded[0] == 1);
}

void testTwoSidePots() {
    // 칩 100, 300, 600, 1000이 모두 올인 -> 메인 팟 + 사이드 팟 2개
    const Result r = play({{{"As", "Ac"}, {"Ks", "Kc"}, {"Qs", "Qc"}, {"Js", "Jc"}}, "2h 7s 9c 4h 3s",
                           {{Action::allIn()}, {Action::allIn()}, {Action::allIn()}, {Action::allIn()}},
                           {100, 300, 600, 1000}});
    checkMoney(r);
    CHECK(r.review.pots.size() == 3);
    CHECK(r.review.pots[0].amount == 400 && r.review.pots[0].winners[0].seat == 0);
    CHECK(r.review.pots[1].amount == 600 && r.review.pots[1].winners[0].seat == 1);
    CHECK(r.review.pots[2].amount == 600 && r.review.pots[2].winners[0].seat == 2);
    CHECK(r.review.pots[0].cappedBy.empty());
    CHECK(r.review.pots[1].cappedBy == std::vector<int>{0});
    CHECK(r.review.pots[2].cappedBy == std::vector<int>{1});
}

void testFoldNoShowdown() {
    const Result r = play({{{"As", "Ac"}, {"Ks", "Kc"}, {"Qs", "Qh"}}, "2h 7s 9c 4h 3s",
                           {{Action::raiseTo(30)}, {Action::fold()}, {Action::fold()}}, {}});
    checkMoney(r);
    CHECK(!r.review.showdown && r.review.entries.empty());
    CHECK(r.review.folded.size() == 2);
    CHECK(r.review.pots.size() == 1 && !r.review.pots[0].compare);
    CHECK(r.review.pots[0].winners[0].seat == 0);
    // 아무도 카드를 공개하지 않았으므로, 홀 카드가 어디에도 없어야 한다.
    const std::string json = reviewToJson(r.review, nullptr);
    for (const char* c : {"\"As\"", "\"Ac\"", "\"Ks\"", "\"Kc\"", "\"Qs\"", "\"Qh\""}) {
        CHECK(json.find(c) == std::string::npos);
    }
    CHECK(!buildReviewFlow(r.review, 0));
}

void testFoldedCardsHiddenAtShowdown() {
    const Result r = play({{{"9h", "8h"}, {"Ac", "Ad"}, {"Ks", "Qs"}}, "7c 6d 5s Kd Qh",
                           {{}, {Action::fold()}, {}}, {}});
    CHECK(r.review.showdown);
    const std::string json = reviewToJson(r.review, nullptr);
    CHECK(json.find("\"Ac\"") == std::string::npos && json.find("\"Ad\"") == std::string::npos);
    CHECK(json.find("\"9h\"") != std::string::npos);
}

void testFlowReversal() {
    // 좌석 0은 턴까지 앞서다가 리버에서 역전당한다.
    const Result r = play({{{"As", "Ac"}, {"Ks", "Kc"}}, "2h 7s 9c 4h Kd", {}, {}});
    const auto flow = buildReviewFlow(r.review, 0);
    CHECK(flow && !flow->won && flow->streets.size() == 3);
    if (!flow) return;
    CHECK(flow->streets[0].status == "ahead" && flow->streets[1].status == "ahead");
    CHECK(flow->streets[2].status == "behind");
}

void testNamesAndBestFive() {
    const std::array<Card, 2> hole = {card("As"), card("2c")};
    const std::vector<Card> board = {card("3d"), card("4h"), card("5s"), card("Kd"), card("Qh")};
    const auto five = bestFive(hole, board);
    CHECK(five[0].rank == 5 && five[4].rank == 14);  // A-2-3-4-5에서 A는 맨 뒤
    const std::vector<Card> royal = {card("Ts"), card("Js"), card("Qs"), card("Ks"), card("As")};
    CHECK(handName(evaluate5(royal.data())) == "로열 플러시");
}

void testRankingAndStandings() {
    // 좌석 1이 폴드, 좌석 0이 스트레이트로 좌석 2를 이긴다.
    const Result r = play({{{"9h", "8h"}, {"Ac", "Ad"}, {"Ks", "Qs"}}, "7c 6d 5s Kd Qh",
                           {{}, {Action::fold()}, {}}, {}});
    const auto& rank = r.review.ranking;
    CHECK(rank.size() == 3);
    CHECK(rank[0].seat == 0 && rank[0].place == 1 && !rank[0].tie && rank[0].delta > 0);
    CHECK(rank[0].handName == "스트레이트 (9까지)" && rank[0].cards);
    CHECK(rank[1].seat == 2 && rank[1].place == 2 && rank[1].handName == "투 페어 K·Q" && rank[1].delta < 0);
    CHECK(rank[2].seat == 1 && rank[2].folded && rank[2].place == 0 && !rank[2].cards && rank[2].handName.empty());
    // 폴드한 사람의 카드는 순위표를 포함한 어디에도 없다.
    const std::string json = reviewToJson(r.review, nullptr);
    CHECK(json.find("\"Ac\"") == std::string::npos && json.find("\"Ad\"") == std::string::npos);
    CHECK(r.review.standings.size() == 3 && r.review.standings[0].seat == 0);
    CHECK(!r.review.standings[0].change);  // 첫 판은 비교할 이전 순위가 없다
}

void testTieRanking() {
    const Result r = play({{{"Ac", "7h"}, {"Ad", "7s"}}, "As Ks 9h 4d 2c", {}, {}});
    CHECK(r.review.ranking.size() == 2);
    CHECK(r.review.ranking[0].place == 1 && r.review.ranking[1].place == 1);
    CHECK(r.review.ranking[0].tie && r.review.ranking[1].tie);
    CHECK(r.review.standings[0].rank == 1 && r.review.standings[1].rank == 1 && r.review.standings[0].tie);
}

void testSidePotRankingAndElimination() {
    const Result r = play({{{"As", "Ac"}, {"Ks", "Kc"}, {"Qs", "Qc"}, {"Js", "Jc"}}, "2h 7s 9c 4h 3s",
                           {{Action::allIn()}, {Action::allIn()}, {Action::allIn()}, {Action::allIn()}},
                           {100, 300, 600, 1000}});
    CHECK(r.review.ranking.size() == 4);
    for (int i = 0; i < 4; ++i) CHECK(r.review.ranking[i].seat == i && r.review.ranking[i].place == i + 1);
    // 메인 팟 400, 사이드 팟 600, 600. 좌석 3은 돌려받은 400만 남는다.
    CHECK(r.review.ranking[0].delta == 300 && r.review.ranking[1].delta == 300);
    CHECK(r.review.ranking[2].delta == 0 && r.review.ranking[3].delta == -600);
    const auto& s = r.review.standings;  // 600, 600, 400, 400
    CHECK(s[0].chips == 600 && s[0].rank == 1 && s[0].tie && s[2].rank == 3);
}

void testStandingsChange() {
    // 이전 순위: 좌석0=1위, 좌석1=2위, 좌석2=3위, 좌석3=4위
    const auto rows = buildStandings({0, 500, 900, 0}, {1, 2, 3, 4});
    CHECK(rows[0].seat == 2 && rows[0].rank == 1 && rows[0].change == 2);
    CHECK(rows[1].seat == 1 && rows[1].rank == 2 && rows[1].change == 0);
    CHECK(rows[2].seat == 0 && rows[2].out && rows[2].rank == 3 && rows[2].change == -2);
    CHECK(rows[3].seat == 3 && rows[3].out && rows[3].tie && rows[3].change == 1);
}

// 단계마다 멈추고, 방장이 눌러야 넘어간다. 다음 판도 눌러야 시작한다.
void testGates() {
    WebOptions options;
    options.seed = 5u;
    options.hintTrials = 20;
    options.aiAction = std::chrono::milliseconds(0);
    Room room(options, [] { RoomSettings s; s.bots = 0; s.turnSeconds = 60; return s; }(), "000000", false, 5);
    std::string error;
    CHECK(room.join("a", "철수", error) && room.join("b", "영희", error) && room.start("a", error));
    auto until = [&](auto done) {
        for (int i = 0; i < 2500; ++i) {
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    // 프리플랍: 둘 다 콜/체크
    auto playRound = [&] {
        until([&] {
            for (const char* who : {"a", "b"}) {
                const RoomView v = *room.view(who);
                if (v.myTurn) room.submit(who, v.table.state.toCall > 0 ? "call" : "check", {}, v.turnId, error);
            }
            return room.view("a")->waiting != Waiting::None;
        });
    };
    const Waiting expected[] = {Waiting::Flop, Waiting::Turn, Waiting::River, Waiting::Showdown};
    const std::size_t boards[] = {0, 3, 4, 5};
    for (int i = 0; i < 4; ++i) {
        playRound();
        const RoomView v = *room.view("a");
        CHECK(v.waiting == expected[i] && v.canContinue && !room.view("b")->canContinue);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(room.view("a")->table.state.board.size() == boards[i]);  // 누르기 전에는 그대로
        CHECK(!room.proceed("b", v.gateId, error));       // 방장이 아님
        CHECK(!room.proceed("a", v.gateId + 1, error));   // 다른 지점 번호
        CHECK(room.proceed("a", v.gateId, error));
        CHECK(!room.proceed("a", v.gateId, error));       // 두 번 눌러도 한 단계만
    }
    CHECK(until([&] { return room.view("a")->waiting == Waiting::NextHand; }));
    const RoomView end = *room.view("a");
    CHECK(end.handOver && end.review && end.review->ranking.size() == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(room.view("a")->table.state.handNumber == 1);
    CHECK(room.proceed("a", end.gateId, error));
    CHECK(until([&] { return room.view("a")->table.state.handNumber == 2; }));
}

// 내가 폴드한 뒤에만 건너뛸 수 있고, 건너뛰면 단계는 자동이지만 다음 판은 기다린다.
void testSkip() {
    WebOptions options;
    options.seed = 11u;
    options.hintTrials = 20;
    options.aiAction = std::chrono::milliseconds(0);
    Room room(options, [] { RoomSettings s; s.bots = 3; return s; }(), "000000", true, 11);
    std::string error;
    CHECK(room.join("a", "나", error) && room.start("a", error));
    bool skipped = false, reachedEnd = false;
    for (int i = 0; i < 3000 && !reachedEnd; ++i) {
        const RoomView v = *room.view("a");
        if (v.myTurn) {
            CHECK(!v.canSkip && !room.skipHand("a", error));
            room.submit("a", v.table.state.toCall > 0 ? "fold" : "check", {}, v.turnId, error);
        } else if (v.canSkip && !skipped) {
            skipped = room.skipHand("a", error);
        } else if (v.waiting == Waiting::NextHand) {
            reachedEnd = true;
        } else if (v.waiting != Waiting::None && !skipped) {
            room.proceed("a", v.gateId, error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(reachedEnd);
    CHECK(room.view("a")->waiting == Waiting::NextHand);
}

// 진행 속도: 봇 행동은 "생각 중" -> "공개" 순서로 하나씩, 정해진 시간만큼 걸린다.
void testPacing() {
    WebOptions options;
    options.seed = 7u;
    options.hintTrials = 20;
    options.aiAction = std::chrono::milliseconds(200);
    Room room(options, [] { RoomSettings s; s.bots = 3; return s; }(), "000000", true, 7);
    std::string error;
    CHECK(room.join("a", "나", error) && room.start("a", error));
    int seq = 0, thinking = 0;
    std::vector<std::chrono::steady_clock::time_point> revealed;
    for (int i = 0; i < 1500 && revealed.size() < 3; ++i) {
        const RoomView v = *room.view("a");
        if (v.actionPhase == "thinking") ++thinking;
        if (v.lastAction && v.lastAction->seq != seq && v.lastAction->seat != v.table.state.mySeat) {
            seq = v.lastAction->seq;
            CHECK(v.actionPhase == "revealed" && v.actingSeat == v.lastAction->seat);
            revealed.push_back(std::chrono::steady_clock::now());
        }
        if (v.myTurn) room.submit("a", v.table.state.toCall > 0 ? "call" : "check", {}, v.turnId, error);
        if (v.waiting != Waiting::None) room.proceed("a", v.gateId, error);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(thinking > 0 && revealed.size() == 3);
    // 연속한 봇 행동 사이에는 최소한 한 사람 몫의 시간이 있다 (조회 간격만큼의 오차 허용)
    for (std::size_t i = 1; i < revealed.size(); ++i) CHECK(revealed[i] - revealed[i - 1] >= std::chrono::milliseconds(180));
    Pace pace;
    CHECK(parsePace("fast", pace) && pace == Pace::Fast && !parsePace("turbo", pace));
    CHECK(room.setPace("a", Pace::Fast, error) && room.view("a")->pace == Pace::Fast);
    CHECK(!room.setPace("x", Pace::Slow, error));
}

// 봇끼리 많은 판을 두어도 리뷰 금액과 족보가 항상 게임 결과와 맞아야 한다.
void testRandomHands() {
    for (unsigned seed = 1; seed <= 40; ++seed) {
        std::vector<std::unique_ptr<Player>> players = makeAllBots(seed);
        std::vector<std::string> names;
        for (const auto& p : players) names.push_back(p->name());
        Game game(GameConfig{1500, 50, 100}, std::move(players), seed);
        for (int h = 0; h < 40 && !game.isOver(); ++h) {
            Result r;
            r.hand = game.playHand();
            r.review = buildHandReview(r.hand, names);
            checkMoney(r);
            for (const ReviewEntry& e : r.review.entries) {
                std::vector<Card> all = r.hand.board;
                all.insert(all.end(), e.hole.begin(), e.hole.end());
                CHECK(e.score == evaluateBest(all));
            }
        }
    }
}

// 방 안에서도: 첫 판이 끝나기 전에는 리뷰가 없고, 상대 카드는 응답에 담기지 않는다.
void testRoomReview() {
    WebOptions options;
    options.seed = 3u;
    options.hintTrials = 50;
    options.aiAction = std::chrono::milliseconds(0);
    options.autoContinue = true;
    options.nextHandDelay = std::chrono::seconds(60);
    RoomSettings settings;
    settings.bots = 2;
    Room room(options, settings, "000000", true, 3);
    std::string error;
    CHECK(room.join("a", "나", error) && room.start("a", error));

    bool finished = false;
    for (int i = 0; i < 3000 && !finished; ++i) {
        const RoomView v = *room.view("a");
        if (v.myTurn) {
            CHECK(!v.review);
            CHECK(viewToJson(v).find("\"review\":null") != std::string::npos);
            for (const auto& revealed : v.revealed) CHECK(!revealed);
            room.submit("a", v.table.state.toCall > 0 ? "call" : "check", {}, v.turnId, error);
        }
        // 승률 흐름은 리뷰보다 조금 늦게 채워질 수 있다.
        finished = v.review && (!v.reviewFlow || v.reviewFlow->streets.back().equity);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(finished);
    const RoomView v = *room.view("a");
    CHECK(v.review && v.review->handNumber == 1);
    CHECK(viewToJson(v).find("\"review\":{\"handNumber\":1") != std::string::npos);
}

}  // namespace

int main() {
    testByCategory();
    testByHighRank();
    testByKicker();
    testSplitWithOddChip();
    testTwoSidePots();
    testFoldNoShowdown();
    testFoldedCardsHiddenAtShowdown();
    testFlowReversal();
    testNamesAndBestFive();
    testRankingAndStandings();
    testTieRanking();
    testSidePotRankingAndElimination();
    testStandingsChange();
    testGates();
    testSkip();
    testPacing();
    testRandomHands();
    testRoomReview();
    std::printf("%d개 검사 중 %d개 실패\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
