#ifndef TERNFOLD_EXEC_SCALARS_H
#define TERNFOLD_EXEC_SCALARS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Capacities include the terminating NUL: at most 20 Int or 24 Float
 * ASCII bytes. Formatters return the byte length excluding that NUL and
 * need a writable buffer of the indicated capacity. */
enum {
    TF_EXEC_INT_TEXT_CAPACITY = 21,
    TF_EXEC_FLOAT_TEXT_CAPACITY = 25
};

size_t tf_exec_int_text(int64_t value, char out[static TF_EXEC_INT_TEXT_CAPACITY]);

/* Float arguments are finite binary64 bit patterns, with either zero sign
 * accepted and normalized. A nonfinite pattern returns 0 (text) or false
 * (conversion) without writing the output. Conversion needs a writable,
 * non-null out pointer and only writes it on success. For language-level
 * float_int, callers use verified canonical Float values or check the
 * domain first: only a finite out-of-range result represents Option::None. */
size_t tf_exec_float_text(uint64_t bits, char out[static TF_EXEC_FLOAT_TEXT_CAPACITY]);
bool tf_exec_float_int(uint64_t bits, int64_t *out);

/* Parse only the indicated byte span (NULL is valid for length zero).
 * Only a successful conversion writes to the caller's non-null *out. */
bool tf_exec_parse_int(const unsigned char *bytes, size_t length, int64_t *out);

#endif
