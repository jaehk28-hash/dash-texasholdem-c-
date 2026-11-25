// main.cpp - 6인 노리밋 텍사스 홀덤 연습 시뮬레이터 (사람 1명 + 봇 5명)
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

#include "bots.h"
#include "game.h"
#include "human_player.h"

namespace {

void printUsage() {
    std::cout << "사용법: holdem [옵션]\n"
                 "  --seed N        난수 시드 (같은 시드면 같은 카드 순서와 봇 행동)\n"
                 "  --no-hint       승률/팟 오즈 힌트를 끈다\n"
                 "  --chips N       시작 칩 (기본 10000)\n"
                 "  --sb N          스몰 블라인드 (기본 50)\n"
                 "  --bb N          빅 블라인드 (기본 100)\n"
                 "  --hint-trials N 힌트 승률 계산 시행 횟수 (기본 10000)\n"
                 "  --help          이 도움말\n";
}

// 옵션 값을 양의 정수로 읽는다. 잘못되면 false.
bool parsePositive(const char* text, int& value) {
    char* end = nullptr;
    long v = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || v <= 0 || v > 100000000) return false;
    value = static_cast<int>(v);
    return true;
}

// 한 판이 끝난 뒤 결과 요약과 핸드 히스토리를 보여 준다.
void showHandResult(const HandSummary& h, const Game& game) {
    std::cout << "\n---------- 제 " << h.handNumber << " 판 결과 ----------\n";
    for (int i = 0; i < game.numSeats(); ++i) {
        if (!h.dealtIn[i]) continue;
        int diff = h.endChips[i] - h.startChips[i];
        std::cout << "  " << game.player(i).name() << ": " << h.endChips[i] << " ("
                  << (diff >= 0 ? "+" : "") << diff << ")\n";
    }
}

void showHistory(const HandSummary& h, const Game& game) {
    std::cout << "\n=============== 핸드 히스토리 (제 " << h.handNumber << " 판) ===============\n";
    for (const std::string& line : h.history) std::cout << "  " << line << "\n";
    std::cout << "  -- 전원의 홀 카드 (연습용 공개) --\n";
    for (int i = 0; i < game.numSeats(); ++i) {
        if (!h.dealtIn[i]) continue;
        std::cout << "  " << game.player(i).name() << ": " << cardToString(h.holes[i][0]) << " "
                  << cardToString(h.holes[i][1]) << "\n";
    }
    std::cout << "=========================================================\n";
}

}  // namespace

int main(int argc, char** argv) {
    GameConfig config;
    HumanOptions human;
    unsigned seed = std::random_device{}();
    bool seedGiven = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needValue = [&](int& target) {
            if (i + 1 >= argc || !parsePositive(argv[i + 1], target)) {
                std::cerr << arg << " 뒤에는 양의 정수가 필요합니다.\n";
                std::exit(1);
            }
            ++i;
        };
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--no-hint") {
            human.showHint = false;
        } else if (arg == "--seed") {
            // 시드는 0도 허용하므로 따로 읽는다.
            if (i + 1 >= argc) { std::cerr << "--seed 뒤에는 숫자가 필요합니다.\n"; return 1; }
            char* end = nullptr;
            long long parsed = std::strtoll(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || *end != '\0' || parsed < 0) {
                std::cerr << "--seed 뒤에는 0 이상의 정수가 필요합니다.\n";
                return 1;
            }
            seed = static_cast<unsigned>(parsed);
            seedGiven = true;
            ++i;
        } else if (arg == "--chips") {
            needValue(config.startingChips);
        } else if (arg == "--sb") {
            needValue(config.smallBlind);
        } else if (arg == "--bb") {
            needValue(config.bigBlind);
        } else if (arg == "--hint-trials") {
            needValue(human.hintTrials);
        } else {
            std::cerr << "알 수 없는 옵션: " << arg << "\n";
            printUsage();
            return 1;
        }
    }
    if (config.smallBlind > config.bigBlind) {
        std::cerr << "스몰 블라인드는 빅 블라인드보다 클 수 없습니다.\n";
        return 1;
    }

    // 좌석 0이 나, 1~5가 봇 5종
    std::vector<std::unique_ptr<Player>> players;
    players.push_back(std::make_unique<HumanPlayer>("나", human, std::cin, std::cout, seed + 1));
    for (auto& bot : makeAllBots(seed)) players.push_back(std::move(bot));

    Game game(config, std::move(players), seed);
    game.setLogger([](const std::string& line) { std::cout << "  " << line << "\n"; });

    std::cout << "6인 노리밋 텍사스 홀덤 연습  (시드 " << seed << (seedGiven ? ", 지정됨" : ", 무작위")
              << ", 칩 " << config.startingChips << ", 블라인드 " << config.smallBlind << "/"
              << config.bigBlind << ")\n";
    std::cout << "상대: 타이트, 루즈, 공격형, 콜링스테이션, 확률계산\n";

    try {
        while (!game.isOver() && game.chips(0) > 0) {
            HandSummary hand = game.playHand();
            showHandResult(hand, game);
            if (game.isOver() || game.chips(0) == 0) break;

            // Enter: 다음 판, h: 핸드 히스토리, q: 종료
            while (true) {
                std::cout << "[Enter] 다음 판   [h] 핸드 히스토리   [q] 종료 > " << std::flush;
                std::string line;
                if (!std::getline(std::cin, line)) throw QuitRequested{};
                if (line == "q" || line == "Q") throw QuitRequested{};
                if (line == "h" || line == "H") {
                    showHistory(hand, game);
                    continue;
                }
                break;
            }
        }
    } catch (const QuitRequested&) {
        std::cout << "\n게임을 종료합니다. 내 칩: " << game.chips(0) << "\n";
        return 0;
    }

    if (game.chips(0) == 0) {
        std::cout << "\n칩이 모두 떨어져 탈락했습니다. 게임 종료!\n";
    } else {
        std::cout << "\n축하합니다! 봇을 모두 이기고 우승했습니다! (최종 칩 " << game.chips(0) << ")\n";
    }
    return 0;
}
