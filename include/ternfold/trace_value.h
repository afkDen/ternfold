#ifndef TERNFOLD_TRACE_VALUE_H
#define TERNFOLD_TRACE_VALUE_H
#include "ternfold/trace.h"

/* Internal executor adapters expose borrowed, validated values to a bounded
 * nonallocating renderer. Adapters never invoke source callbacks or retain
 * values. A child's pointer is used synchronously before its owner can die. */
typedef enum {
    TF_TRACE_VALUE_UNIT, TF_TRACE_VALUE_INT, TF_TRACE_VALUE_FLOAT,
    TF_TRACE_VALUE_BOOL, TF_TRACE_VALUE_CHAR, TF_TRACE_VALUE_TEXT,
    TF_TRACE_VALUE_LIST, TF_TRACE_VALUE_RECORD, TF_TRACE_VALUE_MACHINE,
    TF_TRACE_VALUE_VARIANT, TF_TRACE_VALUE_FUNCTION, TF_TRACE_VALUE_TAG
} TfTraceValueKind;
typedef struct {
    TfTraceValueKind kind;
    uint64_t bits;
    const unsigned char *bytes;
    size_t count, tag;
} TfTraceValueView;
typedef struct {
    void *context;
    TfTraceValueView (*view)(void *, const void *);
    const void *(*child)(void *, const void *, size_t);
} TfTraceValueAdapter;
typedef struct {
    unsigned char bytes[TF_TRACE_PREVIEW_CAPACITY];
    size_t length;
    bool incomplete;
} TfTracePreview;
/* Byte/Unicode-neutral canonical previews. Float uses exact bits, Text hex.
 * Depth <= 4 and at most 32 values; clipping is always explicit. */
void tf_trace_preview(TfTracePreview *out, TfTraceValueAdapter adapter, const void *value);
void tf_trace_preview_pair(TfTracePreview *out, TfTraceValueAdapter adapter,
                           const void *old, const void *input);
#endif
