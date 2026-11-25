// card.cpp - 카드와 덱 구현
#include "card.h"

#include <algorithm>
#include <stdexcept>

namespace {
const char kRankChars[] = "23456789TJQKA";  // 인덱스 0 -> 숫자 2
const char kSuitChars[] = "cdhs";
}  // namespace

bool parseCard(const std::string& text, Card& out) {
    if (text.size() != 2) return false;
    int rank = 0;
    for (int i = 0; i < 13; ++i) {
        if (kRankChars[i] == text[0]) rank = i + 2;
    }
    int suit = -1;
    for (int i = 0; i < 4; ++i) {
        if (kSuitChars[i] == text[1]) suit = i;
    }
    if (rank == 0 || suit < 0) return false;
    out.rank = rank;
    out.suit = static_cast<Suit>(suit);
    return true;
}

std::string cardToString(const Card& card) {
    std::string s;
    s += kRankChars[card.rank - 2];
    s += kSuitChars[static_cast<int>(card.suit)];
    return s;
}

Deck::Deck() { reset(); }

void Deck::reset() {
    cards_.clear();
    for (int s = 0; s < 4; ++s) {
        for (int r = 2; r <= 14; ++r) {
            cards_.push_back(Card{r, static_cast<Suit>(s)});
        }
    }
}

void Deck::shuffle(std::mt19937& rng) {
    // std::shuffle은 구현마다 결과가 달라질 수 있어서,
    // 시드 재현성을 위해 직접 Fisher-Yates로 섞는다.
    for (std::size_t i = cards_.size(); i > 1; --i) {
        std::uniform_int_distribution<std::size_t> dist(0, i - 1);
        std::swap(cards_[i - 1], cards_[dist(rng)]);
    }
}

Card Deck::draw() {
    if (cards_.empty()) throw std::out_of_range("덱이 비어 있습니다");
    Card c = cards_.back();
    cards_.pop_back();
    return c;
}

bool Deck::remove(const Card& card) {
    auto it = std::find(cards_.begin(), cards_.end(), card);
    if (it == cards_.end()) return false;
    cards_.erase(it);
    return true;
}

void Deck::stack(const std::vector<Card>& drawOrder) {
    Deck rest;  // 52장 전체
    for (const Card& c : drawOrder) rest.remove(c);
    // cards_의 뒤쪽이 맨 위이므로, 나머지를 먼저 깔고 지정한 카드를 역순으로 얹는다.
    cards_ = rest.cards_;
    for (auto it = drawOrder.rbegin(); it != drawOrder.rend(); ++it) cards_.push_back(*it);
}
