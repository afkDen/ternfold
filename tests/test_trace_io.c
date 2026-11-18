#include "ternfold/trace_io.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    (void)fprintf(stderr, "trace_io:%d: %s\n", __LINE__, #x); exit(EXIT_FAILURE); \
} } while (0)

#define M2 "TFTRACE 1\nM 2 2 1 0 0 0 0 0 0 0\n"
#define BEGIN "R 1 1 0 1 7 0 0 0 0 0 0 \"\"\n"
#define ACCEPT "R 2 1 0 5 7 0 0 0 0 0 0 \"\"\n"
#define VALID M2 BEGIN ACCEPT "END\n"
#define FILE_PATH "build/test-trace-io.bin"

typedef struct { size_t calls, fail_at, live; } Allocations;
static void *allocate(void *context, size_t size) {
    Allocations *a = context;
    void *pointer;
    CHECK(size > 0 && size <= TF_TRACE_IO_MAX_BYTES + 1u);
    if (a->calls++ == a->fail_at) return NULL;
    pointer = malloc(size);
    if (pointer) ++a->live;
    return pointer;
}
static void deallocate(void *context, void *pointer) {
    Allocations *a = context;
    CHECK(pointer && a->live > 0);
    --a->live;
    free(pointer);
}
static void emit(TfTraceRecorder *r, uint64_t id, uint64_t parent,
                 TfTraceEventKind kind, size_t ordinal,
                 const unsigned char *bytes, size_t length, bool clipped) {
    TfTraceEvent event = {.kind = kind, .invocation_id = id,
        .parent_invocation_id = parent, .function_id = 7,
        .block_id = 22, .let_id = 33, .span_id = 44, .ordinal = ordinal};
    CHECK(tf_trace_capture(r, &event, bytes, length, clipped) == TF_TRACE_RECORDED);
}
static void fixture(TfTraceRecorder *r, TfTraceRecord *records, size_t capacity) {
    static const unsigned char bytes[] = {0, '"', '\\', '\n', '\r', '\t', 0xC3, 0xA9, '%'};
    uint64_t parent, child, rejected, faulted;
    CHECK(tf_trace_recorder_init(r, records, capacity));
    CHECK(tf_trace_next_invocation(r, &parent));
    emit(r, parent, 0, TF_TRACE_STEP_BEGIN, 0, bytes, sizeof bytes, false);
    emit(r, parent, 0, TF_TRACE_WRITER, 3, NULL, 0, false);
    CHECK(tf_trace_next_invocation(r, &child));
    emit(r, child, parent, TF_TRACE_STEP_BEGIN, 0, NULL, 0, false);
    emit(r, child, parent, TF_TRACE_WRITER, 2, NULL, 0, false);
    emit(r, child, parent, TF_TRACE_GUARD, 1, (const unsigned char *)"true", 4, false);
    emit(r, child, parent, TF_TRACE_EMISSION, 1, (const unsigned char *)"private", 7, false);
    emit(r, child, parent, TF_TRACE_ACCEPT, 0, NULL, 0, false);
    emit(r, parent, 0, TF_TRACE_WRITER, 1, NULL, 0, false);
    emit(r, parent, 0, TF_TRACE_GUARD, 1, NULL, 0, false);
    emit(r, parent, 0, TF_TRACE_GUARD, 2, NULL, 0, false);
    emit(r, parent, 0, TF_TRACE_EMISSION, 1, NULL, 0, false);
    emit(r, parent, 0, TF_TRACE_ACCEPT, 0, NULL, 0, false);
    CHECK(tf_trace_next_invocation(r, &rejected));
    emit(r, rejected, 0, TF_TRACE_STEP_BEGIN, 0, NULL, 0, false);
    emit(r, rejected, 0, TF_TRACE_GUARD, 1, (const unsigned char *)"false", 5, false);
    emit(r, rejected, 0, TF_TRACE_REJECT, 0, NULL, 0, false);
    CHECK(tf_trace_next_invocation(r, &faulted));
    emit(r, faulted, 0, TF_TRACE_STEP_BEGIN, 0, NULL, 0, false);
    emit(r, faulted, 0, TF_TRACE_EMISSION, 1, (const unsigned char *)"private", 7, false);
    emit(r, faulted, 0, TF_TRACE_FAULT, 0, NULL, 0, false);
}
static void check_empty_replay(const TfTraceReplay *r) {
    CHECK(!r->recorder.records && !r->invocations && !r->invocation_count &&
          !r->recorder.count && !r->declared_count && !r->complete_file && !r->unknown);
}
static void inspect_text(const char *text, TfTraceIoStatus expected) {
    TfTraceReplay r = {0};
    TfTraceIoStatus status = tf_trace_replay_bytes(&r, text, strlen(text), NULL, NULL);
    CHECK(status == expected);
    if (status != TF_TRACE_IO_OK) check_empty_replay(&r);
    tf_trace_replay_destroy(&r);
}
static void empty_and_golden(void) {
    static const char empty[] = "TFTRACE 1\nM 0 0 0 0 0 0 0 0 0 0\nEND\n";
    TfTraceRecorder recorder = {0};
    TfTraceSerialized serialized = {0};
    TfTraceReplay replay = {0};
    CHECK(tf_trace_serialize(&serialized, &recorder, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(serialized.length == sizeof empty - 1 && !memcmp(serialized.bytes, empty, sizeof empty));
    CHECK(tf_trace_replay_bytes(&replay, serialized.bytes, serialized.length, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(replay.complete_file && !replay.unknown && !replay.invocation_count);
    tf_trace_serialized_destroy(&serialized);
    tf_trace_serialized_destroy(&serialized);
    tf_trace_replay_destroy(&replay);
    tf_trace_replay_destroy(&replay);
    CHECK(tf_trace_replay_bytes(&replay, VALID, sizeof VALID - 1, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(replay.complete_file && !replay.unknown && replay.invocation_count == 1);
    CHECK(replay.invocations[0].outcome == TF_TRACE_OUTCOME_ACCEPT);
    CHECK(tf_trace_serialize(&serialized, &replay.recorder, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(serialized.length == sizeof VALID - 1 && !memcmp(serialized.bytes, VALID, sizeof VALID));
    tf_trace_serialized_destroy(&serialized);
    tf_trace_replay_destroy(&replay);
}
static void roundtrip(void) {
    TfTraceRecord records[32];
    TfTraceRecorder recorder;
    TfTraceSerialized a = {0}, b = {0};
    TfTraceReplay replay = {0}, again = {0};
    fixture(&recorder, records, 32);
    CHECK(tf_trace_serialize(&a, &recorder, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(strstr(a.bytes, "\\x00\\x22\\x5C\\x0A\\x0D\\x09\\xC3\\xA9%"));
    for (size_t i = 0; i < a.length; ++i) CHECK((unsigned char)a.bytes[i] > 0 && (unsigned char)a.bytes[i] < 128);
    CHECK(tf_trace_replay_bytes(&replay, a.bytes, a.length, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(replay.complete_file && !replay.unknown && replay.invocation_count == 4);
    CHECK(replay.recorder.count == recorder.count);
    CHECK(replay.invocations[0].writers == 2 && replay.invocations[0].guards == 2 && replay.invocations[0].emissions == 1);
    CHECK(replay.invocations[1].parent_id == 1 && replay.invocations[1].outcome == TF_TRACE_OUTCOME_ACCEPT);
    CHECK(replay.invocations[2].outcome == TF_TRACE_OUTCOME_REJECT);
    CHECK(replay.invocations[3].outcome == TF_TRACE_OUTCOME_FAULT && replay.invocations[3].emissions == 1);
    for (size_t i = 0; i < recorder.count; ++i) {
        const TfTraceRecord *x = &records[i], *y = &replay.recorder.records[i];
        CHECK(x->sequence_id == y->sequence_id && x->preview_length == y->preview_length &&
              x->preview_truncated == y->preview_truncated && !memcmp(x->preview, y->preview, x->preview_length));
        CHECK(x->event.kind == y->event.kind && x->event.invocation_id == y->event.invocation_id &&
              x->event.parent_invocation_id == y->event.parent_invocation_id &&
              x->event.function_id == y->event.function_id && x->event.block_id == y->event.block_id &&
              x->event.let_id == y->event.let_id && x->event.span_id == y->event.span_id &&
              x->event.ordinal == y->event.ordinal);
    }
    CHECK(tf_trace_serialize(&b, &replay.recorder, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(a.length == b.length && !memcmp(a.bytes, b.bytes, a.length));
    CHECK(tf_trace_replay_bytes(&again, b.bytes, b.length, NULL, NULL) == TF_TRACE_IO_OK);
    for (size_t i = 0; i < replay.invocation_count; ++i)
        CHECK(replay.invocations[i].outcome == again.invocations[i].outcome &&
              replay.invocations[i].last_record == again.invocations[i].last_record);
    tf_trace_serialized_destroy(&a);
    tf_trace_serialized_destroy(&b);
    tf_trace_replay_destroy(&replay);
    tf_trace_replay_destroy(&again);
}
static void all_bytes_and_limits(void) {
    unsigned char bytes[256];
    TfTraceRecord records[8];
    TfTraceRecorder recorder;
    TfTraceSerialized serialized = {0};
    TfTraceReplay replay = {0};
    TfTraceIoLimits limits;
    uint64_t id;
    for (size_t i = 0; i < sizeof bytes; ++i) bytes[i] = (unsigned char)i;
    CHECK(tf_trace_recorder_init(&recorder, records, 8));
    CHECK(tf_trace_next_invocation(&recorder, &id));
    emit(&recorder, id, 0, TF_TRACE_STEP_BEGIN, 0, NULL, 0, false);
    for (size_t i = 0; i < 4; ++i) emit(&recorder, id, 0, TF_TRACE_WRITER, i + 1, bytes + i * 64, 64, false);
    emit(&recorder, id, 0, TF_TRACE_ACCEPT, 0, NULL, 0, false);
    CHECK(tf_trace_serialize(&serialized, &recorder, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(tf_trace_replay_bytes(&replay, serialized.bytes, serialized.length, NULL, NULL) == TF_TRACE_IO_OK);
    for (size_t i = 0; i < 4; ++i) CHECK(!memcmp(replay.recorder.records[i + 1].preview, bytes + i * 64, 64));
    tf_trace_replay_destroy(&replay);
    limits = (TfTraceIoLimits){serialized.length, recorder.count};
    CHECK(tf_trace_replay_bytes(&replay, serialized.bytes, serialized.length, &limits, NULL) == TF_TRACE_IO_OK);
    tf_trace_replay_destroy(&replay);
    {
        TfTraceSerialized exact = {0};
        CHECK(tf_trace_serialize(&exact, &recorder, &limits, NULL) == TF_TRACE_IO_OK);
        CHECK(exact.length == serialized.length);
        tf_trace_serialized_destroy(&exact);
        --limits.max_bytes;
        CHECK(tf_trace_serialize(&exact, &recorder, &limits, NULL) == TF_TRACE_IO_LIMIT);
        CHECK(!exact.bytes && !exact.length);
        CHECK(tf_trace_replay_bytes(&replay, serialized.bytes, serialized.length, &limits, NULL) == TF_TRACE_IO_LIMIT);
        limits.max_bytes = serialized.length;
        --limits.max_records;
        CHECK(tf_trace_serialize(&exact, &recorder, &limits, NULL) == TF_TRACE_IO_LIMIT);
        CHECK(tf_trace_replay_bytes(&replay, serialized.bytes, serialized.length, &limits, NULL) == TF_TRACE_IO_SYNTAX);
    }
    tf_trace_serialized_destroy(&serialized);
}
static void truncation(void) {
    TfTraceReplay replay = {0};
    size_t header_length = sizeof M2 - 1;
    for (size_t cut = header_length; cut < sizeof VALID - 1; ++cut) {
        CHECK(tf_trace_replay_bytes(&replay, VALID, cut, NULL, NULL) == TF_TRACE_IO_OK);
        CHECK(!replay.complete_file && replay.unknown);
        for (size_t i = 0; i < replay.invocation_count; ++i)
            CHECK(replay.invocations[i].outcome == TF_TRACE_OUTCOME_UNKNOWN);
        tf_trace_replay_destroy(&replay);
    }
    for (size_t cut = 0; cut < header_length; ++cut) {
        CHECK(tf_trace_replay_bytes(&replay, VALID, cut, NULL, NULL) == TF_TRACE_IO_SYNTAX);
        check_empty_replay(&replay);
    }
    CHECK(tf_trace_replay_bytes(&replay, M2 BEGIN, sizeof(M2 BEGIN) - 1, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(replay.unknown && replay.invocations[0].observed_terminal == 0);
    tf_trace_replay_destroy(&replay);
    {
        const char text[] = "TFTRACE 1\nM 1 1 1 0 0 0 0 0 0 0\n" BEGIN "END\n";
        CHECK(tf_trace_replay_bytes(&replay, text, sizeof text - 1, NULL, NULL) == TF_TRACE_IO_OK);
        CHECK(replay.complete_file && replay.unknown && replay.invocations[0].outcome == TF_TRACE_OUTCOME_UNKNOWN);
        tf_trace_replay_destroy(&replay);
    }
    {
        TfTraceRecord records[3];
        TfTraceRecorder recorder;
        TfTraceSerialized serialized;
        uint64_t id;
        CHECK(tf_trace_recorder_init(&recorder, records, 3));
        CHECK(tf_trace_next_invocation(&recorder, &id));
        emit(&recorder, id, 0, TF_TRACE_STEP_BEGIN, 0, (const unsigned char *)"x", 1, true);
        emit(&recorder, id, 0, TF_TRACE_ACCEPT, 0, NULL, 0, false);
        CHECK(tf_trace_serialize(&serialized, &recorder, NULL, NULL) == TF_TRACE_IO_OK);
        CHECK(tf_trace_replay_bytes(&replay, serialized.bytes, serialized.length, NULL, NULL) == TF_TRACE_IO_OK);
        CHECK(replay.unknown && replay.invocations[0].observed_terminal == TF_TRACE_ACCEPT &&
              replay.invocations[0].outcome == TF_TRACE_OUTCOME_UNKNOWN);
        tf_trace_replay_destroy(&replay);
        tf_trace_serialized_destroy(&serialized);
        CHECK(tf_trace_recorder_init(&recorder, records, 0));
        CHECK(tf_trace_next_invocation(&recorder, &id));
        {
            TfTraceEvent event = {.kind = TF_TRACE_STEP_BEGIN, .invocation_id = id};
            CHECK(tf_trace_capture(&recorder, &event, NULL, 0, false) == TF_TRACE_OMITTED);
        }
        CHECK(tf_trace_serialize(&serialized, &recorder, NULL, NULL) == TF_TRACE_IO_OK);
        CHECK(tf_trace_replay_bytes(&replay, serialized.bytes, serialized.length, NULL, NULL) == TF_TRACE_IO_OK);
        CHECK(replay.unknown && !replay.invocation_count && replay.recorder.omitted_events == 1);
        tf_trace_replay_destroy(&replay);
        tf_trace_serialized_destroy(&serialized);
    }
}
static void malformed(void) {
    static const char *const cases[] = {
        "", "TFTRACE 2\n", "TFTRACE 01\n", "TFTRACE 1\r\n",
        "TFTRACE 1\nM -1 0 0 0 0 0 0 0 0 0\nEND\n",
        "TFTRACE 1\nM +0 0 0 0 0 0 0 0 0 0\nEND\n",
        "TFTRACE 1\nM 00 0 0 0 0 0 0 0 0 0\nEND\n",
        "TFTRACE 1\nM 4097 0 0 0 0 0 0 0 0 0\nEND\n",
        "TFTRACE 1\nM 18446744073709551616 0 0 0 0 0 0 0 0 0\nEND\n",
        "TFTRACE 1\nM 0 18446744073709551616 0 0 0 0 0 0 0 0\nEND\n",
        "TFTRACE 1\nM 0 0 0 0 0 0 2 0 0 0\nEND\n",
        M2 BEGIN "END\n", VALID "\n", VALID "R", VALID "END\n",
        M2 BEGIN "R 2 1 0 8 7 0 0 0 0 0 0 \"\"\nEND\n",
        M2 BEGIN "R 2 1 0 0 7 0 0 0 0 0 0 \"\"\nEND\n",
        M2 BEGIN "R 0",
        M2 BEGIN "R 2 0 ",
        M2 BEGIN "R 2 1 0 0 ",
        M2 BEGIN "R 2 1 0 5 7 18446744073709551616 0 0 0 0 0 \"\"\nEND\n",
        M2 BEGIN "R 2 1 0 5 7 0 0 0 0 0 65 \"\"\nEND\n",
        M2 BEGIN "R 2 1 0 5 7 0 0 0 0 0 1 \"\"\nEND\n",
        M2 BEGIN "R 2 1 0 5 7 0 0 0 0 0 0 \"x\"\nEND\n",
        M2 BEGIN "R 2 1 0 5 7 0 0 0 0 0 1 \"\\xGG\"\nEND\n",
        M2 BEGIN "R 2 1 0 5 7 0 0 0 0 0 1 \"\\x0a\"\nEND\n",
        M2 BEGIN "R 2 1 0 5 7 0 0 0 0 0 1 \"\\x41\"\nEND\n",
        M2 BEGIN "R 2 1 0 5 7 0 0 0 0 0 1 \"\\n\"\nEND\n",
        M2 BEGIN "R 2 1 0 5 7 0 0 0 0 0 1 \"\t\"\nEND\n",
        M2 BEGIN "R 2 1 0 5 7 0 0 0 0 0 1 \"\xFF\"\nEND\n",
        M2 BEGIN ACCEPT "ENX",
        M2 BEGIN ACCEPT "END\r\n"
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) inspect_text(cases[i], TF_TRACE_IO_SYNTAX);
    {
        char nul[sizeof VALID];
        TfTraceReplay replay;
        memcpy(nul, VALID, sizeof nul);
        nul[sizeof M2 + 5] = '\0';
        CHECK(tf_trace_replay_bytes(&replay, nul, sizeof nul - 1, NULL, NULL) == TF_TRACE_IO_SYNTAX);
        check_empty_replay(&replay);
        CHECK(tf_trace_replay_bytes(&replay, VALID, sizeof VALID, NULL, NULL) == TF_TRACE_IO_SYNTAX);
    }
    inspect_text("TFTRACE 1\nM 0 0 0 1 0 0 0 0 0 0\nEND\n", TF_TRACE_IO_SYNTAX);
    inspect_text("TFTRACE 1\nM 0 0 0 0 1 0 1 0 0 0\nEND\n", TF_TRACE_IO_SYNTAX);
    inspect_text("TFTRACE 1\nM 0 0 0 0 0 1 1 0 0 0\nEND\n", TF_TRACE_IO_SYNTAX);
    inspect_text("TFTRACE 1\nM 0 0 0 0 0 0 1 1 0 0\nEND\n", TF_TRACE_IO_SYNTAX);
    inspect_text("TFTRACE 1\nM 0 0 0 0 0 0 1 0 1 0\nEND\n", TF_TRACE_IO_SYNTAX);
    inspect_text("TFTRACE 1\nM 0 0 0 0 0 0 1 0 0 1\nEND\n", TF_TRACE_IO_SYNTAX);
}

/* Deliberately bypass serialization to submit forged records to replay too. */
static void raw_file(FILE *stream, const TfTraceRecorder *r) {
    CHECK(fprintf(stream, "TFTRACE 1\nM %zu %" PRIu64 " %" PRIu64
          " 0 0 0 0 0 0 0\n", r->count, r->last_sequence_id, r->last_invocation_id) > 0);
    for (size_t i = 0; i < r->count; ++i) {
        const TfTraceRecord *x = &r->records[i];
        const TfTraceEvent *e = &x->event;
        CHECK(fprintf(stream, "R %" PRIu64 " %" PRIu64 " %" PRIu64
              " %u %zu %zu %zu %zu %zu 0 0 \"\"\n", x->sequence_id,
              e->invocation_id, e->parent_invocation_id, (unsigned)e->kind,
              e->function_id, e->block_id, e->let_id, e->span_id, e->ordinal) > 0);
    }
    CHECK(fputs("END\n", stream) >= 0 && fflush(stream) == 0);
    rewind(stream);
}
static void forged_order(void) {
    TfTraceRecord originals[32], records[32];
    TfTraceRecorder recorder;
    fixture(&recorder, originals, 32);
    for (unsigned variant = 0; variant < 21; ++variant) {
        TfTraceSerialized serialized = {0};
        TfTraceReplay replay = {0};
        FILE *stream;
        TfTraceIoStatus expected = TF_TRACE_IO_ORDER;
        memcpy(records, originals, sizeof records);
        recorder.records = records;
        switch (variant) {
        case 0: records[0].event.kind = TF_TRACE_WRITER; break;
        case 1: records[0].event.ordinal = 1; break;
        case 2: records[2].event.parent_invocation_id = 0; break;
        case 3: records[3].event.invocation_id = 1; records[3].event.parent_invocation_id = 0; break;
        case 4: records[4].event.ordinal = 2; break;
        case 5: records[5].event.ordinal = 2; break;
        case 6: records[5].event.kind = TF_TRACE_WRITER; break;
        case 7: records[6].event.kind = TF_TRACE_REJECT; break;
        case 8: records[7].event.ordinal = 3; break; /* Duplicate writer across child. */
        case 9: records[8].event.parent_invocation_id = 1; expected = TF_TRACE_IO_INVALID; break;
        case 10: records[9].event.ordinal = 1; break;
        case 11: records[11].event.ordinal = 1; break;
        case 12: records[14].event.kind = TF_TRACE_WRITER; records[14].event.ordinal = 2; break;
        case 13: records[13].event.kind = TF_TRACE_REJECT; records[13].event.ordinal = 0; break;
        case 14: records[7].event.kind = TF_TRACE_ACCEPT; records[7].event.ordinal = 0; break;
        case 15: records[2].event.invocation_id = 1; records[2].event.parent_invocation_id = 0; break;
        case 16: records[1].sequence_id = 1; expected = TF_TRACE_IO_INVALID; break;
        case 17: records[17].sequence_id = 19; expected = TF_TRACE_IO_INVALID; break;
        case 18: records[1].event.invocation_id = 0; expected = TF_TRACE_IO_INVALID; break;
        case 19: records[6].event.kind = TF_TRACE_FAULT; break;
        case 20: records[4].event.kind = TF_TRACE_FAULT; records[4].event.ordinal = 0; break;
        default: CHECK(false);
        }
        CHECK(tf_trace_serialize(&serialized, &recorder, NULL, NULL) == expected);
        CHECK(!serialized.bytes && !serialized.length);
        stream = fopen(FILE_PATH, "w+b");
        CHECK(stream);
        raw_file(stream, &recorder);
        CHECK(tf_trace_replay_read(&replay, stream, NULL, NULL) ==
              (variant == 18 ? TF_TRACE_IO_SYNTAX : expected));
        check_empty_replay(&replay);
        CHECK(fclose(stream) == 0 && remove(FILE_PATH) == 0);
    }
}
static void forged_summary_and_arguments(void) {
    TfTraceRecord records[32];
    TfTraceRecorder good, recorder;
    TfTraceSerialized serialized;
    TfTraceReplay replay;
    TfTraceIoLimits limits;
    TfAllocator bad = {0};
    fixture(&good, records, 32);
    for (unsigned i = 0; i < 9; ++i) {
        recorder = good;
        switch (i) {
        case 0: recorder.count = recorder.capacity + 1; break;
        case 1: recorder.capacity = SIZE_MAX; break;
        case 2: recorder.records = NULL; break;
        case 3: recorder.clipped_previews = 1; recorder.truncated = true; break;
        case 4: recorder.omitted_events = 1; break;
        case 5: recorder.omitted_preview_bytes = 1; break;
        case 6: recorder.sequence_exhausted = true; recorder.truncated = true; break;
        case 7: recorder.invocation_exhausted = true; recorder.truncated = true; break;
        case 8: recorder.counters_saturated = true; recorder.truncated = true; break;
        default: CHECK(false);
        }
        CHECK(tf_trace_serialize(&serialized, &recorder, NULL, NULL) == TF_TRACE_IO_INVALID);
        CHECK(!serialized.bytes);
    }
    records[0].preview_length = 65;
    CHECK(tf_trace_serialize(&serialized, &good, NULL, NULL) == TF_TRACE_IO_INVALID);
    records[0].preview_length = 9;
    CHECK(tf_trace_serialize(NULL, &good, NULL, NULL) == TF_TRACE_IO_INVALID);
    CHECK(tf_trace_serialize(&serialized, NULL, NULL, NULL) == TF_TRACE_IO_INVALID);
    CHECK(tf_trace_serialize(&serialized, &good, NULL, &bad) == TF_TRACE_IO_INVALID);
    CHECK(tf_trace_replay_bytes(NULL, VALID, sizeof VALID - 1, NULL, NULL) == TF_TRACE_IO_INVALID);
    CHECK(tf_trace_replay_bytes(&replay, NULL, 1, NULL, NULL) == TF_TRACE_IO_INVALID);
    CHECK(tf_trace_replay_bytes(&replay, NULL, 0, NULL, NULL) == TF_TRACE_IO_SYNTAX);
    CHECK(tf_trace_replay_read(&replay, NULL, NULL, NULL) == TF_TRACE_IO_INVALID);
    CHECK(tf_trace_replay_read(NULL, NULL, NULL, NULL) == TF_TRACE_IO_INVALID);
    CHECK(tf_trace_write(NULL, &good, NULL, NULL) == TF_TRACE_IO_INVALID);
    for (unsigned i = 0; i < 4; ++i) {
        limits = (TfTraceIoLimits){TF_TRACE_IO_MAX_BYTES, TF_TRACE_IO_MAX_RECORDS};
        if (i == 0) limits.max_bytes = 0;
        if (i == 1) limits.max_bytes = SIZE_MAX;
        if (i == 2) limits.max_records = 0;
        if (i == 3) limits.max_records = SIZE_MAX;
        CHECK(tf_trace_serialize(&serialized, &good, &limits, NULL) == TF_TRACE_IO_INVALID);
        CHECK(tf_trace_replay_bytes(&replay, VALID, sizeof VALID - 1, &limits, NULL) == TF_TRACE_IO_INVALID);
    }
    tf_trace_serialized_destroy(NULL);
    tf_trace_replay_destroy(NULL);
}
static void endpoints(void) {
    TfTraceRecord records[2] = {0};
    TfTraceRecorder recorder = {.records = records, .capacity = 2, .count = 2,
        .last_sequence_id = UINT64_MAX, .last_invocation_id = UINT64_MAX};
    TfTraceSerialized serialized;
    TfTraceReplay replay;
    records[0] = (TfTraceRecord){.sequence_id = UINT64_MAX - 1,
        .event = {.kind = TF_TRACE_STEP_BEGIN, .invocation_id = UINT64_MAX,
                  .function_id = SIZE_MAX, .block_id = SIZE_MAX, .let_id = SIZE_MAX,
                  .span_id = SIZE_MAX}};
    records[1] = records[0];
    records[1].sequence_id = UINT64_MAX;
    records[1].event.kind = TF_TRACE_FAULT;
    CHECK(tf_trace_serialize(&serialized, &recorder, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(tf_trace_replay_bytes(&replay, serialized.bytes, serialized.length, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(replay.recorder.records[1].sequence_id == UINT64_MAX &&
          replay.invocations[0].id == UINT64_MAX && replay.invocations[0].function_id == SIZE_MAX);
    CHECK(replay.unknown && replay.invocations[0].observed_terminal == TF_TRACE_FAULT);
    tf_trace_replay_destroy(&replay);
    tf_trace_serialized_destroy(&serialized);
    recorder.truncated = recorder.sequence_exhausted = recorder.invocation_exhausted = true;
    recorder.counters_saturated = true;
    recorder.omitted_events = UINT64_MAX;
    CHECK(tf_trace_serialize(&serialized, &recorder, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(tf_trace_replay_bytes(&replay, serialized.bytes, serialized.length, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(replay.unknown && replay.recorder.counters_saturated && replay.recorder.omitted_events == UINT64_MAX);
    tf_trace_replay_destroy(&replay);
    tf_trace_serialized_destroy(&serialized);
}
static void maximum_and_fault_unwind(void) {
    TfTraceRecord *records = calloc(TF_TRACE_IO_MAX_RECORDS, sizeof *records);
    TfTraceRecorder recorder;
    TfTraceSerialized serialized;
    TfTraceReplay replay;
    size_t depth = TF_TRACE_IO_MAX_RECORDS / 2;
    CHECK(records);
    CHECK(tf_trace_recorder_init(&recorder, records, TF_TRACE_IO_MAX_RECORDS));
    /* Deep nesting is inspected iteratively, never using a recursive C call. */
    for (size_t i = 0; i < depth; ++i) {
        uint64_t id;
        CHECK(tf_trace_next_invocation(&recorder, &id));
        emit(&recorder, id, i, TF_TRACE_STEP_BEGIN, 0, NULL, 0, false);
    }
    for (size_t i = depth; i > 0; --i) {
        emit(&recorder, i, i - 1, TF_TRACE_FAULT, 0, NULL, 0, false);
        /* Fault origins can name a different Core function from step_begin. */
        records[recorder.count - 1].event.function_id = 19;
    }
    CHECK(tf_trace_serialize(&serialized, &recorder, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(tf_trace_replay_bytes(&replay, serialized.bytes, serialized.length, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(!replay.unknown && replay.invocation_count == depth);
    for (size_t i = 0; i < depth; ++i) CHECK(replay.invocations[i].outcome == TF_TRACE_OUTCOME_FAULT);
    tf_trace_replay_destroy(&replay);
    tf_trace_serialized_destroy(&serialized);
    CHECK(tf_trace_recorder_reset(&recorder));
    {
        uint64_t id;
        CHECK(tf_trace_next_invocation(&recorder, &id));
        emit(&recorder, id, 0, TF_TRACE_STEP_BEGIN, 0, NULL, 0, false);
        for (size_t i = 1; i < TF_TRACE_IO_MAX_RECORDS - 1; ++i)
            emit(&recorder, id, 0, TF_TRACE_WRITER, TF_TRACE_IO_MAX_RECORDS - i, NULL, 0, false);
        emit(&recorder, id, 0, TF_TRACE_ACCEPT, 0, NULL, 0, false);
    }
    CHECK(tf_trace_serialize(&serialized, &recorder, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(tf_trace_replay_bytes(&replay, serialized.bytes, serialized.length, NULL, NULL) == TF_TRACE_IO_OK);
    CHECK(!replay.unknown && replay.invocations[0].writers == TF_TRACE_IO_MAX_RECORDS - 2);
    tf_trace_replay_destroy(&replay);
    tf_trace_serialized_destroy(&serialized);
    free(records);
}
static void allocation_failures_and_streams(void) {
    TfTraceRecord records[32];
    TfTraceRecorder recorder;
    fixture(&recorder, records, 32);
    for (size_t fail = 0; fail < 3; ++fail) {
        Allocations a = {0, fail, 0};
        TfAllocator allocator = {&a, allocate, deallocate};
        TfTraceSerialized serialized;
        TfTraceIoStatus status = tf_trace_serialize(&serialized, &recorder, NULL, &allocator);
        CHECK(status == (fail < 2 ? TF_TRACE_IO_ALLOCATION : TF_TRACE_IO_OK));
        tf_trace_serialized_destroy(&serialized);
        CHECK(a.live == 0);
    }
    {
        Allocations a = {0, SIZE_MAX, 0};
        TfAllocator allocator = {&a, allocate, deallocate};
        TfTraceReplay replay;
        CHECK(tf_trace_replay_bytes(&replay, VALID "garbage", sizeof(VALID "garbage") - 1,
                                   NULL, &allocator) == TF_TRACE_IO_SYNTAX);
        CHECK(a.calls == 2 && a.live == 0);
        check_empty_replay(&replay);
    }
    for (size_t fail = 0; fail < 3; ++fail) {
        Allocations a = {0, fail, 0};
        TfAllocator allocator = {&a, allocate, deallocate};
        TfTraceReplay replay;
        TfTraceIoStatus status = tf_trace_replay_bytes(&replay, VALID, sizeof VALID - 1, NULL, &allocator);
        CHECK(status == (fail < 2 ? TF_TRACE_IO_ALLOCATION : TF_TRACE_IO_OK));
        if (status != TF_TRACE_IO_OK) check_empty_replay(&replay);
        tf_trace_replay_destroy(&replay);
        CHECK(a.live == 0);
    }
    {
        FILE *stream = fopen(FILE_PATH, "w+b");
        TfTraceReplay replay;
        CHECK(stream);
        CHECK(tf_trace_write(stream, &recorder, NULL, NULL) == TF_TRACE_IO_OK);
        for (size_t fail = 0; fail < 4; ++fail) {
            Allocations a = {0, fail, 0};
            TfAllocator allocator = {&a, allocate, deallocate};
            TfTraceIoStatus status;
            rewind(stream);
            status = tf_trace_replay_read(&replay, stream, NULL, &allocator);
            CHECK(status == (fail < 3 ? TF_TRACE_IO_ALLOCATION : TF_TRACE_IO_OK));
            if (status != TF_TRACE_IO_OK) check_empty_replay(&replay);
            else CHECK(!replay.unknown && replay.invocation_count == 4);
            tf_trace_replay_destroy(&replay);
            CHECK(a.live == 0);
        }
        {
            TfTraceIoLimits small = {1, TF_TRACE_IO_MAX_RECORDS};
            rewind(stream);
            CHECK(tf_trace_replay_read(&replay, stream, &small, NULL) == TF_TRACE_IO_LIMIT);
            check_empty_replay(&replay);
        }
        CHECK(fclose(stream) == 0);
        stream = fopen(FILE_PATH, "rb");
        CHECK(stream);
        CHECK(tf_trace_write(stream, &recorder, NULL, NULL) == TF_TRACE_IO_STREAM);
        CHECK(tf_trace_write(stream, &recorder, NULL, NULL) == TF_TRACE_IO_STREAM);
        (void)fclose(stream);
        stream = fopen(FILE_PATH, "wb");
        CHECK(stream);
        CHECK(tf_trace_replay_read(&replay, stream, NULL, NULL) == TF_TRACE_IO_STREAM);
        check_empty_replay(&replay);
        CHECK(tf_trace_replay_read(&replay, stream, NULL, NULL) == TF_TRACE_IO_STREAM);
        (void)fclose(stream);
    }
    for (size_t fail = 0; fail < 2; ++fail) {
        Allocations a = {0, fail, 0};
        TfAllocator allocator = {&a, allocate, deallocate};
        FILE *stream = fopen(FILE_PATH, "w+b");
        CHECK(stream);
        CHECK(tf_trace_write(stream, &recorder, NULL, &allocator) == TF_TRACE_IO_ALLOCATION);
        CHECK(ftell(stream) == 0 && a.live == 0);
        CHECK(fclose(stream) == 0);
    }
    {
        FILE *stream = fopen(FILE_PATH, "w+b");
        CHECK(stream);
        recorder.records[0].preview_length = 65;
        CHECK(tf_trace_write(stream, &recorder, NULL, NULL) == TF_TRACE_IO_INVALID);
        CHECK(ftell(stream) == 0);
        CHECK(fclose(stream) == 0 && remove(FILE_PATH) == 0);
    }
}
static void malformed_corpus(void) {
    uint32_t seed = UINT32_C(0x472AF3D1);
    unsigned char bytes[sizeof VALID + 32];
    for (size_t i = 0; i < 2000; ++i) {
        Allocations a = {0, SIZE_MAX, 0};
        TfAllocator allocator = {&a, allocate, deallocate};
        TfTraceReplay replay;
        TfTraceIoStatus status;
        size_t length = sizeof VALID - 1;
        memcpy(bytes, VALID, length);
        for (size_t j = 0; j < 1 + i % 7; ++j) {
            seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
            bytes[seed % length] = (unsigned char)(seed >> 24);
        }
        if (i % 3 == 0) length = i % length;
        status = tf_trace_replay_bytes(&replay, bytes, length, NULL, &allocator);
        if (status != TF_TRACE_IO_OK) check_empty_replay(&replay);
        tf_trace_replay_destroy(&replay);
        CHECK(a.live == 0);
    }
}
int main(void) {
    empty_and_golden();
    roundtrip();
    all_bytes_and_limits();
    truncation();
    malformed();
    forged_order();
    forged_summary_and_arguments();
    endpoints();
    maximum_and_fault_unwind();
    allocation_failures_and_streams();
    malformed_corpus();
    (void)printf("trace_io: %zu checks passed\n", checks);
    return EXIT_SUCCESS;
}
