#include "ternfold_rt_numeric.h"

#include "ternfold/checked_numeric.h"

typedef TfCheckedNumericStatus (*int_binary)(int64_t, int64_t, int64_t *);
typedef TfCheckedNumericStatus (*float_binary)(uint64_t, uint64_t, uint64_t *);

bool tf_numeric_float_supported(void) {
    return tf_checked_float_probe() == TF_CHECKED_NUMERIC_OK;
}

static bool start(const tf_context *ctx, tf_value *out) {
    *out = tf_unit();
    return tf_context_fault(ctx) == NULL;
}

static tf_status checked(tf_context *ctx, tf_span_id span, TfCheckedNumericStatus status) {
    switch (status) {
        case TF_CHECKED_NUMERIC_OK: return TF_OK;
        case TF_CHECKED_NUMERIC_OVERFLOW: return tf_context_fail(ctx, TF_R001, span);
        case TF_CHECKED_NUMERIC_ZERO_DIVISOR: return tf_context_fail(ctx, TF_R002, span);
        case TF_CHECKED_NUMERIC_NEGATIVE_EXPONENT: return tf_context_fail(ctx, TF_R005, span);
        case TF_CHECKED_NUMERIC_UNSUPPORTED: return TF_FAULT;
    }
    return tf_context_fail(ctx, TF_R004, span);
}

static tf_status same_type(tf_context *ctx, tf_span_id span, tf_value a, tf_value b,
                           int_binary int_op, float_binary float_op, tf_value *out) {
    if (a.tag == TF_VALUE_INT && b.tag == TF_VALUE_INT) {
        int64_t result;
        if (checked(ctx, span, int_op(a.as.integer, b.as.integer, &result)) != TF_OK)
            return TF_FAULT;
        *out = tf_int(result);
        return TF_OK;
    }
    if (float_op && a.tag == TF_VALUE_FLOAT && b.tag == TF_VALUE_FLOAT) {
        uint64_t result;
        if (checked(ctx, span, float_op(a.as.float_bits, b.as.float_bits, &result)) != TF_OK)
            return TF_FAULT;
        return tf_float(ctx, span, result, out);
    }
    return tf_context_fail(ctx, TF_R004, span);
}

static bool real_operand(tf_value value, uint64_t *bits) {
    if (value.tag == TF_VALUE_FLOAT) *bits = value.as.float_bits;
    else if (value.tag == TF_VALUE_INT) *bits = tf_checked_int_to_float_bits(value.as.integer);
    else return false;
    return true;
}

static tf_status real_divide(tf_context *ctx, tf_span_id span, tf_value a, tf_value b,
                             tf_value *out) {
    uint64_t left, right, result;
    if (!real_operand(a, &left) || !real_operand(b, &right))
        return tf_context_fail(ctx, TF_R004, span);
    if (checked(ctx, span, tf_checked_float_div(left, right, &result)) != TF_OK)
        return TF_FAULT;
    return tf_float(ctx, span, result, out);
}

static tf_status power(tf_context *ctx, tf_span_id span, tf_value a, tf_value b,
                       tf_value *out) {
    if (b.tag == TF_VALUE_INT) {
        if (a.tag == TF_VALUE_INT) {
            int64_t result;
            if (checked(ctx, span, tf_checked_int_pow(a.as.integer, b.as.integer, &result)) != TF_OK)
                return TF_FAULT;
            *out = tf_int(result);
            return TF_OK;
        }
        if (a.tag == TF_VALUE_FLOAT) {
            uint64_t result;
            if (checked(ctx, span, tf_checked_float_pow(a.as.float_bits, b.as.integer, &result)) != TF_OK)
                return TF_FAULT;
            return tf_float(ctx, span, result, out);
        }
    }
    return tf_context_fail(ctx, TF_R004, span);
}

/* With finite bits and canonical zero, nonnegative encodings increase with
 * value and negative encodings decrease. Map them into unsigned order without
 * host FP, conversion, or a potentially overflowing subtraction. */
static uint64_t float_order(uint64_t bits) {
    const uint64_t sign = UINT64_C(0x8000000000000000);
    return (bits & sign) ? ~bits : bits ^ sign;
}

static tf_status compare(tf_context *ctx, tf_span_id span, tf_numeric_op op,
                         tf_value a, tf_value b, tf_value *out) {
    bool less, equal, result;
    if (a.tag == TF_VALUE_INT && b.tag == TF_VALUE_INT) {
        less = a.as.integer < b.as.integer;
        equal = a.as.integer == b.as.integer;
    } else if (a.tag == TF_VALUE_FLOAT && b.tag == TF_VALUE_FLOAT) {
        less = float_order(a.as.float_bits) < float_order(b.as.float_bits);
        equal = a.as.float_bits == b.as.float_bits;
    } else {
        return tf_context_fail(ctx, TF_R004, span);
    }
    switch (op) {
        case TF_RT_NUM_LESS: result = less; break;
        case TF_RT_NUM_LESS_EQUAL: result = less || equal; break;
        case TF_RT_NUM_GREATER: result = !less && !equal; break;
        case TF_RT_NUM_GREATER_EQUAL: result = !less; break;
        case TF_RT_NUM_EQUAL: result = equal; break;
        case TF_RT_NUM_NOT_EQUAL: result = !equal; break;
        default: return tf_context_fail(ctx, TF_R004, span);
    }
    *out = tf_bool(result);
    return TF_OK;
}

tf_status tf_numeric_binary(tf_context *ctx, tf_span_id span, tf_numeric_op op,
                            tf_value a, tf_value b, tf_value *out) {
    if (!start(ctx, out)) return TF_FAULT;
    switch (op) {
        case TF_RT_NUM_ADD:
            return same_type(ctx, span, a, b, tf_checked_int_add, tf_checked_float_add, out);
        case TF_RT_NUM_SUBTRACT:
            return same_type(ctx, span, a, b, tf_checked_int_sub, tf_checked_float_sub, out);
        case TF_RT_NUM_MULTIPLY:
            return same_type(ctx, span, a, b, tf_checked_int_mul, tf_checked_float_mul, out);
        case TF_RT_NUM_REAL_DIVIDE: return real_divide(ctx, span, a, b, out);
        case TF_RT_NUM_FLOOR_DIVIDE:
            return same_type(ctx, span, a, b, tf_checked_int_div, NULL, out);
        case TF_RT_NUM_REMAINDER:
            return same_type(ctx, span, a, b, tf_checked_int_rem, NULL, out);
        case TF_RT_NUM_POWER: return power(ctx, span, a, b, out);
        case TF_RT_NUM_LESS: case TF_RT_NUM_LESS_EQUAL: case TF_RT_NUM_GREATER:
        case TF_RT_NUM_GREATER_EQUAL: case TF_RT_NUM_EQUAL: case TF_RT_NUM_NOT_EQUAL:
            return compare(ctx, span, op, a, b, out);
    }
    return tf_context_fail(ctx, TF_R004, span);
}

tf_status tf_numeric_negate(tf_context *ctx, tf_span_id span, tf_value value,
                            tf_value *out) {
    if (!start(ctx, out)) return TF_FAULT;
    if (value.tag == TF_VALUE_INT) {
        int64_t result;
        if (checked(ctx, span, tf_checked_int_neg(value.as.integer, &result)) != TF_OK)
            return TF_FAULT;
        *out = tf_int(result);
        return TF_OK;
    }
    if (value.tag == TF_VALUE_FLOAT) {
        uint64_t result;
        if (checked(ctx, span, tf_checked_float_neg(value.as.float_bits, &result)) != TF_OK)
            return TF_FAULT;
        return tf_float(ctx, span, result, out);
    }
    return tf_context_fail(ctx, TF_R004, span);
}

tf_status tf_numeric_not(tf_context *ctx, tf_span_id span, tf_value value, tf_value *out) {
    if (!start(ctx, out)) return TF_FAULT;
    if (value.tag != TF_VALUE_BOOL || value.as.boolean > 1)
        return tf_context_fail(ctx, TF_R004, span);
    *out = tf_bool(value.as.boolean == 0);
    return TF_OK;
}
