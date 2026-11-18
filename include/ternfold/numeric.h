#ifndef TERNFOLD_NUMERIC_H
#define TERNFOLD_NUMERIC_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TF_NUMERIC_OK = 0,
    TF_NUMERIC_INVALID,
    TF_NUMERIC_RANGE
} TfNumericStatus;

/* Borrow unsigned, length-delimited ASCII token bytes. Malformed syntax
 * takes precedence over range. NULL input/output is invalid. Only success
 * writes the output. Conversion uses bounded local storage and cannot fail
 * due to host allocation. */
TfNumericStatus tf_parse_int_magnitude(const unsigned char *bytes, size_t length,
                                       uint64_t *out);
TfNumericStatus tf_parse_float_bits(const unsigned char *bytes, size_t length,
                                    uint64_t *out);

/* Int accepts magnitudes through 2^63 inclusive; the caller must reject
 * 2^63 unless the separate minus token grants the minimum-Int exception.
 * Float requires a fraction or exponent and returns nearest/even binary64
 * bits as an unsigned integer. Finite underflow (including +0) succeeds.
 * Leading signs are not token bytes; exponent signs are accepted. */

#endif
