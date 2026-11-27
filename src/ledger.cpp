// ledger.cpp - ledger 한 줄 만들기와 체인 검증
#include "ledger.h"

#include <cstddef>
#include <map>
#include <string>

#include "sha256.h"
#include "web_json.h"

std::string ledgerLine(const std::string& date, const LedgerCounts& c, const std::string& prev) {
    std::string sources;
    for (const auto& [name, count] : c.src) {
        if (!sources.empty()) sources += ",";
        sources += jsonString(name) + ":" + std::to_string(count);
    }
    const std::string body = "{\"date\":" + jsonString(date) +
                             ",\"counts\":{\"visits\":" + std::to_string(c.visits) +
                             ",\"unique_visitors\":" + std::to_string(c.uniqueVisitors) +
                             ",\"solo_starts\":" + std::to_string(c.soloStarts) +
                             ",\"room_starts\":" + std::to_string(c.roomStarts) +
                             ",\"hands_finished\":" + std::to_string(c.handsFinished) +
                             ",\"src\":{" + sources + "}},\"prev\":" + jsonString(prev) + "}";
    return body.substr(0, body.size() - 1) + ",\"hash\":\"" + sha256Hex(body) + "\"}";
}

LedgerState checkLedger(const std::string& content) {
    LedgerState state;
    const std::string datePrefix = "{\"date\":\"";
    const std::string hashPrefix = ",\"hash\":\"";
    const std::size_t tail = hashPrefix.size() + 64 + 2;  // ,"hash":"<64자>"}

    std::string prev = "genesis";
    std::size_t from = 0;
    while (from < content.size()) {
        const std::size_t end = content.find('\n', from);
        if (end == std::string::npos) break;  // 마지막 줄이 잘렸다
        const std::string line = content.substr(from, end - from);
        if (line.size() < datePrefix.size() + 10 + tail || line.compare(0, datePrefix.size(), datePrefix) != 0) break;
        const std::size_t at = line.size() - tail;
        if (line.compare(at, hashPrefix.size(), hashPrefix) != 0 || line.compare(line.size() - 2, 2, "\"}") != 0) break;
        const std::string hash = line.substr(at + hashPrefix.size(), 64);
        const std::string body = line.substr(0, at) + "}";
        if (sha256Hex(body) != hash) break;  // 내용이 바뀌었다
        const std::string link = ",\"prev\":\"" + prev + "\"}";
        if (body.size() < link.size() || body.compare(body.size() - link.size(), link.size(), link) != 0) break;
        const std::string date = line.substr(datePrefix.size(), 10);
        if (date <= state.lastDate) break;  // 날짜는 늘 앞으로만 간다
        state.hashes.push_back(hash);
        state.lastDate = date;
        prev = hash;
        from = end + 1;
    }
    state.ok = from >= content.size();
    return state;
}
