// card.h - 카드와 덱
#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

// 무늬. 홀덤에서는 무늬끼리 우열이 없다.
enum class Suit : std::uint8_t { Clubs, Diamonds, Hearts, Spades };

// 카드 한 장. rank는 2~14 (J=11, Q=12, K=13, A=14).
struct Card {
    int rank = 2;
    Suit suit = Suit::Clubs;

    bool operator==(const Card& o) const { return rank == o.rank && suit == o.suit; }
    bool operator!=(const Card& o) const { return !(*this == o); }
};

// "As", "Td", "7h" 같은 두 글자 문자열을 카드로 바꾼다. 잘못된 입력이면 false.
bool parseCard(const std::string& text, Card& out);

// 카드를 "As" 형태의 문자열로 바꾼다.
std::string cardToString(const Card& card);

// 52장 덱. 매 판 새로 만들어 셔플해서 쓴다.
class Deck {
public:
    // 52장이 순서대로 들어 있는 덱을 만든다.
    Deck();

    // 52장으로 되돌린다 (이미 뽑은 카드도 다시 채운다).
    void reset();

    // 주어진 난수 생성기로 섞는다. 시드가 같으면 결과도 같다.
    void shuffle(std::mt19937& rng);

    // 맨 위 카드 한 장을 뽑는다. 비어 있으면 std::out_of_range 예외.
    Card draw();

    // 번 카드: 맨 위 카드를 한 장 버린다.
    void burn() { draw(); }

    // 남은 카드 수.
    std::size_t size() const { return cards_.size(); }

    // 특정 카드를 덱에서 제거한다 (몬테카를로에서 이미 보이는 카드를 빼는 용도).
    // 제거했으면 true.
    bool remove(const Card& card);

    const std::vector<Card>& cards() const { return cards_; }

    // 테스트용: 주어진 카드들이 앞에서부터 순서대로 뽑히도록 덱을 맞춘다.
    // 나머지 카드는 그 뒤에 (기본 순서로) 이어 붙는다.
    void stack(const std::vector<Card>& drawOrder);

private:
    std::vector<Card> cards_;  // 뒤쪽이 맨 위
};
