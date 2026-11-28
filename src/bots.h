// bots.h - 성격이 다른 봇 5종
//
// 봇은 GameState(공용 카드, 베팅, 팟, 스택, 포지션, 자기 카드)만 보고 판단한다.
// GameState에는 남의 홀 카드가 없으므로, 구조상 남의 패를 알 수 없다.
//
// 모든 봇이 같은 판단 코드를 쓰고, 성격은 BotProfile의 숫자로 정한다.
#pragma once

#include <array>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "player.h"

enum class Difficulty { Easy, Normal, Hard };

// 성격을 정하는 값. 화면의 한 줄 설명("세게 베팅" 등)과 어긋나지 않게 정한다.
struct BotProfile {
    double vpip;           // 프리플랍에서 참여하는 핸드의 비율
    double pfr;            // 프리플랍에서 레이즈하는 핸드의 비율
    double aggression;     // 콜보다 벳/레이즈를 고르는 정도
    double bluff;          // 약한 패로 베팅하는 빈도
    double foldPressure;   // 압박받으면 접는 정도
    double slowplay;       // 강한 패를 숨기고 기다리는 빈도
    double thinkSpeed;     // 생각 시간 배수 (1보다 작으면 빠르게)
};

class Bot : public Player {
public:
    Bot(unsigned seed, BotProfile profile) : rng_(seed), base_(profile) {}

    Action decide(const GameState& s) override;
    void observe(const PublicAction& action) override;
    void handEnded(int chipDelta, int bigBlind) override;

    // 난이도는 실수 확률과 블러프 빈도만 바꾼다.
    void setDifficulty(Difficulty difficulty) { difficulty_ = difficulty; }
    // 앞의 몇 좌석이 사람인가 (사람의 공개된 행동을 아주 약하게 기억한다)
    void setHumanSeats(int count) { humanSeats_ = count; }

    // 지금 적용되는 성격 값: 기본값 + 틸트 + 사람에 대한 기억 + 난이도
    BotProfile profile() const;
    bool tilted() const { return tiltHands_ > 0; }
    // 방금 결정이 "약한 패로 베팅"이었는가 (블러프가 통했을 때의 반응용. 내 카드만 본 판단)
    bool lastWasBluff() const { return lastBluff_; }

protected:
    // 기본 판단. 성격이 특별한 봇은 이것을 바꾼다.
    virtual Action choose(const GameState& s, const BotProfile& p);

    // 0.0 ~ 1.0 사이 난수
    double rand01() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }
    bool chance(double probability) { return rand01() < probability; }
    double mistakeRate() const;

    // "팟의 fraction 배"만큼 벳/레이즈하는 행동을 만든다 (규칙 범위 안으로 보정됨).
    // 레이즈할 수 없는 상황이면 콜(또는 체크)로 대신한다.
    Action raiseByPot(const GameState& s, double fraction) const;
    Action raiseTo(const GameState& s, int target) const;
    // 상황에 맞는 크기 하나를 고른다 (팟의 1/3, 1/2, 2/3, 1배, 가끔 오버벳)
    Action bet(const GameState& s, const BotProfile& p, double strength);
    Action preflopRaise(const GameState& s);  // 2.2~3.5BB로 열거나, 앞 베팅의 2.5~3.5배
    Action checkOrFold(const GameState& s) const;
    Action checkOrCall(const GameState& s) const;

    std::mt19937 rng_;

private:
    BotProfile base_;
    Difficulty difficulty_ = Difficulty::Normal;
    int humanSeats_ = 0;
    int humanActions_ = 0, humanRaises_ = 0, humanFolds_ = 0;
    int tiltHands_ = 0;
    int trapHand_ = -1;  // 이번 판에서 강한 패를 숨기고 체크했다 (다음에 베팅이 오면 레이즈)
    bool lastBluff_ = false;
};

// 1) 타이트: 좋은 핸드만, 신중하게
class TightBot : public Bot {
public:
    explicit TightBot(unsigned seed) : Bot(seed, {0.22, 0.17, 0.55, 0.06, 0.65, 0.15, 1.2}) {}
    std::string name() const override { return "타이트"; }
};

// 2) 루즈: 자주 참여
class LooseBot : public Bot {
public:
    explicit LooseBot(unsigned seed) : Bot(seed, {0.58, 0.16, 0.38, 0.12, 0.28, 0.10, 1.0}) {}
    std::string name() const override { return "루즈"; }
};

// 3) 공격형: 세게 베팅, 블러프도 자주
class AggroBot : public Bot {
public:
    explicit AggroBot(unsigned seed) : Bot(seed, {0.46, 0.36, 0.85, 0.30, 0.35, 0.08, 0.72}) {}
    std::string name() const override { return "공격형"; }
};

// 4) 콜링 스테이션: 잘 안 접고, 거의 콜만
class CallingStationBot : public Bot {
public:
    explicit CallingStationBot(unsigned seed) : Bot(seed, {1.0, 0.05, 0.10, 0.03, 0.02, 0.30, 0.95}) {}
    std::string name() const override { return "콜링스테이션"; }

protected:
    Action choose(const GameState& s, const BotProfile& p) override;
};

// 5) 확률 계산형: 몬테카를로 승률을 팟 오즈와 비교
class OddsBot : public Bot {
public:
    OddsBot(unsigned seed, int trials = 500) : Bot(seed, {0.28, 0.18, 0.55, 0.07, 0.55, 0.10, 1.28}), trials_(trials) {}
    std::string name() const override { return "확률계산"; }

protected:
    Action choose(const GameState& s, const BotProfile& p) override;

private:
    int trials_;  // 승률 계산 시행 횟수
};

// ---- 핸드 강도 도우미 (봇들이 함께 쓴다, 테스트에서도 사용) ----

// 프리플랍 핸드 강도 0.0 ~ 1.0 (Chen 공식 기반). AA가 1.0 근처, 72o가 0.0 근처.
double preflopStrength(const std::array<Card, 2>& hole);
// 모든 시작 핸드 가운데 이 핸드보다 약한 것의 비율 (VPIP, PFR과 비교한다)
double preflopPercentile(const std::array<Card, 2>& hole);

// 플랍 이후 핸드 강도 0.0 ~ 1.0. 만들어진 족보와 홀 카드가 실제로 기여하는지를 본다.
double postflopStrength(const std::array<Card, 2>& hole, const std::vector<Card>& board);

// 상황에 맞는 강도 (프리플랍이면 preflopStrength, 아니면 postflopStrength)
double handStrength(const GameState& s);

// ---- 생각하는 시간 ----
// 느리게(5초) 기준의 시간(ms). 상황의 크기와 남은 카드 수에만 기대고, 카드 강도는 쓰지 않는다.
// 성격의 속도(speed)와 ±20%의 변동을 곱한다.
int thinkTimeMs(const GameState& s, const Action& action, double speed, std::mt19937& rng);

// 봇 5종을 순서대로 만든다 (좌석 0~4). seed에서 봇마다 다른 시드를 만든다.
std::vector<std::unique_ptr<Player>> makeAllBots(unsigned seed);
