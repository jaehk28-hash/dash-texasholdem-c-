// sha256.h - SHA-256 해시 (ledger의 체인에 쓴다. 외부 라이브러리 없이 구현)
#pragma once

#include <string>

// data의 SHA-256을 16진수 64자로 돌려준다.
std::string sha256Hex(const std::string& data);
