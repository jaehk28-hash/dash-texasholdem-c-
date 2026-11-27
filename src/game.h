// game.h - 한 판 진행 (블라인드, 딜, 베팅 라운드, 쇼다운, 팟 분배)
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "card.h"
#include "player.h"
#include "pot.h"

// 게임 설정값
struct GameConfig {
    int startingChips = 10000;
    int smallBlind = 50;
    int bigBlind = 100;
};

// 팟 하나의 실제 지급 결과
struct PotResult {
    int amount = 0;             // 팟 크기
    std::vector<int> eligible;  // 받을 자격이 있던 좌석
    std::vector<int> won;       // 좌석별로 이 팟에서 받은 칩 (크기 = 좌석 수)
};

// 한 판이 끝난 뒤 돌려주는 결과 요약
struct HandSummary {
    int handNumber = 0;
    int button = 0;
    int smallBlindSeat = 0;
    int bigBlindSeat = 0;
    bool showdown = false;               // 쇼다운까지 갔는가
    std::vector<int> startChips;         // 판 시작 시 좌석별 칩
    std::vector<int> endChips;           // 판 종료 후 좌석별 칩
    std::vector<Card> board;
    std::vector<std::array<Card, 2>> holes;  // 좌석별 홀 카드 (참여한 좌석만 유효)
    std::vector<bool> dealtIn;           // 이번 판에 카드를 받은 좌석
    std::vector<bool> folded;            // 이번 판에 폴드한 좌석 (쇼다운에서 누구 카드를 공개할지 판단)
    std::vector<int> contributed;        // 좌석별로 이번 판에 낸 칩 (돌려받은 베팅은 뺀 값)
    std::vector<PotResult> pots;         // 팟별 지급 결과. [0]이 메인 팟
    std::vector<std::string> history;    // 행동 기록 (핸드 히스토리)
};

// 화면 갱신용: 특정 좌석의 시점에서 본 현재 테이블.
// 상대의 홀 카드는 담기지 않는다 (state.myHole은 보는 사람 자신의 카드).
struct TableSnapshot {
    GameState state;
    bool holeDealt = false;  // 이번 판의 홀 카드를 이미 나눠 줬는가 (false면 myHole은 의미 없음)
};

// 다음에 일어날 단계. 화면이 "다음 단계로"를 기다릴 때 쓴다.
//  HandEnd는 모두 폴드해서 쇼다운 없이 판이 끝나기 직전이다.
enum class Stage { Flop, Turn, River, Showdown, HandEnd };

// 테이블 전체를 관리하고 한 판씩 진행하는 클래스.
class Game {
public:
    // players의 순서가 곧 좌석 순서(시계 방향)다. 모두 startingChips로 시작한다.
    Game(const GameConfig& config, std::vector<std::unique_ptr<Player>> players, unsigned seed);

    // 한 판을 처음부터 끝까지 진행한다. 칩이 있는 사람이 2명 미만이면 호출하면 안 된다.
    HandSummary playHand();

    // 칩이 남은(탈락하지 않은) 플레이어 수
    int activePlayers() const;
    // 한 명만 남았으면 게임 종료
    bool isOver() const { return activePlayers() <= 1; }

    int numSeats() const { return static_cast<int>(seats_.size()); }
    int chips(int seat) const { return seats_[seat].chips; }
    const Player& player(int seat) const { return *seats_[seat].player; }
    const GameConfig& config() const { return config_; }

    // viewer 좌석이 볼 수 있는 정보만 담아 돌려준다.
    // 게임을 진행하는 스레드에서만 호출할 것 (예: 로거 콜백 안).
    TableSnapshot snapshot(int viewer) const;

    // 진행 중 사건을 한 줄씩 받는 콜백 (화면 출력용). 기록은 HandSummary.history에도 남는다.
    void setLogger(std::function<void(const std::string&)> logger) { logger_ = std::move(logger); }

    // 플랍/턴/리버를 깔기 직전, 쇼다운(또는 판 마무리) 직전에 불린다.
    // 이 함수가 돌아올 때까지 게임은 기다린다. 게임 결과에는 영향이 없다.
    void setStageHook(std::function<void(Stage)> hook) { stageHook_ = std::move(hook); }

    // ---- 테스트/벤치마크용 보조 기능 ----
    void setChips(int seat, int chips) { seats_[seat].chips = chips; }
    void setNextButton(int seat) { forcedButton_ = seat; }  // 다음 판의 버튼 좌석을 강제
    void setNextDeck(const std::vector<Card>& drawOrder) { nextDeck_ = drawOrder; hasNextDeck_ = true; }

private:
    struct Seat {
        std::unique_ptr<Player> player;
        int chips = 0;  // 남은 칩
        // ---- 한 판 동안만 쓰는 상태 ----
        bool inHand = false;   // 카드를 받았는가
        bool folded = false;
        bool allIn = false;
        bool acted = false;    // 이번 라운드에 이미 행동했는가
        int bet = 0;           // 이번 라운드에 건 칩
        int contributed = 0;   // 이번 판에 낸 총 칩
        int levelAtAction = 0; // 내가 마지막으로 행동한 직후의 최고 베팅액 (레이즈 권리 판정용)
        std::array<Card, 2> hole;
    };

    int next(int seat) const { return (seat + 1) % numSeats(); }
    int nextInHand(int seat) const;           // 시계 방향으로 다음 참여자
    int liveCount() const;                    // 폴드하지 않은 사람 수
    int canActCount() const;                  // 폴드/올인이 아니라 행동할 수 있는 사람 수
    int totalChips() const;                   // 좌석 칩 + 팟 + 라운드 베팅 합계
    bool needsAction(int seat) const;
    bool canRaise(int seat) const;
    GameState makeState(int seat, Street street) const;
    void log(const std::string& line);
    std::string seatName(int seat) const;

    void putChips(int seat, int amount);      // 칩을 베팅에 넣는다 (부족하면 올인)
    void postBlind(int seat, int amount, const char* label);
    bool bettingRound(int startSeat, Street street);  // 폴드로 한 명만 남으면 false
    void applyAction(int seat, Action action);
    void returnUncalledBet();
    void collectBets();
    void dealBoard(int count, const char* label);
    void finishHand(HandSummary& summary);

    GameConfig config_;
    std::vector<Seat> seats_;
    std::mt19937 rng_;
    std::function<void(const std::string&)> logger_;
    std::function<void(Stage)> stageHook_;

    Deck deck_;
    std::vector<Card> board_;
    int button_ = -1;
    int sbSeat_ = 0;
    int bbSeat_ = 0;
    int handNumber_ = 0;
    int pot_ = 0;          // 이전 라운드까지 모인 팟
    int currentBet_ = 0;   // 이번 라운드 최고 베팅액
    int lastRaise_ = 0;    // 직전 (풀) 벳/레이즈 증가분 = 최소 레이즈 폭
    int expectedTotal_ = 0;  // 칩 보존 검사용: 판 시작 시 전체 칩
    bool showdown_ = false;
    bool holeDealt_ = false;  // 이번 판의 홀 카드를 나눠 줬는가

    int forcedButton_ = -1;
    bool hasNextDeck_ = false;
    std::vector<Card> nextDeck_;
    std::vector<std::string>* history_ = nullptr;  // 진행 중인 판의 기록
};
