// bench.cpp - 성능/정확도 측정 프로그램 (docs/bench.md를 만들어 낸다)
//
// 측정 내용
//  1) 봇끼리 N판: 봇별 평균 칩 변화, bb/100, 이긴 판 비율
//  2) 승률(몬테카를로) 시행 횟수 1천/1만/10만별 정확도와 소요 시간
//  3) 족보 판정 속도 (빠른 방식 vs 전수 검사)
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "bots.h"
#include "equity.h"
#include "evaluator.h"
#include "game.h"

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string fmt(const char* format, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), format, v);
    return buf;
}

std::vector<Card> cardsOf(const std::string& text) {
    std::vector<Card> v;
    std::istringstream in(text);
    std::string tok;
    while (in >> tok) {
        Card c;
        parseCard(tok, c);
        v.push_back(c);
    }
    return v;
}

// ---------------------------------------------------------------------------
// 1) 봇 대결
// ---------------------------------------------------------------------------
std::string benchBots(int hands, unsigned seed) {
    const int kSeats = 5;
    Game game(GameConfig{}, makeAllBots(seed), seed);
    const int bb = game.config().bigBlind;
    const int stack = game.config().startingChips;

    // 봇별 누적 통계 (Welford 방식으로 평균/분산을 한 번에 계산)
    struct Stat {
        double mean = 0, m2 = 0;
        long long wins = 0, losses = 0, even = 0;
        long long count = 0;
        void add(double x) {
            ++count;
            double d = x - mean;
            mean += d / count;
            m2 += d * (x - mean);
            if (x > 0) ++wins;
            else if (x < 0) ++losses;
            else ++even;
        }
        double stddev() const { return count > 1 ? std::sqrt(m2 / (count - 1)) : 0.0; }
    };
    Stat stats[kSeats];
    long long showdowns = 0;

    auto start = Clock::now();
    for (int i = 0; i < hands; ++i) {
        // 매 판 모두 같은 칩(100BB)으로 시작: 탈락 없이 순수 전략 비교
        for (int s = 0; s < kSeats; ++s) game.setChips(s, stack);
        HandSummary h = game.playHand();
        if (h.showdown) ++showdowns;
        for (int s = 0; s < kSeats; ++s) stats[s].add(h.endChips[s] - h.startChips[s]);
    }
    const double elapsed = secondsSince(start);

    std::ostringstream md;
    md << "| 봇 | 평균 칩 변화/판 | bb/100 | 95% 신뢰구간(bb/100) | 이긴 판 | 진 판 | 이긴 판 비율 |\n";
    md << "|---|---:|---:|---:|---:|---:|---:|\n";
    double sumMean = 0;
    for (int s = 0; s < kSeats; ++s) {
        const Stat& st = stats[s];
        const double bb100 = st.mean / bb * 100.0;
        const double ci = 1.96 * st.stddev() / std::sqrt(static_cast<double>(st.count)) / bb * 100.0;
        sumMean += st.mean;
        md << "| " << game.player(s).name() << " | " << fmt("%+.1f", st.mean) << " | " << fmt("%+.1f", bb100)
           << " | ±" << fmt("%.1f", ci) << " | " << st.wins << " | " << st.losses << " | "
           << fmt("%.1f", 100.0 * st.wins / st.count) << "% |\n";
    }
    md << "\n- 진행: " << hands << "판, 시드 " << seed << ", 5인 테이블(봇 5종 1명씩), 매 판 칩 "
       << stack << "(" << stack / bb << "BB)으로 초기화, 블라인드 " << game.config().smallBlind << "/" << bb << "\n";
    md << "- 쇼다운까지 간 판: " << showdowns << " (" << fmt("%.1f", 100.0 * showdowns / hands) << "%) - 콜링스테이션이 거의 모든 베팅을 따라가므로 쇼다운 비율이 매우 높다\n";
    md << "- 소요 시간: " << fmt("%.1f", elapsed) << "초 (판당 " << fmt("%.0f", elapsed / hands * 1e6) << "µs)\n";
    md << "- 블라인드(BB)가 " << bb << "이라 \"평균 칩 변화/판\"과 bb/100은 숫자가 같다. 블라인드를 바꾸면 달라진다.\n";
    md << "- 합계 검산: 평균 칩 변화의 합 = " << fmt("%.6f", sumMean) << " (제로섬이므로 0이어야 한다)\n";
    return md.str();
}

// ---------------------------------------------------------------------------
// 2) 승률 시행 횟수별 정확도
// ---------------------------------------------------------------------------
struct Scenario {
    const char* name;
    const char* hole;
    const char* board;
    int opponents;
};

std::string benchEquity() {
    const Scenario scenarios[] = {
        {"AA, 프리플랍, 상대 1명", "As Ad", "", 1},
        {"AKs, 프리플랍, 상대 3명", "As Ks", "", 3},
        {"72o, 프리플랍, 상대 5명", "7s 2d", "", 5},
        {"AKs 플러시 드로우, 플랍 Qs 7s 2d, 상대 2명", "As Ks", "Qs 7s 2d", 2},
        {"99 셋, 턴 9c Kd 2s 4h, 상대 4명", "9h 9d", "9c Kd 2s 4h", 4},
    };
    const int trialCounts[3] = {1000, 10000, 100000};
    const int repeats[3] = {200, 100, 30};
    const int referenceTrials = 4000000;

    std::ostringstream md;
    md << "기준값은 같은 상황을 " << referenceTrials << "회 시뮬레이션한 결과이다. "
       << "시행 횟수마다 시드를 바꿔 여러 번 반복해서 오차를 쟀다.\n\n";
    md << "| 상황 | 기준 승률 | 시행 횟수 | 반복 | 평균 절대 오차 | 최대 오차 | 표준편차 | 1회 소요 시간 |\n";
    md << "|---|---:|---:|---:|---:|---:|---:|---:|\n";

    for (const Scenario& sc : scenarios) {
        std::vector<Card> h = cardsOf(sc.hole);
        std::array<Card, 2> hole = {h[0], h[1]};
        std::vector<Card> board = cardsOf(sc.board);

        std::mt19937 refRng(12345);
        const double reference = estimateEquity(hole, board, sc.opponents, referenceTrials, refRng);

        for (int t = 0; t < 3; ++t) {
            double sumAbs = 0, maxAbs = 0, sum = 0, sumSq = 0;
            auto start = Clock::now();
            for (int r = 0; r < repeats[t]; ++r) {
                std::mt19937 rng(1000 + r);
                const double e = estimateEquity(hole, board, sc.opponents, trialCounts[t], rng);
                const double err = std::fabs(e - reference);
                sumAbs += err;
                maxAbs = std::max(maxAbs, err);
                sum += e;
                sumSq += e * e;
            }
            const double perRun = secondsSince(start) / repeats[t];
            const double mean = sum / repeats[t];
            const double sd = std::sqrt(std::max(0.0, sumSq / repeats[t] - mean * mean));
            md << "| " << (t == 0 ? sc.name : "") << " | " << (t == 0 ? fmt("%.2f", reference * 100) + "%" : "")
               << " | " << trialCounts[t] << " | " << repeats[t] << " | ±" << fmt("%.2f", sumAbs / repeats[t] * 100)
               << "%p | " << fmt("%.2f", maxAbs * 100) << "%p | " << fmt("%.2f", sd * 100) << "%p | "
               << fmt("%.2f", perRun * 1000) << " ms |\n";
        }
    }
    md << "\n이론적으로 오차(표준편차)는 시행 횟수의 제곱근에 반비례해서, 시행을 100배 늘리면 오차는 약 1/10로 줄고 "
          "시간은 약 100배 늘어난다. 표의 값이 대체로 이 경향을 따른다.\n";
    return md.str();
}

// ---------------------------------------------------------------------------
// 3) 족보 판정 속도
// ---------------------------------------------------------------------------
std::string benchEvaluator() {
    std::mt19937 rng(9);
    const int kHands = 200000;
    std::vector<std::vector<Card>> sets;
    for (int i = 0; i < kHands; ++i) {
        Deck d;
        d.shuffle(rng);
        std::vector<Card> c;
        for (int k = 0; k < 7; ++k) c.push_back(d.draw());
        sets.push_back(c);
    }
    volatile HandScore sink = 0;  // 최적화로 계산이 통째로 사라지지 않게 한다
    auto t0 = Clock::now();
    for (const auto& c : sets) sink = sink + evaluateBest(c);
    const double fast = secondsSince(t0);
    t0 = Clock::now();
    for (const auto& c : sets) sink = sink + evaluateBestBruteForce(c);
    const double slow = secondsSince(t0);

    std::ostringstream md;
    md << "| 방식 | 7장 " << kHands << "개 판정 시간 | 초당 판정 수 |\n|---|---:|---:|\n";
    md << "| 빠른 방식 (개수 세기) | " << fmt("%.3f", fast) << "초 | " << fmt("%.1f", kHands / fast / 1e6) << "백만 |\n";
    md << "| 전수 검사 (21조합) | " << fmt("%.3f", slow) << "초 | " << fmt("%.1f", kHands / slow / 1e6) << "백만 |\n";
    md << "\n빠른 방식은 전수 검사보다 약 " << fmt("%.1f", slow / fast) << "배 빠르다 (검증 코드: " << "tests/test_evaluator.cpp).\n";
    return md.str();
}

}  // namespace

int main(int argc, char** argv) {
    int hands = 100000;
    unsigned seed = 2024;
    std::string outPath;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--hands" && i + 1 < argc) hands = std::atoi(argv[++i]);
        else if (arg == "--seed" && i + 1 < argc) seed = static_cast<unsigned>(std::atoll(argv[++i]));
        else if (arg == "--out" && i + 1 < argc) outPath = argv[++i];
        else {
            std::cerr << "사용법: holdem_bench [--hands N] [--seed S] [--out docs/bench.md]\n";
            return 1;
        }
    }
    if (hands <= 0) return 1;

    std::cerr << "[1/3] 봇 대결 " << hands << "판...\n";
    const std::string bots = benchBots(hands, seed);
    std::cerr << "[2/3] 승률 정확도...\n";
    const std::string equity = benchEquity();
    std::cerr << "[3/3] 족보 판정 속도...\n";
    const std::string evaluator = benchEvaluator();

    std::ostringstream doc;
    doc << "# 성능과 승률 측정 결과\n\n"
        << "`holdem_bench`가 자동으로 만든 문서이다. 다시 만들려면 `./build/holdem_bench --out docs/bench.md`.\n\n"
        << "- 컴파일러: " << __VERSION__ << ", 최적화 -O2 (CMake 기본 Release)\n\n"
        << "## 1. 봇끼리 " << hands << "판\n\n" << bots << "\n"
        << "## 2. 승률(몬테카를로) 시행 횟수별 정확도와 시간\n\n" << equity << "\n"
        << "## 3. 족보 판정 속도\n\n" << evaluator;

    if (outPath.empty()) {
        std::cout << doc.str();
    } else {
        std::ofstream f(outPath);
        if (!f) {
            std::cerr << outPath << " 를 열 수 없습니다.\n";
            return 1;
        }
        f << doc.str();
        std::cerr << outPath << " 에 저장했습니다.\n";
    }
    return 0;
}
