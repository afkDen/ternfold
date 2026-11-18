#include "ternfold/numeric.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef TfNumericStatus (*Parse)(const unsigned char *, size_t, uint64_t *);

static void expect(Parse parse, const void *bytes, size_t length,
                   TfNumericStatus status, uint64_t bits, const char *label)
{
    const uint64_t sentinel = UINT64_C(0xcafecafecafecafe);
    uint64_t out = sentinel;
    const TfNumericStatus got = parse(bytes, length, &out);
    const uint64_t expected = status == TF_NUMERIC_OK ? bits : sentinel;
    if (got != status || out != expected) {
        fprintf(stderr, "%s (%zu bytes): expected status %d, output %016" PRIx64
                "; got status %d, output %016" PRIx64 "\n",
                label, length, (int)status, expected, (int)got, out);
        exit(EXIT_FAILURE);
    }
}

#define INT(text, status, bits) \
    expect(tf_parse_int_magnitude, text, sizeof(text) - 1, status, bits, text)
#define FLOAT(text, status, bits) \
    expect(tf_parse_float_bits, text, sizeof(text) - 1, status, bits, text)
#define HEX(text, bits) FLOAT(text, TF_NUMERIC_OK, UINT64_C(bits))

static void test_int(void)
{
    INT("0", TF_NUMERIC_OK, 0);
    INT("42", TF_NUMERIC_OK, 42);
    INT("9223372036854775807", TF_NUMERIC_OK, UINT64_C(0x7fffffffffffffff));
    /* The caller decides whether the separate minus grants this boundary. */
    INT("9223372036854775808", TF_NUMERIC_OK, UINT64_C(0x8000000000000000));
    INT("9223372036854775809", TF_NUMERIC_RANGE, 0);
    INT("18446744073709551616", TF_NUMERIC_RANGE, 0);
    INT("92233720368547758090_", TF_NUMERIC_INVALID, 0);
    INT("-9223372036854775808", TF_NUMERIC_INVALID, 0);
    INT("+0", TF_NUMERIC_INVALID, 0);
    INT("00", TF_NUMERIC_INVALID, 0);
    INT("0.1", TF_NUMERIC_INVALID, 0);
    INT("1e2", TF_NUMERIC_INVALID, 0);
    INT("1\0", TF_NUMERIC_INVALID, 0);
    INT("", TF_NUMERIC_INVALID, 0);
    const unsigned char raw[] = {0x31, 0x32, 0xff};
    expect(tf_parse_int_magnitude, raw, 2, TF_NUMERIC_OK, 12, "nonterminated Int");
    expect(tf_parse_int_magnitude, raw, 3, TF_NUMERIC_INVALID, 0, "non-ASCII Int");
    expect(tf_parse_int_magnitude, NULL, 1, TF_NUMERIC_INVALID, 0, "NULL Int input");
    if (tf_parse_int_magnitude(raw, 2, NULL) != TF_NUMERIC_INVALID) exit(EXIT_FAILURE);
}

static void test_float(void)
{
    HEX("0.0", 0);
    HEX("0E-400", 0);
    HEX("1e-00000000000", 0x3ff0000000000000);
    HEX("0.1", 0x3fb999999999999a);
    HEX("12.50E+1", 0x405f400000000000);
    HEX("9007199254740993e0", 0x4340000000000000);
    HEX("9007199254740995e0", 0x4340000000000002);
    HEX("1.00000000000000011102230246251565404236316680908203124", 0x3ff0000000000000);
    HEX("1.00000000000000011102230246251565404236316680908203125", 0x3ff0000000000000);
    HEX("1.00000000000000011102230246251565404236316680908203126", 0x3ff0000000000001);
    HEX("1.99999999999999988897769753748434595763683319091796875", 0x4000000000000000);
    HEX("2.4703282292062327e-324", 0);
    HEX("2.4703282292062328e-324", 1);
    HEX("4.9406564584124654e-324", 1);
    HEX("2.2250738585072011e-308", 0x000fffffffffffff);
    HEX("2.2250738585072012e-308", 0x0010000000000000);
    HEX("1.7976931348623158e308", 0x7fefffffffffffff);
    FLOAT("1.7976931348623159e308", TF_NUMERIC_RANGE, 0);
    FLOAT("1e309", TF_NUMERIC_RANGE, 0);
    FLOAT("1e999foo", TF_NUMERIC_INVALID, 0);
    FLOAT("0e-999foo", TF_NUMERIC_INVALID, 0);
    FLOAT("0.1\0", TF_NUMERIC_INVALID, 0);
    FLOAT("-0.0", TF_NUMERIC_INVALID, 0);
    FLOAT("+1e2", TF_NUMERIC_INVALID, 0);
    FLOAT("00.1", TF_NUMERIC_INVALID, 0);
    FLOAT("01e1", TF_NUMERIC_INVALID, 0);
    FLOAT("1", TF_NUMERIC_INVALID, 0);
    FLOAT("", TF_NUMERIC_INVALID, 0);
    FLOAT(".5", TF_NUMERIC_INVALID, 0);
    FLOAT("1.", TF_NUMERIC_INVALID, 0);
    FLOAT("1.e2", TF_NUMERIC_INVALID, 0);
    FLOAT("1,5", TF_NUMERIC_INVALID, 0);
    FLOAT("1.2.3", TF_NUMERIC_INVALID, 0);
    FLOAT("1e2E3", TF_NUMERIC_INVALID, 0);
    FLOAT("1e", TF_NUMERIC_INVALID, 0);
    FLOAT("1e+", TF_NUMERIC_INVALID, 0);
    FLOAT("1e-+2", TF_NUMERIC_INVALID, 0);
    FLOAT("0x1p2", TF_NUMERIC_INVALID, 0);
    FLOAT(" NaN", TF_NUMERIC_INVALID, 0);
    FLOAT("inf", TF_NUMERIC_INVALID, 0);
    FLOAT("1.0 ", TF_NUMERIC_INVALID, 0);
    const unsigned char raw[] = {0x31, 0x2e, 0x30, 0xff};
    expect(tf_parse_float_bits, raw, 3, TF_NUMERIC_OK, UINT64_C(0x3ff0000000000000),
           "nonterminated Float");
    expect(tf_parse_float_bits, raw, 4, TF_NUMERIC_INVALID, 0, "non-ASCII Float");
    expect(tf_parse_float_bits, NULL, 1, TF_NUMERIC_INVALID, 0, "NULL Float input");
    if (tf_parse_float_bits(raw, 3, NULL) != TF_NUMERIC_INVALID) exit(EXIT_FAILURE);
}

/* Generate exact dyadics in base ten independently of the converter.
 * m * 2^e is (m * 5^-e)e(e) for negative e, otherwise (m * 2^e)e0.
 * The coefficient and one-unit neighbors exercise exact midpoint decisions. */
static void dyadic_boundary(uint64_t m, int e, uint64_t below,
                            TfNumericStatus tie_status, uint64_t tie,
                            TfNumericStatus above_status, uint64_t above)
{
    unsigned char digits[800]; /* reversed decimal digits, least significant first */
    size_t used = 0;
    do {
        digits[used++] = (unsigned char)(m % 10u);
        m /= 10u;
    } while (m != 0);
    const unsigned factor = e < 0 ? 5u : 2u;
    for (int step = 0; step < (e < 0 ? -e : e); ++step) {
        unsigned carry = 0;
        for (size_t i = 0; i < used; ++i) {
            const unsigned next = digits[i] * factor + carry;
            digits[i] = (unsigned char)(next % 10u);
            carry = next / 10u;
        }
        if (carry != 0) {
            if (used == sizeof digits) exit(EXIT_FAILURE);
            digits[used++] = (unsigned char)carry;
        }
    }
    char token[1000];
    for (size_t i = 0; i < used; ++i) token[i] = (char)('0' + digits[used - 1 - i]);
    /* All coefficients here end in 2, 4, 5, 6, or 8, so +/- 1 stays one byte. */
    if (token[used - 1] <= '0' || token[used - 1] >= '9') exit(EXIT_FAILURE);
    const int n = snprintf(token + used, sizeof token - used, "e%d", e < 0 ? e : 0);
    if (n < 1 || (size_t)n >= sizeof token - used) exit(EXIT_FAILURE);
    const size_t length = used + (size_t)n;
    expect(tf_parse_float_bits, token, length, tie_status, tie, "exact midpoint");
    --token[used - 1];
    expect(tf_parse_float_bits, token, length, TF_NUMERIC_OK, below, "below midpoint");
    token[used - 1] += 2;
    expect(tf_parse_float_bits, token, length, above_status, above, "above midpoint");
    --token[used - 1];

    /* A far-off positive tail is the only difference from the exact tie.
     * Both zero padding and a nonzero digit exercise the prefix cut. */
    const size_t tail = 100;
    token[used] = '.';
    memset(token + used + 1, '0', tail);
    const int padded_n = snprintf(token + used + 1 + tail, sizeof token - used - 1 - tail,
                                  "e%d", e < 0 ? e : 0);
    if (padded_n != n) exit(EXIT_FAILURE);
    expect(tf_parse_float_bits, token, length + 1 + tail, tie_status, tie, "zero tail at midpoint");
    token[used + tail] = '1';
    expect(tf_parse_float_bits, token, length + 1 + tail, above_status, above,
           "nonzero tail after midpoint");
}

static void test_midpoints(void)
{
    dyadic_boundary(1, -1075, 0, TF_NUMERIC_OK, 0, TF_NUMERIC_OK, 1);
    dyadic_boundary(3, -1075, 1, TF_NUMERIC_OK, 2, TF_NUMERIC_OK, 2);
    dyadic_boundary(UINT64_C(9007199254740991), -1075, UINT64_C(0x000fffffffffffff),
                    TF_NUMERIC_OK, UINT64_C(0x0010000000000000),
                    TF_NUMERIC_OK, UINT64_C(0x0010000000000000));
    dyadic_boundary(UINT64_C(9007199254740993), -1075, UINT64_C(0x0010000000000000),
                    TF_NUMERIC_OK, UINT64_C(0x0010000000000000),
                    TF_NUMERIC_OK, UINT64_C(0x0010000000000001));
    dyadic_boundary(UINT64_C(18014398509481983), 970, UINT64_C(0x7fefffffffffffff),
                    TF_NUMERIC_RANGE, 0, TF_NUMERIC_RANGE, 0);
}

static void test_long_inputs(void)
{
    enum { DIGITS = 24000 };
    char token[DIGITS + 12];
    const uint64_t one = UINT64_C(0x3ff0000000000000);
    memset(token, '9', DIGITS);
    expect(tf_parse_int_magnitude, token, DIGITS, TF_NUMERIC_RANGE, 0, "huge Int");
    token[DIGITS - 1] = 'x';
    expect(tf_parse_int_magnitude, token, DIGITS, TF_NUMERIC_INVALID, 0, "invalid huge Int");

    memset(token, '0', DIGITS);
    token[0] = '1';
    memcpy(token + DIGITS, "e-23999", 7);
    expect(tf_parse_float_bits, token, DIGITS + 7, TF_NUMERIC_OK, one,
           "long integral part cancels negative exponent");
    token[0] = '0';
    token[1] = '.';
    token[DIGITS - 1] = '1';
    memcpy(token + DIGITS, "e+23998", 7);
    expect(tf_parse_float_bits, token, DIGITS + 7, TF_NUMERIC_OK, one,
           "long fraction cancels positive exponent");

    memcpy(token, "1e+", 3);
    memset(token + 3, '9', DIGITS);
    expect(tf_parse_float_bits, token, DIGITS + 3, TF_NUMERIC_RANGE, 0, "huge exponent");
    token[2] = '-';
    expect(tf_parse_float_bits, token, DIGITS + 3, TF_NUMERIC_OK, 0, "huge negative exponent");
    token[0] = '0';
    token[2] = '+';
    expect(tf_parse_float_bits, token, DIGITS + 3, TF_NUMERIC_OK, 0, "zero with huge exponent");
    token[DIGITS + 2] = 'x';
    expect(tf_parse_float_bits, token, DIGITS + 3, TF_NUMERIC_INVALID, 0, "invalid huge exponent");

    memset(token + 3, '0', DIGITS);
    token[0] = '1';
    token[DIGITS + 2] = '1';
    expect(tf_parse_float_bits, token, DIGITS + 3, TF_NUMERIC_OK, UINT64_C(0x4024000000000000),
           "huge spelling of small exponent");
}

int main(void)
{
    test_int();
    test_float();
    test_midpoints();
    test_long_inputs();
    puts("numeric literal tests passed");
    return EXIT_SUCCESS;
}
