#ifndef TERNFOLD_RT_VALUE_OPS_H
#define TERNFOLD_RT_VALUE_OPS_H

#include "ternfold_rt.h"

/* Borrow inputs; output is disjoint and initially owns no heap reference.
 * Success returns an owned value (Bool for equality); failure returns Unit.
 * First context fault wins. Type/representation misuse, including equality
 * on any function-containing type, is R004. No numeric widening occurs.
 * Scratch uses the context allocator and live-byte budget, including peak
 * reallocation storage. Each operation also permits at most 10 million
 * traversal/hash work units; resource exhaustion is R003. No recursive C
 * traversal, destruction, or ambient output is used. */
#define TF_VALUE_OPS_WORK_LIMIT 10000000u

tf_status tf_value_equal(tf_context *ctx, tf_span_id span,
                          tf_value left, tf_value right, tf_value *out);
tf_status tf_text_concat(tf_context *ctx, tf_span_id span,
                          tf_value left, tf_value right, tf_value *out);
tf_status tf_list_concat(tf_context *ctx, tf_span_id span,
                          tf_value left, tf_value right, tf_value *out);

#endif
