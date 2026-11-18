#include "ternfold/trace_io.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static void *default_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}
static void default_deallocate(void *context, void *pointer) {
    (void)context;
    free(pointer);
}
static bool configuration(TfTraceIoLimits *limits, TfAllocator *allocator,
                          const TfTraceIoLimits *requested,
                          const TfAllocator *custom) {
    *limits = requested ? *requested :
        (TfTraceIoLimits){TF_TRACE_IO_MAX_BYTES, TF_TRACE_IO_MAX_RECORDS};
    *allocator = custom ? *custom :
        (TfAllocator){NULL, default_allocate, default_deallocate};
    return limits->max_bytes > 0 && limits->max_bytes <= TF_TRACE_IO_MAX_BYTES &&
        limits->max_records > 0 && limits->max_records <= TF_TRACE_IO_MAX_RECORDS &&
        allocator->allocate && allocator->deallocate;
}
void tf_trace_serialized_destroy(TfTraceSerialized *serialized) {
    if (!serialized) return;
    if (serialized->bytes)
        serialized->allocator.deallocate(serialized->allocator.context, serialized->bytes);
    *serialized = (TfTraceSerialized){0};
}
void tf_trace_replay_destroy(TfTraceReplay *replay) {
    if (!replay) return;
    if (replay->recorder.records)
        replay->allocator.deallocate(replay->allocator.context, replay->recorder.records);
    if (replay->invocations)
        replay->allocator.deallocate(replay->allocator.context, replay->invocations);
    *replay = (TfTraceReplay){0};
}

/* These are structural invariants, not verification against external Core. */
static bool summary_valid(const TfTraceRecorder *r, size_t declared) {
    return r->clipped_previews <= declared &&
        (r->omitted_preview_bytes == 0 || r->clipped_previews != 0) &&
        (!(r->omitted_events || r->clipped_previews || r->sequence_exhausted ||
           r->invocation_exhausted || r->counters_saturated) || r->truncated) &&
        (!r->sequence_exhausted || r->last_sequence_id == UINT64_MAX) &&
        (!r->invocation_exhausted || r->last_invocation_id == UINT64_MAX) &&
        (!r->counters_saturated || r->omitted_events == UINT64_MAX ||
            r->clipped_previews == UINT64_MAX || r->omitted_preview_bytes == UINT64_MAX);
}

typedef struct {
    TfTraceReplay *replay;
    size_t active; /* Index+1, zero denotes no active invocation. */
    uint64_t sequence, clipped;
    bool fault_pending; /* A child fault can only unwind its active ancestors. */
} Validation;

static size_t invocation_index(const TfTraceReplay *r, uint64_t id) {
    size_t lo = 0, hi = r->invocation_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (r->invocations[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < r->invocation_count && r->invocations[lo].id == id ? lo + 1 : 0;
}

static TfTraceIoStatus validate_record(Validation *v, const TfTraceRecord *record,
                                      size_t index) {
    TfTraceReplay *r = v->replay;
    const TfTraceEvent *e = &record->event;
    TfTraceInvocation *inv;
    if (record->preview_length > TF_TRACE_PREVIEW_CAPACITY ||
        record->sequence_id <= v->sequence ||
        record->sequence_id > r->recorder.last_sequence_id ||
        e->kind < TF_TRACE_STEP_BEGIN || e->kind > TF_TRACE_FAULT ||
        e->invocation_id == 0 || e->invocation_id > r->recorder.last_invocation_id ||
        e->parent_invocation_id >= e->invocation_id) return TF_TRACE_IO_INVALID;
    if (v->fault_pending && e->kind != TF_TRACE_FAULT) return TF_TRACE_IO_ORDER;
    if (e->kind == TF_TRACE_STEP_BEGIN) {
        uint64_t parent = v->active ? r->invocations[v->active - 1].id : 0;
        if (e->parent_invocation_id != parent || e->ordinal != 0 ||
            (r->invocation_count &&
             e->invocation_id <= r->invocations[r->invocation_count - 1].id))
            return TF_TRACE_IO_ORDER;
        /* At most one new invocation per retained record. */
        if (r->invocation_count >= r->declared_count) return TF_TRACE_IO_ORDER;
        inv = &r->invocations[r->invocation_count++];
        *inv = (TfTraceInvocation){.id = e->invocation_id,
            .parent_id = parent, .function_id = e->function_id,
            .first_record = index, .last_record = index};
        v->active = r->invocation_count;
    } else {
        if (!v->active) return TF_TRACE_IO_ORDER;
        inv = &r->invocations[v->active - 1];
        if (inv->id != e->invocation_id || inv->parent_id != e->parent_invocation_id)
            return TF_TRACE_IO_ORDER;
        switch (e->kind) {
        case TF_TRACE_WRITER:
            if (!e->ordinal || inv->guards || inv->emissions) return TF_TRACE_IO_ORDER;
            /* Source cell ordinals need not be scheduled in numeric order. */
            for (size_t i = inv->first_record + 1; i < index; ++i) {
                const TfTraceEvent *prior = &r->recorder.records[i].event;
                if (prior->invocation_id == inv->id && prior->kind == TF_TRACE_WRITER &&
                    prior->ordinal == e->ordinal) return TF_TRACE_IO_ORDER;
            }
            ++inv->writers;
            break;
        case TF_TRACE_GUARD:
            if (inv->emissions || e->ordinal != inv->guards + 1) return TF_TRACE_IO_ORDER;
            ++inv->guards;
            break;
        case TF_TRACE_EMISSION:
            if (e->ordinal != inv->emissions + 1) return TF_TRACE_IO_ORDER;
            ++inv->emissions;
            break;
        case TF_TRACE_REJECT:
            if (!inv->guards || inv->emissions ||
                r->recorder.records[inv->last_record].event.kind != TF_TRACE_GUARD)
                return TF_TRACE_IO_ORDER;
            /* Fall through to terminal handling. */
            /* fallthrough */
        case TF_TRACE_ACCEPT:
        case TF_TRACE_FAULT:
            if (e->ordinal != 0) return TF_TRACE_IO_ORDER;
            inv->observed_terminal = e->kind;
            v->active = invocation_index(r, inv->parent_id);
            v->fault_pending = e->kind == TF_TRACE_FAULT && v->active != 0;
            break;
        default: return TF_TRACE_IO_ORDER;
        }
        inv->last_record = index;
    }
    v->sequence = record->sequence_id;
    if (record->preview_truncated) ++v->clipped;
    return TF_TRACE_IO_OK;
}

static TfTraceIoStatus prepare(TfTraceReplay *r, size_t count, TfAllocator allocator) {
    r->allocator = allocator;
    r->declared_count = count;
    if (!count) return TF_TRACE_IO_OK;
    if (count > SIZE_MAX / sizeof *r->invocations) return TF_TRACE_IO_LIMIT;
    r->invocations = allocator.allocate(allocator.context, count * sizeof *r->invocations);
    return r->invocations ? TF_TRACE_IO_OK : TF_TRACE_IO_ALLOCATION;
}

typedef struct { char *bytes; size_t length, capacity; } Builder;
static bool append(Builder *b, const char *format, ...) {
    int written;
    va_list args;
    va_start(args, format);
    written = vsnprintf(b->bytes + b->length, b->capacity - b->length, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= b->capacity - b->length) return false;
    b->length += (size_t)written;
    return true;
}

TfTraceIoStatus tf_trace_serialize(TfTraceSerialized *out,
    const TfTraceRecorder *recorder, const TfTraceIoLimits *requested,
    const TfAllocator *custom) {
    TfTraceIoLimits limits;
    TfAllocator allocator;
    TfTraceReplay work = {0};
    Validation validation = {.replay = &work};
    TfTraceIoStatus status;
    Builder b = {0};
    if (!out) return TF_TRACE_IO_INVALID;
    *out = (TfTraceSerialized){0};
    if (!configuration(&limits, &allocator, requested, custom) || !recorder ||
        recorder->capacity > SIZE_MAX / sizeof(TfTraceRecord) ||
        recorder->count > recorder->capacity ||
        (recorder->capacity && !recorder->records)) return TF_TRACE_IO_INVALID;
    if (recorder->count > limits.max_records) return TF_TRACE_IO_LIMIT;
    if (!summary_valid(recorder, recorder->count)) return TF_TRACE_IO_INVALID;
    status = prepare(&work, recorder->count, allocator);
    if (status != TF_TRACE_IO_OK) return status;
    work.recorder = *recorder; /* Borrowed here, never pass to replay_destroy. */
    for (size_t i = 0; i < recorder->count; ++i) {
        status = validate_record(&validation, &recorder->records[i], i);
        if (status != TF_TRACE_IO_OK) goto cleanup;
    }
    if (validation.clipped != recorder->clipped_previews) {
        status = TF_TRACE_IO_INVALID;
        goto cleanup;
    }
    /* Configuration hard cap proves this +1 cannot overflow. */
    b.capacity = limits.max_bytes + 1;
    b.bytes = allocator.allocate(allocator.context, b.capacity);
    if (!b.bytes) { status = TF_TRACE_IO_ALLOCATION; goto cleanup; }
    status = TF_TRACE_IO_LIMIT;
    if (!append(&b, "TFTRACE 1\nM %zu %" PRIu64 " %" PRIu64 " %" PRIu64
            " %" PRIu64 " %" PRIu64 " %u %u %u %u\n", recorder->count,
            recorder->last_sequence_id, recorder->last_invocation_id,
            recorder->omitted_events, recorder->clipped_previews,
            recorder->omitted_preview_bytes, (unsigned)recorder->truncated,
            (unsigned)recorder->counters_saturated, (unsigned)recorder->sequence_exhausted,
            (unsigned)recorder->invocation_exhausted)) goto cleanup;
    for (size_t i = 0; i < recorder->count; ++i) {
        const TfTraceRecord *record = &recorder->records[i];
        const TfTraceEvent *e = &record->event;
        if (!append(&b, "R %" PRIu64 " %" PRIu64 " %" PRIu64
                " %u %zu %zu %zu %zu %zu %u %zu \"", record->sequence_id,
                e->invocation_id, e->parent_invocation_id, (unsigned)e->kind,
                e->function_id, e->block_id, e->let_id, e->span_id, e->ordinal,
                (unsigned)record->preview_truncated, record->preview_length)) goto cleanup;
        for (size_t j = 0; j < record->preview_length; ++j) {
            unsigned char c = record->preview[j];
            if (c >= 32 && c <= 126 && c != '"' && c != '\\') {
                if (!append(&b, "%c", (int)c)) goto cleanup;
            } else if (!append(&b, "\\x%02X", (unsigned)c)) goto cleanup;
        }
        if (!append(&b, "\"\n")) goto cleanup;
    }
    if (!append(&b, "END\n")) goto cleanup;
    *out = (TfTraceSerialized){b.bytes, b.length, allocator};
    b.bytes = NULL;
    status = TF_TRACE_IO_OK;
cleanup:
    if (work.invocations) allocator.deallocate(allocator.context, work.invocations);
    if (b.bytes) allocator.deallocate(allocator.context, b.bytes);
    return status;
}

TfTraceIoStatus tf_trace_write(FILE *stream, const TfTraceRecorder *recorder,
    const TfTraceIoLimits *limits, const TfAllocator *allocator) {
    TfTraceSerialized serialized = {0};
    TfTraceIoStatus status;
    if (!stream) return TF_TRACE_IO_INVALID;
    if (ferror(stream)) return TF_TRACE_IO_STREAM;
    status = tf_trace_serialize(&serialized, recorder, limits, allocator);
    if (status != TF_TRACE_IO_OK) return status;
    if (fwrite(serialized.bytes, 1, serialized.length, stream) != serialized.length ||
        fflush(stream) != 0 || ferror(stream)) status = TF_TRACE_IO_STREAM;
    tf_trace_serialized_destroy(&serialized);
    return status;
}

/* A parser recognizes canonical byte syntax, including incomplete valid prefixes
 * of the final event/END line. Physical LF ends a line; CR is never whitespace. */
typedef struct { const unsigned char *bytes; size_t length, at; bool incomplete; } Parser;
static bool character(Parser *p, unsigned char c) {
    if (p->at == p->length) { p->incomplete = true; return false; }
    if (p->bytes[p->at] != c) return false;
    ++p->at;
    return true;
}
static bool literal(Parser *p, const char *text) {
    for (size_t i = 0; text[i]; ++i)
        if (!character(p, (unsigned char)text[i])) return false;
    return true;
}
static bool number(Parser *p, uint64_t maximum, uint64_t *out) {
    uint64_t value = 0;
    size_t start = p->at;
    if (p->at == p->length) { p->incomplete = true; return false; }
    if (p->bytes[p->at] < '0' || p->bytes[p->at] > '9') return false;
    while (p->at < p->length && p->bytes[p->at] >= '0' && p->bytes[p->at] <= '9') {
        unsigned digit = (unsigned)(p->bytes[p->at] - '0');
        if ((p->at > start && p->bytes[start] == '0') ||
            value > maximum / 10 ||
            (value == maximum / 10 && digit > maximum % 10)) return false;
        value = value * 10 + digit;
        ++p->at;
    }
    *out = value;
    return true;
}
static bool field(Parser *p, uint64_t maximum, uint64_t *out) {
    return number(p, maximum, out) && character(p, ' ');
}
static bool positive_field(Parser *p, uint64_t maximum, uint64_t *out) {
    return number(p, maximum, out) && *out != 0 && character(p, ' ');
}
static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}
static bool preview(Parser *p, TfTraceRecord *record) {
    if (!character(p, '"')) return false;
    for (size_t i = 0; i < record->preview_length; ++i) {
        unsigned char c;
        if (p->at == p->length) { p->incomplete = true; return false; }
        c = p->bytes[p->at++];
        if (c == '\\') {
            int hi, lo;
            if (!character(p, 'x')) return false;
            if (p->at == p->length) { p->incomplete = true; return false; }
            hi = hex_digit(p->bytes[p->at++]);
            if (hi < 0) return false;
            if (p->at == p->length) { p->incomplete = true; return false; }
            lo = hex_digit(p->bytes[p->at++]);
            if (lo < 0) return false;
            c = (unsigned char)(hi * 16 + lo);
            if (c >= 32 && c <= 126 && c != '"' && c != '\\') return false;
        } else if (c < 32 || c > 126 || c == '"') return false;
        record->preview[i] = c;
    }
    return character(p, '"') && character(p, '\n');
}
static bool record_line(Parser *p, TfTraceRecord *record) {
    uint64_t kind, function, block, let_id, span, ordinal, clipped, length;
    if (!literal(p, "R ") || !positive_field(p, UINT64_MAX, &record->sequence_id) ||
        !positive_field(p, UINT64_MAX, &record->event.invocation_id) ||
        !field(p, UINT64_MAX, &record->event.parent_invocation_id) ||
        !positive_field(p, TF_TRACE_FAULT, &kind) || !field(p, SIZE_MAX, &function) ||
        !field(p, SIZE_MAX, &block) || !field(p, SIZE_MAX, &let_id) ||
        !field(p, SIZE_MAX, &span) || !field(p, SIZE_MAX, &ordinal) ||
        !field(p, 1, &clipped) || !field(p, TF_TRACE_PREVIEW_CAPACITY, &length)) return false;
    record->event.kind = (TfTraceEventKind)kind;
    record->event.function_id = (size_t)function;
    record->event.block_id = (size_t)block;
    record->event.let_id = (size_t)let_id;
    record->event.span_id = (size_t)span;
    record->event.ordinal = (size_t)ordinal;
    record->preview_truncated = clipped != 0;
    record->preview_length = (size_t)length;
    return preview(p, record);
}
static bool header(Parser *p, TfTraceReplay *r, const TfTraceIoLimits *limits) {
    uint64_t count, truncated, saturated, sequence_exhausted, invocation_exhausted;
    TfTraceRecorder *s = &r->recorder;
    if (!literal(p, "TFTRACE 1\nM ") || !field(p, limits->max_records, &count) ||
        !field(p, UINT64_MAX, &s->last_sequence_id) ||
        !field(p, UINT64_MAX, &s->last_invocation_id) ||
        !field(p, UINT64_MAX, &s->omitted_events) ||
        !field(p, UINT64_MAX, &s->clipped_previews) ||
        !field(p, UINT64_MAX, &s->omitted_preview_bytes) ||
        !field(p, 1, &truncated) || !field(p, 1, &saturated) ||
        !field(p, 1, &sequence_exhausted) || !number(p, 1, &invocation_exhausted) ||
        !character(p, '\n')) return false;
    r->declared_count = (size_t)count;
    s->truncated = truncated != 0;
    s->counters_saturated = saturated != 0;
    s->sequence_exhausted = sequence_exhausted != 0;
    s->invocation_exhausted = invocation_exhausted != 0;
    return summary_valid(s, r->declared_count);
}
static void outcomes(TfTraceReplay *r) {
    r->unknown = !r->complete_file || r->recorder.truncated;
    /* Missing minted invocations or sequence gaps are not proof of execution.
     * Boundary-seeded recorder IDs remain inspectable, conservatively UNKNOWN. */
    if (r->recorder.last_invocation_id != r->invocation_count ||
        r->recorder.last_sequence_id != r->recorder.count) r->unknown = true;
    for (size_t i = 0; i < r->recorder.count; ++i)
        if (r->recorder.records[i].sequence_id != i + 1) r->unknown = true;
    for (size_t i = 0; i < r->invocation_count; ++i)
        if (!r->invocations[i].observed_terminal) r->unknown = true;
    for (size_t i = 0; i < r->invocation_count; ++i) {
        TfTraceInvocation *inv = &r->invocations[i];
        inv->outcome = TF_TRACE_OUTCOME_UNKNOWN;
        if (!r->unknown) {
            if (inv->observed_terminal == TF_TRACE_ACCEPT) inv->outcome = TF_TRACE_OUTCOME_ACCEPT;
            if (inv->observed_terminal == TF_TRACE_REJECT) inv->outcome = TF_TRACE_OUTCOME_REJECT;
            if (inv->observed_terminal == TF_TRACE_FAULT) inv->outcome = TF_TRACE_OUTCOME_FAULT;
        }
    }
}

TfTraceIoStatus tf_trace_replay_bytes(TfTraceReplay *out, const void *bytes,
    size_t length, const TfTraceIoLimits *requested, const TfAllocator *custom) {
    TfTraceIoLimits limits;
    TfAllocator allocator;
    TfTraceReplay work = {0};
    Validation validation = {.replay = &work};
    Parser p = {bytes, length, 0, false};
    TfTraceIoStatus status;
    if (!out) return TF_TRACE_IO_INVALID;
    *out = (TfTraceReplay){0};
    if (!configuration(&limits, &allocator, requested, custom) || (!bytes && length))
        return TF_TRACE_IO_INVALID;
    if (length > limits.max_bytes) return TF_TRACE_IO_LIMIT;
    if (!header(&p, &work, &limits)) return TF_TRACE_IO_SYNTAX;
    status = prepare(&work, work.declared_count, allocator);
    if (status != TF_TRACE_IO_OK) return status;
    if (work.declared_count) {
        if (work.declared_count > SIZE_MAX / sizeof(TfTraceRecord)) {
            status = TF_TRACE_IO_LIMIT;
            goto cleanup;
        }
        work.recorder.records = allocator.allocate(allocator.context,
            work.declared_count * sizeof(TfTraceRecord));
        if (!work.recorder.records) { status = TF_TRACE_IO_ALLOCATION; goto cleanup; }
        work.recorder.capacity = work.declared_count;
    }
    while (p.at < p.length) {
        TfTraceRecord record = {0};
        if (p.bytes[p.at] == 'E') {
            if (!literal(&p, "END\n")) {
                if (p.incomplete) break;
                status = TF_TRACE_IO_SYNTAX;
                goto cleanup;
            }
            if (p.at != p.length || work.recorder.count != work.declared_count) {
                status = TF_TRACE_IO_SYNTAX;
                goto cleanup;
            }
            work.complete_file = true;
            break;
        }
        if (work.recorder.count == work.declared_count) {
            status = TF_TRACE_IO_SYNTAX;
            goto cleanup;
        }
        if (!record_line(&p, &record)) {
            if (p.incomplete) break;
            status = TF_TRACE_IO_SYNTAX;
            goto cleanup;
        }
        status = validate_record(&validation, &record, work.recorder.count);
        if (status != TF_TRACE_IO_OK) goto cleanup;
        work.recorder.records[work.recorder.count++] = record;
    }
    if (validation.clipped > work.recorder.clipped_previews ||
        (work.recorder.count == work.declared_count &&
         validation.clipped != work.recorder.clipped_previews)) {
        status = TF_TRACE_IO_INVALID;
        goto cleanup;
    }
    outcomes(&work);
    *out = work;
    return TF_TRACE_IO_OK;
cleanup:
    tf_trace_replay_destroy(&work);
    return status;
}

TfTraceIoStatus tf_trace_replay_read(TfTraceReplay *out, FILE *stream,
    const TfTraceIoLimits *requested, const TfAllocator *custom) {
    TfTraceIoLimits limits;
    TfAllocator allocator;
    unsigned char *bytes;
    size_t count;
    TfTraceIoStatus status;
    if (!out) return TF_TRACE_IO_INVALID;
    *out = (TfTraceReplay){0};
    if (!configuration(&limits, &allocator, requested, custom) || !stream)
        return TF_TRACE_IO_INVALID;
    if (ferror(stream)) return TF_TRACE_IO_STREAM;
    bytes = allocator.allocate(allocator.context, limits.max_bytes + 1);
    if (!bytes) return TF_TRACE_IO_ALLOCATION;
    count = fread(bytes, 1, limits.max_bytes + 1, stream);
    if (ferror(stream)) status = TF_TRACE_IO_STREAM;
    else if (count > limits.max_bytes) status = TF_TRACE_IO_LIMIT;
    else status = tf_trace_replay_bytes(out, bytes, count, &limits, &allocator);
    allocator.deallocate(allocator.context, bytes);
    return status;
}
