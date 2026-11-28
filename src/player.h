// player.h - 플레이어 인터페이스와 게임 상태 (사람과 봇이 모두 이 인터페이스를 쓴다)
#pragma once

#include <array>
#include <string>
#include <vector>

#include "card.h"

// 행동 종류
enum class ActionType { Fold, Check, Call, Raise, AllIn };

// 플레이어가 고른 행동.
//  - Raise: amount는 "이번 라운드에 내가 건 총액" 기준의 목표 금액 (raise-to).
//           아직 아무도 벳하지 않았다면 벳(bet)이고, 이미 벳이 있으면 레이즈다.
//           예) 빅블라인드 100일 때 300까지 올리려면 amount = 300
//  - 나머지 행동은 amount를 쓰지 않는다.
struct Action {
    ActionType type = ActionType::Fold;
    int amount = 0;

    static Action fold() { return {ActionType::Fold, 0}; }
    static Action check() { return {ActionType::Check, 0}; }
    static Action call() { return {ActionType::Call, 0}; }
    static Action raiseTo(int amount) { return {ActionType::Raise, amount}; }
    static Action allIn() { return {ActionType::AllIn, 0}; }
};

// 베팅 라운드 구분
enum class Street { Preflop, Flop, Turn, River };

// 테이블의 한 좌석에 대해 모두가 볼 수 있는 공개 정보
struct PlayerView {
    std::string name;
    int chips = 0;         // 남은 칩 (이번 라운드에 건 칩은 제외)
    int bet = 0;           // 이번 라운드에 건 칩
    int contributed = 0;   // 이번 판에 지금까지 낸 총 칩
    bool inHand = false;   // 이번 판에 카드를 받았는가 (탈락자는 false)
    bool folded = false;
    bool allIn = false;
};

// 결정을 내릴 때 플레이어에게 보여 주는 게임 상황.
// 상대의 홀 카드는 들어 있지 않다 (자기 카드만 myHole에 있다).
struct GameState {
    int handNumber = 0;
    Street street = Street::Preflop;
    int numSeats = 0;
    int mySeat = 0;
    int button = 0;
    int smallBlindSeat = 0;
    int bigBlindSeat = 0;
    int smallBlind = 0;
    int bigBlind = 0;

    std::array<Card, 2> myHole;
    std::vector<Card> board;           // 공용 카드 (0, 3, 4, 5장)
    std::vector<PlayerView> players;   // 좌석 순서대로

    int pot = 0;          // 지금까지 모인 칩 전부 (이번 라운드 베팅 포함)
    int currentBet = 0;   // 이번 라운드의 최고 베팅액 (이만큼 맞춰야 콜)
    int toCall = 0;       // 내가 콜하려면 더 내야 하는 칩 (내 칩이 모자라면 내 칩만큼)
    int minRaiseTo = 0;   // 최소 레이즈 목표 금액 (raise-to 기준)
    int maxRaiseTo = 0;   // 올인했을 때의 목표 금액 (내 베팅 + 내 남은 칩)
    bool canRaise = false;  // 레이즈할 수 있는가 (레이즈 권리가 닫혔거나 칩이 모자라면 false)
    int myChips = 0;
    int myBet = 0;

    // 아직 폴드하지 않은 사람 수
    int playersLeft() const {
        int n = 0;
        for (const auto& p : players) {
            if (p.inHand && !p.folded) ++n;
        }
        return n;
    }
};

// 모두에게 보이는 행동 하나 (누가, 무엇을, 어느 라운드에). 카드 정보는 없다.
struct PublicAction {
    int seat = -1;
    ActionType type = ActionType::Fold;
    Street street = Street::Preflop;
};

// 좌석의 포지션 이름 (BTN, SB, BB, UTG, HJ, CO). 이번 판에 참여하지 않는 좌석은 "-".
// 헤즈업에서는 버튼이 SB를 겸하므로 "BTN/SB"로 표시한다.
std::string positionName(const GameState& state, int seat);

// 모든 플레이어(사람, 봇)의 공통 부모 클래스.
// 새 봇을 만들려면 이 클래스를 상속해서 name()과 decide()만 구현하면 된다.
class Player {
public:
    virtual ~Player() = default;

    // 화면에 표시할 이름
    virtual std::string name() const = 0;

    // 내 차례에 호출된다. 현재 상황을 보고 행동을 하나 돌려준다.
    // 규칙에 맞지 않는 행동을 돌려주면 게임이 가장 가까운 합법 행동으로 고친다.
    virtual Action decide(const GameState& state) = 0;

    // 아래 둘은 원하는 플레이어만 쓴다. 모두가 본 일만 알려 준다.
    virtual void observe(const PublicAction&) {}                    // 누군가 행동했다
    virtual void handEnded(int /*chipDelta*/, int /*bigBlind*/) {}  // 한 판이 끝나고 내 칩이 변한 양
};
