#ifndef TERNFOLD_RT_CONSTANTS_H
#define TERNFOLD_RT_CONSTANTS_H

#include "ternfold_rt.h"

typedef enum {
    TF_RT_CONST_UNIT, TF_RT_CONST_INT, TF_RT_CONST_FLOAT, TF_RT_CONST_BOOL, TF_RT_CONST_CHAR,
    TF_RT_CONST_TEXT, TF_RT_CONST_LIST, TF_RT_CONST_RECORD, TF_RT_CONST_VARIANT
} tf_rt_constant_kind;
typedef struct {
    tf_rt_constant_kind kind;
    tf_type_id type;
    tf_span_id span;
    union {
        int64_t integer;
        uint64_t bits;
        struct { const unsigned char *bytes; size_t length; } text;
        struct { const size_t *ids; size_t count; size_t tag; } children;
    } as;
} tf_rt_constant;

typedef struct tf_constant_pool tf_constant_pool;

/* Child IDs start at one and must refer to earlier descriptors. Creation
 * copies all reachable literal data into context-owned runtime values; source
 * descriptor storage can then expire. On failure *out is NULL and all prefix
 * values are released. The context must outlive the pool. */
tf_status tf_constants_create(tf_context *ctx, const tf_rt_constant *descriptors,
                              size_t count, tf_constant_pool **out);
/* Returns a new owner. Errors and invalid IDs write Unit. */
tf_status tf_constant_get(tf_constant_pool *pool, tf_span_id span, size_t id, tf_value *out);
/* Iterative cleanup is valid after a context fault; NULL allowed. */
void tf_constants_destroy(tf_constant_pool *pool);

#endif
