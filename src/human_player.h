// human_player.h - 콘솔에서 입력받는 사람 플레이어
#pragma once

#include <iosfwd>
#include <istream>
#include <ostream>
#include <random>
#include <string>

#include "player.h"

// 사람 플레이어 옵션
struct HumanOptions {
    bool showHint = true;     // 승률/팟 오즈 힌트를 보여 줄까 (--no-hint로 끈다)
    int hintTrials = 10000;   // 승률 계산 시행 횟수
};

// 입력이 끝났거나(EOF) 사용자가 종료(q)를 선택했을 때 던지는 예외. main에서 받아 정상 종료한다.
struct QuitRequested {};

// 입력 한 줄을 행동으로 해석한다.
//  성공하면 true와 함께 out에 행동을 채운다.
//  잘못된 입력이거나 규칙에 어긋나면 false와 함께 error에 한국어 안내 문구를 채운다.
// 사용법: f(폴드) / c(체크·콜) / r 금액(레이즈 목표 금액) / r pot, r half / a(올인)
bool parseHumanCommand(const std::string& line, const GameState& state, Action& out, std::string& error);

// 팟 오즈: 콜하려고 내는 칩 / (콜 후 팟 크기). 0.0 ~ 1.0.
// 콜해서 이기려면 승률이 이 값보다 높아야 본전 이상이다. 콜 비용이 없으면 0.
double potOdds(const GameState& state);

// 힌트 한 덩어리(포지션, 팟 크기, 콜 비용, 팟 오즈, 승률)를 출력한다.
// equity는 estimateEquity()로 구한 현재 승률(0~1).
void printHint(std::ostream& out, const GameState& state, double equity, int trials);

// 화면에 현재 테이블 상황을 출력한다 (힌트 제외).
void printTable(std::ostream& out, const GameState& state);

// 콘솔 입력으로 행동을 고르는 플레이어
class HumanPlayer : public Player {
public:
    HumanPlayer(std::string name, HumanOptions options, std::istream& in, std::ostream& out, unsigned seed);

    std::string name() const override { return name_; }

    // 상황을 보여 주고, 올바른 입력이 들어올 때까지 다시 묻는다.
    Action decide(const GameState& state) override;

private:
    std::string name_;
    HumanOptions options_;
    std::istream& in_;
    std::ostream& out_;
    std::mt19937 rng_;
};
