#ifndef TERNFOLD_RT_BUILTINS_H
#define TERNFOLD_RT_BUILTINS_H

#include "ternfold_rt.h"

/* Deliberately independent of frontend/Core enum ordinals. Map/fold belong
 * to the explicit-frame dispatcher, not this non-callback interface. */
typedef enum {
    TF_BUILTIN_LEN = 1, TF_BUILTIN_AT, TF_BUILTIN_APPEND, TF_BUILTIN_RANGE,
    TF_BUILTIN_INT_TEXT, TF_BUILTIN_INT_FLOAT, TF_BUILTIN_FLOAT_INT,
    TF_BUILTIN_FLOAT_TEXT, TF_BUILTIN_BOOL_TEXT, TF_BUILTIN_PARSE_INT,
    TF_BUILTIN_TEXT_LEN, TF_BUILTIN_TEXT_AT, TF_BUILTIN_TEXT_SLICE
} tf_builtin;

/* Borrow the SAME immutable program/layout storage supplied to context_init.
 * Option layouts are Some(payload) at tag 0, None at tag 1.
 * Scratch is temporary storage for append/range only. Both allocator callbacks
 * NULL select malloc/free; otherwise both are required, with malloc alignment
 * and no runtime reentry, as for tf_rt_allocator. It may use the same
 * injection allocator as the context. scratch_byte_limit is an additional cap
 * (zero permits empty buffers only). Scratch is charged to ctx->live_bytes,
 * sharing the context's total byte budget with all live boxes. Its charge is
 * removed before return, including on faults; scratch never escapes into values.
 * This runtime module reads config and updates live_bytes for that purpose. */
typedef struct {
    const tf_rt_program *program;
    tf_rt_allocator scratch_allocator;
    size_t scratch_byte_limit;
} tf_builtin_env;

/* Borrow env, args and every argument value. out must own no heap reference
 * and be disjoint from args. Success produces one owned value of result_type;
 * failure leaves Unit. Existing context faults take precedence. Bad identity,
 * arity, types or layouts report R004; allocation/retain/length limits R003.
 * Link with src/execution/scalars.c and src/checked_numeric.c; the latter's
 * header is a scalar-only dependency. Generated callers need this header only.
 * No callbacks, I/O, emissions, or publication occur in this API. */
tf_status tf_builtin_call(tf_context *ctx, const tf_builtin_env *env,
                          tf_span_id span, tf_builtin builtin,
                          tf_type_id result_type, const tf_value *args,
                          size_t argc, tf_value *out);

#endif
