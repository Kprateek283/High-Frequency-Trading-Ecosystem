#include "tests.h"
#include "core/realtime.h"

#include <cstdlib>
#include <sstream>

// rt::acquire() cannot be tested for success without CAP_SYS_NICE, so this
// covers the part that is pure logic: how RT_PRIORITY is parsed and clamped, and
// that the report line stays machine-readable for the benchmark harness.
void test_realtime() {
    setenv("RT_PRIORITY", "80", 1);
    CHECK(rt::priority() == 80);

    // 0 means "do not request realtime at all" -- the developer-box default path.
    setenv("RT_PRIORITY", "0", 1);
    CHECK(rt::priority() == 0);
    CHECK(rt::acquire() == false);              // disabled => never counted
    CHECK(rt::requested().load() == 0);

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
}
