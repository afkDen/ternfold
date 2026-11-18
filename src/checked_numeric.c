#include "ternfold/checked_numeric.h"

#include <float.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

#define FLOAT_SIGN UINT64_C(0x8000000000000000)
#define FLOAT_EXP UINT64_C(0x7ff0000000000000)
#define FLOAT_FRAC UINT64_C(0x000fffffffffffff)
#define FLOAT_ONE UINT64_C(0x3ff0000000000000)

static uint64_t magnitude(int64_t value) {
    /* Adding before negation keeps INT64_MIN in the signed domain. */
    return value < 0 ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
}

TfCheckedNumericStatus tf_checked_int_add(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return TF_CHECKED_NUMERIC_OVERFLOW;
    *out = a + b;
    return TF_CHECKED_NUMERIC_OK;
}

TfCheckedNumericStatus tf_checked_int_sub(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b))
        return TF_CHECKED_NUMERIC_OVERFLOW;
    *out = a - b;
    return TF_CHECKED_NUMERIC_OK;
}

TfCheckedNumericStatus tf_checked_int_mul(int64_t a, int64_t b, int64_t *out) {
    bool negative = (a < 0) != (b < 0);
    uint64_t left = magnitude(a), right = magnitude(b);
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    uint64_t product;
    if (right != 0 && left > limit / right) return TF_CHECKED_NUMERIC_OVERFLOW;
    product = left * right;
    if (product > (uint64_t)INT64_MAX) *out = INT64_MIN; /* Exactly 2^63. */
    else *out = negative ? -(int64_t)product : (int64_t)product;
    return TF_CHECKED_NUMERIC_OK;
}

TfCheckedNumericStatus tf_checked_int_neg(int64_t value, int64_t *out) {
    if (value == INT64_MIN) return TF_CHECKED_NUMERIC_OVERFLOW;
    *out = -value;
    return TF_CHECKED_NUMERIC_OK;
}

static TfCheckedNumericStatus int_div_domain(int64_t a, int64_t b) {
    if (b == 0) return TF_CHECKED_NUMERIC_ZERO_DIVISOR;
    if (a == INT64_MIN && b == -1) return TF_CHECKED_NUMERIC_OVERFLOW;
    return TF_CHECKED_NUMERIC_OK;
}

TfCheckedNumericStatus tf_checked_int_div(int64_t a, int64_t b, int64_t *out) {
    TfCheckedNumericStatus status = int_div_domain(a, b);
    int64_t quotient;
    if (status != TF_CHECKED_NUMERIC_OK) return status;
    quotient = a / b;
    /* INT64_MIN is possible only for INT64_MIN/1, with no remainder. */
    if ((a < 0) != (b < 0) && a % b != 0) --quotient;
    *out = quotient;
    return TF_CHECKED_NUMERIC_OK;
}

TfCheckedNumericStatus tf_checked_int_rem(int64_t a, int64_t b, int64_t *out) {
    TfCheckedNumericStatus status = int_div_domain(a, b);
    if (status != TF_CHECKED_NUMERIC_OK) return status;
    *out = a % b;
    return TF_CHECKED_NUMERIC_OK;
}

TfCheckedNumericStatus tf_checked_int_pow(int64_t base, int64_t exponent,
                                         int64_t *out) {
    int64_t result = 1, factor = base;
    uint64_t remaining;
    if (exponent < 0) return TF_CHECKED_NUMERIC_NEGATIVE_EXPONENT;
    remaining = (uint64_t)exponent;
    while (remaining != 0) {
        TfCheckedNumericStatus status;
        if ((remaining & 1) != 0) {
            status = tf_checked_int_mul(result, factor, &result);
            if (status != TF_CHECKED_NUMERIC_OK) return status;
        }
        remaining >>= 1;
        if (remaining != 0) {
            status = tf_checked_int_mul(factor, factor, &factor);
            if (status != TF_CHECKED_NUMERIC_OK) return status;
        }
    }
    *out = result;
    return TF_CHECKED_NUMERIC_OK;
}

uint64_t tf_checked_int_to_float_bits(int64_t value) {
    uint64_t significand = magnitude(value), scan = significand;
    unsigned top = 0;
    if (significand == 0) return 0;
    while ((scan >>= 1) != 0) ++top;
    if (top <= 52) significand <<= 52 - top;
    else {
        unsigned shift = top - 52; /* At most eleven bits are discarded. */
        uint64_t half = UINT64_C(1) << (shift - 1);
        uint64_t tail = significand & ((UINT64_C(1) << shift) - 1);
        significand >>= shift;
        if (tail > half || (tail == half && (significand & 1) != 0)) {
            ++significand;
            if (significand == (UINT64_C(1) << 53)) {
                significand >>= 1;
                ++top;
            }
        }
    }
    return (value < 0 ? FLOAT_SIGN : 0) | ((uint64_t)(top + 1023) << 52) |
           (significand & FLOAT_FRAC);
}

static bool float_finite(uint64_t bits) {
    return (bits & FLOAT_EXP) != FLOAT_EXP;
}

static uint64_t float_normalize(uint64_t bits) {
    return (bits & ~FLOAT_SIGN) == 0 ? 0 : bits;
}

TfCheckedNumericStatus tf_checked_float_neg(uint64_t value, uint64_t *out) {
    if (!float_finite(value)) return TF_CHECKED_NUMERIC_OVERFLOW;
    *out = float_normalize(value ^ FLOAT_SIGN);
    return TF_CHECKED_NUMERIC_OK;
}

typedef enum { FLOAT_ADD, FLOAT_SUB, FLOAT_MUL, FLOAT_DIV } FloatOp;

/* The native branch relies on Clang honoring the C floating-point pragmas
 * and providing IEC 60559 arithmetic (including infinity on overflow).
 * Refuse unknown compiler modes rather than using unchecked host doubles. */
#if defined(__clang__) && defined(__x86_64__) && !defined(__FAST_MATH__) && \
    (!defined(_WIN32) || defined(__MINGW32__)) && \
    (!defined(__FINITE_MATH_ONLY__) || __FINITE_MATH_ONLY__ == 0) && \
    CHAR_BIT == 8 && FLT_RADIX == 2 && DBL_MANT_DIG == 53 && \
    DBL_MIN_EXP == -1021 && DBL_MAX_EXP == 1024 && FLT_EVAL_METHOD == 0

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

typedef struct {
    _Alignas(16) unsigned char image[512];
} NativeEnv;

#define SSE_FLAGS       UINT32_C(0x003f)
#define SSE_TRAP_MASKS  UINT32_C(0x1f80)
#define SSE_ROUND_MASK  UINT32_C(0x6000)
#define SSE_DAZ_FTZ     UINT32_C(0x8040)
#define X87_TRAP_MASKS  UINT16_C(0x003f)
#define X87_ROUND_MASK  UINT16_C(0x0c00)

static uint32_t native_mxcsr(void) {
    uint32_t value;
    __asm__ __volatile__("stmxcsr %0" : "=m"(value) : : "memory");
    return value;
}

static uint16_t native_x87_control(void) {
    uint16_t value;
    __asm__ __volatile__("fnstcw %0" : "=m"(value) : : "memory");
    return value;
}

static double bits_to_double(uint64_t bits) {
    double value = 0.0;
    if (sizeof value == sizeof bits) memcpy(&value, &bits, sizeof bits);
    return value;
}

static uint64_t double_to_bits(double value) {
    uint64_t bits = 0;
    if (sizeof value == sizeof bits) memcpy(&bits, &value, sizeof bits);
    return bits;
}

static uint64_t native_binary(FloatOp op, double a, double b) {
    /* Materialize each operation separately at binary64 width. */
    volatile double rounded = 0.0;
    switch (op) {
    case FLOAT_ADD: rounded = a + b; break;
    case FLOAT_SUB: rounded = a - b; break;
    case FLOAT_MUL: rounded = a * b; break;
    case FLOAT_DIV: rounded = a / b; break;
    }
    return double_to_bits(rounded);
}

static bool native_layout(void) {
    return sizeof(double) == sizeof(uint64_t) &&
           double_to_bits(-0x1.23456789abcdep+0) == UINT64_C(0xbff23456789abcde) &&
           double_to_bits(DBL_MAX) == UINT64_C(0x7fefffffffffffff) &&
           double_to_bits(DBL_MIN) == UINT64_C(0x0010000000000000) &&
           double_to_bits(DBL_TRUE_MIN) == 1;
}

static bool native_rounding(void) {
    /* Read actual operands so these probes cannot be constant-folded. The
     * two halfway additions reject each directed mode; the products test
     * both gradual underflow and acceptance of subnormal inputs. */
    volatile double one = 1.0, odd = 0x1.0000000000001p+0, tie = 0x1p-53;
    volatile double half = 0.5, normal = DBL_MIN, tiny = DBL_TRUE_MIN;
    volatile double scale = 0x1p52;
    return native_binary(FLOAT_ADD, one, tie) == FLOAT_ONE &&
           native_binary(FLOAT_ADD, odd, tie) == UINT64_C(0x3ff0000000000002) &&
           native_binary(FLOAT_MUL, normal, half) == UINT64_C(0x0008000000000000) &&
           native_binary(FLOAT_MUL, tiny, scale) == UINT64_C(0x0010000000000000);
}

static TfCheckedNumericStatus native_restore(const NativeEnv *saved,
                                             TfCheckedNumericStatus status) {
    NativeEnv current, verified;
    uint64_t scratch;
    /* FNSTENV/FLDENV truncate FIP/FDP to 32 bits in long mode. Instead,
     * patch a fresh FXSAVE64 image with the original x87 environment/stack
     * and MXCSR. This entire save/patch/restore is one assembly block.
     * XMM0-15 and MXCSR_MASK remain from the CURRENT register state, so no
     * stale SIMD values can overwrite compiler temporaries or ABI registers.
     * Only GPR loads/stores occur between FXSAVE64 and FXRSTOR64.
     * FXSAVE64/FXRSTOR64 are non-waiting even for pending x87 exceptions. */
    __asm__ __volatile__(
        "fxsave64 %0;"
        "movq 0(%3), %1; movq %1, 0(%4);"
        "movq 8(%3), %1; movq %1, 8(%4);"
        "movq 16(%3), %1; movq %1, 16(%4);"
        "movq 32(%3), %1; movq %1, 32(%4);"
        "movq 40(%3), %1; movq %1, 40(%4);"
        "movq 48(%3), %1; movq %1, 48(%4);"
        "movq 56(%3), %1; movq %1, 56(%4);"
        "movq 64(%3), %1; movq %1, 64(%4);"
        "movq 72(%3), %1; movq %1, 72(%4);"
        "movq 80(%3), %1; movq %1, 80(%4);"
        "movq 88(%3), %1; movq %1, 88(%4);"
        "movq 96(%3), %1; movq %1, 96(%4);"
        "movq 104(%3), %1; movq %1, 104(%4);"
        "movq 112(%3), %1; movq %1, 112(%4);"
        "movq 120(%3), %1; movq %1, 120(%4);"
        "movq 128(%3), %1; movq %1, 128(%4);"
        "movq 136(%3), %1; movq %1, 136(%4);"
        "movq 144(%3), %1; movq %1, 144(%4);"
        "movq 152(%3), %1; movq %1, 152(%4);"
        "movl 24(%3), %k1; movl %k1, 24(%4);"
        "fxrstor64 %0; fxsave64 %2"
        : "=m"(current.image), "=&r"(scratch), "=m"(verified.image)
        : "r"(saved->image), "r"(current.image) : "memory");
    /* Byte 5 and the six bytes after each 80-bit register are reserved. */
    if (memcmp(verified.image, saved->image, 5) != 0 ||
        memcmp(verified.image + 6, saved->image + 6, 22) != 0 ||
        memcmp(verified.image + 160, current.image + 160, 256) != 0)
        return TF_CHECKED_NUMERIC_UNSUPPORTED;
    for (size_t i = 0; i < 8; ++i) {
        /* Each 80-bit x87 value has six reserved padding bytes. */
        if (memcmp(verified.image + 32 + i * 16, saved->image + 32 + i * 16, 10) != 0)
            return TF_CHECKED_NUMERIC_UNSUPPORTED;
    }
    return status;
}

static TfCheckedNumericStatus native_begin(NativeEnv *saved) {
    uint32_t mxcsr;
    uint16_t control;
    /* UCRT feholdexcept can report success without masking SSE traps.
     * Snapshot both units directly with full 64-bit x87 pointers.
     * Clear pending x87 flags with non-waiting FNCLEX before FLDCW.
     * Avoid CRT fenv operations that may merge the two units' independently
     * configured states. */
    __asm__ __volatile__("fxsave64 %0" : "=m"(saved->image) : : "memory");
    memcpy(&mxcsr, saved->image + 24, sizeof mxcsr);
    memcpy(&control, saved->image, sizeof control);
    mxcsr = (mxcsr | SSE_TRAP_MASKS) & ~(SSE_FLAGS | SSE_ROUND_MASK);
    control = (uint16_t)((control | X87_TRAP_MASKS) &
                         ~(uint32_t)X87_ROUND_MASK);
    __asm__ __volatile__("fnclex; fldcw %0; ldmxcsr %1"
                         : : "m"(control), "m"(mxcsr) : "memory");
    /* Check control state before doing any floating arithmetic. */
    if (native_mxcsr() != mxcsr || native_x87_control() != control ||
        (mxcsr & SSE_DAZ_FTZ) != 0 || !native_layout() || !native_rounding())
        return native_restore(saved, TF_CHECKED_NUMERIC_UNSUPPORTED);
    return TF_CHECKED_NUMERIC_OK;
}

static TfCheckedNumericStatus float_step(FloatOp op, uint64_t a, uint64_t b,
                                        uint64_t *out) {
    uint64_t bits;
    if (!float_finite(a) || !float_finite(b)) return TF_CHECKED_NUMERIC_OVERFLOW;
    if (op == FLOAT_DIV && float_normalize(b) == 0)
        return TF_CHECKED_NUMERIC_ZERO_DIVISOR;
    bits = native_binary(op, bits_to_double(a), bits_to_double(b));
    if (!float_finite(bits)) return TF_CHECKED_NUMERIC_OVERFLOW;
    *out = float_normalize(bits);
    return TF_CHECKED_NUMERIC_OK;
}

TfCheckedNumericStatus tf_checked_float_probe(void) {
    NativeEnv saved;
    TfCheckedNumericStatus status = native_begin(&saved);
    if (status != TF_CHECKED_NUMERIC_OK) return status;
    return native_restore(&saved, status);
}

static TfCheckedNumericStatus float_binary(FloatOp op, uint64_t a, uint64_t b,
                                           uint64_t *out) {
    NativeEnv saved;
    uint64_t result;
    TfCheckedNumericStatus status = native_begin(&saved);
    TfCheckedNumericStatus restored;
    if (status != TF_CHECKED_NUMERIC_OK) return status;
    status = float_step(op,a,b,&result);
    restored = native_restore(&saved,TF_CHECKED_NUMERIC_OK);
    if (restored != TF_CHECKED_NUMERIC_OK) return restored;
    if (status == TF_CHECKED_NUMERIC_OK) *out = result;
    return status;
}

static TfCheckedNumericStatus float_power(uint64_t base, int64_t exponent,
                                          uint64_t *out) {
    uint64_t result = FLOAT_ONE, factor = base, remaining = magnitude(exponent);
    TfCheckedNumericStatus status;
    if (!float_finite(base)) return TF_CHECKED_NUMERIC_OVERFLOW;
    if (exponent < 0) {
        status = float_step(FLOAT_DIV, FLOAT_ONE, base, &factor);
        if (status != TF_CHECKED_NUMERIC_OK) return status;
    }
    while (remaining != 0) {
        if ((remaining & 1) != 0) {
            status = float_step(FLOAT_MUL, result, factor, &result);
            if (status != TF_CHECKED_NUMERIC_OK) return status;
        }
        remaining >>= 1;
        if (remaining != 0) {
            status = float_step(FLOAT_MUL, factor, factor, &factor);
            if (status != TF_CHECKED_NUMERIC_OK) return status;
        }
    }
    *out = result;
    return TF_CHECKED_NUMERIC_OK;
}

TfCheckedNumericStatus tf_checked_float_pow(uint64_t base, int64_t exponent,
                                           uint64_t *out) {
    NativeEnv saved;
    uint64_t result;
    TfCheckedNumericStatus status = native_begin(&saved);
    TfCheckedNumericStatus restored;
    if (status != TF_CHECKED_NUMERIC_OK) return status;
    status = float_power(base,exponent,&result);
    restored = native_restore(&saved,TF_CHECKED_NUMERIC_OK);
    if (restored != TF_CHECKED_NUMERIC_OK) return restored;
    if (status == TF_CHECKED_NUMERIC_OK) *out = result;
    return status;
}

#else

TfCheckedNumericStatus tf_checked_float_probe(void) {
    return TF_CHECKED_NUMERIC_UNSUPPORTED;
}

static TfCheckedNumericStatus float_binary(FloatOp op, uint64_t a, uint64_t b,
                                           uint64_t *out) {
    (void)op;
    (void)a;
    (void)b;
    (void)out;
    return TF_CHECKED_NUMERIC_UNSUPPORTED;
}

TfCheckedNumericStatus tf_checked_float_pow(uint64_t base, int64_t exponent,
                                           uint64_t *out) {
    (void)base;
    (void)exponent;
    (void)out;
    return TF_CHECKED_NUMERIC_UNSUPPORTED;
}

#endif

TfCheckedNumericStatus tf_checked_float_add(uint64_t a, uint64_t b, uint64_t *out) {
    return float_binary(FLOAT_ADD, a, b, out);
}

TfCheckedNumericStatus tf_checked_float_sub(uint64_t a, uint64_t b, uint64_t *out) {
    return float_binary(FLOAT_SUB, a, b, out);
}

TfCheckedNumericStatus tf_checked_float_mul(uint64_t a, uint64_t b, uint64_t *out) {
    return float_binary(FLOAT_MUL, a, b, out);
}

TfCheckedNumericStatus tf_checked_float_div(uint64_t a, uint64_t b, uint64_t *out) {
    return float_binary(FLOAT_DIV, a, b, out);
}
