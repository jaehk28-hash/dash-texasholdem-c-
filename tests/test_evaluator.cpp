// test_evaluator.cpp - 카드/덱/족보 판정 테스트 (단순 assert 방식)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <map>
#include <random>
#include <sstream>

#include "card.h"
#include "check.h"
#include "evaluator.h"

// "As Kd 7h" 같은 문자열을 카드 목록으로 바꾸는 도우미
static std::vector<Card> H(const std::string& text) {
    std::vector<Card> cards;
    std::istringstream in(text);
    std::string tok;
    while (in >> tok) {
        Card c;
        if (!parseCard(tok, c)) {
            std::printf("카드 파싱 오류: %s\n", tok.c_str());
            std::exit(2);
        }
        cards.push_back(c);
    }
    return cards;
}

static HandScore S(const std::string& text) { return evaluateBest(H(text)); }
static HandCategory C(const std::string& text) { return categoryOf(S(text)); }

static void testCardsAndDeck() {
    Card c;
    CHECK(parseCard("As", c) && c.rank == 14 && c.suit == Suit::Spades);
    CHECK(parseCard("2c", c) && c.rank == 2 && c.suit == Suit::Clubs);
    CHECK(!parseCard("1s", c));
    CHECK(!parseCard("Ax", c));
    CHECK(!parseCard("Asd", c));
    CHECK(cardToString(Card{10, Suit::Diamonds}) == "Td");

    Deck deck;
    CHECK(deck.size() == 52);
    std::mt19937 rng(1);
    deck.shuffle(rng);
    std::map<std::string, int> seen;
    for (int i = 0; i < 52; ++i) ++seen[cardToString(deck.draw())];
    CHECK(seen.size() == 52);  // 52장이 모두 다르다
    CHECK(deck.size() == 0);

    // 같은 시드 -> 같은 순서
    Deck a, b;
    std::mt19937 r1(42), r2(42);
    a.shuffle(r1);
    b.shuffle(r2);
    CHECK(a.cards() == b.cards());

    Deck d2;
    CHECK(d2.remove(Card{14, Suit::Spades}) && d2.size() == 51);
    CHECK(!d2.remove(Card{14, Suit::Spades}));
}

static void testCategories() {
    CHECK(C("As Kd 9h 5c 2d") == HandCategory::HighCard);
    CHECK(C("As Ad 9h 5c 2d") == HandCategory::OnePair);
    CHECK(C("As Ad 9h 9c 2d") == HandCategory::TwoPair);
    CHECK(C("As Ad Ah 9c 2d") == HandCategory::ThreeOfAKind);
    CHECK(C("5s 6d 7h 8c 9d") == HandCategory::Straight);
    CHECK(C("As Ks 9s 5s 2s") == HandCategory::Flush);
    CHECK(C("As Ad Ah 9c 9d") == HandCategory::FullHouse);
    CHECK(C("As Ad Ah Ac 9d") == HandCategory::FourOfAKind);
    CHECK(C("5s 6s 7s 8s 9s") == HandCategory::StraightFlush);
    CHECK(C("Ts Js Qs Ks As") == HandCategory::StraightFlush);  // 로열
}

static void testWheel() {
    // 휠: A-2-3-4-5는 스트레이트이고 하이 카드는 5
    CHECK(C("As 2d 3h 4c 5d") == HandCategory::Straight);
    // 휠은 6-하이 스트레이트보다 약하다
    CHECK(S("As 2d 3h 4c 5d") < S("2s 3d 4h 5c 6d"));
    // 휠은 A-하이가 아니라 5-하이: 같은 휠끼리는 동점
    CHECK(S("As 2d 3h 4c 5d") == S("Ad 2s 3c 4h 5s"));
    // 휠도 하이 카드(A 하이)보다는 강하다
    CHECK(S("As 2d 3h 4c 5d") > S("As Kd Qh Jc 9d"));
    // 스트레이트 플러시 휠
    CHECK(C("As 2s 3s 4s 5s") == HandCategory::StraightFlush);
    CHECK(S("As 2s 3s 4s 5s") < S("2h 3h 4h 5h 6h"));
    CHECK(S("As 2s 3s 4s 5s") < S("Ts Js Qs Ks As"));
    // 모서리 감싸기(Q-K-A-2-3)는 스트레이트가 아니다
    CHECK(C("Qs Kd Ah 2c 3d") == HandCategory::HighCard);
    CHECK(C("Js Qd Kh Ac 2d") == HandCategory::HighCard);
    // 10-J-Q-K-A는 A-하이 스트레이트
    CHECK(S("Ts Jd Qh Kc Ad") > S("9s Td Jh Qc Kd"));
}

static void testKickers() {
    // 원 페어: 페어가 같으면 키커 순서대로
    CHECK(S("As Ad Kh 7c 2d") > S("Ah Ac Qh 7d 2s"));
    CHECK(S("As Ad Kh 7c 3d") > S("Ah Ac Kd 7d 2s"));  // 마지막 키커
    CHECK(S("As Ad Kh 7c 2d") == S("Ah Ac Kd 7d 2s"));  // 완전 동점
    // 페어 숫자가 높은 쪽이 이긴다
    CHECK(S("3s 3d Ah Kc Qd") > S("2s 2d Ah Kc Qd"));
    // 투 페어: 높은 페어 -> 낮은 페어 -> 키커
    CHECK(S("As Ad 3h 3c 2d") > S("Ks Kd Qh Qc Jd"));
    CHECK(S("Ks Kd 4h 4c 2d") > S("Ks Kd 3h 3c Ad"));
    CHECK(S("Ks Kd 4h 4c 3d") > S("Kh Kc 4d 4s 2d"));
    // 트리플은 키커 비교
    CHECK(S("7s 7d 7h Ac 2d") > S("7s 7d 7h Kc Qd"));
    // 풀 하우스: 트리플 숫자가 우선
    CHECK(S("3s 3d 3h 2c 2d") > S("2s 2d 2h Ac Ad"));
    // 포카드: 키커
    CHECK(S("9s 9d 9h 9c Ad") > S("9s 9d 9h 9c Kd"));
    // 플러시: 높은 카드부터 차례로
    CHECK(S("As Ks 9s 5s 3s") > S("As Ks 9s 5s 2s"));
    // 하이 카드
    CHECK(S("As Kd Qh Jc 9d") > S("As Kd Qh Jc 8d"));
    // 족보 서열
    CHECK(S("2s 2d 3h 3c 4d") > S("As Ad Kh Qc Jd"));
    CHECK(S("As Ks 9s 5s 2s") > S("5s 6d 7h 8c 9d"));
    CHECK(S("2s 2d 2h 3c 3d") > S("As Ks 9s 5s 2s"));
    CHECK(S("2s 2d 2h 2c 3d") > S("As Ad Ah Kc Kd"));
    CHECK(S("5s 6s 7s 8s 9s") > S("As Ad Ah Ac Kd"));
}

static void testSevenCards() {
    // 보드 플레이: 보드 5장이 최고면 두 사람은 동점
    CHECK(S("2c 3d As Ks Qs Jd 9h") == S("4c 5d As Ks Qs Jd 9h"));
    // 7장 중 최고 5장: 플러시와 스트레이트가 같이 있으면 플러시
    CHECK(C("2s 5s 9s Ks 3s 4d 6h") == HandCategory::Flush);
    // 트리플 둘 -> 풀 하우스 (높은 트리플 + 낮은 트리플의 페어)
    HandScore fh = S("As Ad Ah Ks Kd Kh 2c");
    CHECK(categoryOf(fh) == HandCategory::FullHouse);
    CHECK(fh == S("As Ad Ah Ks Kd 2c 3c"));
    // 페어 셋 -> 투 페어 (가장 높은 두 페어 + 남은 최고 키커)
    CHECK(S("As Ad Ks Kd Qs Qd 2c") == S("As Ad Ks Kd Qh 3c 2c"));
    CHECK(S("As Ad Ks Kd Qs Qd 2c") > S("As Ad Ks Kd 5h 3c 2c"));
    // 6장 연속 스트레이트: 가장 높은 것이 하이
    CHECK(S("4s 5d 6h 7c 8d 9s 2c") == S("5s 6d 7h 8c 9d 3s 2c"));
    // 7장 중 휠과 일반 스트레이트가 같이 있으면 높은 쪽
    CHECK(S("As 2d 3h 4c 5d 6s 7c") == S("3s 4d 5h 6c 7d 2s 9c"));
    // 스트레이트 플러시 + 포카드가 같이 있어도 스트레이트 플러시가 위
    CHECK(C("5s 6s 7s 8s 9s 9d 9h") == HandCategory::StraightFlush);
    // 플러시가 6장일 때 가장 높은 5장
    CHECK(C("As Ks Qs 9s 5s 2s 3d") == HandCategory::Flush);
    CHECK(S("As Ks Qs 9s 5s 2s 3d") == S("As Ks Qs 9s 5s 7d 8c"));
    // 5장, 6장 입력도 허용
    CHECK(C("As Ad 9h 5c 2d") == HandCategory::OnePair);
    CHECK(C("As Ad 9h 5c 2d 3d") == HandCategory::OnePair);
    // 잘못된 장수는 예외
    bool threw = false;
    try { evaluateBest(H("As Kd 9h 5c")); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

// 5장 모든 조합(2,598,960가지)의 족보별 개수가 알려진 값과 정확히 같은지 확인
static void testExactFiveCardCounts() {
    std::vector<Card> deck;
    for (int s = 0; s < 4; ++s)
        for (int r = 2; r <= 14; ++r) deck.push_back(Card{r, static_cast<Suit>(s)});

    long long counts[9] = {};
    Card h[5];
    for (int a = 0; a < 48; ++a) { h[0] = deck[a];
    for (int b = a + 1; b < 49; ++b) { h[1] = deck[b];
    for (int c = b + 1; c < 50; ++c) { h[2] = deck[c];
    for (int d = c + 1; d < 51; ++d) { h[3] = deck[d];
    for (int e = d + 1; e < 52; ++e) { h[4] = deck[e];
        ++counts[static_cast<int>(categoryOf(evaluate5(h)))];
    }}}}}
    // 하이카드, 원페어, 투페어, 트리플, 스트레이트, 플러시, 풀하우스, 포카드, 스플(로열 포함)
    const long long expected[9] = {1302540, 1098240, 123552, 54912, 10200, 5108, 3744, 624, 40};
    for (int i = 0; i < 9; ++i) CHECK(counts[i] == expected[i]);
}

// 무작위 5장을 많이 뽑아 원 페어 비율이 이론값(약 42.26%)에 가까운지 확인
static void testOnePairProbability() {
    std::mt19937 rng(20240101);
    const int trials = 300000;
    int pairs = 0;
    for (int i = 0; i < trials; ++i) {
        Deck deck;
        deck.shuffle(rng);
        Card h[5];
        for (auto& c : h) c = deck.draw();
        if (categoryOf(evaluate5(h)) == HandCategory::OnePair) ++pairs;
    }
    double p = static_cast<double>(pairs) / trials;
    std::printf("원 페어 시뮬레이션 비율: %.4f (이론값 0.4226)\n", p);
    CHECK(std::fabs(p - 0.4226) < 0.005);
}

// 빠른 판정 방식이 전수 검사(레퍼런스)와 항상 같은 점수를 내는지 무작위로 대조한다.
static void testFastMatchesBruteForce() {
    std::mt19937 rng(77);
    int mismatches = 0;
    for (int n = 5; n <= 7; ++n) {
        for (int i = 0; i < 150000; ++i) {
            Deck deck;
            deck.shuffle(rng);
            std::vector<Card> cards;
            for (int k = 0; k < n; ++k) cards.push_back(deck.draw());
            if (evaluateBest(cards) != evaluateBestBruteForce(cards)) ++mismatches;
        }
    }
    CHECK(mismatches == 0);

    // 족보가 골고루 나오도록 작은 숫자 범위/한 무늬로 편향된 덱에서도 대조한다
    // (페어, 풀 하우스, 플러시, 스트레이트가 많이 나온다).
    for (int i = 0; i < 100000; ++i) {
        std::vector<Card> pool;
        const int maxRank = 5 + static_cast<int>(rng() % 6);  // 2..maxRank만 사용
        for (int s = 0; s < 4; ++s)
            for (int r = 2; r <= maxRank; ++r) pool.push_back(Card{r, static_cast<Suit>(s)});
        if (rng() % 2) pool.push_back(Card{14, Suit::Spades});
        for (std::size_t j = pool.size(); j > 1; --j) std::swap(pool[j - 1], pool[rng() % j]);
        if (pool.size() < 7) continue;
        pool.resize(7);
        if (evaluateBest(pool) != evaluateBestBruteForce(pool)) ++mismatches;
    }
    CHECK(mismatches == 0);
}

int main() {
    testCardsAndDeck();
    testCategories();
    testWheel();
    testKickers();
    testSevenCards();
    testExactFiveCardCounts();
    testOnePairProbability();
    testFastMatchesBruteForce();
    std::printf("%d개 검사 중 %d개 실패\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
