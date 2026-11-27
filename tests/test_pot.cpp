// test_pot.cpp - 팟/사이드 팟 계산 테스트
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <numeric>
#include <random>
#include <vector>

#include "check.h"
#include "pot.h"

// 좌석 0..n-1이 낸 금액과 폴드 여부로 PotEntry 목록을 만든다.
static std::vector<PotEntry> entries(const std::vector<int>& paid, const std::vector<int>& foldedSeats = {}) {
    std::vector<PotEntry> v;
    for (std::size_t i = 0; i < paid.size(); ++i) {
        PotEntry e;
        e.seat = static_cast<int>(i);
        e.contributed = paid[i];
        e.folded = std::find(foldedSeats.begin(), foldedSeats.end(), static_cast<int>(i)) != foldedSeats.end();
        v.push_back(e);
    }
    return v;
}

static int total(const std::vector<int>& v) { return std::accumulate(v.begin(), v.end(), 0); }

static void testSinglePot() {
    auto pots = buildPots(entries({100, 100, 100}));
    CHECK(pots.size() == 1);
    CHECK(pots[0].amount == 300);
    CHECK(pots[0].eligible == (std::vector<int>{0, 1, 2}));
}

static void testOneShortAllIn() {
    // 좌석0 올인 50, 나머지 100씩 -> 메인 150(3명), 사이드 100(1,2)
    auto pots = buildPots(entries({50, 100, 100}));
    CHECK(pots.size() == 2);
    CHECK(pots[0].amount == 150 && pots[0].eligible == (std::vector<int>{0, 1, 2}));
    CHECK(pots[1].amount == 100 && pots[1].eligible == (std::vector<int>{1, 2}));
}

static void testMultipleAllIns() {
    // 100, 300, 600, 600 -> 메인 400(4명), 사이드1 600(3명), 사이드2 600(2명)
    auto pots = buildPots(entries({100, 300, 600, 600}));
    CHECK(pots.size() == 3);
    CHECK(pots[0].amount == 400 && pots[0].eligible.size() == 4);
    CHECK(pots[1].amount == 600 && pots[1].eligible == (std::vector<int>{1, 2, 3}));
    CHECK(pots[2].amount == 600 && pots[2].eligible == (std::vector<int>{2, 3}));
    CHECK(pots[0].amount + pots[1].amount + pots[2].amount == 1600);
}

static void testFoldedDeadMoney() {
    // 좌석3은 70을 내고 폴드. 좌석0은 50 올인, 좌석1,2는 200.
    auto pots = buildPots(entries({50, 200, 200, 70}, {3}));
    // 층 50: 50*3 + 폴드 50 = 200 (0,1,2) / 층 200: 폴드 20 + 150*2 = 320 (1,2)
    CHECK(pots.size() == 2);
    CHECK(pots[0].amount == 200 && pots[0].eligible == (std::vector<int>{0, 1, 2}));
    CHECK(pots[1].amount == 320 && pots[1].eligible == (std::vector<int>{1, 2}));
}

static void testFoldersOnlyChipsKept() {
    // 좌석 0이 폴드한 칩도 팟에 남는다. 살아있는 사람이 한 명이면 팟 하나, 자격자 1명.
    auto pots = buildPots(entries({100, 0, 40}, {2}));
    CHECK(pots.size() == 1);
    CHECK(pots[0].amount == 140 && pots[0].eligible == (std::vector<int>{0}));
    // 아무도 칩을 내지 않았다면 팟 없음
    CHECK(buildPots(entries({0, 0})).empty());
}

static void testFoldedExtraGoesToLastPot() {
    // 폴드한 사람이 살아있는 사람보다 더 낸 비정상 입력도 칩이 사라지지 않는다.
    auto pots = buildPots(entries({100, 100, 150}, {2}));
    int sum = 0;
    for (auto& p : pots) sum += p.amount;
    CHECK(sum == 350);
}

// 점수 배열을 만드는 도우미 (숫자가 클수록 강함)
static std::vector<HandScore> sc(std::initializer_list<unsigned> v) { return std::vector<HandScore>(v); }

static void testWinnerTakesAll() {
    auto pots = buildPots(entries({100, 100, 100}));
    auto w = distributePots(pots, sc({5, 9, 7}), 0, 3);
    CHECK(w == (std::vector<int>{0, 300, 0}));
}

static void testSidePotDifferentWinners() {
    // 좌석0(최고 핸드)은 50 올인, 좌석1이 2등, 좌석2가 3등. 각 100씩.
    auto pots = buildPots(entries({50, 100, 100}));
    auto w = distributePots(pots, sc({9, 7, 5}), 0, 3);
    CHECK(w == (std::vector<int>{150, 100, 0}));  // 0이 메인, 1이 사이드
}

static void testMultiAllInDistribution() {
    // 좌석 점수: 0이 1등(100 올인), 1이 2등(300 올인), 2가 3등(600), 3이 꼴찌(600)
    auto pots = buildPots(entries({100, 300, 600, 600}));
    auto w = distributePots(pots, sc({9, 8, 7, 1}), 0, 4);
    // 메인 400 -> 0 / 사이드1 600 -> 1 / 사이드2 600 -> 2
    CHECK(w == (std::vector<int>{400, 600, 600, 0}));
}

static void testTieSplit() {
    auto pots = buildPots(entries({100, 100, 100, 100}));
    auto w = distributePots(pots, sc({8, 8, 3, 3}), 0, 4);
    CHECK(w == (std::vector<int>{200, 200, 0, 0}));
}

static void testOddChip() {
    // 총 301칩을 좌석 1,2가 동점으로 나눈다. 버튼이 0이면 버튼 왼쪽 첫 승자는 1.
    std::vector<Pot> pots = {Pot{301, {0, 1, 2}}};
    auto w = distributePots(pots, sc({1, 8, 8}), 0, 3);
    CHECK(w == (std::vector<int>{0, 151, 150}));
    // 버튼이 1이면 왼쪽 첫 승자는 2
    w = distributePots(pots, sc({1, 8, 8}), 1, 3);
    CHECK(w == (std::vector<int>{0, 150, 151}));
    // 버튼이 2이면 한 바퀴 돌아 0, 1 순서 -> 승자 중 1이 먼저
    w = distributePots(pots, sc({1, 8, 8}), 2, 3);
    CHECK(w == (std::vector<int>{0, 151, 150}));
}

static void testOddChipsMultipleWinners() {
    // 3명이 동점으로 100칩을 나누면 34/33/33 (버튼 왼쪽부터)
    std::vector<Pot> pots = {Pot{100, {0, 1, 2}}};
    auto w = distributePots(pots, sc({5, 5, 5}), 0, 3);
    CHECK(w == (std::vector<int>{33, 34, 33}));
    // 2칩이 남는 경우: 5명이 동점으로 52칩 -> 10씩 + 2칩, 버튼 3이면 4,0 이 11
    std::vector<Pot> pots2 = {Pot{52, {0, 1, 2, 3, 4}}};
    w = distributePots(pots2, sc({5, 5, 5, 5, 5}), 3, 5);
    CHECK(w == (std::vector<int>{11, 10, 10, 10, 11}));
}

static void testOddChipPerPot() {
    // 팟마다 홀수 칩을 따로 처리한다. 메인 151(0,1,2 동점)->1명씩... 사이드 51(1,2 동점)
    std::vector<Pot> pots = {Pot{151, {0, 1, 2}}, Pot{51, {1, 2}}};
    auto w = distributePots(pots, sc({4, 4, 4}), 0, 3);
    // 메인: 151/3 = 50 나머지 1 -> 좌석1 51. 사이드: 51/2 = 25 나머지 1 -> 좌석1 26
    CHECK(w == (std::vector<int>{50, 77, 75}));
    CHECK(total(w) == 202);
}

static void testFoldedCannotWin() {
    // 폴드한 좌석 0의 점수가 가장 높아도 팟을 못 가져간다.
    auto pots = buildPots(entries({100, 100, 100}, {0}));
    auto w = distributePots(pots, sc({99, 5, 7}), 0, 3);
    CHECK(w == (std::vector<int>{0, 0, 300}));
}

static void testSoleSurvivor() {
    // 한 명만 남으면 점수 없이도 팟을 가져간다.
    auto pots = buildPots(entries({100, 100, 100}, {0, 2}));
    auto w = distributePots(pots, sc({0, 0, 0}), 0, 3);
    CHECK(w == (std::vector<int>{0, 300, 0}));
}

// 무작위 입력으로 칩 총합 보존을 확인한다.
static void testRandomConservation() {
    std::mt19937 rng(7);
    for (int iter = 0; iter < 20000; ++iter) {
        int n = 2 + static_cast<int>(rng() % 5);
        std::vector<PotEntry> es;
        std::vector<HandScore> scores;
        int sum = 0;
        int liveCount = 0;
        for (int i = 0; i < n; ++i) {
            PotEntry e;
            e.seat = i;
            e.contributed = static_cast<int>(rng() % 6) * 50 + static_cast<int>(rng() % 3);
            e.folded = (rng() % 3 == 0);
            if (!e.folded) ++liveCount;
            sum += e.contributed;
            es.push_back(e);
            scores.push_back(rng() % 4);  // 동점이 자주 나오도록 작은 범위
        }
        if (liveCount == 0) continue;  // 실제 게임에서는 생기지 않는 경우
        auto pots = buildPots(es);
        int potSum = 0;
        for (auto& p : pots) potSum += p.amount;
        auto w = distributePots(pots, scores, static_cast<int>(rng() % n), n);
        bool noNegative = std::all_of(w.begin(), w.end(), [](int x) { return x >= 0; });
        if (potSum != sum || total(w) != potSum || !noNegative) {
            CHECK(false);
            return;
        }
        ++g_checks;
    }
}

int main() {
    testSinglePot();
    testOneShortAllIn();
    testMultipleAllIns();
    testFoldedDeadMoney();
    testFoldersOnlyChipsKept();
    testFoldedExtraGoesToLastPot();
    testWinnerTakesAll();
    testSidePotDifferentWinners();
    testMultiAllInDistribution();
    testTieSplit();
    testOddChip();
    testOddChipsMultipleWinners();
    testOddChipPerPot();
    testFoldedCannotWin();
    testSoleSurvivor();
    testRandomConservation();
    std::printf("%d개 검사 중 %d개 실패\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
