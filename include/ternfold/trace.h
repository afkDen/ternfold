#ifndef TERNFOLD_TRACE_H
#define TERNFOLD_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Partial M15: local recorder only. No executable hooks, value formatting,
 * serialization or replay. IDs are meaningful with the exact verified Core
 * metadata retained separately by the host. This is not an execution API. */
enum { TF_TRACE_PREVIEW_CAPACITY = 64 };

typedef enum {
    TF_TRACE_STEP_BEGIN = 1,
    TF_TRACE_WRITER,
    TF_TRACE_GUARD,
    TF_TRACE_EMISSION, /* A computed PRIVATE value, never publication. */
    TF_TRACE_ACCEPT,
    TF_TRACE_REJECT,
    TF_TRACE_FAULT
} TfTraceEventKind;

typedef struct {
    TfTraceEventKind kind;
    uint64_t invocation_id, parent_invocation_id;
    /* Numeric Core IDs, zero absent. Block/let IDs are function-local;
     * let_id identifies the Core result slot. No names or values borrowed. */
    size_t function_id, block_id, let_id, span_id;
    size_t ordinal; /* One-based cell/guard/emission ordinal, zero absent. */
} TfTraceEvent;

typedef struct {
    TfTraceEvent event;
    uint64_t sequence_id;
    size_t preview_length; /* Counted bytes, possibly NUL, never a C string. */
    bool preview_truncated;
    unsigned char preview[TF_TRACE_PREVIEW_CAPACITY];
} TfTraceRecord;

/* Caller owns both control state and fixed record array for their full use.
 * Single-threaded; use separate recorders for concurrent executions. Fields
 * are inspectable but must not be mutated after init (except boundary-test
 * seeding). Read only records[0..count). Reset leaves old storage bytes alone.
 * No function allocates, calls user code, performs I/O, or retains a value.
 * The control object must not overlap the array or any argument/output. */
typedef struct {
    TfTraceRecord *records;
    size_t capacity, count;
    uint64_t last_sequence_id, last_invocation_id;
    uint64_t omitted_events;
    uint64_t clipped_previews; /* Retained records with incomplete previews. */
    uint64_t omitted_preview_bytes; /* Known bytes clipped from retained records. */
    bool truncated; /* Any omission, incomplete preview, or exhausted ID. */
    bool counters_saturated; /* A loss counter exceeded UINT64_MAX. */
    bool sequence_exhausted, invocation_exhausted; /* Capture disabled. */
} TfTraceRecorder;

typedef enum {
    TF_TRACE_INVALID = 0,
    TF_TRACE_RECORDED,
    TF_TRACE_OMITTED
} TfTraceCaptureStatus;

/* NULL storage is valid only at capacity zero. Reject impossible array sizes
 * before arithmetic. Invalid arguments leave existing control/storage alone.
 * A zero-initialized recorder is also a valid zero-capacity recorder. */
bool tf_trace_recorder_init(TfTraceRecorder *recorder, TfTraceRecord *records,
                            size_t capacity);
/* Keep storage/capacity, clear counters/flags and restart both ID domains at 1. */
bool tf_trace_recorder_reset(TfTraceRecorder *recorder);

/* Mint a fresh dynamic step ID (ordinary function calls do not need one).
 * Returns false, leaving *id alone, for invalid arguments or disabled capture.
 * UINT64_MAX is issued once; a later request marks invocation exhaustion and
 * disables capture. Exhaustion never wraps or raises a language fault. */
bool tf_trace_next_invocation(TfTraceRecorder *recorder, uint64_t *id);

/* Copy metadata and min(preview_length,64) bytes before returning. The preview
 * is borrowed only for this call; NULL is valid only for zero length. Inputs
 * may refer to existing records, including the next destination record.
 * preview_incomplete also flags caller depth/work clipping with unknown byte
 * loss; omitted_preview_bytes counts only known byte clipping here.
 *
 * Invocation must have been minted by this recorder since reset, and parent
 * must be zero or a smaller minted ID. This local API does not validate active
 * parents, event ordering, Core metadata or transition outcomes; hooks must.
 * Every valid event attempt consumes a sequence ID, including capacity drops.
 * UINT64_MAX is issued once, then capture is disabled and losses are counted.
 * Full storage preserves its earliest records. Loss counters saturate, never
 * wrap; counters_saturated distinguishes exact UINT64_MAX from overflow.
 * Invalid calls do not consume IDs or change counters/records. Capture status
 * is host observation only and must not replace the language result. */
TfTraceCaptureStatus tf_trace_capture(TfTraceRecorder *recorder,
                                    const TfTraceEvent *event,
                                    const unsigned char *preview,
                                    size_t preview_length,
                                    bool preview_incomplete);

#endif
