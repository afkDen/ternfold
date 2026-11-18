#include "ternfold/trace.h"

#include <string.h>

static bool valid_recorder(const TfTraceRecorder *recorder) {
    return recorder && recorder->capacity <= SIZE_MAX / sizeof(TfTraceRecord) &&
           (recorder->capacity == 0 || recorder->records) &&
           recorder->count <= recorder->capacity;
}

static void add_loss(TfTraceRecorder *recorder, uint64_t *counter, size_t amount) {
    if (amount > UINT64_MAX - *counter) {
        *counter = UINT64_MAX;
        recorder->counters_saturated = true;
    } else {
        *counter += (uint64_t)amount;
    }
}

bool tf_trace_recorder_init(TfTraceRecorder *recorder, TfTraceRecord *records,
                            size_t capacity) {
    if (!recorder || capacity > SIZE_MAX / sizeof(TfTraceRecord) ||
        (capacity != 0 && !records)) return false;
    *recorder = (TfTraceRecorder){.records = records, .capacity = capacity};
    return true;
}

bool tf_trace_recorder_reset(TfTraceRecorder *recorder) {
    if (!valid_recorder(recorder)) return false;
    return tf_trace_recorder_init(recorder, recorder->records, recorder->capacity);
}

bool tf_trace_next_invocation(TfTraceRecorder *recorder, uint64_t *id) {
    if (!valid_recorder(recorder) || !id) return false;
    if (recorder->sequence_exhausted || recorder->invocation_exhausted) return false;
    if (recorder->last_invocation_id == UINT64_MAX) {
        recorder->invocation_exhausted = true;
        recorder->truncated = true;
        return false;
    }
    ++recorder->last_invocation_id;
    *id = recorder->last_invocation_id;
    return true;
}

TfTraceCaptureStatus tf_trace_capture(TfTraceRecorder *recorder,
                                    const TfTraceEvent *event,
                                    const unsigned char *preview,
                                    size_t preview_length,
                                    bool preview_incomplete) {
    TfTraceRecord record = {0};
    if (!valid_recorder(recorder) || !event ||
        event->kind < TF_TRACE_STEP_BEGIN || event->kind > TF_TRACE_FAULT ||
        event->invocation_id == 0 ||
        event->invocation_id > recorder->last_invocation_id ||
        event->parent_invocation_id >= event->invocation_id ||
        (preview_length != 0 && !preview)) return TF_TRACE_INVALID;

    if (recorder->last_sequence_id == UINT64_MAX) recorder->sequence_exhausted = true;
    if (recorder->sequence_exhausted || recorder->invocation_exhausted) {
        recorder->truncated = true;
        add_loss(recorder, &recorder->omitted_events, 1);
        return TF_TRACE_OMITTED;
    }
    ++recorder->last_sequence_id;
    if (recorder->count == recorder->capacity) {
        recorder->truncated = true;
        add_loss(recorder, &recorder->omitted_events, 1);
        return TF_TRACE_OMITTED;
    }

    /* Stage first: input metadata/bytes may alias the destination record. */
    record.event = *event;
    record.sequence_id = recorder->last_sequence_id;
    record.preview_length = preview_length < TF_TRACE_PREVIEW_CAPACITY ?
                            preview_length : TF_TRACE_PREVIEW_CAPACITY;
    record.preview_truncated = preview_incomplete || preview_length > record.preview_length;
    if (record.preview_length != 0) memcpy(record.preview, preview, record.preview_length);
    if (record.preview_truncated) {
        recorder->truncated = true;
        add_loss(recorder, &recorder->clipped_previews, 1);
        add_loss(recorder, &recorder->omitted_preview_bytes, preview_length - record.preview_length);
    }
    recorder->records[recorder->count] = record;
    ++recorder->count;
    return TF_TRACE_RECORDED;
}
