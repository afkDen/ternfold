#ifndef TERNFOLD_RT_CALLS_H
#define TERNFOLD_RT_CALLS_H

#include "ternfold_rt.h"

typedef tf_status (*tf_rt_code)(tf_context *, const tf_value *, size_t,
                                const tf_value *, size_t, tf_value *);
typedef enum { TF_RT_ORDINARY, TF_RT_ENTRY_ONLY, TF_RT_PRELUDE_WRAPPER } tf_rt_call_kind;
typedef struct {
    tf_rt_code code;
    tf_rt_call_kind kind;
    tf_rt_shape parameters;
    tf_type_id result;
} tf_rt_body;
typedef struct { const tf_rt_body *bodies; size_t count; tf_function_id entry; } tf_rt_dispatch;

/* Partial ABI for leaf/smoke bodies. Recursive reentry uses the host C stack
 * and is not an accepted language recursion boundary. The explicit-frame
 * dispatcher under M10 replaces recursive native use before M11 acceptance. */

/* Generated immutable tables must cover the declared extents and match the
 * context's function IDs. Inputs/captures are borrowed and success outputs
 * are owned. This boundary sets a call frame and always unwinds it; generated
 * bodies must not separately enter/leave that same frame. */
tf_status tf_call_static(tf_context *ctx, const tf_rt_dispatch *table, tf_span_id span,
                        tf_function_id target, const tf_value *args, size_t count, tf_value *out);
tf_status tf_call_closure(tf_context *ctx, const tf_rt_dispatch *table, tf_span_id span,
                         tf_value closure, const tf_value *args, size_t count, tf_value *out);
tf_status tf_call_entry(tf_context *ctx, const tf_rt_dispatch *table,
                       const tf_value *args, size_t count, tf_value *out);

/* Effective origin for a synthetic prelude wrapper. Callback dispatch in a
 * builtin passes the builtin's effective span as its invocation span. */
tf_span_id tf_invocation_span(const tf_context *ctx);

#endif
