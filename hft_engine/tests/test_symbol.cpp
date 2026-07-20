#include "tests.h"
#include "protocol/messages.h"
#include <cstring>

// The canonical symbol scheme (review A1/A2).
//
// Before this landed, four clients used four different encodings and the
// gateway understood exactly one of them, so the documented benchmark matched
// zero orders. The historical encodings are asserted as *rejected* below —
// deliberately, and by name. If someone reintroduces one, these fail.

namespace {

uint16_t decode(const char* s) {
    char buf[8];
    std::memcpy(buf, s, 8);
    return decode_symbol(buf);
}

} // namespace

void test_symbol() {
    // --- Round trip across the whole valid range. ---
    for (uint16_t i = 0; i < MAX_INSTRUMENTS; ++i) {
        char buf[8];
        encode_symbol(buf, i);
        CHECK(decode_symbol(buf) == i);
    }

    // --- The exact wire bytes, spelled out. No encoder involved. ---
    CHECK(decode("STK00000") == 0);
    CHECK(decode("STK00001") == 1);
    CHECK(decode("STK00042") == 42);
    CHECK(decode("STK00255") == 255);      // the last valid instrument

    // --- The cap. MAX_INSTRUMENTS is 256, so 256 is the first invalid id. ---
    CHECK(decode("STK00256") == INVALID_INSTRUMENT);
    CHECK(decode("STK00999") == INVALID_INSTRUMENT);
    CHECK(decode("STK99999") == INVALID_INSTRUMENT);

    // INVALID_INSTRUMENT must stay out of range, because that — not a special
    // case in the caller — is what makes the risk engine reject it.
    CHECK(INVALID_INSTRUMENT >= MAX_INSTRUMENTS);

    // --- Every historical client encoding, all of which must be rejected. ---
    // These are the four schemes review A1 found in the tree (its table lists
    // each with the client that emitted it).
    CHECK(decode("INSTR0  ") == INVALID_INSTRUMENT);  // hft-trading-firm
    CHECK(decode("INSTR1  ") == INVALID_INSTRUMENT);
    CHECK(decode("INSTR2  ") == INVALID_INSTRUMENT);
    CHECK(decode("INSTR3  ") == INVALID_INSTRUMENT);
    CHECK(decode("AAPL    ") == INVALID_INSTRUMENT);  // market_maker, generate_pcap
    CHECK(decode("MSFT    ") == INVALID_INSTRUMENT);  // generate_pcap
    CHECK(decode("GOOG    ") == INVALID_INSTRUMENT);
    CHECK(decode("AMZN    ") == INVALID_INSTRUMENT);
    CHECK(decode("UNKNOWN ") == INVALID_INSTRUMENT);  // the firm's old fallback

    // --- Garbage in the numeric field. ---
    // The old decode did arithmetic on whatever bytes were there, so a
    // non-digit silently produced a wrong-but-in-range instrument id. It is
    // now an explicit rejection.
    CHECK(decode("STKABCDE") == INVALID_INSTRUMENT);
    CHECK(decode("STK0000A") == INVALID_INSTRUMENT);
    CHECK(decode("STKA0000") == INVALID_INSTRUMENT);
    CHECK(decode("STK    0") == INVALID_INSTRUMENT);
    CHECK(decode("STK00 00") == INVALID_INSTRUMENT);
    CHECK(decode("STK-0001") == INVALID_INSTRUMENT);
    CHECK(decode("STK+0001") == INVALID_INSTRUMENT);
    CHECK(decode("STK0000 ") == INVALID_INSTRUMENT);  // trailing space, not a digit

    // A specific regression: "STK0000:" — ':' is '9'+1 in ASCII, so a decode
    // that only bounds-checks the result would map it to 10 rather than reject.
    CHECK(decode("STK0000:") == INVALID_INSTRUMENT);
    CHECK(decode("STK0000/") == INVALID_INSTRUMENT);  // '/' is '0'-1

    // --- Wrong prefix. ---
    CHECK(decode("stk00000") == INVALID_INSTRUMENT);  // case sensitive
    CHECK(decode("XYZ00000") == INVALID_INSTRUMENT);
    CHECK(decode("ST 00000") == INVALID_INSTRUMENT);
    CHECK(decode("        ") == INVALID_INSTRUMENT);
    CHECK(decode("\0\0\0\0\0\0\0\0") == INVALID_INSTRUMENT);  // zeroed field

    // --- The encoder writes exactly 8 bytes, no terminator. ---
    {
        char buf[9];
        std::memset(buf, '#', sizeof(buf));
        encode_symbol(buf, 255);
        CHECK(std::memcmp(buf, "STK00255", 8) == 0);
        CHECK(buf[8] == '#');   // nothing written past the field
    }
    {
        char buf[8];
        encode_symbol(buf, 0);
        CHECK(std::memcmp(buf, "STK00000", 8) == 0);
    }
}
