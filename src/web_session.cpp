// web_session.cpp - 게임 방 구현
#include "web_session.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bots.h"
#include "equity.h"
#include "human_player.h"
#include "web_json.h"

namespace {

constexpr std::size_t kLogLines = 60;  // 화면에 보관하는 로그 줄 수

// 게임 스레드를 중간에 빠져나오게 할 때 던지는 예외
struct StopRequested {};

// 접속 토큰: 추측하기 어렵게 32자리 16진수 난수
std::string makeToken() {
    std::random_device rd;
    std::string token;
    for (int i = 0; i < 4; ++i) {
        char buf[9];
        std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(rd()));
        token += buf;
    }
    return token;
}

// 방 코드: 친구에게 불러 주기 쉬운 6자리 숫자
std::string makeCode() {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 999999);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06d", dist(rd));
    return buf;
}

std::string cardsJson(const std::array<Card, 2>& cards) {
    return "[" + jsonString(cardToString(cards[0])) + "," + jsonString(cardToString(cards[1])) + "]";
}

const char* boolJson(bool b) { return b ? "true" : "false"; }

}  // namespace

// ---------------------------------------------------------------------------
// 설정과 닉네임 검사
// ---------------------------------------------------------------------------
bool validateSettings(const RoomSettings& s, bool solo, std::string& error) {
    const GameConfig& c = s.config;
    if (c.startingChips < 100 || c.startingChips > 1000000) {
        error = "시작 칩은 100 ~ 1,000,000 사이여야 합니다.";
    } else if (c.bigBlind < 2 || c.bigBlind > c.startingChips) {
        error = "빅 블라인드는 2 이상, 시작 칩 이하여야 합니다.";
    } else if (c.smallBlind < 1 || c.smallBlind > c.bigBlind) {
        error = "스몰 블라인드는 1 이상, 빅 블라인드 이하여야 합니다.";
    } else if (s.bots < (solo ? 1 : 0) || s.bots > kSeats - 1) {
        error = solo ? "봇은 1 ~ 5명이어야 합니다." : "봇은 0 ~ 5명이어야 합니다.";
    } else if (solo ? s.turnSeconds != 0 : (s.turnSeconds < 10 || s.turnSeconds > 300)) {
        error = "제한 시간은 10 ~ 300초여야 합니다.";
    } else {
        return true;
    }
    return false;
}

bool cleanNickname(std::string& name, std::string& error) {
    const std::size_t first = name.find_first_not_of(' ');
    const std::size_t last = name.find_last_not_of(' ');
    name = first == std::string::npos ? "" : name.substr(first, last - first + 1);

    error = "닉네임은 한글, 영문, 숫자로 1~10자까지 가능합니다.";
    int length = 0;
    for (std::size_t i = 0; i < name.size(); ++length) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x80) {
            const bool ok = std::isalnum(c) || c == ' ' || c == '_' || c == '-';
            if (!ok) return false;
            ++i;
            continue;
        }
        // 한글은 UTF-8에서 3바이트
        if ((c & 0xF0) != 0xE0 || i + 2 >= name.size()) return false;
        const unsigned char c1 = static_cast<unsigned char>(name[i + 1]);
        const unsigned char c2 = static_cast<unsigned char>(name[i + 2]);
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return false;
        const unsigned code = ((c & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
        const bool hangul = (code >= 0xAC00 && code <= 0xD7A3) || (code >= 0x3131 && code <= 0x318E);
        if (!hangul) return false;
        i += 3;
    }
    if (length < 1 || length > 10) return false;
    error.clear();
    return true;
}

// ---------------------------------------------------------------------------
// 플레이어 두 종류
// ---------------------------------------------------------------------------

// 콘솔 HumanPlayer 대신, 웹 요청을 기다리는 사람 플레이어
class WebHumanPlayer : public Player {
public:
    WebHumanPlayer(Room& room, int seat, std::string name)
        : room_(room), seat_(seat), name_(std::move(name)) {}
    std::string name() const override { return name_; }
    Action decide(const GameState& state) override { return room_.waitForAction(seat_, state); }

private:
    Room& room_;
    int seat_;
    std::string name_;
};

// 봇을 감싸서, 행동하기 전에 잠깐 쉬게 한다 (화면에서 진행이 보이도록).
class PacedPlayer : public Player {
public:
    PacedPlayer(Room& room, std::unique_ptr<Player> bot) : room_(room), bot_(std::move(bot)) {}
    std::string name() const override { return bot_->name(); }
    Action decide(const GameState& state) override {
        room_.think(state.mySeat);
        return bot_->decide(state);
    }

private:
    Room& room_;
    std::unique_ptr<Player> bot_;
};

// ---------------------------------------------------------------------------
// 행동 검사
// ---------------------------------------------------------------------------
bool buildWebAction(const std::string& name, std::optional<long long> amount, const GameState& s,
                    Action& out, std::string& error) {
    // 규칙 검사는 콘솔 모드의 parseHumanCommand를 그대로 재사용한다.
    if (name == "fold") {
        if (s.toCall == 0) {
            error = "콜 비용이 없으니 폴드할 필요가 없습니다. 체크하세요.";
            return false;
        }
        return parseHumanCommand("f", s, out, error);
    }
    if (name == "check") {
        if (s.toCall != 0) {
            error = "콜해야 하는 상황이라 체크할 수 없습니다.";
            return false;
        }
        out = Action::check();
        return true;
    }
    if (name == "call") {
        if (s.toCall == 0) {
            error = "콜할 금액이 없습니다.";
            return false;
        }
        out = Action::call();
        return true;
    }
    if (name == "raise") {
        if (!amount || *amount < 0 || *amount > 999999999) {
            error = "레이즈 금액(amount)은 0 이상의 정수여야 합니다.";
            return false;
        }
        return parseHumanCommand("r " + std::to_string(*amount), s, out, error);
    }
    if (name == "allin") return parseHumanCommand("a", s, out, error);
    error = "알 수 없는 행동입니다.";
    return false;
}

// ---------------------------------------------------------------------------
// JSON 만들기
// ---------------------------------------------------------------------------
namespace {

std::string cardListJson(const Card* cards, std::size_t count) {
    std::string j = "[";
    for (std::size_t i = 0; i < count; ++i) j += (i ? "," : "") + jsonString(cardToString(cards[i]));
    return j + "]";
}

std::string intListJson(const std::vector<int>& values) {
    std::string j = "[";
    for (std::size_t i = 0; i < values.size(); ++i) j += (i ? "," : "") + std::to_string(values[i]);
    return j + "]";
}

std::string bestHandJson(const std::optional<BestHand>& best) {
    if (!best) return ",\"best5\":[],\"hand\":\"\",\"label\":\"\",\"groups\":[]";
    std::string j = ",\"best5\":[";
    for (std::size_t i = 0; i < best->cards.size(); ++i) {
        const BestCard& c = best->cards[i];
        j += i ? "," : "";
        j += "{\"card\":" + jsonString(cardToString(c.card));
        j += ",\"group\":" + (c.group ? jsonString(std::string(1, c.group)) : std::string("null"));
        j += std::string(",\"hole\":") + boolJson(c.hole) + "}";
    }
    j += "],\"hand\":" + jsonString(best->code) + ",\"label\":" + jsonString(best->label) + ",\"groups\":[";
    for (std::size_t i = 0; i < best->groups.size(); ++i) {
        j += i ? "," : "";
        j += "{\"id\":" + jsonString(std::string(1, best->groups[i].id)) +
             ",\"label\":" + jsonString(best->groups[i].label) + "}";
    }
    return j + "]";
}

std::string compareJson(const std::optional<HandCompare>& c) {
    if (!c) return "null";
    const char* type = c->type == HandCompare::Type::Category ? "category"
                       : c->type == HandCompare::Type::Rank   ? "rank"
                                                              : "tie";
    return std::string("{\"type\":\"") + type + "\",\"role\":" + jsonString(c->role) +
           ",\"winRank\":" + std::to_string(c->winRank) +
           ",\"loseRank\":" + std::to_string(c->loseRank) + ",\"shared\":" + intListJson(c->shared) +
           ",\"boardPlays\":" + boolJson(c->boardPlays) + "}";
}

}  // namespace

bool parsePace(const std::string& text, Pace& out) {
    if (text == "slow") out = Pace::Slow;
    else if (text == "normal") out = Pace::Normal;
    else if (text == "fast") out = Pace::Fast;
    else return false;
    return true;
}

const char* paceName(Pace pace) {
    return pace == Pace::Slow ? "slow" : pace == Pace::Normal ? "normal" : "fast";
}

std::string reviewToJson(const HandReview& r, const ReviewFlow* flow) {
    std::string j = "{\"handNumber\":" + std::to_string(r.handNumber);
    j += std::string(",\"showdown\":") + boolJson(r.showdown);
    j += ",\"board\":" + cardListJson(r.board.data(), r.board.size());
    j += ",\"names\":[";
    for (std::size_t i = 0; i < r.names.size(); ++i) j += (i ? "," : "") + jsonString(r.names[i]);
    j += "],\"entries\":[";
    for (std::size_t i = 0; i < r.entries.size(); ++i) {
        const ReviewEntry& e = r.entries[i];
        j += i ? "," : "";
        j += "{\"seat\":" + std::to_string(e.seat) + ",\"place\":" + std::to_string(e.place);
        j += ",\"hole\":" + cardListJson(e.hole.data(), e.hole.size());
        j += ",\"best\":" + cardListJson(e.best.data(), e.best.size());
        j += ",\"key\":[";
        for (std::size_t k = 0; k < e.key.size(); ++k) j += std::string(k ? "," : "") + boolJson(e.key[k]);
        j += "],\"category\":" + std::to_string(static_cast<int>(categoryOf(e.score)));
        j += ",\"handName\":" + jsonString(e.name);
        j += ",\"detail\":" + jsonString(e.detail);
        j += ",\"nextSeat\":" + std::to_string(e.nextSeat);
        j += ",\"versusNext\":" + compareJson(e.versusNext) + "}";
    }
    j += "],\"folded\":" + intListJson(r.folded);
    j += ",\"pots\":[";
    for (std::size_t i = 0; i < r.pots.size(); ++i) {
        const ReviewPot& p = r.pots[i];
        j += i ? "," : "";
        j += "{\"amount\":" + std::to_string(p.amount) + ",\"winners\":[";
        for (std::size_t w = 0; w < p.winners.size(); ++w) {
            j += w ? "," : "";
            j += "{\"seat\":" + std::to_string(p.winners[w].seat) +
                 ",\"amount\":" + std::to_string(p.winners[w].amount) + "}";
        }
        j += "],\"eligible\":" + intListJson(p.eligible);
        j += ",\"cappedBy\":" + intListJson(p.cappedBy);
        j += ",\"loserSeat\":" + std::to_string(p.loserSeat);
        j += std::string(",\"oddChip\":") + boolJson(p.oddChip);
        j += ",\"compare\":" + compareJson(p.compare) + "}";
    }
    j += "],\"ranking\":[";
    for (std::size_t i = 0; i < r.ranking.size(); ++i) {
        const HandRankRow& row = r.ranking[i];
        j += i ? "," : "";
        j += "{\"seat\":" + std::to_string(row.seat) + ",\"place\":" + std::to_string(row.place);
        j += std::string(",\"folded\":") + boolJson(row.folded) + ",\"tie\":" + boolJson(row.tie);
        j += ",\"handName\":" + jsonString(row.handName);
        j += ",\"cards\":" + (row.cards ? cardListJson(row.cards->data(), row.cards->size()) : std::string("[]"));
        j += ",\"delta\":" + std::to_string(row.delta) + bestHandJson(row.best) + "}";
    }
    j += "],\"standings\":[";
    for (std::size_t i = 0; i < r.standings.size(); ++i) {
        const StandingRow& row = r.standings[i];
        j += i ? "," : "";
        j += "{\"seat\":" + std::to_string(row.seat) + ",\"rank\":" + std::to_string(row.rank);
        j += std::string(",\"tie\":") + boolJson(row.tie) + ",\"chips\":" + std::to_string(row.chips);
        j += ",\"change\":" + (row.change ? std::to_string(*row.change) : std::string("null"));
        j += std::string(",\"out\":") + boolJson(row.out) + "}";
    }
    j += "],\"me\":";
    if (!flow) return j + "null}";
    j += std::string("{\"won\":") + boolJson(flow->won) + ",\"streets\":[";
    for (std::size_t i = 0; i < flow->streets.size(); ++i) {
        const StreetStanding& s = flow->streets[i];
        j += i ? "," : "";
        j += "{\"street\":" + jsonString(s.street) + ",\"status\":" + jsonString(s.status);
        j += ",\"equity\":" + (s.equity ? std::to_string(*s.equity) : std::string("null")) + "}";
    }
    return j + "]}}";
}

std::string viewToJson(const RoomView& v) {
    const GameState& s = v.table.state;
    const int seats = static_cast<int>(s.players.size());
    const char* phase = v.phase == RoomPhase::Lobby     ? "lobby"
                        : v.phase == RoomPhase::Playing ? "playing"
                                                        : "over";

    std::string j = "{\"hasGame\":true";
    j += std::string(",\"phase\":\"") + phase + "\"";
    j += ",\"code\":" + jsonString(v.code);
    j += std::string(",\"solo\":") + boolJson(v.solo);
    j += std::string(",\"isHost\":") + boolJson(v.isHost);
    j += ",\"settings\":{\"chips\":" + std::to_string(v.settings.config.startingChips) +
         ",\"sb\":" + std::to_string(v.settings.config.smallBlind) +
         ",\"bb\":" + std::to_string(v.settings.config.bigBlind) +
         ",\"bots\":" + std::to_string(v.settings.bots) +
         ",\"hint\":" + boolJson(v.settings.hint) +
         ",\"turnSeconds\":" + std::to_string(v.settings.turnSeconds) + "}";

    j += ",\"members\":[";
    for (std::size_t i = 0; i < v.members.size(); ++i) {
        j += i ? "," : "";
        j += "{\"name\":" + jsonString(v.members[i].name);
        j += std::string(",\"isHost\":") + boolJson(v.members[i].isHost);
        j += std::string(",\"isMe\":") + boolJson(v.members[i].isMe) + "}";
    }
    j += "]";

    j += ",\"handNumber\":" + std::to_string(s.handNumber);
    j += ",\"pot\":" + std::to_string(v.handOver || v.phase == RoomPhase::Over ? 0 : s.pot);
    j += std::string(",\"myTurn\":") + boolJson(v.myTurn);
    j += ",\"turnSeat\":" + std::to_string(v.turnSeat);
    j += ",\"actingSeat\":" + std::to_string(v.actingSeat);
    j += ",\"actionPhase\":" + jsonString(v.actionPhase);
    j += ",\"lastAction\":";
    if (v.lastAction) {
        j += "{\"seq\":" + std::to_string(v.lastAction->seq) + ",\"seat\":" +
             std::to_string(v.lastAction->seat) + ",\"text\":" + jsonString(v.lastAction->text) + "}";
    } else {
        j += "null";
    }
    j += ",\"myHand\":";
    if (v.myHand) {
        j += "{\"name\":" + jsonString(v.myHand->name) + ",\"code\":" + jsonString(v.myHand->code) + ",\"level\":" + std::to_string(v.myHand->level) +
             ",\"boardOnly\":" + boolJson(v.myHand->boardOnly) + "}";
    } else {
        j += "null";
    }
    j += std::string(",\"pace\":\"") + paceName(v.pace) + "\"";
    j += ",\"waiting\":";
    if (v.waiting != Waiting::None) {
        const char* next = v.waiting == Waiting::Flop       ? "flop"
                           : v.waiting == Waiting::Turn     ? "turn"
                           : v.waiting == Waiting::River    ? "river"
                           : v.waiting == Waiting::Showdown ? "showdown"
                                                            : "nextHand";
        j += std::string("{\"next\":\"") + next + "\",\"gateId\":" + std::to_string(v.gateId) +
             ",\"canContinue\":" + boolJson(v.canContinue) + "}";
    } else {
        j += "null";
    }
    j += std::string(",\"canSkip\":") + boolJson(v.canSkip) + ",\"skipping\":" + boolJson(v.skipping);
    j += ",\"turnId\":" + std::to_string(v.turnId);
    j += ",\"secondsLeft\":" + std::to_string(v.secondsLeft);
    j += std::string(",\"handOver\":") + boolJson(v.handOver);
    j += ",\"message\":" + jsonString(v.message);

    j += ",\"board\":[";
    for (std::size_t i = 0; i < s.board.size(); ++i) {
        j += (i ? "," : "") + jsonString(cardToString(s.board[i]));
    }
    j += "]";

    j += std::string(",\"myCards\":") + (v.table.holeDealt ? cardsJson(s.myHole) : "[]");

    j += ",\"players\":[";
    for (int i = 0; i < seats; ++i) {
        const PlayerView& p = s.players[i];
        const bool me = i == s.mySeat;
        // 남의 카드는 revealed에 들어 있을 때(쇼다운)만 응답에 넣는다.
        std::string cards = "[]";
        if (me && v.table.holeDealt) cards = cardsJson(s.myHole);
        else if (i < static_cast<int>(v.revealed.size()) && v.revealed[i]) cards = cardsJson(*v.revealed[i]);

        std::string position = positionName(s, i);
        if (position == "-") position = "";

        j += i ? "," : "";
        j += "{\"seat\":" + std::to_string(i);
        j += ",\"name\":" + jsonString(p.name);
        j += ",\"chips\":" + std::to_string(p.chips);
        j += ",\"bet\":" + std::to_string(v.handOver ? 0 : p.bet);
        j += std::string(",\"inHand\":") + boolJson(p.inHand);
        j += std::string(",\"folded\":") + boolJson(p.folded);
        j += std::string(",\"allIn\":") + boolJson(p.allIn);
        j += std::string(",\"isMe\":") + boolJson(me);
        j += ",\"position\":" + jsonString(position);
        j += ",\"cards\":" + cards + "}";
    }
    j += "]";

    // 가능한 행동 (내 차례일 때만)
    j += ",\"actions\":";
    if (v.myTurn) {
        const bool allIn = s.canRaise || s.maxRaiseTo <= s.currentBet;
        j += std::string("{\"fold\":") + boolJson(s.toCall > 0);
        j += std::string(",\"check\":") + boolJson(s.toCall == 0);
        j += std::string(",\"call\":") + boolJson(s.toCall > 0);
        j += ",\"callAmount\":" + std::to_string(s.toCall);
        j += std::string(",\"raise\":") + boolJson(s.canRaise);
        // 최소 레이즈가 내 칩보다 크면 올인 금액만 가능하다.
        j += ",\"minRaiseTo\":" + std::to_string(std::min(s.minRaiseTo, s.maxRaiseTo));
        j += ",\"maxRaiseTo\":" + std::to_string(s.maxRaiseTo);
        j += ",\"currentBet\":" + std::to_string(s.currentBet);
        j += std::string(",\"allIn\":") + boolJson(allIn) + "}";
    } else {
        j += "null";
    }

    // 승률 힌트
    j += ",\"hint\":";
    if (v.myTurn && v.equity) {
        j += "{\"equity\":" + std::to_string(*v.equity);
        j += ",\"potOdds\":" + std::to_string(potOdds(s));
        j += ",\"opponents\":" + std::to_string(v.opponents) + "}";
    } else {
        j += "null";
    }

    j += ",\"log\":[";
    bool first = true;
    for (const std::string& line : v.log) {
        j += (first ? "" : ",") + jsonString(line);
        first = false;
    }
    j += "]";

    // 지난 판 리뷰 (카드 목록이 들어 있으므로 현재 판 정보와 섞이지 않게 맨 뒤에 둔다)
    j += ",\"review\":";
    j += v.review ? reviewToJson(*v.review, v.reviewFlow ? &*v.reviewFlow : nullptr) : "null";
    j += "}";
    return j;
}

// ---------------------------------------------------------------------------
// Room: 들어오기, 나가기, 시작, 행동
// ---------------------------------------------------------------------------
Room::Room(const WebOptions& options, const RoomSettings& settings, std::string code, bool solo,
           unsigned seed)
    : options_(options), settings_(settings), code_(std::move(code)), solo_(solo), nextSeed_(seed),
      hintRng_(seed + 1) {
    pace_ = settings_.pace;}

Room::~Room() {
    stop();
    std::lock_guard<std::mutex> startLock(startMutex_);
    if (thread_.joinable()) thread_.join();
}

void Room::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        turnSeat_ = -1;
    }
    cv_.notify_all();
}

const Room::Member* Room::findMember(const std::string& token) const {
    for (const Member& m : members_) {
        if (!m.left && m.token == token) return &m;
    }
    return nullptr;
}

void Room::addLog(const std::string& line) {
    log_.push_back(line);
    while (log_.size() > kLogLines) log_.pop_front();
}

bool Room::join(const std::string& token, std::string name, std::string& error) {
    if (!cleanNickname(name, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != RoomPhase::Lobby) {
        error = "이미 게임이 시작된 방입니다.";
        return false;
    }
    if (static_cast<int>(members_.size()) >= std::min(options_.maxPlayers, kSeats)) {
        error = "이 방은 인원이 가득 찼어요.";
        return false;
    }
    // 로그에서 구별해야 하므로 같은 이름과 봇 이름은 쓸 수 없다.
    for (const char* bot : {"타이트", "루즈", "공격형", "콜링스테이션", "확률계산"}) {
        if (name == bot) {
            error = "봇과 같은 이름은 쓸 수 없습니다.";
            return false;
        }
    }
    for (const Member& m : members_) {
        if (m.name == name) {
            error = "같은 닉네임을 쓰는 사람이 있습니다.";
            return false;
        }
    }
    members_.push_back(Member{token, name, -1, false});
    return true;
}

void Room::leave(const std::string& token) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = members_.begin(); it != members_.end(); ++it) {
            if (it->left || it->token != token) continue;
            if (phase_ == RoomPhase::Lobby) {
                members_.erase(it);
            } else {
                it->left = true;  // 좌석은 남겨 두고 자동으로 체크/폴드시킨다
                if (phase_ == RoomPhase::Playing) addLog(it->name + ": 방에서 나갔습니다");
            }
            break;
        }
    }
    cv_.notify_all();  // 나간 사람의 차례를 기다리고 있었다면 깨운다
}

bool Room::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::all_of(members_.begin(), members_.end(), [](const Member& m) { return m.left; });
}

bool Room::start(const std::string& token, std::string& error) {
    std::lock_guard<std::mutex> startLock(startMutex_);
    std::vector<std::string> names;
    int botCount = 0;
    unsigned seed = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Member* me = findMember(token);
        // 방장 = 남아 있는 사람 중 가장 먼저 들어온 사람
        const auto host = std::find_if(members_.begin(), members_.end(),
                                       [](const Member& m) { return !m.left; });
        if (stopping_ || !me || host == members_.end() || host->token != token) {
            error = "방장만 게임을 시작할 수 있습니다.";
            return false;
        }
        if (phase_ == RoomPhase::Playing) {
            error = "이미 게임이 진행 중입니다.";
            return false;
        }
        const int humans = static_cast<int>(std::count_if(
            members_.begin(), members_.end(), [](const Member& m) { return !m.left; }));
        botCount = std::max(0, std::min(settings_.bots, std::min(options_.maxPlayers, kSeats) - humans));
        if (humans + botCount < 2) {
            error = "2명 이상이어야 합니다. 친구를 기다리거나 봇이 있는 방을 만드세요.";
            return false;
        }

        // 나간 사람을 지우고 들어온 순서대로 앉힌다.
        members_.erase(std::remove_if(members_.begin(), members_.end(),
                                      [](const Member& m) { return m.left; }),
                       members_.end());
        for (std::size_t i = 0; i < members_.size(); ++i) {
            members_[i].seat = static_cast<int>(i);
            names.push_back(members_[i].name);
        }
        phase_ = RoomPhase::Playing;
        handOver_ = false;
        turnSeat_ = -1;
        pending_.reset();
        tables_.clear();
        revealed_.clear();
        review_.reset();
        flows_.clear();
        actingSeat_ = -1;
        actionPhase_.clear();
        lastAction_.reset();
        waiting_ = Waiting::None;
        skip_ = false;
        holdAction_ = false;
        log_.clear();
        seed = nextSeed_++;
    }
    // 끝난 게임의 스레드를 정리하고 새로 시작한다 (mutex_를 풀고 기다려야 한다).
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this, names, botCount, seed] { run(names, botCount, seed); });
    if (options_.stats) options_.stats->gameStarted(solo_, settings_.bots);
    return true;
}

bool Room::submit(const std::string& token, const std::string& action, std::optional<long long> amount,
                  std::optional<long long> turnId, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Member* me = findMember(token);
        if (stopping_ || !me || turnSeat_ < 0 || me->seat != turnSeat_ || pending_) {
            error = "지금은 내 차례가 아닙니다.";
            return false;
        }
        // 제한 시간이 지난 뒤 늦게 도착한 요청이 다음 차례에 적용되지 않게 한다.
        if (turnId && *turnId != turnId_) {
            error = "이미 지나간 차례입니다.";
            return false;
        }
        Action chosen;
        if (!buildWebAction(action, amount, turnState_, chosen, error)) return false;
        pending_ = chosen;
    }
    cv_.notify_all();  // 기다리던 게임 스레드를 깨운다
    return true;
}

std::optional<RoomView> Room::view(const std::string& token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Member* me = findMember(token);
    if (!me) return std::nullopt;

    RoomView v;
    v.phase = phase_;
    v.code = code_;
    v.solo = solo_;
    v.settings = settings_;
    bool first = true;
    for (const Member& m : members_) {
        if (m.left) continue;
        v.members.push_back(MemberInfo{m.name, first, m.token == token});
        if (first && m.token == token) v.isHost = true;
        first = false;
    }
    if (phase_ == RoomPhase::Lobby) return v;

    if (me->seat >= 0 && me->seat < static_cast<int>(tables_.size())) {
        v.table = tables_[me->seat];  // 내 시점의 테이블만 복사한다
    }
    v.revealed = revealed_;
    v.handOver = handOver_;
    v.actingSeat = actingSeat_;
    v.actionPhase = actionPhase_;
    v.lastAction = lastAction_;
    v.pace = pace_;
    // 이미 "다음"을 눌렀으면 게임 스레드가 깨어나기 전이라도 대기 중이 아닌 것으로 알린다.
    v.waiting = continue_ ? Waiting::None : waiting_;
    v.gateId = gateId_;
    v.canContinue = v.waiting != Waiting::None && v.isHost;
    v.canSkip = v.isHost && canSkipLocked(*me);
    v.skipping = skip_;
    // 지금 족보: 내 홀 카드와 공용 카드만 쓰므로 남의 정보는 섞이지 않는다.
    if (v.table.holeDealt && v.table.state.board.size() >= 3) {
        std::vector<Card> cards = v.table.state.board;
        cards.insert(cards.end(), v.table.state.myHole.begin(), v.table.state.myHole.end());
        const HandScore score = evaluateBest(cards);
        MyHand hand;
        hand.name = describeHand(score);
        hand.code = handCode(score);
        hand.level = strengthLevel(score);
        hand.boardOnly = v.table.state.board.size() == 5 && evaluate5(v.table.state.board.data()) == score;
        v.myHand = hand;
    }
    v.review = review_;
    if (me->seat >= 0 && me->seat < static_cast<int>(flows_.size())) v.reviewFlow = flows_[me->seat];
    v.log = log_;
    v.turnId = turnId_;
    v.turnSeat = turnSeat_;
    v.myTurn = turnSeat_ >= 0 && turnSeat_ == me->seat && !pending_;
    if (turnSeat_ >= 0 && settings_.turnSeconds > 0) {
        const auto left = deadline_ - std::chrono::steady_clock::now();
        v.secondsLeft = std::max(
            0, static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(left).count()));
    }
    if (v.myTurn) {
        v.equity = equity_;
        v.opponents = std::max(1, turnState_.playersLeft() - 1);
    }
    if (phase_ == RoomPhase::Over) {
        const GameState& s = v.table.state;
        int alive = 0;
        for (const PlayerView& p : s.players) {
            if (p.chips > 0) ++alive;
        }
        const bool haveChips = !s.players.empty() && s.players[s.mySeat].chips > 0;
        if (s.players.empty()) v.message = "서버 오류로 게임이 중단되었습니다.";
        else if (!haveChips) v.message = "칩이 모두 떨어져 탈락했습니다.";
        else if (alive == 1) v.message = "축하합니다! 우승했습니다!";
        else v.message = "게임이 끝났습니다.";
    }
    return v;
}

// ---------------------------------------------------------------------------
// Room: 게임 스레드
// ---------------------------------------------------------------------------
bool Room::seatLeft(int seat) const {
    for (const Member& m : members_) {
        if (m.seat == seat) return m.left;
    }
    return true;
}

Action Room::waitForAction(int seat, const GameState& state) {
    // 나간 사람: 잠깐 쉬고 자동으로 체크하거나 폴드한다.
    const Action automatic = state.toCall == 0 ? Action::check() : Action::fold();
    bool left = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        left = seatLeft(seat);
    }
    if (left) {
        think(seat);
        return automatic;
    }
    finishAction();  // 앞사람의 행동을 다 보여 준 뒤에 내 차례를 연다

    // 승률 계산은 시간이 걸리므로 mutex를 잡기 전에 한다.
    std::optional<double> equity;
    if (options_.showHint && settings_.hint) {
        const int opponents = std::max(1, state.playersLeft() - 1);
        equity = estimateEquity(state.myHole, state.board, opponents, options_.hintTrials, hintRng_);
    }

    std::unique_lock<std::mutex> lock(mutex_);
    tables_[seat] = TableSnapshot{state, true};
    turnState_ = state;
    equity_ = equity;
    pending_.reset();
    turnSeat_ = seat;
    actingSeat_ = seat;
    actionPhase_.clear();
    ++turnId_;
    deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(settings_.turnSeconds);

    // 행동이 들어오거나, 나가거나, stop()이 불릴 때까지 잠든다 (CPU를 쓰지 않는다).
    auto ready = [this, seat] { return stopping_ || pending_.has_value() || seatLeft(seat); };
    bool timedOut = false;
    if (settings_.turnSeconds > 0) timedOut = !cv_.wait_until(lock, deadline_, ready);
    else cv_.wait(lock, ready);

    turnSeat_ = -1;
    if (stopping_) throw StopRequested{};
    if (timedOut) addLog(state.players[seat].name + ": 시간 초과");
    const Action action = pending_ ? *pending_ : automatic;
    pending_.reset();
    return action;
}

void Room::pause(std::chrono::milliseconds time) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool skipping = skip_;
    // 대기 중에 "빠르게 보기"를 누르면 바로 깨어난다.
    cv_.wait_for(lock, time, [this, skipping] { return stopping_ || skip_ != skipping; });
    if (stopping_) throw StopRequested{};
}

void Room::paceHalf() {
    std::chrono::milliseconds time;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        time = options_.aiAction;
        if (pace_ == Pace::Normal) time = time * 3 / 5;
        if (pace_ == Pace::Fast) time = time / 5;
        if (skip_) time = std::min(time, std::chrono::milliseconds(AI_SKIP_MS));
    }
    pause(time / 2);
}

void Room::finishAction() {
    if (holdAction_) {
        holdAction_ = false;
        paceHalf();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    actingSeat_ = -1;
    actionPhase_.clear();
}

void Room::think(int seat) {
    finishAction();  // 한 번에 한 명씩: 앞사람 행동을 다 보여 준 다음에 시작
    {
        std::lock_guard<std::mutex> lock(mutex_);
        actingSeat_ = seat;
        actionPhase_ = "thinking";
    }
    paceHalf();
    holdAction_ = true;
}

void Room::waitAt(Waiting point) {
    if (options_.autoContinue) {
        if (point == Waiting::NextHand) pause(options_.nextHandDelay);
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    // 빠르게 보기는 이번 판의 단계만 넘긴다. 다음 판은 항상 눌러야 시작한다.
    const bool skippable = point != Waiting::NextHand;
    if (skippable && skip_) return;
    waiting_ = point;
    ++gateId_;
    continue_ = false;
    cv_.wait(lock, [this, skippable] { return stopping_ || continue_ || (skippable && skip_); });
    waiting_ = Waiting::None;
    continue_ = false;
    if (stopping_) throw StopRequested{};
}

bool Room::isHostLocked(const std::string& token) const {
    // 방장 = 남아 있는 사람 중 가장 먼저 들어온 사람
    const auto host = std::find_if(members_.begin(), members_.end(), [](const Member& m) { return !m.left; });
    return host != members_.end() && host->token == token;
}

bool Room::canSkipLocked(const Member& viewer) const {
    if (phase_ != RoomPhase::Playing || handOver_ || skip_) return false;
    auto canStillAct = [this](int seat) {
        if (seat < 0 || seat >= static_cast<int>(tables_.size())) return true;
        const auto& players = tables_[seat].state.players;
        if (seat >= static_cast<int>(players.size())) return true;
        const PlayerView& p = players[seat];
        return p.inHand && !p.folded && !p.allIn;
    };
    if (canStillAct(viewer.seat)) return false;
    // 다른 사람이 아직 행동할 수 있으면 재촉하지 않는다.
    return std::none_of(members_.begin(), members_.end(),
                        [&](const Member& m) { return !m.left && canStillAct(m.seat); });
}

bool Room::proceed(const std::string& token, std::optional<long long> gateId, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !findMember(token) || !isHostLocked(token)) {
            error = "방장만 다음으로 넘길 수 있습니다.";
            return false;
        }
        if (waiting_ == Waiting::None || continue_ || (gateId && *gateId != gateId_)) {
            error = "지금은 넘길 단계가 없습니다.";
            return false;
        }
        continue_ = true;
    }
    cv_.notify_all();
    return true;
}

bool Room::skipHand(const std::string& token, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Member* me = findMember(token);
        if (stopping_ || !me || !isHostLocked(token) || !canSkipLocked(*me)) {
            error = "지금은 건너뛸 수 없습니다.";
            return false;
        }
        skip_ = true;
    }
    cv_.notify_all();
    return true;
}

bool Room::setPace(const std::string& token, Pace pace, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !findMember(token) || !isHostLocked(token)) {
        error = "방장만 진행 속도를 바꿀 수 있습니다.";
        return false;
    }
    pace_ = pace;
    return true;
}

void Room::publishReview(const HandSummary& hand, const std::vector<std::string>& names, int humans,
                         std::vector<int>& previousRanks) {
    auto review = std::make_shared<const HandReview>(buildHandReview(hand, names, previousRanks));
    previousRanks.assign(names.size(), 0);
    for (const StandingRow& row : review->standings) previousRanks[row.seat] = row.rank;
    std::vector<std::optional<ReviewFlow>> flows;
    for (int seat = 0; seat < humans; ++seat) flows.push_back(buildReviewFlow(*review, seat));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        review_ = review;
        flows_ = flows;
    }
    if (!options_.showHint || !settings_.hint) return;

    // 승률 계산은 시간이 걸릴 수 있으므로 리뷰를 먼저 공개한 뒤, mutex 밖에서 계산해 채운다.
    for (int seat = 0; seat < humans; ++seat) {
        if (!flows[seat]) continue;
        const int opponents = static_cast<int>(review->entries.size()) - 1;
        if (opponents < 1) continue;
        for (const ReviewEntry& e : review->entries) {
            if (e.seat != seat) continue;
            for (std::size_t i = 0; i < flows[seat]->streets.size(); ++i) {
                const std::vector<Card> board(review->board.begin(), review->board.begin() + 3 + i);
                flows[seat]->streets[i].equity =
                    estimateEquity(e.hole, board, opponents, options_.hintTrials, hintRng_);
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        flows_[seat] = flows[seat];
    }
}

void Room::run(std::vector<std::string> humanNames, int botCount, unsigned seed) {
    try {
        const int humans = static_cast<int>(humanNames.size());
        std::vector<std::unique_ptr<Player>> players;
        for (int i = 0; i < humans; ++i) {
            players.push_back(std::make_unique<WebHumanPlayer>(*this, i, humanNames[i]));
        }
        std::vector<std::unique_ptr<Player>> bots = makeAllBots(seed);
        for (int i = 0; i < botCount; ++i) {
            players.push_back(std::make_unique<PacedPlayer>(*this, std::move(bots[i])));
        }
        Game game(settings_.config, std::move(players), seed);

        // 사람마다 "자기 시점"의 테이블을 따로 만들어 둔다.
        auto snapshots = [&game, humans] {
            std::vector<TableSnapshot> tables;
            for (int i = 0; i < humans; ++i) tables.push_back(game.snapshot(i));
            return tables;
        };
        {
            auto tables = snapshots();
            std::lock_guard<std::mutex> lock(mutex_);
            tables_ = std::move(tables);
        }

        std::vector<std::string> seatNames;
        for (int i = 0; i < game.numSeats(); ++i) seatNames.push_back(game.player(i).name());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            seatNames_ = seatNames;
        }

        // 플랍/턴/리버/쇼다운 직전에 멈추고 "다음 단계로"를 기다린다.
        game.setStageHook([this, &snapshots](Stage stage) {
            finishAction();  // 마지막 행동을 다 보여 준다
            if (stage == Stage::HandEnd) return;
            auto tables = snapshots();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                tables_ = std::move(tables);
            }
            waitAt(stage == Stage::Flop    ? Waiting::Flop
                   : stage == Stage::Turn  ? Waiting::Turn
                   : stage == Stage::River ? Waiting::River
                                           : Waiting::Showdown);
        });

        // 카드를 나눠 준 직후에는 로그가 없어, 따로 갱신해야 내 카드가 바로 보인다.
        game.setDealHook([this, &snapshots] {
            auto tables = snapshots();
            std::lock_guard<std::mutex> lock(mutex_);
            tables_ = std::move(tables);
        });

        // 게임에서 일이 생길 때마다 로그를 남기고 화면 정보를 갱신한다.
        game.setLogger([this, &snapshots](const std::string& line) {
            auto tables = snapshots();
            std::lock_guard<std::mutex> lock(mutex_);
            if (line.rfind("=====", 0) == 0) {  // 새 판 시작: 지난 판의 공개 카드를 지운다
                revealed_.clear();
                handOver_ = false;
                skip_ = false;
                lastAction_.reset();
                actingSeat_ = -1;
                actionPhase_.clear();
            }
            tables_ = std::move(tables);
            // 지금 행동 중인 사람의 행동 줄이면 말풍선으로 공개한다.
            if (actingSeat_ >= 0 && actingSeat_ < static_cast<int>(seatNames_.size())) {
                const std::string prefix = seatNames_[actingSeat_] + ": ";
                if (line.rfind(prefix, 0) == 0 && line.find("반환") == std::string::npos) {
                    lastAction_ = ActionNote{++actionSeq_, actingSeat_, line.substr(prefix.size())};
                    actionPhase_ = "revealed";
                }
            }
            addLog(line);
        });

        // 칩이 남아 있고 방을 안 나간 사람이 있는 동안 계속한다.
        auto someoneCanPlay = [this, &game, humans] {
            std::lock_guard<std::mutex> lock(mutex_);
            for (int i = 0; i < humans; ++i) {
                if (!seatLeft(i) && game.chips(i) > 0) return true;
            }
            return false;
        };

        std::vector<int> previousRanks;  // 칩 순위 변동(▲▼) 계산용
        while (!game.isOver() && someoneCanPlay()) {
            const HandSummary hand = game.playHand();
            if (options_.stats) options_.stats->handFinished();
            auto tables = snapshots();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                tables_ = std::move(tables);
                handOver_ = true;
                revealed_.assign(game.numSeats(), std::nullopt);
                // 쇼다운까지 갔을 때만, 폴드하지 않은 사람의 카드를 공개한다.
                if (hand.showdown) {
                    for (int i = 0; i < game.numSeats(); ++i) {
                        if (hand.dealtIn[i] && !hand.folded[i]) revealed_[i] = hand.holes[i];
                    }
                }
            }
            publishReview(hand, seatNames, humans, previousRanks);
            if (game.isOver() || !someoneCanPlay()) break;
            waitAt(Waiting::NextHand);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = RoomPhase::Over;
    } catch (const StopRequested&) {
        // 방 정리: 조용히 끝낸다.
    } catch (const std::exception& e) {
        // 스레드 밖으로 예외가 나가면 서버 전체가 죽으므로 여기서 모두 받는다.
        std::fprintf(stderr, "게임 스레드 오류: %s\n", e.what());
        std::lock_guard<std::mutex> lock(mutex_);
        turnSeat_ = -1;
        tables_.clear();
        phase_ = RoomPhase::Over;
    }
}

// ---------------------------------------------------------------------------
// RoomManager
// ---------------------------------------------------------------------------
void RoomManager::leaveLocked(const std::string& token, std::vector<std::shared_ptr<Room>>& removed) {
    const auto found = tokens_.find(token);
    if (found == tokens_.end()) return;
    const auto it = rooms_.find(found->second);
    tokens_.erase(found);
    if (it == rooms_.end()) return;
    it->second.room->leave(token);
    if (it->second.room->empty()) {  // 아무도 없으면 방을 없앤다
        it->second.room->stop();
        removed.push_back(std::move(it->second.room));
        rooms_.erase(it);
    }
}

std::string RoomManager::create(const std::string& token, const std::string& name,
                                const RoomSettings& settings, bool solo, std::string& error,
                                CreateFail& fail, const std::string& clientId, Clock::time_point now) {
    fail = CreateFail::Invalid;
    std::string nickname = name;
    // 옛 방을 떠나기 전에 입력부터 검사한다.
    if (!validateSettings(settings, solo, error) || !cleanNickname(nickname, error)) return "";

    std::vector<std::shared_ptr<Room>> removed;  // lock보다 먼저 선언 (나중에 없어진다)
    std::lock_guard<std::mutex> lock(mutex_);

    // 내가 혼자 있는 방은 곧 없어지므로 자리 계산에서 뺀다.
    std::string freedCode;
    const auto old = tokens_.find(token);
    if (old != tokens_.end()) {
        const auto others = std::count_if(tokens_.begin(), tokens_.end(),
                                          [&](const auto& t) { return t.second == old->second; });
        if (others == 1) freedCode = old->second;
    }
    if (rooms_.size() - (freedCode.empty() ? 0 : 1) >= static_cast<std::size_t>(options_.maxRooms)) {
        error = "지금 방이 가득 찼어요. 잠시 뒤에 다시 시도해 주세요.";
        fail = CreateFail::RoomsFull;
        return "";
    }
    if (!clientId.empty()) {
        const auto mine = std::count_if(rooms_.begin(), rooms_.end(), [&](const auto& r) {
            return r.second.creator == clientId && r.first != freedCode;
        });
        if (mine >= options_.maxRoomsPerClient) {
            error = "한 브라우저에서 만들 수 있는 방은 " + std::to_string(options_.maxRoomsPerClient) +
                    "개까지예요. 기존 방을 마친 뒤 다시 시도해 주세요.";
            fail = CreateFail::ClientFull;
            return "";
        }
    }

    std::string code = makeCode();
    while (rooms_.count(code)) code = makeCode();
    // --seed를 주면 첫 방은 그 시드, 이후 방은 1씩 증가
    const unsigned seed = options_.seed ? *options_.seed + roomsCreated_ : std::random_device{}();
    auto room = std::make_shared<Room>(options_, settings, code, solo, seed);

    // 클라이언트가 보낸 모르는 토큰은 쓰지 않고 새로 발급한다.
    const std::string key = tokens_.count(token) ? token : makeToken();
    if (!room->join(key, nickname, error)) return "";
    if (solo && !room->start(key, error)) return "";

    leaveLocked(key, removed);
    ++roomsCreated_;
    rooms_[code] = Entry{std::move(room), now, clientId};
    tokens_[key] = code;
    closed_.erase(token);
    fail = CreateFail::None;
    return key;
}

std::string RoomManager::join(const std::string& token, const std::string& code,
                              const std::string& name, std::string& error, bool& unknownCode,
                              Clock::time_point now) {
    unknownCode = false;
    std::vector<std::shared_ptr<Room>> removed;
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = rooms_.find(code);
    if (it == rooms_.end() || it->second.room->solo()) {  // 혼자 하기 방에는 못 들어간다
        error = "그런 코드의 방이 없습니다.";
        unknownCode = true;
        return "";
    }
    const std::string key = tokens_.count(token) ? token : makeToken();
    const auto current = tokens_.find(key);
    if (current != tokens_.end() && current->second == code) return key;  // 이미 이 방

    if (!it->second.room->join(key, name, error)) return "";
    it->second.lastInput = now;
    leaveLocked(key, removed);  // 전에 있던 방에서는 나간다
    tokens_[key] = code;
    closed_.erase(token);
    return key;
}

void RoomManager::leave(const std::string& token) {
    std::vector<std::shared_ptr<Room>> removed;
    std::lock_guard<std::mutex> lock(mutex_);
    closed_.erase(token);
    leaveLocked(token, removed);
}

bool RoomManager::closedByIdle(const std::string& token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_.count(token) > 0;
}

std::shared_ptr<Room> RoomManager::find(const std::string& token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = tokens_.find(token);
    if (found == tokens_.end()) return nullptr;
    const auto it = rooms_.find(found->second);
    return it == rooms_.end() ? nullptr : it->second.room;
}

void RoomManager::touch(const std::string& token, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = tokens_.find(token);
    if (found == tokens_.end()) return;
    const auto it = rooms_.find(found->second);
    if (it != rooms_.end()) it->second.lastInput = now;
}

std::size_t RoomManager::cleanupIdle(Clock::time_point now) {
    std::vector<std::shared_ptr<Room>> removed;  // mutex를 푼 뒤에 없앤다
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 닫힘 안내는 1시간 동안만 기억한다.
        for (auto c = closed_.begin(); c != closed_.end();) {
            c = now - c->second >= std::chrono::hours(1) ? closed_.erase(c) : std::next(c);
        }
        const std::chrono::minutes limit(options_.idleMinutes);
        for (auto it = rooms_.begin(); it != rooms_.end();) {
            if (now - it->second.lastInput < limit) {
                ++it;
                continue;
            }
            for (auto t = tokens_.begin(); t != tokens_.end();) {
                if (t->second != it->first) {
                    ++t;
                    continue;
                }
                closed_[t->first] = now;
                t = tokens_.erase(t);
            }
            it->second.room->stop();
            removed.push_back(std::move(it->second.room));
            it = rooms_.erase(it);
        }
    }
    return removed.size();
}

std::size_t RoomManager::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rooms_.size();
}
