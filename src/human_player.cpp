// human_player.cpp - 콘솔 입력 플레이어 구현
#include "human_player.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "equity.h"
#include "evaluator.h"

namespace {

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> splitWords(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> words;
    std::string w;
    while (in >> w) words.push_back(w);
    return words;
}

// "300" 같은 숫자 문자열만 허용한다 (부호, 소수점, 글자 섞임은 거부).
bool parseAmount(const std::string& text, int& value) {
    if (text.empty() || text.size() > 9) return false;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    value = std::stoi(text);
    return true;
}

const char* streetName(Street s) {
    switch (s) {
        case Street::Preflop: return "프리플랍";
        case Street::Flop: return "플랍";
        case Street::Turn: return "턴";
        case Street::River: return "리버";
    }
    return "?";
}

std::string cardsToString(const std::vector<Card>& cards) {
    std::string s;
    for (const Card& c : cards) s += (s.empty() ? "" : " ") + cardToString(c);
    return s.empty() ? "(없음)" : s;
}

}  // namespace

bool parseHumanCommand(const std::string& line, const GameState& s, Action& out, std::string& error) {
    const std::vector<std::string> words = splitWords(toLower(line));
    if (words.empty()) {
        error = "입력이 비어 있습니다. f / c / r 금액 / a 중에서 고르세요.";
        return false;
    }
    const std::string& cmd = words[0];

    // 폴드
    if (cmd == "f" || cmd == "fold" || cmd == "폴드") {
        if (s.toCall == 0) {
            error = "콜 비용이 없으니 폴드할 필요가 없습니다. c로 체크하세요.";
            return false;
        }
        out = Action::fold();
        return true;
    }

    // 체크 / 콜
    if (cmd == "c" || cmd == "check" || cmd == "call" || cmd == "x" || cmd == "체크" || cmd == "콜") {
        out = s.toCall == 0 ? Action::check() : Action::call();
        return true;
    }

    // 올인
    if (cmd == "a" || cmd == "all" || cmd == "allin" || cmd == "올인") {
        // 레이즈 권리가 없는데 콜 금액을 넘는 올인은 레이즈가 되어 버리므로 막는다.
        if (!s.canRaise && s.maxRaiseTo > s.currentBet) {
            error = "지금은 레이즈할 수 없어서 올인할 수 없습니다. 콜(c)이나 폴드(f)만 가능합니다.";
            return false;
        }
        out = Action::allIn();
        return true;
    }

    // 벳 / 레이즈
    if (cmd == "r" || cmd == "raise" || cmd == "b" || cmd == "bet" || cmd == "레이즈" || cmd == "벳") {
        if (!s.canRaise) {
            error = "지금은 레이즈할 수 없습니다. 콜(c)이나 폴드(f)만 가능합니다.";
            return false;
        }
        if (words.size() < 2) {
            error = "금액을 같이 입력하세요. 예) r 300  (이번 라운드에 총 300까지 건다는 뜻)";
            return false;
        }
        int amount = 0;
        if (words[1] == "pot" || words[1] == "half") {
            // 팟 크기 비율: 콜한 뒤의 팟의 100% 또는 50%를 더 얹는다.
            double fraction = words[1] == "pot" ? 1.0 : 0.5;
            amount = s.currentBet + static_cast<int>(std::lround((s.pot + s.toCall) * fraction));
            amount = std::max(amount, s.minRaiseTo);
            amount = std::min(amount, s.maxRaiseTo);
        } else if (!parseAmount(words[1], amount)) {
            error = "금액은 0 이상의 숫자만 입력하세요. (예: r 300)";
            return false;
        }
        if (amount > s.maxRaiseTo) {
            error = "가진 칩보다 많습니다. 최대 " + std::to_string(s.maxRaiseTo) + "까지 가능합니다.";
            return false;
        }
        if (amount < s.minRaiseTo && amount != s.maxRaiseTo) {
            error = "최소 " + std::to_string(s.minRaiseTo) + "까지 올려야 합니다.";
            return false;
        }
        out = amount == s.maxRaiseTo ? Action::allIn() : Action::raiseTo(amount);
        return true;
    }

    error = "알 수 없는 입력입니다: \"" + line + "\"  (f / c / r 금액 / a)";
    return false;
}

void printTable(std::ostream& out, const GameState& s) {
    out << "\n==================== 제 " << s.handNumber << " 판 · " << streetName(s.street)
        << " ====================\n";
    out << "공용 카드: " << cardsToString(s.board) << "\n";
    for (int i = 0; i < s.numSeats; ++i) {
        const PlayerView& p = s.players[i];
        if (!p.inHand) continue;
        // 버튼과 블라인드는 테이블에서 눈에 보이는 정보라 항상 표시한다.
        // (UTG/HJ/CO 같은 이름은 힌트에서만 알려 준다.)
        std::string pos = positionName(s, i);
        if (pos != "BTN" && pos != "SB" && pos != "BB" && pos != "BTN/SB") pos = "";
        out << (i == s.mySeat ? " ▶ " : "   ") << std::left << std::setw(6) << pos << " "
            << p.name << "  칩 " << p.chips;
        if (p.bet > 0) out << "  베팅 " << p.bet;
        if (p.folded) out << "  [폴드]";
        else if (p.allIn) out << "  [올인]";
        out << "\n";
    }
    out << "내 카드: " << cardToString(s.myHole[0]) << " " << cardToString(s.myHole[1]);
    if (!s.board.empty()) {
        std::vector<Card> all = {s.myHole[0], s.myHole[1]};
        all.insert(all.end(), s.board.begin(), s.board.end());
        out << "   (현재 족보: " << categoryNameKo(categoryOf(evaluateBest(all))) << ")";
    }
    out << "\n팟: " << s.pot << "   콜 비용: " << s.toCall << "\n";
}

double potOdds(const GameState& s) {
    if (s.toCall <= 0) return 0.0;
    return static_cast<double>(s.toCall) / (s.pot + s.toCall);
}

void printHint(std::ostream& out, const GameState& s, double equity, int trials) {
    const int opponents = std::max(1, s.playersLeft() - 1);
    const double odds = potOdds(s);
    out << std::fixed << std::setprecision(1);
    out << "[힌트] 포지션 " << positionName(s, s.mySeat) << "  |  팟 " << s.pot << "  |  콜 비용 "
        << s.toCall << "  |  팟 오즈 " << odds * 100 << "%\n";
    out << "       현재 승률 " << equity * 100 << "% (상대 " << opponents << "명이 무작위 카드를 가졌다고 가정, "
        << trials << "회 시뮬레이션)\n";
    if (s.toCall == 0) {
        out << "       콜 비용이 없습니다. 체크하면 공짜로 다음 카드를 볼 수 있습니다.\n";
    } else if (equity > odds) {
        out << "       승률(" << equity * 100 << "%) > 팟 오즈(" << odds * 100 << "%) -> 콜하면 장기적으로 이득입니다.\n";
    } else {
        out << "       승률(" << equity * 100 << "%) < 팟 오즈(" << odds * 100 << "%) -> 콜은 손해입니다 (드로우라면 이후 베팅 가치를 더 고려).\n";
    }
    out << std::defaultfloat << std::setprecision(6);
}

HumanPlayer::HumanPlayer(std::string name, HumanOptions options, std::istream& in, std::ostream& out,
                         unsigned seed)
    : name_(std::move(name)), options_(options), in_(in), out_(out), rng_(seed) {}

Action HumanPlayer::decide(const GameState& s) {
    printTable(out_, s);

    // 힌트: --no-hint면 계산도 하지 않는다.
    if (options_.showHint) {
        const int opponents = std::max(1, s.playersLeft() - 1);
        const double equity = estimateEquity(s.myHole, s.board, opponents, options_.hintTrials, rng_);
        printHint(out_, s, equity, options_.hintTrials);
    }

    // 가능한 행동 안내
    out_ << "선택: [f]폴드  ";
    if (s.toCall == 0) out_ << "[c]체크  ";
    else out_ << "[c]콜 " << s.toCall << "  ";
    if (s.canRaise) {
        // 최소 레이즈가 내 칩보다 크면 올인만 가능하다.
        const int low = std::min(s.minRaiseTo, s.maxRaiseTo);
        out_ << "[r 금액]" << (s.currentBet == 0 ? "벳" : "레이즈") << " (" << low << " ~ "
             << s.maxRaiseTo << ", pot/half 가능)  ";
    }
    out_ << "[a]올인  [q]종료\n";

    while (true) {
        out_ << "> " << std::flush;
        std::string line;
        if (!std::getline(in_, line)) throw QuitRequested{};  // 입력 끝(EOF)
        std::string lower = toLower(line);
        if (lower == "q" || lower == "quit" || lower == "exit") throw QuitRequested{};

        Action action;
        std::string error;
        if (parseHumanCommand(line, s, action, error)) return action;
        out_ << "  ! " << error << "\n";
    }
}
