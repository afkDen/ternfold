#include "../src/execution/scalars.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "test_exec_scalars:%d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void int_text_cases(void) {
    const struct { int64_t value; const char *expected; } cases[] = {
        {0, "0"}, {1, "1"}, {-1, "-1"}, {10, "10"}, {-10, "-10"},
        {INT64_C(1000000000000000000), "1000000000000000000"},
        {INT64_MAX, "9223372036854775807"},
        {INT64_MIN, "-9223372036854775808"}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        char buffer[TF_EXEC_INT_TEXT_CAPACITY + 2];
        memset(buffer, '!', sizeof buffer);
        const size_t n = tf_exec_int_text(cases[i].value, buffer + 1);
        CHECK(n == strlen(cases[i].expected));
        CHECK(memcmp(buffer + 1, cases[i].expected, n + 1) == 0);
        CHECK(buffer[0] == '!' && buffer[n + 2] == '!');
    }
}

static void parse_int_cases(void) {
    const struct { const char *text; int64_t expected; } valid[] = {
        {"0", 0}, {"-0", 0}, {"8", 8}, {"-9", -9}, {"100", 100},
        {"9223372036854775807", INT64_MAX},
        {"-9223372036854775807", -INT64_MAX},
        {"-9223372036854775808", INT64_MIN}
    };
    const char *const invalid[] = {
        "", "-", "+0", "+1", "00", "-00", "01", "-01", " 1", "1 ", "\t1",
        "1\n", "1\r", "1 2", "--1", "1-", "1_0", "0x10", "1.0", "1e2",
        "9223372036854775808", "-9223372036854775809",
        "18446744073709551616", "123456789012345678901",
        "\xef\xbc\x91", "\xe2\x88\x92\x31"
    };
    for (size_t i = 0; i < sizeof valid / sizeof *valid; ++i) {
        int64_t out = 42;
        CHECK(tf_exec_parse_int((const unsigned char *)valid[i].text,
                                strlen(valid[i].text), &out));
        CHECK(out == valid[i].expected);
    }
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        int64_t out = 42;
        CHECK(!tf_exec_parse_int((const unsigned char *)invalid[i],
                                 strlen(invalid[i]), &out));
        CHECK(out == 42);
    }
    const unsigned char unterminated[] = {'4', '2'};
    const unsigned char nul[] = {'1', 0, '2'};
    int64_t out = 314;
    CHECK(tf_exec_parse_int(unterminated, sizeof unterminated, &out) && out == 42);
    CHECK(tf_exec_parse_int(nul, 1, &out) && out == 1);
    CHECK(!tf_exec_parse_int(nul, 2, &out) && out == 1);
    CHECK(!tf_exec_parse_int(nul, sizeof nul, &out) && out == 1);
    CHECK(!tf_exec_parse_int(NULL, 0, &out) && out == 1);

    for (unsigned i = 0; i <= 255; ++i) {
        const unsigned char byte = (unsigned char)i;
        const bool is_digit = i >= 0x30 && i <= 0x39;
        out = 42;
        CHECK(tf_exec_parse_int(&byte, 1, &out) == is_digit);
        CHECK(out == (is_digit ? (int64_t)(i - 0x30) : 42));
    }
}

static void float_int_cases(void) {
    const struct { uint64_t bits; bool valid; int64_t expected; } cases[] = {
        {UINT64_C(0x0000000000000000), true, 0},
        {UINT64_C(0x8000000000000000), true, 0},
        {UINT64_C(0x000fffffffffffff), true, 0},
        {UINT64_C(0x8000000000000001), true, 0},
        {UINT64_C(0x3fefffffffffffff), true, 0}, /* 1 - 2^-53 */
        {UINT64_C(0xbfefffffffffffff), true, 0},
        {UINT64_C(0x3ff0000000000000), true, 1},
        {UINT64_C(0xbff0000000000000), true, -1},
        {UINT64_C(0x3fffffffffffffff), true, 1}, /* 2 - 2^-52 */
        {UINT64_C(0xbfffffffffffffff), true, -1},
        {UINT64_C(0x4006000000000000), true, 2}, /* 2.75 */
        {UINT64_C(0xc006000000000000), true, -2},
        {UINT64_C(0x432fffffffffffff), true, INT64_C(4503599627370495)}, /* 2^52 - 0.5 */
        {UINT64_C(0x4330000000000001), true, INT64_C(4503599627370497)},
        {UINT64_C(0x4340000000000001), true, INT64_C(9007199254740994)},
        {UINT64_C(0x43dfffffffffffff), true, INT64_MAX - 1023}, /* 2^63 - 1024 */
        {UINT64_C(0x43e0000000000000), false, 0}, /* 2^63, also int_float(IntMax) */
        {UINT64_C(0x43e0000000000001), false, 0}, /* 2^63 + 2048 */
        {UINT64_C(0xc3dfffffffffffff), true, INT64_MIN + 1024},
        {UINT64_C(0xc3e0000000000000), true, INT64_MIN},
        {UINT64_C(0xc3e0000000000001), false, 0},
        {UINT64_C(0xc3f0000000000000), false, 0}, /* -2^64 */
        {UINT64_C(0x7fefffffffffffff), false, 0},
        {UINT64_C(0xffefffffffffffff), false, 0},
        {UINT64_C(0x7ff0000000000000), false, 0},
        {UINT64_C(0xfff0000000000000), false, 0},
        {UINT64_C(0x7ff0000000000001), false, 0}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        int64_t out = 42;
        CHECK(tf_exec_float_int(cases[i].bits, &out) == cases[i].valid);
        CHECK(out == (cases[i].valid ? cases[i].expected : 42));
    }
}

static void float_text_cases(void) {
    /* Expected strings derived by dividing the exact binary rational by
     * the decimal rounding quantum and comparing twice the remainder to
     * the divisor. No decimal input parser or host Float formatter. */
    const struct { uint64_t bits; const char *expected; } cases[] = {
        {UINT64_C(0x0000000000000000), "0.0e0"},
        {UINT64_C(0x8000000000000000), "0.0e0"},
        {UINT64_C(0x3ff0000000000000), "1.0e0"},
        {UINT64_C(0x3ff8000000000000), "1.5e0"},
        {UINT64_C(0xc004000000000000), "-2.5e0"},
        {UINT64_C(0x3fb999999999999a), "1.0000000000000001e-1"},
        {UINT64_C(0x3fb9999999999999), "9.9999999999999992e-2"},
        {UINT64_C(0x3fefffffffffffff), "9.9999999999999989e-1"},
        {UINT64_C(0x3ff0000000000001), "1.0000000000000002e0"},
        {UINT64_C(0x4340000000000001), "9.007199254740994e15"},
        {UINT64_C(0x0000000000000001), "4.9406564584124654e-324"},
        {UINT64_C(0x8000000000000001), "-4.9406564584124654e-324"},
        {UINT64_C(0x0000000000000002), "9.8813129168249309e-324"},
        {UINT64_C(0x000fffffffffffff), "2.2250738585072009e-308"},
        {UINT64_C(0x0010000000000000), "2.2250738585072014e-308"},
        {UINT64_C(0x0010000000000001), "2.2250738585072019e-308"},
        {UINT64_C(0x7fefffffffffffff), "1.7976931348623157e308"},
        {UINT64_C(0xffefffffffffffff), "-1.7976931348623157e308"},
        /* The exact middle values are 1 + 1/2^17 and 1 + 3/2^17:
         * 1.00000762939453125 and 1.00002288818359375. */
        {UINT64_C(0x3ff00007ffffffff), "1.000007629394531e0"},
        {UINT64_C(0x3ff0000800000000), "1.0000076293945312e0"},
        {UINT64_C(0x3ff0000800000001), "1.0000076293945315e0"},
        {UINT64_C(0x3ff00017ffffffff), "1.0000228881835935e0"},
        {UINT64_C(0x3ff0001800000000), "1.0000228881835938e0"},
        {UINT64_C(0x3ff0001800000001), "1.000022888183594e0"},
        {UINT64_C(0xbff0000800000000), "-1.0000076293945312e0"},
        {UINT64_C(0xbff0001800000000), "-1.0000228881835938e0"},
        /* Guard digit 5, even last retained digit, then a nonzero tail. */
        {UINT64_C(0x0000444b5d18bd1d), "3.7099624095746561e-310"},
        /* Below, inside, and above the decade carry interval. */
        {UINT64_C(0x3d06849b86a12b9a), "9.9999999999999984e-15"},
        {UINT64_C(0x3d06849b86a12b9b), "1.0e-14"},
        {UINT64_C(0x3d06849b86a12b9c), "1.0000000000000002e-14"},
        {UINT64_C(0x5447688bb5394c25), "1.0e98"},
        {UINT64_C(0x009c16c5c5253575), "1.0e-305"}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        char buffer[TF_EXEC_FLOAT_TEXT_CAPACITY + 2];
        memset(buffer, '!', sizeof buffer);
        const size_t n = tf_exec_float_text(cases[i].bits, buffer + 1);
        CHECK(n == strlen(cases[i].expected));
        CHECK(memcmp(buffer + 1, cases[i].expected, n + 1) == 0);
        CHECK(buffer[0] == '!' && buffer[n + 2] == '!');
    }

    const uint64_t nonfinite[] = {
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff0000000000001), UINT64_C(0xfff8000000000000)
    };
    for (size_t i = 0; i < sizeof nonfinite / sizeof *nonfinite; ++i) {
        char buffer[TF_EXEC_FLOAT_TEXT_CAPACITY];
        memset(buffer, '!', sizeof buffer);
        CHECK(tf_exec_float_text(nonfinite[i], buffer) == 0);
        for (size_t j = 0; j < sizeof buffer; ++j) CHECK(buffer[j] == '!');
    }
}

int main(void) {
    int_text_cases();
    parse_int_cases();
    float_int_cases();
    float_text_cases();
    printf("test_exec_scalars: %zu checks passed\n", checks);
    return 0;
}
