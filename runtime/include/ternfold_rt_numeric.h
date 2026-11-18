#ifndef TERNFOLD_RT_NUMERIC_H
#define TERNFOLD_RT_NUMERIC_H

#include "ternfold_rt.h"

/* Link ternfold_rt_numeric.c and the shared src/checked_numeric.c, with
 * include/ on the build include path. Build the shared helper in strict
 * floating-point mode (no fast/unsafe math). Generated C needs only this
 * header and the native ABI header. */
typedef enum {
    TF_RT_NUM_ADD = 1, TF_RT_NUM_SUBTRACT, TF_RT_NUM_MULTIPLY,
    TF_RT_NUM_REAL_DIVIDE, TF_RT_NUM_FLOOR_DIVIDE, TF_RT_NUM_REMAINDER,
    TF_RT_NUM_POWER, TF_RT_NUM_LESS, TF_RT_NUM_LESS_EQUAL,
    TF_RT_NUM_GREATER, TF_RT_NUM_GREATER_EQUAL, TF_RT_NUM_EQUAL,
    TF_RT_NUM_NOT_EQUAL
} tf_numeric_op;

/* The driver must probe on the execution thread before evaluating native
 * code. False means unsupported host FP capabilities: report a host error
 * and do not start evaluation. This does not read or fault a context. The
 * shared helper also checks/restores the FP environment for every operation
 * that uses host arithmetic. */
bool tf_numeric_float_supported(void);

/* Borrow valid values. The out-slot is writable, disjoint from inputs and
 * owns no heap reference. Write one inline owner on success and Unit on
 * failure; an existing fault takes precedence and is left untouched.
 * Invalid operators or operand tags fault R004. Arithmetic faults carry the
 * supplied operator span. If the shared FP helper still reports unsupported
 * at use, return TF_FAULT without recording a language fault. Unwind on any
 * TF_FAULT; at the driver boundary, a failed evaluation without a context
 * fault is a host error and cannot publish output.
 *
 * Add/subtract/multiply preserve matching Int or Float types. Real division
 * accepts all Int/Float pairs and returns Float. Floor division/remainder
 * require Ints. Power takes an Int exponent and preserves the base type.
 * Relations, including equality, accept only matching Int or Float types
 * and return Bool; general value equality and short circuiting are separate. */
tf_status tf_numeric_binary(tf_context *ctx, tf_span_id span, tf_numeric_op op,
                            tf_value a, tf_value b, tf_value *out);
tf_status tf_numeric_negate(tf_context *ctx, tf_span_id span, tf_value value,
                            tf_value *out);
tf_status tf_numeric_not(tf_context *ctx, tf_span_id span, tf_value value,
                         tf_value *out);

#endif
