#pragma once
// Minimal plain-assert test harness. No framework: assert() and main() are
// enough for deterministic single-threaded unit tests, and they add no
// dependency to a repo whose whole point is having none.
//
// Tests must be built with NDEBUG undefined or every assert compiles away to
// nothing and the suite silently passes. tests/CMakeLists.txt passes -UNDEBUG;
// the static_assert in this header is the tripwire if that ever regresses.
#include <cassert>
#include <cstdio>
#include <cstdlib>

#ifdef NDEBUG
#error "engine_tests must be built with NDEBUG undefined; assert() would be a no-op"
#endif

// Each test file exposes one of these; main.cpp runs them all.
void test_wire();
void test_symbol();
void test_queue();
void test_orderbook();
void test_framing();
void test_identity();

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #cond);                                               \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

#define RUN(fn)                                                                \
    do {                                                                       \
        std::printf("  %-24s", #fn);                                           \
        std::fflush(stdout);                                                   \
        fn();                                                                  \
        std::printf("ok\n");                                                   \
    } while (0)
