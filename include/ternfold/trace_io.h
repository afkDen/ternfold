#ifndef TERNFOLD_TRACE_IO_H
#define TERNFOLD_TRACE_IO_H

#include "ternfold/trace.h"
#include "ternfold/allocator.h"
#include <stdio.h>

/* Post-execution host APIs. No capture callbacks or language execution. */
enum { TF_TRACE_IO_MAX_RECORDS = 4096, TF_TRACE_IO_MAX_BYTES = 4 * 1024 * 1024 };
typedef struct { size_t max_bytes, max_records; } TfTraceIoLimits;
typedef enum {
    TF_TRACE_IO_OK = 0, TF_TRACE_IO_INVALID, TF_TRACE_IO_LIMIT,
    TF_TRACE_IO_ALLOCATION, TF_TRACE_IO_SYNTAX, TF_TRACE_IO_ORDER,
    TF_TRACE_IO_STREAM
} TfTraceIoStatus;

typedef struct {
    char *bytes; /* Owned ASCII, length-counted; also terminated for convenience. */
    size_t length;
    TfAllocator allocator;
} TfTraceSerialized;

typedef enum {
    TF_TRACE_OUTCOME_UNKNOWN = 0, TF_TRACE_OUTCOME_ACCEPT,
    TF_TRACE_OUTCOME_REJECT, TF_TRACE_OUTCOME_FAULT
} TfTraceOutcome;

typedef struct {
    uint64_t id, parent_id;
    size_t function_id, first_record, last_record;
    size_t writers, guards, emissions;
    TfTraceEventKind observed_terminal; /* Zero if missing. Emissions are private. */
    TfTraceOutcome outcome;
} TfTraceInvocation;

typedef struct {
    TfTraceRecorder recorder; /* Owned records; capacity == declared_count. */
    size_t declared_count;
    TfTraceInvocation *invocations; /* Owned, in increasing invocation-ID order. */
    size_t invocation_count;
    bool complete_file; /* All declared records, END and final LF received. */
    bool unknown; /* Incomplete file, recorder truncation or missing terminals. */
    TfAllocator allocator;
} TfTraceReplay;

/* NULL limits/allocator select hard maxima/malloc+free. Custom limits must be
 * positive and <= hard maxima. Allocator/context must outlive owned outputs.
 * Inputs/FILEs are borrowed for the call, never retained or closed. Outputs
 * must be fresh, nonoverlapping objects; every failure leaves them zeroed and
 * destroyable. C pointers must designate their stated live array lengths.
 * No mutable globals. See docs/trace-format.md for validation and EOF rules. */
TfTraceIoStatus tf_trace_serialize(TfTraceSerialized *out,
    const TfTraceRecorder *recorder, const TfTraceIoLimits *limits,
    const TfAllocator *allocator);
void tf_trace_serialized_destroy(TfTraceSerialized *serialized);

/* Stage/validate before writing any bytes. Flushes to detect buffered errors;
 * a STREAM error can leave host file bytes partially written (no rollback).
 * Open persisted streams in binary mode. Never pass program stdout as sink. */
TfTraceIoStatus tf_trace_write(FILE *stream, const TfTraceRecorder *recorder,
    const TfTraceIoLimits *limits, const TfAllocator *allocator);

TfTraceIoStatus tf_trace_replay_bytes(TfTraceReplay *out, const void *bytes,
    size_t length, const TfTraceIoLimits *limits, const TfAllocator *allocator);
TfTraceIoStatus tf_trace_replay_read(TfTraceReplay *out, FILE *stream,
    const TfTraceIoLimits *limits, const TfAllocator *allocator);
void tf_trace_replay_destroy(TfTraceReplay *replay);

#endif
