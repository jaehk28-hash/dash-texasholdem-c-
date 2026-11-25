// bots.h - 성격이 다른 봇 5종
#pragma once

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "player.h"

// 모든 봇이 공유하는 부모. 난수 생성기와 자주 쓰는 도우미를 가지고 있다.
class Bot : public Player {
public:
    explicit Bot(unsigned seed) : rng_(seed) {}

protected:
    // 0.0 ~ 1.0 사이 난수
    double rand01() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }

    // "팟의 fraction 배"만큼 벳/레이즈하는 행동을 만든다 (규칙 범위 안으로 보정됨).
    // 레이즈할 수 없는 상황이면 콜(또는 체크)로 대신한다.
    Action raiseByPot(const GameState& s, double fraction) const;

    // 공짜로 볼 수 있으면 체크, 아니면 폴드
    Action checkOrFold(const GameState& s) const;

    std::mt19937 rng_;
};

// 1) 타이트: 좋은 핸드만 플레이한다. 들어오면 강하게 베팅하고, 약하면 빨리 접는다.
class TightBot : public Bot {
public:
    explicit TightBot(unsigned seed) : Bot(seed) {}
    std::string name() const override { return "타이트"; }
    Action decide(const GameState& s) override;
};

// 2) 루즈: 거의 모든 핸드에 참여하고 콜을 많이 하지만, 가끔 레이즈한다.
class LooseBot : public Bot {
public:
    explicit LooseBot(unsigned seed) : Bot(seed) {}
    std::string name() const override { return "루즈"; }
    Action decide(const GameState& s) override;
};

// 3) 공격형(애그로): 자주 벳/레이즈하고 블러프도 한다. 폴드는 잘 하지 않는다.
class AggroBot : public Bot {
public:
    explicit AggroBot(unsigned seed) : Bot(seed) {}
    std::string name() const override { return "공격형"; }
    Action decide(const GameState& s) override;
};

// 4) 콜링 스테이션: 거의 항상 콜만 한다. 레이즈는 아주 강한 핸드일 때만.
class CallingStationBot : public Bot {
public:
    explicit CallingStationBot(unsigned seed) : Bot(seed) {}
    std::string name() const override { return "콜링스테이션"; }
    Action decide(const GameState& s) override;
};

// 5) 확률 계산형: 몬테카를로로 승률을 구하고 팟 오즈와 비교해서 행동한다.
class OddsBot : public Bot {
public:
    OddsBot(unsigned seed, int trials = 500) : Bot(seed), trials_(trials) {}
    std::string name() const override { return "확률계산"; }
    Action decide(const GameState& s) override;

private:
    int trials_;  // 승률 계산 시행 횟수
};

// ---- 핸드 강도 도우미 (봇들이 함께 쓴다, 테스트에서도 사용) ----

// 프리플랍 핸드 강도 0.0 ~ 1.0 (Chen 공식 기반). AA가 1.0 근처, 72o가 0.0 근처.
double preflopStrength(const std::array<Card, 2>& hole);

// 플랍 이후 핸드 강도 0.0 ~ 1.0. 만들어진 족보와 홀 카드가 실제로 기여하는지를 본다.
double postflopStrength(const std::array<Card, 2>& hole, const std::vector<Card>& board);

// 상황에 맞는 강도 (프리플랍이면 preflopStrength, 아니면 postflopStrength)
double handStrength(const GameState& s);

// 봇 5종을 순서대로 만든다 (좌석 0~4). seed에서 봇마다 다른 시드를 만든다.
std::vector<std::unique_ptr<Player>> makeAllBots(unsigned seed);
