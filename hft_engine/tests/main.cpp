#include "tests.h"
#include "matching/order.h"

// The engine's global stats object lives in exchange.cpp, which the test binary
// does not link. OrderBook::broadcast increments it on a dropped report, so the
// suite supplies its own definition.
EngineStats g_stats;

int main() {
    std::printf("engine_tests\n");
    RUN(test_wire);
    RUN(test_symbol);
    RUN(test_queue);
    RUN(test_orderbook);
    RUN(test_framing);
    RUN(test_identity);
    std::printf("all passed\n");
    return 0;
}
