#include "ternfold/diagnostics.h"
#include "internal.h"

#include <string.h>

struct TfDiagnosticBag {
    TfAllocator allocator;
    TfDiagnosticRecord **records;
    size_t count, capacity, cap, suppressed, revision;
    bool has_errors;
};

static void drop(TfAllocator a, const void *p) {
    if (p != NULL) a.deallocate(a.context, (void *)p);
}
bool tf_diagnostic_code_registered(const char *code) {
    unsigned number;
    if (code == NULL || strlen(code) != 4 || (code[0] != 'E' && code[0] != 'R') ||
        code[1] < '0' || code[1] > '9' || code[2] < '0' || code[2] > '9' ||
        code[3] < '0' || code[3] > '9') return false;
    number = (unsigned)(code[1]-'0')*100u + (unsigned)(code[2]-'0')*10u + (unsigned)(code[3]-'0');
    if (code[0] == 'R') return number >= 1 && number <= 6;
    return (number >= 1 && number <= 3) || (number >= 101 && number <= 105) ||
        (number >= 110 && number <= 112) || (number >= 201 && number <= 208) ||
        (number >= 301 && number <= 307) || (number >= 310 && number <= 312) ||
        (number >= 401 && number <= 407);
}
static bool valid_text(TfDiagnosticText text) {
    size_t at = 0, end;
    uint32_t scalar;
    if (text.length == SIZE_MAX || (text.data == NULL && text.length != 0)) return false;
    while (at < text.length) {
        if (!tf_decode((const unsigned char *)text.data, text.length, at, &scalar, &end)) return false;
        at = end;
    }
    return true;
}
static bool valid_span(TfSpan span) {
    TfSpan checked;
    if (span.source == NULL || span.source->name == NULL || span.source->bytes == NULL ||
        span.start > span.end || span.end > span.source->length) return false;
    return !span.source->valid || tf_source_span(span.source, span.start, span.end, &checked);
}
static bool valid_record(const TfDiagnosticRecord *r) {
    size_t i;
    bool primary = false;
    if (r == NULL || !tf_diagnostic_code_registered(r->code) ||
        (r->severity != TF_DIAG_ERROR && r->severity != TF_DIAG_WARNING && r->severity != TF_DIAG_NOTE) ||
        !valid_text(r->message) ||
        (r->label_count != 0 && r->labels == NULL) || r->label_count > SIZE_MAX/sizeof(*r->labels) ||
        (r->note_count != 0 && r->notes == NULL) || r->note_count > SIZE_MAX/sizeof(*r->notes) ||
        (r->help_count != 0 && r->help == NULL) || r->help_count > SIZE_MAX/sizeof(*r->help) ||
        (r->edit_count != 0 && r->edits == NULL) || r->edit_count > SIZE_MAX/sizeof(*r->edits)) return false;
    for (i = 0; i < r->label_count; ++i) {
        if (!valid_span(r->labels[i].span) || !valid_text(r->labels[i].message)) return false;
        primary = primary || r->labels[i].primary;
    }
    if (r->label_count != 0 && !primary) return false;
    for (i = 0; i < r->note_count; ++i) if (!valid_text(r->notes[i])) return false;
    for (i = 0; i < r->help_count; ++i) if (!valid_text(r->help[i])) return false;
    for (i = 0; i < r->edit_count; ++i)
        if (!valid_span(r->edits[i].span) || !valid_text(r->edits[i].replacement)) return false;
    return true;
}
static bool clone_text(TfAllocator a, TfDiagnosticText text, TfDiagnosticText *out) {
    char *bytes = a.allocate(a.context, text.length + 1);
    if (bytes == NULL) return false;
    if (text.length != 0) memcpy(bytes, text.data, text.length);
    bytes[text.length] = '\0';
    *out = (TfDiagnosticText){bytes,text.length};
    return true;
}
static void destroy_record(TfAllocator a, TfDiagnosticRecord *r) {
    size_t i;
    if (r == NULL) return;
    drop(a, r->code); drop(a, r->message.data);
    for (i = 0; i < r->label_count; ++i) drop(a, r->labels[i].message.data);
    for (i = 0; i < r->note_count; ++i) drop(a, r->notes[i].data);
    for (i = 0; i < r->help_count; ++i) drop(a, r->help[i].data);
    for (i = 0; i < r->edit_count; ++i) drop(a, r->edits[i].replacement.data);
    drop(a, r->labels); drop(a, r->notes); drop(a, r->help); drop(a, r->edits); drop(a, r);
}
static void *array(TfAllocator a, size_t count, size_t width) {
    void *p;
    if (count == 0) return NULL;
    p = a.allocate(a.context, count * width);
    if (p != NULL) memset(p, 0, count * width);
    return p;
}
static TfDiagnosticRecord *clone_record(TfAllocator a, const TfDiagnosticRecord *r) {
    TfDiagnosticRecord *fresh = a.allocate(a.context, sizeof(*fresh));
    TfDiagnosticText code = {0};
    TfDiagnosticLabel *labels;
    TfDiagnosticText *notes, *help;
    TfDiagnosticEdit *edits;
    size_t i;
    if (fresh == NULL) return NULL;
    *fresh = (TfDiagnosticRecord){0};
    fresh->severity = r->severity;
    if (!clone_text(a, (TfDiagnosticText){r->code,4}, &code)) goto fail;
    fresh->code = code.data;
    if (!clone_text(a, r->message, &fresh->message)) goto fail;
    labels = array(a, r->label_count, sizeof(*labels));
    fresh->labels = labels;
    if (r->label_count != 0 && labels == NULL) goto fail;
    fresh->label_count = r->label_count;
    for (i = 0; i < r->label_count; ++i) {
        labels[i].span = r->labels[i].span; labels[i].primary = r->labels[i].primary;
        if (!clone_text(a, r->labels[i].message, &labels[i].message)) goto fail;
    }
    notes = array(a, r->note_count, sizeof(*notes)); fresh->notes = notes;
    if (r->note_count != 0 && notes == NULL) goto fail;
    fresh->note_count = r->note_count;
    for (i = 0; i < r->note_count; ++i) if (!clone_text(a, r->notes[i], &notes[i])) goto fail;
    help = array(a, r->help_count, sizeof(*help)); fresh->help = help;
    if (r->help_count != 0 && help == NULL) goto fail;
    fresh->help_count = r->help_count;
    for (i = 0; i < r->help_count; ++i) if (!clone_text(a, r->help[i], &help[i])) goto fail;
    edits = array(a, r->edit_count, sizeof(*edits)); fresh->edits = edits;
    if (r->edit_count != 0 && edits == NULL) goto fail;
    fresh->edit_count = r->edit_count;
    for (i = 0; i < r->edit_count; ++i) {
        edits[i].span = r->edits[i].span;
        if (!clone_text(a, r->edits[i].replacement, &edits[i].replacement)) goto fail;
    }
    return fresh;
fail:
    destroy_record(a, fresh);
    return NULL;
}
TfStatus tf_diagnostic_bag_create(TfDiagnosticBag **out, size_t cap, const TfAllocator *allocator) {
    TfAllocator a = tf_allocator(allocator);
    TfDiagnosticBag *bag;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (!tf_allocator_valid(a)) return TF_HOST_ERROR;
    bag = a.allocate(a.context, sizeof(*bag));
    if (bag == NULL) return TF_HOST_ERROR;
    *bag = (TfDiagnosticBag){0}; bag->allocator = a; bag->cap = cap;
    *out = bag;
    return TF_OK;
}
void tf_diagnostic_bag_destroy(TfDiagnosticBag *bag) {
    size_t i;
    TfAllocator a;
    if (bag == NULL) return;
    a = bag->allocator;
    for (i = 0; i < bag->count; ++i) destroy_record(a, bag->records[i]);
    drop(a, bag->records); drop(a, bag);
}
TfStatus tf_diagnostic_bag_add(TfDiagnosticBag *bag, const TfDiagnosticRecord *record) {
    TfDiagnosticRecord *fresh;
    void *storage;
    if (bag == NULL || !valid_record(record) || bag->revision == SIZE_MAX) return TF_HOST_ERROR;
    if (bag->count == bag->cap) {
        if (bag->suppressed == SIZE_MAX) return TF_HOST_ERROR;
        ++bag->suppressed;
    } else {
        if (bag->count == SIZE_MAX) return TF_HOST_ERROR;
        fresh = clone_record(bag->allocator, record);
        if (fresh == NULL) return TF_HOST_ERROR;
        storage = bag->records;
        if (!tf_grow(&storage, &bag->capacity, bag->count + 1, sizeof(*bag->records), bag->allocator)) {
            destroy_record(bag->allocator, fresh); return TF_HOST_ERROR;
        }
        bag->records = storage; bag->records[bag->count++] = fresh;
    }
    bag->has_errors = bag->has_errors || record->severity == TF_DIAG_ERROR;
    ++bag->revision;
    return TF_OK;
}
size_t tf_diagnostic_bag_count(const TfDiagnosticBag *bag) { return bag != NULL ? bag->count : 0; }
size_t tf_diagnostic_bag_suppressed(const TfDiagnosticBag *bag) { return bag != NULL ? bag->suppressed : 0; }
bool tf_diagnostic_bag_has_errors(const TfDiagnosticBag *bag) { return bag != NULL && bag->has_errors; }
const TfDiagnosticRecord *tf_diagnostic_bag_get(const TfDiagnosticBag *bag, size_t index) {
    return bag != NULL && index < bag->count ? bag->records[index] : NULL;
}
TfStatus tf_diagnostic_phase_valid(TfDiagnosticPhaseResult *out, const void *payload,
                                   const TfDiagnosticBag *bag) {
    if (out == NULL) return TF_HOST_ERROR;
    *out = (TfDiagnosticPhaseResult){0};
    if (payload == NULL || bag == NULL || bag->has_errors) return TF_HOST_ERROR;
    *out = (TfDiagnosticPhaseResult){payload,bag,bag->revision,TF_DIAG_PHASE_VALID};
    return TF_OK;
}
TfStatus tf_diagnostic_phase_error(TfDiagnosticPhaseResult *out, const TfDiagnosticBag *bag) {
    if (out == NULL) return TF_HOST_ERROR;
    *out = (TfDiagnosticPhaseResult){0};
    if (bag == NULL || !bag->has_errors) return TF_HOST_ERROR;
    *out = (TfDiagnosticPhaseResult){NULL,bag,bag->revision,TF_DIAG_PHASE_ERROR};
    return TF_OK;
}
TfDiagnosticPhaseKind tf_diagnostic_phase_kind(const TfDiagnosticPhaseResult *result) {
    if (result == NULL || result->private_bag == NULL ||
        result->private_revision != result->private_bag->revision) return TF_DIAG_PHASE_INVALID;
    if (result->private_kind == TF_DIAG_PHASE_VALID && result->private_payload != NULL &&
        !result->private_bag->has_errors) return TF_DIAG_PHASE_VALID;
    if (result->private_kind == TF_DIAG_PHASE_ERROR && result->private_payload == NULL &&
        result->private_bag->has_errors) return TF_DIAG_PHASE_ERROR;
    return TF_DIAG_PHASE_INVALID;
}
const void *tf_diagnostic_phase_payload(const TfDiagnosticPhaseResult *result) {
    return tf_diagnostic_phase_kind(result) == TF_DIAG_PHASE_VALID ? result->private_payload : NULL;
}
const TfDiagnosticBag *tf_diagnostic_phase_bag(const TfDiagnosticPhaseResult *result) {
    return tf_diagnostic_phase_kind(result) != TF_DIAG_PHASE_INVALID ? result->private_bag : NULL;
}
static TfStatus add_legacy(TfDiagnosticBag *bag, TfDiagnostic diagnostic) {
    const char *message = tf_error_message(diagnostic.code);
    TfDiagnosticLabel label = {diagnostic.span,true,{NULL,0}};
    TfDiagnosticRecord record = {0};
    record.code = tf_error_code(diagnostic.code); record.severity = TF_DIAG_ERROR;
    record.message = (TfDiagnosticText){message,strlen(message)};
    record.labels = &label; record.label_count = 1;
    return tf_diagnostic_bag_add(bag, &record);
}
TfStatus tf_diagnostic_bag_source(TfDiagnosticBag *bag, const TfSource *source) {
    if (bag == NULL || source == NULL || source->name == NULL || source->bytes == NULL) return TF_HOST_ERROR;
    if (source->valid) return TF_OK;
    if (source->error.span.source != source ||
        (source->error.code != TF_E001 && source->error.code != TF_E002 && source->error.code != TF_E003))
        return TF_HOST_ERROR;
    return add_legacy(bag, source->error);
}
TfStatus tf_diagnostic_bag_lexer(TfDiagnosticBag *bag, const TfLexResult *result) {
    size_t old_count, old_suppressed, old_revision, i;
    bool old_errors;
    if (bag == NULL || result == NULL || result->source == NULL || !result->source->valid ||
        (result->diagnostic_count != 0 && result->diagnostics == NULL) ||
        result->diagnostic_count > SIZE_MAX/sizeof(*result->diagnostics) ||
        ((!result->has_errors) && (result->diagnostic_count != 0 || result->suppressed != 0))) return TF_HOST_ERROR;
    old_count = bag->count; old_suppressed = bag->suppressed; old_errors = bag->has_errors;
    old_revision = bag->revision;
    for (i = 0; i < result->diagnostic_count; ++i) {
        TfDiagnostic d = result->diagnostics[i];
        if (d.span.source != result->source || d.code < TF_E101 || d.code > TF_E105 ||
            add_legacy(bag, d) != TF_OK) goto fail;
    }
    if (result->suppressed > SIZE_MAX - bag->suppressed) goto fail;
    if ((result->suppressed != 0 || (result->has_errors && !bag->has_errors)) && bag->revision == SIZE_MAX) goto fail;
    if (result->suppressed != 0 || (result->has_errors && !bag->has_errors)) ++bag->revision;
    bag->suppressed += result->suppressed;
    bag->has_errors = bag->has_errors || result->has_errors;
    return TF_OK;
fail:
    while (bag->count > old_count) destroy_record(bag->allocator, bag->records[--bag->count]);
    bag->suppressed = old_suppressed; bag->has_errors = old_errors;
    bag->revision = old_revision;
    return TF_HOST_ERROR;
}

static bool put(FILE *stream, const char *text) { return fputs(text, stream) != EOF; }
/* Escapes strict UTF-8 text without truncating at NUL. Invalid name bytes and
 * raw invalid-source bytes are represented reversibly with byte escapes. */
static bool escaped(FILE *stream, const unsigned char *bytes, size_t length, bool raw, bool quoted) {
    size_t at = 0;
    if (quoted && fputc('"', stream) == EOF) return false;
    while (at < length) {
        unsigned char c = bytes[at];
        size_t end;
        uint32_t scalar;
        if (c == '"' || c == '\\') {
            if (fputc('\\', stream) == EOF || fputc(c, stream) == EOF) return false;
            ++at;
        } else if (c < 0x20 || c == 0x7f || (raw && c >= 0x80) ||
                   !tf_decode(bytes, length, at, &scalar, &end)) {
            if (fprintf(stream, "\\u%04x", (unsigned)c) < 0) return false;
            ++at;
        } else {
            if (fwrite(bytes + at, 1, end - at, stream) != end - at) return false;
            at = end;
        }
    }
    return !quoted || fputc('"', stream) != EOF;
}
static bool text_view(FILE *stream, TfDiagnosticText text, bool quoted) {
    return escaped(stream, (const unsigned char *)text.data, text.length, false, quoted);
}
static bool name(FILE *stream, const TfSource *source) {
    return escaped(stream, (const unsigned char *)source->name, strlen(source->name), false, true);
}
static const char *severity(TfDiagnosticSeverity s) {
    return s == TF_DIAG_ERROR ? "error" : s == TF_DIAG_WARNING ? "warning" : "note";
}
static bool json_location(FILE *stream, const TfSource *source, size_t byte) {
    TfLocation l;
    if (!source->valid) return put(stream, "null");
    if (!tf_source_location(source, byte, &l)) return false;
    return fprintf(stream, "{\"line\":%zu,\"column\":%zu,\"display_column\":%zu}",
                   l.line,l.column,l.display_column) >= 0;
}
static bool json_span(FILE *stream, TfSpan span) {
    return put(stream,"{\"source\":") && name(stream,span.source) &&
        fprintf(stream,",\"start_byte\":%zu,\"end_byte\":%zu,\"start\":",span.start,span.end) >= 0 &&
        json_location(stream,span.source,span.start) && put(stream,",\"end\":") &&
        json_location(stream,span.source,span.end) && put(stream,",\"source_text\":") &&
        escaped(stream,span.source->bytes+span.start,span.end-span.start,!span.source->valid,true) &&
        put(stream,span.source->valid ? ",\"raw\":false}" : ",\"raw\":true}");
}
static bool json_texts(FILE *stream, const TfDiagnosticText *texts, size_t count) {
    size_t i;
    if (!put(stream,"[")) return false;
    for (i = 0; i < count; ++i) {
        if ((i != 0 && !put(stream,",")) || !text_view(stream,texts[i],true)) return false;
    }
    return put(stream,"]");
}
static bool json_record(FILE *stream, const TfDiagnosticRecord *r) {
    size_t i;
    if (fprintf(stream,"{\"code\":\"%s\",\"severity\":\"%s\",\"message\":",r->code,severity(r->severity)) < 0 ||
        !text_view(stream,r->message,true) || !put(stream,",\"labels\":[")) return false;
    for (i = 0; i < r->label_count; ++i) {
        if ((i != 0 && !put(stream,",")) || !put(stream,r->labels[i].primary ?
            "{\"primary\":true,\"message\":" : "{\"primary\":false,\"message\":") ||
            !text_view(stream,r->labels[i].message,true) || !put(stream,",\"span\":") ||
            !json_span(stream,r->labels[i].span) || !put(stream,"}")) return false;
    }
    if (!put(stream,"],\"notes\":") || !json_texts(stream,r->notes,r->note_count) ||
        !put(stream,",\"help\":") || !json_texts(stream,r->help,r->help_count) || !put(stream,",\"edits\":[")) return false;
    for (i = 0; i < r->edit_count; ++i) {
        if ((i != 0 && !put(stream,",")) || !put(stream,"{\"replacement\":") ||
            !text_view(stream,r->edits[i].replacement,true) || !put(stream,",\"span\":") ||
            !json_span(stream,r->edits[i].span) || !put(stream,"}")) return false;
    }
    return put(stream,"]}");
}
static bool spaces(FILE *stream, size_t count, int c) {
    size_t i;
    for (i = 0; i < count; ++i) if (fputc(c,stream) == EOF) return false;
    return true;
}
/* Widths below describe the escaped snippet, separately from source coordinates.
 * TAB stops use the source's scalar display column, controls occupy 6 cells. */
static bool snippet_prefix(FILE *stream, const TfSource *s, size_t start, size_t end,
                           bool write, size_t *width) {
    size_t at = start, display = 1, total = 0;
    while (at < end) {
        uint32_t scalar;
        size_t next, step, printed;
        if (!tf_decode(s->bytes,s->length,at,&scalar,&next)) return false;
        step = scalar == '\t' ? 4 - (display-1)%4 : 1;
        printed = scalar == '\t' ? step : (scalar < 0x20 || scalar == 0x7f) ? 6 :
            (scalar == '"' || scalar == '\\') ? 2 : 1;
        if (total > SIZE_MAX - printed || display > SIZE_MAX - step) return false;
        if (write) {
            if (scalar == '\t') { if (!spaces(stream,step,' ')) return false; }
            else if (!escaped(stream,s->bytes+at,next-at,false,false)) return false;
        }
        total += printed; display += step; at = next;
    }
    *width = total;
    return true;
}
static bool text_span_header(FILE *stream, TfSpan span) {
    TfLocation start, end;
    if (!name(stream,span.source)) return false;
    if (span.source->valid) {
        if (!tf_source_location(span.source,span.start,&start) || !tf_source_location(span.source,span.end,&end)) return false;
        if (fprintf(stream,":%zu:%zu..%zu:%zu (display %zu..%zu)",start.line,start.column,
                    end.line,end.column,start.display_column,end.display_column) < 0) return false;
    }
    return fprintf(stream," [bytes %zu..%zu)",span.start,span.end) >= 0;
}
static bool text_label(FILE *stream, TfDiagnosticLabel label) {
    const TfSource *s = label.span.source;
    TfLocation first, last;
    size_t line;
    if (!put(stream,label.primary ? "  --> " : "  ::: ") || !text_span_header(stream,label.span) ||
        !put(stream,": ") || !text_view(stream,label.message,false) || !put(stream,"\n")) return false;
    if (!s->valid) return put(stream,"  raw | ") &&
        escaped(stream,s->bytes+label.span.start,label.span.end-label.span.start,true,true) && put(stream,"\n");
    if (!tf_source_location(s,label.span.start,&first) || !tf_source_location(s,label.span.end,&last)) return false;
    /* A nonempty half-open label ending at the next line start excludes it. */
    if (label.span.end > label.span.start && last.column == 1 && last.line > first.line) --last.line;
    for (line = first.line; line <= last.line; ++line) {
        size_t begin, end, mark_start, mark_end, left, right, unused;
        size_t digits = 1, number = line;
        while (number >= 10) { number /= 10; ++digits; }
        begin = s->scalar_offsets[s->line_scalars[line-1]];
        end = line < s->line_count ? s->scalar_offsets[s->line_scalars[line]] : s->length;
        if (end > begin && s->bytes[end-1] == '\n') --end;
        if (end > begin && s->bytes[end-1] == '\r') --end;
        mark_start = label.span.start > begin ? label.span.start : begin;
        mark_end = label.span.end < end ? label.span.end : end;
        if (mark_start > end) mark_start = end;
        if (mark_end < mark_start) mark_end = mark_start;
        if (fprintf(stream,"  %zu | ",line) < 0 || !snippet_prefix(stream,s,begin,end,true,&unused) ||
            !put(stream,"\n  ") || !spaces(stream,digits,' ') || !put(stream," | ") ||
            !snippet_prefix(stream,s,begin,mark_start,false,&left) ||
            !snippet_prefix(stream,s,begin,mark_end,false,&right) || !spaces(stream,left,' ') ||
            !spaces(stream,right > left ? right-left : 1,label.primary ? '^' : '-') || !put(stream,"\n")) return false;
    }
    return true;
}
static bool text_record(FILE *stream, const TfDiagnosticRecord *r, bool color) {
    size_t i;
    if (color && !put(stream,r->severity == TF_DIAG_ERROR ? "\x1b[31m" : r->severity == TF_DIAG_WARNING ? "\x1b[33m" : "\x1b[36m")) return false;
    if (fprintf(stream,"%s[%s]",severity(r->severity),r->code) < 0 || (color && !put(stream,"\x1b[0m")) ||
        !put(stream,": ") || !text_view(stream,r->message,false) || !put(stream,"\n")) return false;
    for (i = 0; i < r->label_count; ++i) if (!text_label(stream,r->labels[i])) return false;
    for (i = 0; i < r->note_count; ++i)
        if (!put(stream,"  note: ") || !text_view(stream,r->notes[i],false) || !put(stream,"\n")) return false;
    for (i = 0; i < r->help_count; ++i)
        if (!put(stream,"  help: ") || !text_view(stream,r->help[i],false) || !put(stream,"\n")) return false;
    for (i = 0; i < r->edit_count; ++i)
        if (!put(stream,"  edit: ") || !text_span_header(stream,r->edits[i].span) || !put(stream," => ") ||
            !text_view(stream,r->edits[i].replacement,true) || !put(stream,"\n")) return false;
    return true;
}
TfStatus tf_diagnostic_bag_write(FILE *stream, const TfDiagnosticBag *bag,
                                TfDiagnosticFormat format, TfDiagnosticColor color) {
    size_t i;
    if (stream == NULL || bag == NULL || (format != TF_DIAG_TEXT && format != TF_DIAG_JSON) ||
        (color != TF_DIAG_COLOR_NEVER && color != TF_DIAG_COLOR_ALWAYS) || ferror(stream)) return TF_HOST_ERROR;
    if (format == TF_DIAG_JSON) {
        if (!put(stream,"{\"schema_version\":2,\"diagnostics\":[")) return TF_HOST_ERROR;
        for (i = 0; i < bag->count; ++i)
            if ((i != 0 && !put(stream,",")) || !json_record(stream,bag->records[i])) return TF_HOST_ERROR;
        if (fprintf(stream,"],\"suppressed\":%zu,\"has_errors\":%s}\n",bag->suppressed,
                    bag->has_errors ? "true" : "false") < 0) return TF_HOST_ERROR;
    } else {
        for (i = 0; i < bag->count; ++i)
            if (!text_record(stream,bag->records[i],color == TF_DIAG_COLOR_ALWAYS)) return TF_HOST_ERROR;
        if (bag->suppressed != 0 && fprintf(stream,"%zu further diagnostics suppressed\n",bag->suppressed) < 0) return TF_HOST_ERROR;
    }
    return ferror(stream) ? TF_HOST_ERROR : TF_OK;
}
