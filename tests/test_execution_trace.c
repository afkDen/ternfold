#include "ternfold/trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        (void)fprintf(stderr, "execution trace: line %d: %s\n", __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static TfTraceEvent event_for(uint64_t invocation, uint64_t parent) {
    return (TfTraceEvent){.kind = TF_TRACE_STEP_BEGIN,
        .invocation_id = invocation, .parent_invocation_id = parent,
        .function_id = 11, .block_id = 22, .let_id = 33, .span_id = 44, .ordinal = 1};
}

static void same_event(const TfTraceEvent *a, const TfTraceEvent *b) {
    CHECK(a->kind == b->kind);
    CHECK(a->invocation_id == b->invocation_id);
    CHECK(a->parent_invocation_id == b->parent_invocation_id);
    CHECK(a->function_id == b->function_id);
    CHECK(a->block_id == b->block_id);
    CHECK(a->let_id == b->let_id);
    CHECK(a->span_id == b->span_id);
    CHECK(a->ordinal == b->ordinal);
}

static void same_state(const TfTraceRecorder *a, const TfTraceRecorder *b) {
    CHECK(a->records == b->records);
    CHECK(a->capacity == b->capacity);
    CHECK(a->count == b->count);
    CHECK(a->last_sequence_id == b->last_sequence_id);
    CHECK(a->last_invocation_id == b->last_invocation_id);
    CHECK(a->omitted_events == b->omitted_events);
    CHECK(a->clipped_previews == b->clipped_previews);
    CHECK(a->omitted_preview_bytes == b->omitted_preview_bytes);
    CHECK(a->truncated == b->truncated);
    CHECK(a->counters_saturated == b->counters_saturated);
    CHECK(a->sequence_exhausted == b->sequence_exhausted);
    CHECK(a->invocation_exhausted == b->invocation_exhausted);
}

static void empty_state(const TfTraceRecorder *recorder) {
    CHECK(recorder->count == 0);
    CHECK(recorder->last_sequence_id == 0);
    CHECK(recorder->last_invocation_id == 0);
    CHECK(recorder->omitted_events == 0);
    CHECK(recorder->clipped_previews == 0);
    CHECK(recorder->omitted_preview_bytes == 0);
    CHECK(!recorder->truncated);
    CHECK(!recorder->counters_saturated);
    CHECK(!recorder->sequence_exhausted);
    CHECK(!recorder->invocation_exhausted);
}

static void kinds_and_copies(void) {
    static const TfTraceEventKind kinds[] = {TF_TRACE_STEP_BEGIN, TF_TRACE_WRITER,
        TF_TRACE_GUARD, TF_TRACE_EMISSION, TF_TRACE_ACCEPT, TF_TRACE_REJECT, TF_TRACE_FAULT};
    TfTraceRecord records[7];
    TfTraceRecorder recorder;
    uint64_t root = 0, nested = 0;
    unsigned char preview[] = {'a', 0, 0xff, 'z'};
    CHECK(tf_trace_recorder_init(&recorder, records, 7));
    empty_state(&recorder);
    CHECK(tf_trace_next_invocation(&recorder, &root) && root == 1);
    CHECK(tf_trace_next_invocation(&recorder, &nested) && nested == 2);
    TfTraceEvent event = event_for(nested, root);
    /* Numeric metadata is copied without narrowing, including absent IDs. */
    event.function_id = SIZE_MAX;
    event.block_id = SIZE_MAX - 1;
    event.let_id = SIZE_MAX - 2;
    event.span_id = SIZE_MAX - 3;
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; ++i) {
        event.kind = kinds[i];
        event.ordinal = i;
        CHECK(tf_trace_capture(&recorder, &event, preview, sizeof preview, false) == TF_TRACE_RECORDED);
        same_event(&records[i].event, &event);
        CHECK(records[i].sequence_id == (uint64_t)i + 1);
        CHECK(records[i].preview_length == sizeof preview);
        CHECK(!records[i].preview_truncated);
        CHECK(memcmp(records[i].preview, preview, sizeof preview) == 0);
        for (size_t j = sizeof preview; j < TF_TRACE_PREVIEW_CAPACITY; ++j)
            CHECK(records[i].preview[j] == 0);
    }
    memset(preview, 0, sizeof preview);
    event = (TfTraceEvent){0};
    CHECK(records[0].event.kind == TF_TRACE_STEP_BEGIN);
    CHECK(records[0].event.invocation_id == nested);
    CHECK(records[0].event.parent_invocation_id == root);
    CHECK(records[0].preview[0] == 'a' && records[0].preview[1] == 0);
    CHECK(records[0].preview[2] == 0xff && records[0].preview[3] == 'z');
    CHECK(recorder.count == 7 && recorder.last_sequence_id == 7);
    CHECK(!recorder.truncated && !recorder.counters_saturated);
    CHECK(recorder.omitted_events == 0 && recorder.clipped_previews == 0);
    CHECK(recorder.omitted_preview_bytes == 0);
}

static void capacity_and_reset(void) {
    struct {
        unsigned char before[16];
        TfTraceRecord records[2];
        unsigned char after[16];
    } store;
    TfTraceRecorder recorder;
    uint64_t id = 0;
    memset(&store, 0xa5, sizeof store);
    CHECK(tf_trace_recorder_init(&recorder, store.records, 2));
    CHECK(tf_trace_next_invocation(&recorder, &id));
    TfTraceEvent event = event_for(id, 0);
    for (unsigned i = 0; i < 1000; ++i) {
        event.ordinal = i;
        CHECK(tf_trace_capture(&recorder, &event, NULL, 0, false) ==
              (i < 2 ? TF_TRACE_RECORDED : TF_TRACE_OMITTED));
    }
    CHECK(recorder.count == 2 && recorder.capacity == 2);
    CHECK(recorder.last_sequence_id == 1000 && recorder.omitted_events == 998);
    CHECK(recorder.truncated && !recorder.counters_saturated);
    CHECK(store.records[0].event.ordinal == 0 && store.records[1].event.ordinal == 1);
    CHECK(store.records[0].sequence_id == 1 && store.records[1].sequence_id == 2);
    for (size_t i = 0; i < sizeof store.before; ++i) CHECK(store.before[i] == 0xa5);
    for (size_t i = 0; i < sizeof store.after; ++i) CHECK(store.after[i] == 0xa5);
    CHECK(tf_trace_recorder_reset(&recorder));
    CHECK(recorder.records == store.records && recorder.capacity == 2);
    empty_state(&recorder);
    CHECK(store.records[1].sequence_id == 2); /* Reset is O(1), old bytes remain. */
    CHECK(tf_trace_next_invocation(&recorder, &id) && id == 1);
    event = event_for(id, 0);
    CHECK(tf_trace_capture(&recorder, &event, NULL, 0, false) == TF_TRACE_RECORDED);
    CHECK(store.records[0].sequence_id == 1 && recorder.count == 1);

    CHECK(tf_trace_recorder_init(&recorder, NULL, 0));
    CHECK(tf_trace_next_invocation(&recorder, &id));
    for (unsigned i = 0; i < 3; ++i)
        CHECK(tf_trace_capture(&recorder, &event, NULL, 0, false) == TF_TRACE_OMITTED);
    CHECK(recorder.count == 0 && recorder.omitted_events == 3);
    CHECK(recorder.last_sequence_id == 3 && recorder.truncated);
    CHECK(recorder.clipped_previews == 0 && recorder.omitted_preview_bytes == 0);
    CHECK(tf_trace_recorder_reset(&recorder));
    empty_state(&recorder);
    CHECK(recorder.records == NULL && recorder.capacity == 0);
    CHECK(tf_trace_recorder_init(&recorder, store.records, 0));
    CHECK(tf_trace_next_invocation(&recorder, &id));
    CHECK(tf_trace_capture(&recorder, &event, NULL, 0, true) == TF_TRACE_OMITTED);
    CHECK(store.records[0].sequence_id == 1 && store.records[1].sequence_id == 2);
    CHECK(recorder.clipped_previews == 0); /* Dropped previews are not inspected. */
}

static void invalid_arguments(void) {
    TfTraceRecord records[2];
    unsigned char saved[sizeof records];
    TfTraceRecorder recorder;
    uint64_t id = 99;
    memset(records, 0x5a, sizeof records);
    CHECK(tf_trace_recorder_init(&recorder, records, 2));
    CHECK(tf_trace_next_invocation(&recorder, &id));
    const TfTraceRecorder before = recorder;
    const TfTraceEvent valid = event_for(id, 0);
    memcpy(saved, records, sizeof saved);
    CHECK(!tf_trace_recorder_init(NULL, records, 2));
    CHECK(!tf_trace_recorder_init(&recorder, NULL, 1));
    CHECK(!tf_trace_recorder_init(&recorder, records, SIZE_MAX / sizeof *records + 1));
    CHECK(!tf_trace_recorder_reset(NULL));
    CHECK(!tf_trace_next_invocation(NULL, &id) && id == 1);
    CHECK(!tf_trace_next_invocation(&recorder, NULL));
    CHECK(tf_trace_capture(NULL, &valid, NULL, 0, false) == TF_TRACE_INVALID);
    CHECK(tf_trace_capture(&recorder, NULL, NULL, 0, false) == TF_TRACE_INVALID);
    CHECK(tf_trace_capture(&recorder, &valid, NULL, 1, false) == TF_TRACE_INVALID);
    for (unsigned i = 0; i < 7; ++i) {
        TfTraceEvent event = valid;
        switch (i) {
        case 0: event.kind = (TfTraceEventKind)0; break;
        case 1: event.kind = (TfTraceEventKind)(TF_TRACE_FAULT + 1); break;
        case 2: event.kind = (TfTraceEventKind)-1; break;
        case 3: event.invocation_id = 0; break;
        case 4: event.invocation_id = 2; break;
        case 5: event.parent_invocation_id = 1; break;
        default: event.parent_invocation_id = UINT64_MAX; break;
        }
        CHECK(tf_trace_capture(&recorder, &event, NULL, 0, false) == TF_TRACE_INVALID);
        same_state(&recorder, &before);
    }
    same_state(&recorder, &before);
    CHECK(memcmp(records, saved, sizeof saved) == 0);

    /* Structural rejection before indexing a caller array. */
    for (unsigned i = 0; i < 3; ++i) {
        TfTraceRecorder malformed = before;
        if (i == 0) malformed.count = 3;
        else if (i == 1) malformed.records = NULL;
        else malformed.capacity = SIZE_MAX;
        TfTraceRecorder unchanged = malformed;
        CHECK(!tf_trace_recorder_reset(&malformed));
        CHECK(!tf_trace_next_invocation(&malformed, &id) && id == 1);
        CHECK(tf_trace_capture(&malformed, &valid, NULL, 0, false) == TF_TRACE_INVALID);
        same_state(&malformed, &unchanged);
    }
    CHECK(memcmp(records, saved, sizeof saved) == 0);
    /* Zero initialization is a supported zero-capacity state. */
    TfTraceRecorder zero = {0};
    CHECK(tf_trace_next_invocation(&zero, &id) && id == 1);
    CHECK(tf_trace_capture(&zero, &valid, NULL, 0, false) == TF_TRACE_OMITTED);
}

static void previews_and_aliases(void) {
    static const size_t lengths[] = {0, 1, 63, 64, 65, 69};
    unsigned char bytes[69];
    TfTraceRecord records[8];
    TfTraceRecorder recorder;
    uint64_t id = 0;
    for (size_t i = 0; i < sizeof bytes; ++i) bytes[i] = (unsigned char)i;
    CHECK(tf_trace_recorder_init(&recorder, records, 8));
    CHECK(tf_trace_next_invocation(&recorder, &id));
    TfTraceEvent event = event_for(id, 0);
    for (size_t i = 0; i < sizeof lengths / sizeof *lengths; ++i) {
        const size_t kept = lengths[i] < TF_TRACE_PREVIEW_CAPACITY ? lengths[i] : TF_TRACE_PREVIEW_CAPACITY;
        CHECK(tf_trace_capture(&recorder, &event, bytes, lengths[i], false) == TF_TRACE_RECORDED);
        CHECK(records[i].preview_length == kept);
        CHECK(records[i].preview_truncated == (lengths[i] > kept));
        CHECK(memcmp(records[i].preview, bytes, kept) == 0);
    }
    CHECK(recorder.clipped_previews == 2 && recorder.omitted_preview_bytes == 6);
    CHECK(recorder.truncated && !recorder.counters_saturated);
    CHECK(tf_trace_capture(&recorder, &event, NULL, 0, true) == TF_TRACE_RECORDED);
    CHECK(records[6].preview_length == 0 && records[6].preview_truncated);
    CHECK(tf_trace_capture(&recorder, &event, bytes, 3, true) == TF_TRACE_RECORDED);
    CHECK(records[7].preview_length == 3 && records[7].preview_truncated);
    CHECK(recorder.clipped_previews == 4 && recorder.omitted_preview_bytes == 6);
    CHECK(tf_trace_capture(&recorder, &event, bytes, sizeof bytes, true) == TF_TRACE_OMITTED);
    CHECK(recorder.clipped_previews == 4 && recorder.omitted_preview_bytes == 6);

    /* Metadata and bytes may come from the exact destination being replaced. */
    CHECK(tf_trace_recorder_reset(&recorder));
    CHECK(tf_trace_next_invocation(&recorder, &id));
    CHECK(tf_trace_capture(&recorder, &records[7].event, records[7].preview,
                           records[7].preview_length, false) == TF_TRACE_RECORDED);
    CHECK(records[0].preview_length == 3 && memcmp(records[0].preview, bytes, 3) == 0);
    CHECK(tf_trace_recorder_reset(&recorder));
    CHECK(tf_trace_next_invocation(&recorder, &id));
    CHECK(tf_trace_capture(&recorder, &records[0].event, records[0].preview,
                           records[0].preview_length, false) == TF_TRACE_RECORDED);
    CHECK(records[0].preview_length == 3 && memcmp(records[0].preview, bytes, 3) == 0);
    CHECK(records[0].sequence_id == 1 && !records[0].preview_truncated);
    /* Reusing a slot with a short preview clears every unused preview byte. */
    CHECK(tf_trace_recorder_reset(&recorder));
    CHECK(tf_trace_next_invocation(&recorder, &id));
    CHECK(tf_trace_capture(&recorder, &event, bytes, 1, false) == TF_TRACE_RECORDED);
    for (size_t i = 1; i < TF_TRACE_PREVIEW_CAPACITY; ++i) CHECK(records[0].preview[i] == 0);
}

static void loss_saturation(void) {
    TfTraceRecord records[3];
    TfTraceRecorder recorder;
    uint64_t id = 0;
    unsigned char bytes[69] = {0};
    CHECK(tf_trace_recorder_init(&recorder, NULL, 0));
    CHECK(tf_trace_next_invocation(&recorder, &id));
    TfTraceEvent event = event_for(id, 0);
    recorder.omitted_events = UINT64_MAX - 1;
    CHECK(tf_trace_capture(&recorder, &event, NULL, 0, false) == TF_TRACE_OMITTED);
    CHECK(recorder.omitted_events == UINT64_MAX && !recorder.counters_saturated);
    CHECK(tf_trace_capture(&recorder, &event, NULL, 0, false) == TF_TRACE_OMITTED);
    CHECK(recorder.omitted_events == UINT64_MAX && recorder.counters_saturated);
    CHECK(tf_trace_capture(&recorder, &event, NULL, 0, false) == TF_TRACE_OMITTED);
    CHECK(recorder.omitted_events == UINT64_MAX);
    CHECK(tf_trace_recorder_init(&recorder, records, 3));
    CHECK(tf_trace_next_invocation(&recorder, &id));
    recorder.clipped_previews = UINT64_MAX - 1;
    CHECK(tf_trace_capture(&recorder, &event, NULL, 0, true) == TF_TRACE_RECORDED);
    CHECK(recorder.clipped_previews == UINT64_MAX && !recorder.counters_saturated);
    CHECK(tf_trace_capture(&recorder, &event, NULL, 0, true) == TF_TRACE_RECORDED);
    CHECK(recorder.clipped_previews == UINT64_MAX && recorder.counters_saturated);
    CHECK(recorder.omitted_preview_bytes == 0);
    CHECK(tf_trace_recorder_reset(&recorder));
    CHECK(tf_trace_next_invocation(&recorder, &id));
    recorder.omitted_preview_bytes = UINT64_MAX - 5;
    CHECK(tf_trace_capture(&recorder, &event, bytes, 69, false) == TF_TRACE_RECORDED);
    CHECK(recorder.omitted_preview_bytes == UINT64_MAX && !recorder.counters_saturated);
    CHECK(tf_trace_capture(&recorder, &event, bytes, 65, false) == TF_TRACE_RECORDED);
    CHECK(recorder.omitted_preview_bytes == UINT64_MAX && recorder.counters_saturated);
    CHECK(recorder.clipped_previews == 2);
    CHECK(tf_trace_recorder_reset(&recorder));
    empty_state(&recorder);
}

static void id_exhaustion(void) {
    TfTraceRecord records[3];
    TfTraceRecorder recorder;
    uint64_t id = 0;
    CHECK(tf_trace_recorder_init(&recorder, records, 3));
    recorder.last_invocation_id = UINT64_MAX - 1;
    CHECK(tf_trace_next_invocation(&recorder, &id) && id == UINT64_MAX);
    CHECK(!recorder.invocation_exhausted && !recorder.truncated);
    TfTraceEvent event = event_for(id, UINT64_MAX - 1);
    recorder.last_sequence_id = UINT64_MAX - 1;
    CHECK(tf_trace_capture(&recorder, &event, NULL, 0, false) == TF_TRACE_RECORDED);
    CHECK(records[0].sequence_id == UINT64_MAX && records[0].event.invocation_id == UINT64_MAX);
    CHECK(!recorder.sequence_exhausted && !recorder.truncated);
    CHECK(tf_trace_capture(&recorder, &event, NULL, 0, false) == TF_TRACE_OMITTED);
    CHECK(recorder.sequence_exhausted && recorder.truncated && recorder.omitted_events == 1);
    CHECK(recorder.last_sequence_id == UINT64_MAX && recorder.count == 1);
    CHECK(tf_trace_capture(&recorder, &event, NULL, 0, false) == TF_TRACE_OMITTED);
    CHECK(recorder.omitted_events == 2 && records[0].sequence_id == UINT64_MAX);
    id = 37;
    CHECK(!tf_trace_next_invocation(&recorder, &id) && id == 37);
    const TfTraceRecorder before = recorder;
    CHECK(tf_trace_capture(&recorder, &event, NULL, 1, false) == TF_TRACE_INVALID);
    same_state(&recorder, &before);

    CHECK(tf_trace_recorder_reset(&recorder));
    recorder.last_invocation_id = UINT64_MAX - 1;
    CHECK(tf_trace_next_invocation(&recorder, &id) && id == UINT64_MAX);
    CHECK(!tf_trace_next_invocation(&recorder, NULL));
    CHECK(!recorder.invocation_exhausted); /* Invalid arguments have no effects. */
    id = 37;
    CHECK(!tf_trace_next_invocation(&recorder, &id) && id == 37);
    CHECK(recorder.last_invocation_id == UINT64_MAX && recorder.invocation_exhausted);
    CHECK(recorder.truncated && !recorder.counters_saturated);
    CHECK(tf_trace_capture(&recorder, &event, NULL, 0, false) == TF_TRACE_OMITTED);
    CHECK(recorder.count == 0 && recorder.omitted_events == 1 && recorder.last_sequence_id == 0);
    CHECK(tf_trace_recorder_reset(&recorder));
    empty_state(&recorder);
    CHECK(tf_trace_next_invocation(&recorder, &id) && id == 1);
    event = event_for(id, 0);
    CHECK(tf_trace_capture(&recorder, &event, NULL, 0, false) == TF_TRACE_RECORDED);
    CHECK(records[0].sequence_id == 1);
}

static void independent_recorders(void) {
    TfTraceRecord a[3], b[3];
    TfTraceRecorder first, second;
    uint64_t id = 0;
    CHECK(tf_trace_recorder_init(&first, a, 3));
    CHECK(tf_trace_recorder_init(&second, b, 3));
    CHECK(tf_trace_next_invocation(&first, &id) && id == 1);
    CHECK(tf_trace_next_invocation(&second, &id) && id == 1);
    TfTraceEvent event = event_for(id, 0);
    for (size_t i = 0; i < 3; ++i) {
        event.ordinal = i + 1;
        const unsigned char byte = (unsigned char)i;
        CHECK(tf_trace_capture(&first, &event, &byte, 1, false) == TF_TRACE_RECORDED);
        CHECK(tf_trace_capture(&second, &event, &byte, 1, false) == TF_TRACE_RECORDED);
        same_event(&a[i].event, &b[i].event);
        CHECK(a[i].sequence_id == b[i].sequence_id);
        CHECK(a[i].preview_length == b[i].preview_length);
        CHECK(a[i].preview_truncated == b[i].preview_truncated);
        CHECK(memcmp(a[i].preview, b[i].preview, sizeof a[i].preview) == 0);
    }
    CHECK(tf_trace_recorder_reset(&first));
    CHECK(second.count == 3 && second.last_sequence_id == 3 && second.last_invocation_id == 1);
}

int main(void) {
    kinds_and_copies();
    capacity_and_reset();
    invalid_arguments();
    previews_and_aliases();
    loss_saturation();
    id_exhaustion();
    independent_recorders();
    (void)printf("execution trace recorder: %u checks passed (partial M15; no executor hooks)\n", checks);
    return EXIT_SUCCESS;
}
