#ifndef TERNFOLD_CHECKED_NUMERIC_H
#define TERNFOLD_CHECKED_NUMERIC_H

#include <stdint.h>

typedef enum {
    TF_CHECKED_NUMERIC_OK = 0,
    TF_CHECKED_NUMERIC_OVERFLOW,          /* R001 */
    TF_CHECKED_NUMERIC_ZERO_DIVISOR,      /* R002 */
    TF_CHECKED_NUMERIC_NEGATIVE_EXPONENT, /* R005 */
    TF_CHECKED_NUMERIC_UNSUPPORTED       /* Host capability failure */
} TfCheckedNumericStatus;

/* Scalar inputs are passed by value; no allocation or ownership transfer.
 * Supply a writable, non-null out pointer. Only success writes *out.
 * Callers attach source spans and map arithmetic faults to E306 or Rxxx;
 * UNSUPPORTED is a host error, not a source diagnostic or language fault. */
TfCheckedNumericStatus tf_checked_int_add(int64_t a, int64_t b, int64_t *out);
TfCheckedNumericStatus tf_checked_int_sub(int64_t a, int64_t b, int64_t *out);
TfCheckedNumericStatus tf_checked_int_mul(int64_t a, int64_t b, int64_t *out);
TfCheckedNumericStatus tf_checked_int_neg(int64_t value, int64_t *out);
/* Floor quotient and truncating remainder, respectively. Both fault on
 * zero and on INT64_MIN/-1. */
TfCheckedNumericStatus tf_checked_int_div(int64_t a, int64_t b, int64_t *out);
TfCheckedNumericStatus tf_checked_int_rem(int64_t a, int64_t b, int64_t *out);
TfCheckedNumericStatus tf_checked_int_pow(int64_t base, int64_t exponent,
                                         int64_t *out);

/* Exact nearest/even conversion to binary64 bits, independent of the host
 * floating-point representation and environment. For mixed or Int real /,
 * convert each Int operand here before calling tf_checked_float_div. */
uint64_t tf_checked_int_to_float_bits(int64_t value);

/* Float operands/results are IEEE 754 binary64 bit patterns. Both input zero
 * signs are accepted, output zero is always positive, and nonfinite operands
 * or intermediates report OVERFLOW. Negation is bit-only like Int conversion.
 *
 * Probe and host arithmetic require strict Clang on x86-64 with binary64
 * double and no excess evaluation precision. Compile without fast/unsafe
 * math. Each call saves SSE and x87 state, verifies traps are masked on both
 * units before any arithmetic, and checks nearest/even and gradual underflow.
 * The caller's controls, flags and full x87 pointers/stack are restored and
 * checked before publishing a result; live SIMD registers are preserved.
 * Failure to establish or verify the environment reports
 * UNSUPPORTED (including in preference to operand faults). */
TfCheckedNumericStatus tf_checked_float_probe(void);
TfCheckedNumericStatus tf_checked_float_neg(uint64_t value, uint64_t *out);
TfCheckedNumericStatus tf_checked_float_add(uint64_t a, uint64_t b, uint64_t *out);
TfCheckedNumericStatus tf_checked_float_sub(uint64_t a, uint64_t b, uint64_t *out);
TfCheckedNumericStatus tf_checked_float_mul(uint64_t a, uint64_t b, uint64_t *out);
TfCheckedNumericStatus tf_checked_float_div(uint64_t a, uint64_t b, uint64_t *out);
/* Negative exponents start with a rounded, checked reciprocal. */
TfCheckedNumericStatus tf_checked_float_pow(uint64_t base, int64_t exponent,
                                           uint64_t *out);

#endif
