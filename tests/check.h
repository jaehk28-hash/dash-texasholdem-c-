// check.h - 테스트용 간단 CHECK 매크로 (실패해도 멈추지 않고 개수를 센다)
#pragma once
#include <cstdio>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                             \
    do {                                                        \
        ++g_checks;                                             \
        if (!(cond)) {                                          \
            ++g_failures;                                       \
            std::printf("실패: %s (줄 %d)\n", #cond, __LINE__); \
        }                                                       \
    } while (0)
