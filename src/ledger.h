// ledger.h - 날짜별 집계를 한 줄씩 덧붙이는 기록 (data/stats-ledger.jsonl)
//
// 한 줄: {"date":"YYYY-MM-DD","counts":{...},"prev":"앞줄의 hash","hash":"..."}
// hash는 그 줄에서 ,"hash":"..." 부분을 뺀 글자의 SHA-256, 첫 줄의 prev는 "genesis".
// 파일은 덧붙이기만 한다. 담는 것은 날짜와 숫자뿐이다.
// 형식을 바꾸면 tools/ledger/verify.py도 같이 바꿔야 한다.
#pragma once

#include <map>
#include <string>
#include <vector>

struct LedgerCounts {
    long long visits = 0;          // 그날 온 ID 수 (같은 ID는 한 번)
    long long uniqueVisitors = 0;  // 그날 처음 온 ID 수 (모두 더하면 누적 고유 방문자 수)
    long long soloStarts = 0;
    long long roomStarts = 0;
    long long handsFinished = 0;
    std::map<std::string, long long> src;  // 유입 경로별 방문 수 (정해 둔 이름만)
};

struct LedgerState {
    bool ok = true;                   // 체인이 처음부터 끝까지 맞는가 (파일이 없으면 true)
    std::vector<std::string> hashes;  // 줄마다의 hash
    std::string lastDate;             // 비어 있으면 아직 한 줄도 없다
};

// 줄바꿈 없는 한 줄을 만든다.
std::string ledgerLine(const std::string& date, const LedgerCounts& counts, const std::string& prev);

// 파일 내용 전체를 검증한다.
LedgerState checkLedger(const std::string& content);
