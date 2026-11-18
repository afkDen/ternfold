#include "ternfold/checked_numeric.h"

#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && defined(__MINGW32__) && defined(__clang__) && defined(__x86_64__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#define TF_TEST_WINDOWS_X64_TRAPS 1
#endif

static size_t checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "test_checked_numeric:%d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

#define OK TF_CHECKED_NUMERIC_OK
#define OVERFLOW TF_CHECKED_NUMERIC_OVERFLOW
#define ZERO TF_CHECKED_NUMERIC_ZERO_DIVISOR
#define NEGATIVE TF_CHECKED_NUMERIC_NEGATIVE_EXPONENT
#define UNSUPPORTED TF_CHECKED_NUMERIC_UNSUPPORTED

typedef TfCheckedNumericStatus (*IntBinary)(int64_t, int64_t, int64_t *);
typedef TfCheckedNumericStatus (*FloatBinary)(uint64_t, uint64_t, uint64_t *);

static void int_cases(void) {
    const struct {
        IntBinary op;
        int64_t a, b, result;
        TfCheckedNumericStatus status;
    } cases[] = {
        {tf_checked_int_add, INT64_MAX, 1, 0, OVERFLOW},
        {tf_checked_int_add, INT64_MIN, -1, 0, OVERFLOW},
        {tf_checked_int_add, INT64_MIN, INT64_MIN, 0, OVERFLOW},
        {tf_checked_int_add, INT64_MAX, INT64_MIN, -1, OK},
        {tf_checked_int_add, INT64_MIN + 1, -1, INT64_MIN, OK},
        {tf_checked_int_add, INT64_MAX - 1, 1, INT64_MAX, OK},
        {tf_checked_int_sub, INT64_MIN, 1, 0, OVERFLOW},
        {tf_checked_int_sub, INT64_MAX, -1, 0, OVERFLOW},
        {tf_checked_int_sub, 0, INT64_MIN, 0, OVERFLOW},
        {tf_checked_int_sub, INT64_MIN, INT64_MIN, 0, OK},
        {tf_checked_int_sub, -1, INT64_MIN, INT64_MAX, OK},
        {tf_checked_int_sub, 0, INT64_MAX, -INT64_MAX, OK},
        {tf_checked_int_mul, INT64_MIN, 0, 0, OK},
        {tf_checked_int_mul, INT64_MAX, 1, INT64_MAX, OK},
        {tf_checked_int_mul, INT64_MIN, 1, INT64_MIN, OK},
        {tf_checked_int_mul, INT64_MIN, -1, 0, OVERFLOW},
        {tf_checked_int_mul, -1, INT64_MIN, 0, OVERFLOW},
        {tf_checked_int_mul, INT64_MIN, INT64_MIN, 0, OVERFLOW},
        {tf_checked_int_mul, INT64_MAX, 2, 0, OVERFLOW},
        {tf_checked_int_mul, INT64_MAX, -2, 0, OVERFLOW},
        {tf_checked_int_mul, INT64_MIN / 2, 2, INT64_MIN, OK},
        {tf_checked_int_mul, 2, INT64_MIN / 2, INT64_MIN, OK},
        {tf_checked_int_mul, INT64_MIN / 2 - 1, 2, 0, OVERFLOW},
        {tf_checked_int_mul, -3037000499LL, -3037000499LL, 9223372030926249001LL, OK},
        {tf_checked_int_mul, -3037000500LL, -3037000500LL, 0, OVERFLOW},
        {tf_checked_int_div, INT64_MIN, 1, INT64_MIN, OK},
        {tf_checked_int_div, INT64_MIN, INT64_MIN, 1, OK},
        {tf_checked_int_div, INT64_MIN, -1, 0, OVERFLOW},
        {tf_checked_int_div, INT64_MIN, 0, 0, ZERO},
        {tf_checked_int_div, INT64_MAX, INT64_MIN, -1, OK},
        {tf_checked_int_div, INT64_MIN, INT64_MAX, -2, OK},
        {tf_checked_int_div, -6, 3, -2, OK},
        {tf_checked_int_div, 7, 3, 2, OK},
        {tf_checked_int_div, -7, 3, -3, OK},
        {tf_checked_int_div, 7, -3, -3, OK},
        {tf_checked_int_div, -7, -3, 2, OK},
        {tf_checked_int_rem, 7, 3, 1, OK},
        {tf_checked_int_rem, -7, 3, -1, OK},
        {tf_checked_int_rem, 7, -3, 1, OK},
        {tf_checked_int_rem, -7, -3, -1, OK},
        {tf_checked_int_rem, INT64_MIN, INT64_MAX, -1, OK},
        {tf_checked_int_rem, INT64_MAX, INT64_MIN, INT64_MAX, OK},
        {tf_checked_int_rem, INT64_MIN, -1, 0, OVERFLOW},
        {tf_checked_int_rem, 0, 0, 0, ZERO},
        {tf_checked_int_pow, 0, 0, 1, OK},
        {tf_checked_int_pow, 0, -1, 0, NEGATIVE},
        {tf_checked_int_pow, -1, INT64_MIN, 0, NEGATIVE},
        {tf_checked_int_pow, -1, INT64_MAX, -1, OK},
        {tf_checked_int_pow, 0, INT64_MAX, 0, OK},
        {tf_checked_int_pow, 1, INT64_MAX, 1, OK},
        {tf_checked_int_pow, INT64_MAX, 0, 1, OK},
        {tf_checked_int_pow, INT64_MIN, 1, INT64_MIN, OK},
        {tf_checked_int_pow, INT64_MAX, 1, INT64_MAX, OK},
        {tf_checked_int_pow, INT64_MAX, 2, 0, OVERFLOW},
        {tf_checked_int_pow, 2, 62, INT64_C(1) << 62, OK},
        {tf_checked_int_pow, 2, 63, 0, OVERFLOW},
        {tf_checked_int_pow, -2, 63, INT64_MIN, OK},
        {tf_checked_int_pow, -2, 64, 0, OVERFLOW}
    };
    int64_t out = 123;
    CHECK(tf_checked_int_neg(INT64_MIN, &out) == OVERFLOW && out == 123);
    CHECK(tf_checked_int_neg(-INT64_MAX, &out) == OK && out == INT64_MAX);
    CHECK(tf_checked_int_neg(INT64_MAX, &out) == OK && out == -INT64_MAX);
    CHECK(tf_checked_int_neg(0, &out) == OK && out == 0);
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        out = 123;
        CHECK(cases[i].op(cases[i].a, cases[i].b, &out) == cases[i].status);
        CHECK(out == (cases[i].status == OK ? cases[i].result : 123));
    }
    /* Check floor inequalities and the separate truncating quotient identity
     * over small operands; the oracle's intermediate products fit in Int. */
    for (int64_t a = -25; a <= 25; ++a) {
        for (int64_t b = -11; b <= 11; ++b) {
            int64_t floor, rem, trunc;
            if (b == 0) continue;
            CHECK(tf_checked_int_div(a, b, &floor) == OK);
            CHECK(tf_checked_int_rem(a, b, &rem) == OK);
            CHECK(b > 0 ? floor * b <= a && a < (floor + 1) * b :
                          floor * b >= a && a > (floor + 1) * b);
            CHECK(rem == 0 || (rem < 0) == (a < 0));
            CHECK(b > 0 ? -b < rem && rem < b : b < rem && rem < -b);
            trunc = floor + (rem != 0 && ((a < 0) != (b < 0)) ? 1 : 0);
            CHECK(trunc * b + rem == a);
        }
    }
}

#define F_SIGN UINT64_C(0x8000000000000000)
#define F_ONE UINT64_C(0x3ff0000000000000)
#define F_TWO UINT64_C(0x4000000000000000)
#define F_HALF UINT64_C(0x3fe0000000000000)
#define F_MAX UINT64_C(0x7fefffffffffffff)
#define F_MIN_NORMAL UINT64_C(0x0010000000000000)
#define F_INF UINT64_C(0x7ff0000000000000)
#define F_SNAN UINT64_C(0x7ff0000000000001)

static void conversion_and_sign(void) {
    const struct { int64_t value; uint64_t bits; } cases[] = {
        {0, 0}, {1, F_ONE}, {-1, F_SIGN | F_ONE}, {2, F_TWO},
        {INT64_C(9007199254740991), UINT64_C(0x433fffffffffffff)},
        {INT64_C(9007199254740992), UINT64_C(0x4340000000000000)},
        {INT64_C(9007199254740993), UINT64_C(0x4340000000000000)}, /* Even down. */
        {INT64_C(9007199254740995), UINT64_C(0x4340000000000002)}, /* Even up. */
        {-INT64_C(9007199254740993), UINT64_C(0xc340000000000000)},
        {-INT64_C(9007199254740995), UINT64_C(0xc340000000000002)},
        {INT64_C(18014398509481985), UINT64_C(0x4350000000000000)},
        {INT64_C(18014398509481987), UINT64_C(0x4350000000000001)},
        {INT64_MAX - 512, UINT64_C(0x43dfffffffffffff)}, /* Just below carry tie. */
        {INT64_MAX - 511, UINT64_C(0x43e0000000000000)}, /* Carry to 2^63. */
        {INT64_MAX, UINT64_C(0x43e0000000000000)},
        {INT64_MIN, UINT64_C(0xc3e0000000000000)}
    };
    uint64_t out = 123;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i)
        CHECK(tf_checked_int_to_float_bits(cases[i].value) == cases[i].bits);
    CHECK(tf_checked_float_neg(F_INF, &out) == OVERFLOW && out == 123);
    CHECK(tf_checked_float_neg(F_SNAN, &out) == OVERFLOW && out == 123);
    CHECK(tf_checked_float_neg(0, &out) == OK && out == 0);
    CHECK(tf_checked_float_neg(F_SIGN, &out) == OK && out == 0);
    CHECK(tf_checked_float_neg(1, &out) == OK && out == (F_SIGN | 1));
    CHECK(tf_checked_float_neg(F_SIGN | F_MAX, &out) == OK && out == F_MAX);
}

static void float_cases(void) {
    const struct {
        FloatBinary op;
        uint64_t a, b, result;
        TfCheckedNumericStatus status;
    } cases[] = {
        {tf_checked_float_add, F_ONE, F_HALF, UINT64_C(0x3ff8000000000000), OK},
        {tf_checked_float_add, F_ONE, UINT64_C(0x3ca0000000000000), F_ONE, OK},
        {tf_checked_float_add, F_ONE + 1, UINT64_C(0x3ca0000000000000), F_ONE + 2, OK},
        {tf_checked_float_add, F_SIGN, F_SIGN, 0, OK},
        {tf_checked_float_add, F_MAX, F_MAX, 0, OVERFLOW},
        {tf_checked_float_add, F_ONE, F_SNAN, 0, OVERFLOW},
        {tf_checked_float_sub, F_ONE, F_HALF, F_HALF, OK},
        {tf_checked_float_sub, F_SIGN, 0, 0, OK},
        {tf_checked_float_sub, F_MIN_NORMAL, F_MIN_NORMAL - 1, 1, OK},
        {tf_checked_float_sub, F_SIGN | F_MAX, F_MAX, 0, OVERFLOW},
        {tf_checked_float_mul, F_SIGN | F_TWO, F_HALF, F_SIGN | F_ONE, OK},
        {tf_checked_float_mul, F_MIN_NORMAL, F_HALF, F_MIN_NORMAL / 2, OK},
        {tf_checked_float_mul, F_SIGN | 1, F_HALF, 0, OK}, /* Halfway to zero. */
        {tf_checked_float_mul, 3, F_HALF, 2, OK}, /* Halfway to even subnormal. */
        {tf_checked_float_mul, F_MAX, F_TWO, 0, OVERFLOW},
        {tf_checked_float_div, F_ONE, F_TWO, F_HALF, OK},
        {tf_checked_float_div, F_ONE, UINT64_C(0x4024000000000000),
                               UINT64_C(0x3fb999999999999a), OK},
        {tf_checked_float_div, F_SIGN, F_ONE, 0, OK},
        {tf_checked_float_div, F_ONE, F_SIGN, 0, ZERO},
        {tf_checked_float_div, 0, 0, 0, ZERO},
        {tf_checked_float_div, F_ONE, 1, 0, OVERFLOW},
        {tf_checked_float_div, 1, 1, F_ONE, OK},
        {tf_checked_float_div, F_ONE, F_INF, 0, OVERFLOW}
    };
    const struct {
        uint64_t base;
        int64_t exponent;
        uint64_t result;
        TfCheckedNumericStatus status;
    } powers[] = {
        {0, 0, F_ONE, OK},
        {F_SNAN, 0, 0, OVERFLOW},
        {F_MAX, 1, F_MAX, OK},
        {F_MAX, 2, 0, OVERFLOW},
        {F_TWO, 1024, 0, OVERFLOW},
        {F_SIGN | F_TWO, 3, UINT64_C(0xc020000000000000), OK},
        {F_SIGN | F_ONE, INT64_MIN, F_ONE, OK},
        {F_SIGN | F_ONE, INT64_MIN + 1, F_SIGN | F_ONE, OK},
        {F_TWO, INT64_MIN, 0, OK},
        {F_HALF, INT64_MIN, 0, OVERFLOW},
        {F_TWO, -1074, 1, OK},
        {F_TWO, -1075, 0, OK},
        {F_SIGN, -1, 0, ZERO},
        {1, -1, 0, OVERFLOW},
        /* (rounded 1/5)^2 differs by one ulp from rounded 1/25. */
        {UINT64_C(0x4014000000000000), -2, UINT64_C(0x3fa47ae147ae147c), OK},
        /* Squaring differs by one ulp from repeated left-to-right multiply. */
        {UINT64_C(0x3ff199999999999a), 5, UINT64_C(0x3ff9c4a6223e186c), OK}
    };
    uint64_t out;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        out = 123;
        CHECK(cases[i].op(cases[i].a, cases[i].b, &out) == cases[i].status);
        CHECK(out == (cases[i].status == OK ? cases[i].result : 123));
    }
    for (size_t i = 0; i < sizeof powers / sizeof *powers; ++i) {
        out = 123;
        CHECK(tf_checked_float_pow(powers[i].base, powers[i].exponent, &out) ==
              powers[i].status);
        CHECK(out == (powers[i].status == OK ? powers[i].result : 123));
    }

    /* Exercise caller conversion for Int/Int, Float/Int and Int/Float. */
    CHECK(tf_checked_float_div(tf_checked_int_to_float_bits(5),
          tf_checked_int_to_float_bits(2), &out) == OK &&
          out == UINT64_C(0x4004000000000000));
    CHECK(tf_checked_float_div(out, tf_checked_int_to_float_bits(2), &out) == OK &&
          out == UINT64_C(0x3ff4000000000000));
    CHECK(tf_checked_float_div(tf_checked_int_to_float_bits(5), F_HALF, &out) == OK &&
          out == UINT64_C(0x4024000000000000));
    CHECK(tf_checked_float_div(tf_checked_int_to_float_bits(INT64_MIN),
          tf_checked_int_to_float_bits(-1), &out) == OK &&
          out == UINT64_C(0x43e0000000000000));
    CHECK(tf_checked_float_div(tf_checked_int_to_float_bits(9007199254740993LL),
          tf_checked_int_to_float_bits(9007199254740992LL), &out) == OK && out == F_ONE);
}

static void check_environment(int rounding) {
    fenv_t saved;
    uint64_t out = 123;
    int flags;
    CHECK(feholdexcept(&saved) == 0);
    CHECK(fesetround(rounding) == 0);
    CHECK(feraiseexcept(FE_INVALID) == 0);
    flags = fetestexcept(FE_ALL_EXCEPT);
    CHECK((flags & FE_INVALID) != 0);
    CHECK(tf_checked_float_probe() == OK);
    CHECK(fegetround() == rounding && fetestexcept(FE_ALL_EXCEPT) == flags);
    CHECK(tf_checked_float_add(F_ONE, UINT64_C(0x3ca0000000000000), &out) == OK &&
          out == F_ONE);
    CHECK(tf_checked_float_add(F_ONE + 1, UINT64_C(0x3ca0000000000000), &out) == OK &&
          out == F_ONE + 2);
    CHECK(fegetround() == rounding && fetestexcept(FE_ALL_EXCEPT) == flags);
    CHECK(tf_checked_float_mul(F_MAX, F_TWO, &out) == OVERFLOW && out == F_ONE + 2);
    CHECK(fegetround() == rounding && fetestexcept(FE_ALL_EXCEPT) == flags);
    CHECK(tf_checked_float_div(F_ONE, 0, &out) == ZERO && out == F_ONE + 2);
    CHECK(fegetround() == rounding && fetestexcept(FE_ALL_EXCEPT) == flags);
    CHECK(tf_checked_float_pow(F_TWO, -1075, &out) == OK && out == 0);
    CHECK(fegetround() == rounding && fetestexcept(FE_ALL_EXCEPT) == flags);
    CHECK(tf_checked_int_to_float_bits(INT64_MAX) == UINT64_C(0x43e0000000000000));
    CHECK(fegetround() == rounding && fetestexcept(FE_ALL_EXCEPT) == flags);
    CHECK(fesetenv(&saved) == 0);
}

static void unsupported_cases(void) {
    uint64_t out = 123;
    CHECK(tf_checked_float_probe() == UNSUPPORTED);
    CHECK(tf_checked_float_add(F_ONE, F_ONE, &out) == UNSUPPORTED && out == 123);
    CHECK(tf_checked_float_sub(F_ONE, F_ONE, &out) == UNSUPPORTED && out == 123);
    CHECK(tf_checked_float_mul(F_MAX, F_MAX, &out) == UNSUPPORTED && out == 123);
    CHECK(tf_checked_float_div(F_ONE, 0, &out) == UNSUPPORTED && out == 123);
    CHECK(tf_checked_float_pow(F_SIGN, -1, &out) == UNSUPPORTED && out == 123);
}

#ifdef TF_TEST_WINDOWS_X64_TRAPS
typedef struct {
    unsigned mxcsr;
    uint16_t x87_control, x87_status;
} TrapState;

static TrapState trap_state(void) {
    TrapState state;
    state.mxcsr = _mm_getcsr();
    __asm__ __volatile__("fnstcw %0; fnstsw %1"
                         : "=m"(state.x87_control), "=m"(state.x87_status));
    return state;
}

typedef enum { TRAP_OVERFLOW, TRAP_INEXACT, TRAP_FTZ, TRAP_PENDING_X87 } TrapCase;

static void check_traps(TrapCase test) {
    unsigned sse_unmask = test == TRAP_OVERFLOW ? 8u : 32u;
    unsigned x87_unmask = test == TRAP_PENDING_X87 ? 1u : sse_unmask;
    uint16_t control = UINT16_C(0x037f);
    uint64_t out = 123;
    TfCheckedNumericStatus status;
    TrapState before, after;

    /* Seed distinct sticky flags in x87 and SSE, and different rounding
     * modes in their control words. For the pending-x87 case, unmask the
     * preexisting invalid flag without executing a waiting instruction. */
    __asm__ __volatile__("fnclex; fldcw %0; fldz; fldz; "
                         "fdivp %%st, %%st(1); fstp %%st(0)"
                         : : "m"(control) : "st", "st(1)", "memory");
    control = (uint16_t)(UINT16_C(0x077f) & ~x87_unmask);
    __asm__ __volatile__("fldcw %0" : : "m"(control) : "memory");
    _mm_setcsr((0x5f84u & ~(sse_unmask << 7)) | (test == TRAP_FTZ ? 0x8000u : 0u));
    before = trap_state();
    CHECK((before.mxcsr & (sse_unmask << 7)) == 0);
    CHECK((before.x87_control & x87_unmask) == 0);
    CHECK((before.x87_status & 0x3fu) == 1);
    CHECK((before.mxcsr & 0x3fu) == 4);
    CHECK(((before.x87_status & 0x80u) != 0) == (test == TRAP_PENDING_X87));

    if (test == TRAP_OVERFLOW) {
        /* The old code survived its probe with this trap enabled, then
         * terminated instead of returning a fault from this operation. */
        status = tf_checked_float_mul(F_MAX, F_TWO, &out);
        CHECK(status == OVERFLOW && out == 123);
    } else if (test == TRAP_FTZ) {
        CHECK(tf_checked_float_pow(F_TWO, 2, &out) == UNSUPPORTED && out == 123);
    } else {
        /* Halfway rounding raises inexact during the probe itself. */
        status = tf_checked_float_probe();
        CHECK(status == OK);
    }
    after = trap_state();
    CHECK(after.mxcsr == before.mxcsr);
    CHECK(after.x87_control == before.x87_control);
    CHECK((after.x87_status & 0xffu) == (before.x87_status & 0xffu));
}

static void check_x87_pointers(void) {
    const uint64_t ip = UINT64_C(0x0000000140123456);
    const uint64_t dp = UINT64_C(0x0000000140654321);
    for (unsigned operation = 0; operation < 6; ++operation) {
        _Alignas(16) unsigned char before[512], after[512];
        uint64_t actual_ip, actual_dp, out = 123;
        TfCheckedNumericStatus status;
        /* Seed high pointers, a pending invalid exception, and a nonempty
         * x87 stack. Patch only x87 fields in one assembly block: never
         * reload a stale SIMD image after the compiler has used XMM values. */
        __asm__ __volatile__(
            "fninit; fld1; fldz; fxsave64 %0;"
            "movw $0x037e, 0(%1); orw $0x0081, 2(%1);"
            "movw $0x01f1, 6(%1); movq %2, 8(%1); movq %3, 16(%1);"
            "fxrstor64 %0; fxsave64 %0"
            : "=m"(before) : "r"(before), "r"(ip), "r"(dp)
            : "st", "st(1)", "memory");
        memcpy(&actual_ip, before + 8, 8);
        memcpy(&actual_dp, before + 16, 8);
        CHECK(actual_ip == ip && actual_dp == dp);
        switch (operation) {
        case 0: status = tf_checked_float_probe(); break;
        case 1: status = tf_checked_float_add(F_ONE, F_TWO, &out); break;
        case 2: status = tf_checked_float_mul(F_MAX, F_TWO, &out); break;
        case 3: status = tf_checked_float_div(F_ONE, 0, &out); break;
        case 4: status = tf_checked_float_pow(F_TWO, -1075, &out); break;
        default:
            _mm_setcsr(_mm_getcsr() | 0x8000u);
            status = tf_checked_float_probe();
            break;
        }
        __asm__ __volatile__("fxsave64 %0; fninit"
                             : "=m"(after) : : "st", "st(1)", "memory");
        CHECK(status == (operation == 2 ? OVERFLOW : operation == 3 ? ZERO :
                         operation == 5 ? UNSUPPORTED : OK));
        CHECK(memcmp(before, after, 5) == 0); /* Control/status/tag. */
        CHECK(memcmp(before + 6, after + 6, 18) == 0); /* FOP, full FIP/FDP. */
        for (size_t i = 0; i < 8; ++i)
            CHECK(memcmp(before + 32 + i * 16, after + 32 + i * 16, 10) == 0);
        if (operation == 2 || operation == 3 || operation == 5) CHECK(out == 123);
        _mm_setcsr(0x1f80u);
    }
}

static void check_live_simd(void) {
    __m128i vectors[10];
    uint64_t out = 123;
    for (unsigned pass = 0; pass < 4; ++pass) {
        for (size_t i = 0; i < 10; ++i) {
            int64_t value = (int64_t)((i + 1) * 12345 + pass);
            vectors[i] = _mm_set_epi64x(-value, value);
        }
        /* Force ten distinct vector values live across the public call.
         * On Windows optimized builds use nonvolatile XMM6-15 (or spills).
         * Barriers prevent folding the final comparisons to constants. */
        __asm__ __volatile__("" :
            "+x"(vectors[0]), "+x"(vectors[1]), "+x"(vectors[2]), "+x"(vectors[3]),
            "+x"(vectors[4]), "+x"(vectors[5]), "+x"(vectors[6]), "+x"(vectors[7]),
            "+x"(vectors[8]), "+x"(vectors[9]));
        switch (pass) {
        case 0:
            CHECK(tf_checked_float_add(F_ONE, F_TWO, &out) == OK &&
                  out == UINT64_C(0x4008000000000000));
            break;
        case 1:
            CHECK(tf_checked_float_mul(F_MAX, F_TWO, &out) == OVERFLOW);
            break;
        case 2:
            CHECK(tf_checked_float_pow(F_TWO, -1074, &out) == OK && out == 1);
            break;
        default:
            _mm_setcsr(0x9f80u);
            CHECK(tf_checked_float_probe() == UNSUPPORTED);
            _mm_setcsr(0x1f80u);
            break;
        }
        __asm__ __volatile__("" :
            "+x"(vectors[0]), "+x"(vectors[1]), "+x"(vectors[2]), "+x"(vectors[3]),
            "+x"(vectors[4]), "+x"(vectors[5]), "+x"(vectors[6]), "+x"(vectors[7]),
            "+x"(vectors[8]), "+x"(vectors[9]));
        for (size_t i = 0; i < 10; ++i) {
            uint64_t words[2], value = (i + 1) * 12345 + pass;
            _mm_storeu_si128((__m128i *)(void *)words, vectors[i]);
            CHECK(words[0] == value && words[1] == (uint64_t)(-(int64_t)value));
        }
    }
}

static int trap_child(void) {
    check_traps(TRAP_OVERFLOW);
    check_traps(TRAP_INEXACT);
    check_traps(TRAP_FTZ);
    check_traps(TRAP_PENDING_X87);
    check_x87_pointers();
    /* Leave default masked controls for the C library at process exit. */
    {
        uint16_t control = UINT16_C(0x037f);
        __asm__ __volatile__("fnclex; fldcw %0" : : "m"(control) : "memory");
        _mm_setcsr(0x1f80u);
    }
    check_live_simd();
    printf("checked numeric trap child: %zu checks passed\n", checks);
    return 0;
}

static void check_trap_subprocess(void) {
    wchar_t path[32768], args[] = L"test_checked_numeric --trap-child";
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION child;
    DWORD length = GetModuleFileNameW(NULL, path, (DWORD)(sizeof path / sizeof *path));
    DWORD result, code = 0;
    UINT error_mode;
    BOOL launched, exited;
    CHECK(length > 0 && length < sizeof path / sizeof *path);
    startup.cb = sizeof startup;
    error_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                              SEM_NOOPENFILEERRORBOX);
    launched = CreateProcessW(path, args, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child);
    (void)SetErrorMode(error_mode);
    CHECK(launched != 0);
    result = WaitForSingleObject(child.hProcess, 30000);
    if (result != WAIT_OBJECT_0) (void)TerminateProcess(child.hProcess, 1);
    exited = result == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &code);
    CHECK(CloseHandle(child.hThread) != 0);
    CHECK(CloseHandle(child.hProcess) != 0);
    CHECK(exited != 0);
    if (code != 0) fprintf(stderr, "checked numeric trap child exit: 0x%08lx\n",
                           (unsigned long)code);
    CHECK(code == 0);
}
#endif

int main(int argc, char **argv) {
#ifdef TF_TEST_WINDOWS_X64_TRAPS
    if (argc == 2 && strcmp(argv[1], "--trap-child") == 0) return trap_child();
#endif
    int_cases();
    conversion_and_sign();
    if (argc == 2 && strcmp(argv[1], "--expect-unsupported") == 0) {
        unsupported_cases();
    } else {
        CHECK(argc == 1);
        CHECK(tf_checked_float_probe() == OK);
        float_cases();
        check_environment(FE_TONEAREST);
        check_environment(FE_UPWARD);
        check_environment(FE_DOWNWARD);
        check_environment(FE_TOWARDZERO);
#ifdef TF_TEST_WINDOWS_X64_TRAPS
        check_trap_subprocess();
#endif
    }
    printf("checked numeric: %zu checks passed\n", checks);
    return 0;
}
