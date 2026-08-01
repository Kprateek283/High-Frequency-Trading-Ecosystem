#include "tests.h"
#include "core/realtime.h"

#include <cstdlib>
#include <sstream>
#include <chrono>

// rt::acquire() cannot be tested for success without CAP_SYS_NICE, so this
// covers the part that is pure logic: how RT_PRIORITY is parsed and clamped, and
// that the report line stays machine-readable for the benchmark harness.
void test_realtime() {
    setenv("RT_PRIORITY", "80", 1);
    CHECK(rt::priority() == 80);

    // 0 means "do not request realtime at all" -- the developer-box default path.
    setenv("RT_PRIORITY", "0", 1);
    CHECK(rt::priority() == 0);
    CHECK(rt::acquire() == false);              // disabled => no RT requested
    CHECK(rt::requested().load() == 0);

    // ...but the thread must still be counted as STARTED, and await() must still
    // block on that count. Skipping this is what turned READY into a 100 ms sleep
    // whenever RT_PRIORITY=0, letting a benchmark drive orders into a gateway
    // whose workers did not exist yet.
    CHECK(rt::started().load() == 1);
    rt::await(1, 50);                           // already satisfied: returns at once
    CHECK(rt::started().load() == 1);

    // An unmet barrier must time out rather than hang or return early.
    auto t0 = std::chrono::steady_clock::now();
    rt::await(2, 30);                           // never satisfied: one thread only
    auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
    CHECK(waited >= 25);

    // Out of range must disable rather than pass a bogus priority to the kernel;
    // 99 shares a band with kernel migration/watchdog threads, so a typo like
    // 990 silently becoming 99 would be the worst outcome.
    setenv("RT_PRIORITY", "990", 1);
    CHECK(rt::priority() == 0);
    setenv("RT_PRIORITY", "-5", 1);
    CHECK(rt::priority() == 0);
    setenv("RT_PRIORITY", "99", 1);
    CHECK(rt::priority() == 99);                // allowed, just not the default

    unsetenv("RT_PRIORITY");
    CHECK(rt::priority() == 80);                // default when unset

    // The harness greps this prefix; keep it stable.
    std::ostringstream os;
    rt::report(os);
    CHECK(os.str().rfind("RT_SCHED: granted=", 0) == 0);
    CHECK(os.str().find(" threads=") != std::string::npos);
}
