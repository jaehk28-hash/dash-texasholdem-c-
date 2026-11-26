// review.h - 한 판이 끝난 뒤의 "핸드 리뷰" 데이터 만들기
//
// 여기서는 계산만 한다. 문장은 화면(web/index.html)에서 만든다.
// 카드 공개 규칙: 쇼다운에 끝까지 남은 사람의 홀 카드만 담는다.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "card.h"
#include "evaluator.h"
#include "game.h"

// 두 핸드의 승부가 어디에서 갈렸는지
struct HandCompare {
    enum class Type { Category, Rank, Tie };
    Type type = Type::Tie;
    // type == Rank일 때: 무엇을 비교하다 갈렸는가
    //   "pair", "highPair", "lowPair", "trips", "fullPair", "quads",
    //   "straight", "flushCard", "highCard", "kicker"
    std::string role;
    int winRank = 0;          // 갈린 자리의 이긴 쪽 숫자 (2~14, A 낮은 스트레이트는 5)
    int loseRank = 0;         // 진 쪽 숫자
    std::vector<int> shared;  // 갈리기 전까지 서로 같았던 숫자들 (중요한 순)
    bool boardPlays = false;  // 동점이고, 그 5장이 공용 카드 5장 그대로인가
};

// 두 점수를 비교한다. win은 lose보다 작지 않아야 한다.
HandCompare compareScores(HandScore win, HandScore lose);

// 7장(홀 2 + 공용 5)에서 만든 가장 좋은 5장. 중요한 카드부터 정렬해서 돌려준다.
std::array<Card, 5> bestFive(const std::array<Card, 2>& hole, const std::vector<Card>& board);

// 족보 이름. A 하이 스트레이트 플러시는 "로열 플러시"로 부른다.
std::string handName(HandScore score);

// "원 페어 A", "투 페어 K·9"처럼 숫자까지 붙인 족보 설명
std::string describeHand(HandScore score);

// 족보의 대략적인 세기: 1(하이 카드) ~ 6(풀 하우스 이상)
int strengthLevel(HandScore score);

// "이번 판 순위"의 한 줄. 폴드한 사람은 순위가 없고(place 0) 카드도 담지 않는다.
struct HandRankRow {
    int seat = 0;
    int place = 0;
    bool folded = false;
    bool tie = false;                          // 같은 순위가 또 있다 (무승부)
    std::string handName;                      // 쇼다운에 참여했을 때만
    std::optional<std::array<Card, 2>> cards;  // 쇼다운에 참여했을 때만
    int delta = 0;                             // 이번 판의 칩 증감
};

// "현재 칩 순위"의 한 줄
struct StandingRow {
    int seat = 0;
    int rank = 0;
    bool tie = false;
    int chips = 0;
    std::optional<int> change;  // 이전 판보다 올라간 칸 수 (내려가면 음수). 첫 판은 빈 값
    bool out = false;           // 칩이 0 (탈락)
};

// 칩이 많은 순서. previousRanks는 좌석별 이전 순위 (없으면 빈 목록)
std::vector<StandingRow> buildStandings(const std::vector<int>& chips, const std::vector<int>& previousRanks);

// 쇼다운에 참여한 한 사람
struct ReviewEntry {
    int seat = 0;
    int place = 0;  // 1등부터. 동점이면 같은 등수
    std::array<Card, 2> hole;
    std::array<Card, 5> best;
    std::array<bool, 5> key{};  // 승부에 쓰인 카드 (족보를 이루는 카드 + 승부를 가른 카드)
    HandScore score = 0;
    std::string name;    // 족보 이름
    std::string detail;  // 숫자까지 붙인 설명 ("투 페어 K·9")
    // 바로 아래 순위와의 비교 (마지막 순위에는 없다)
    std::optional<HandCompare> versusNext;
    int nextSeat = -1;
};

struct ReviewWinner {
    int seat = 0;
    int amount = 0;
};

// 팟 하나의 정산
struct ReviewPot {
    int amount = 0;
    std::vector<ReviewWinner> winners;
    std::vector<int> eligible;  // 이 팟을 받을 자격이 있던 좌석
    std::vector<int> cappedBy;  // 사이드 팟이 생기게 한 올인 좌석 (메인 팟은 비어 있다)
    std::optional<HandCompare> compare;  // 승자와 가장 강한 패자의 비교 (동점뿐이면 Tie)
    int loserSeat = -1;                  // compare의 상대 (진 사람이 없으면 -1)
    bool oddChip = false;                // 똑같이 나눠지지 않아 남는 칩이 있었는가
};

struct HandReview {
    int handNumber = 0;
    bool showdown = false;
    std::vector<Card> board;
    std::vector<std::string> names;    // 좌석별 이름
    std::vector<ReviewEntry> entries;  // 강한 순. 쇼다운이 없으면 비어 있다
    std::vector<int> folded;           // 카드를 받았지만 폴드한 좌석
    std::vector<ReviewPot> pots;       // [0]이 메인 팟
    std::vector<HandRankRow> ranking;  // 이번 판 순위 (카드를 받은 사람 전원)
    std::vector<StandingRow> standings;  // 현재 칩 순위 (테이블 전원)
};

HandReview buildHandReview(const HandSummary& hand, const std::vector<std::string>& names,
                           const std::vector<int>& previousRanks = {});

// ---- 보는 사람 자신의 흐름 ("플랍에서는 앞섰는데...") ----
struct StreetStanding {
    std::string street;            // "flop", "turn", "river"
    std::string status;            // "ahead", "tied", "behind" (쇼다운에 남은 사람들끼리 비교)
    std::optional<double> equity;  // 힌트가 켜져 있으면 나중에 채운다
};

struct ReviewFlow {
    bool won = false;  // 팟을 조금이라도 가져갔는가
    std::vector<StreetStanding> streets;
};

// seat이 쇼다운에 참여했으면 단계별 우열을 돌려준다. 아니면 빈 값.
std::optional<ReviewFlow> buildReviewFlow(const HandReview& review, int seat);
