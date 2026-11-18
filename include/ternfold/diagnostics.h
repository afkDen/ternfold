#ifndef TERNFOLD_DIAGNOSTICS_H
#define TERNFOLD_DIAGNOSTICS_H

#include "ternfold/lexer.h"

typedef struct TfDiagnosticBag TfDiagnosticBag;
typedef struct { const char *data; size_t length; } TfDiagnosticText;
typedef enum { TF_DIAG_ERROR, TF_DIAG_WARNING, TF_DIAG_NOTE } TfDiagnosticSeverity;
typedef struct {
    TfSpan span;
    bool primary;
    TfDiagnosticText message;
} TfDiagnosticLabel;
typedef struct { TfSpan span; TfDiagnosticText replacement; } TfDiagnosticEdit;
typedef struct {
    const char *code; /* registered E### or R###, exactly four bytes + NUL */
    TfDiagnosticSeverity severity;
    TfDiagnosticText message;
    const TfDiagnosticLabel *labels;
    size_t label_count;
    const TfDiagnosticText *notes;
    size_t note_count;
    const TfDiagnosticText *help;
    size_t help_count;
    const TfDiagnosticEdit *edits;
    size_t edit_count;
} TfDiagnosticRecord;
typedef enum { TF_DIAG_TEXT, TF_DIAG_JSON } TfDiagnosticFormat;
typedef enum { TF_DIAG_COLOR_NEVER, TF_DIAG_COLOR_ALWAYS } TfDiagnosticColor;

typedef enum {
    TF_DIAG_PHASE_INVALID = 0, TF_DIAG_PHASE_VALID, TF_DIAG_PHASE_ERROR
} TfDiagnosticPhaseKind;
/* Value handle, containing only borrows. Fields are private by contract: use
 * factories/accessors, never initialize or change fields directly except {0}.
 * Copying this handle copies borrows; it does not extend any lifetime.
 * Payload and bag (and their source/allocator dependencies) must outlive every
 * use of the handle. It owns nothing and needs no destruction. Valid requires a
 * non-NULL phase payload and no diagnosed errors, including suppressed errors;
 * warnings/notes alone are permitted. Error requires diagnosed errors and has
 * no payload. This marks a phase outcome, not verified Core or executability.
 *
 * A bag should remain frozen while its result is consumed. Successful diagnostic
 * mutation invalidates existing handles, including suppression at cap zero;
 * kind then returns INVALID and both borrow accessors return NULL. Failed
 * transactional mutations do not invalidate handles. No-op adapters on a clean
 * source/result preserve handles. Access after bag/payload destruction is never
 * permitted; revision checking does not detect expired object lifetimes.
 * Factories overwrite *out with {0} on failure; NULL out returns HOST_ERROR.
 */
typedef struct {
    const void *private_payload;
    const TfDiagnosticBag *private_bag;
    size_t private_revision;
    TfDiagnosticPhaseKind private_kind;
} TfDiagnosticPhaseResult;
TfStatus tf_diagnostic_phase_valid(TfDiagnosticPhaseResult *out, const void *payload,
                                   const TfDiagnosticBag *bag);
TfStatus tf_diagnostic_phase_error(TfDiagnosticPhaseResult *out, const TfDiagnosticBag *bag);
TfDiagnosticPhaseKind tf_diagnostic_phase_kind(const TfDiagnosticPhaseResult *result);
const void *tf_diagnostic_phase_payload(const TfDiagnosticPhaseResult *result);
const TfDiagnosticBag *tf_diagnostic_phase_bag(const TfDiagnosticPhaseResult *result);

/* Bag deep-copies every string/vector (including embedded NUL in text views),
 * borrowing only stable TfSource objects. Text must be strict UTF-8; NULL/0 is
 * empty, NULL/nonzero is invalid. Source names follow the existing source API;
 * rendering escapes arbitrary name bytes. Do not mutate/destroy borrowed
 * sources until the bag and its borrowers are destroyed. Allocator/context must
 * outlive the bag. NULL allocator selects malloc/free. Not thread safe.
 * get() borrows a read-only record, stable across adds and failed adds; expires
 * at bag destruction. A zeroed NULL bag may be destroyed; live owners not copied.
 *
 * create requires a fresh output; failure sets *out=NULL. All other mutators
 * return TF_HOST_ERROR for invalid arguments, overflow or OOM, leaving records,
 * suppression and error state unchanged. add validates even when cap is full.
 * cap counts retained records of all severities; excess records increment
 * suppressed. has_errors records errors even at cap zero. Adapter calls are
 * transactional as a whole; lexer suppressed errors count toward suppression.
 * Labels/edits may cross files, overlap or be zero-width/EOF. Valid-source
 * boundaries must pass tf_source_span; invalid-source ranges use raw bounded
 * bytes and never receive invented coordinates. At least one primary label is
 * required if labels are present. Label-less global diagnostics are supported.
 */
TfStatus tf_diagnostic_bag_create(TfDiagnosticBag **out, size_t cap,
                                 const TfAllocator *allocator);
void tf_diagnostic_bag_destroy(TfDiagnosticBag *bag);
TfStatus tf_diagnostic_bag_add(TfDiagnosticBag *bag, const TfDiagnosticRecord *record);
size_t tf_diagnostic_bag_count(const TfDiagnosticBag *bag);
size_t tf_diagnostic_bag_suppressed(const TfDiagnosticBag *bag);
bool tf_diagnostic_bag_has_errors(const TfDiagnosticBag *bag);
const TfDiagnosticRecord *tf_diagnostic_bag_get(const TfDiagnosticBag *bag, size_t index);
bool tf_diagnostic_code_registered(const char *code);
TfStatus tf_diagnostic_bag_source(TfDiagnosticBag *bag, const TfSource *source);
TfStatus tf_diagnostic_bag_lexer(TfDiagnosticBag *bag, const TfLexResult *result);

/* Writes LF bytes to a borrowed FILE. Caller selects binary mode, flushes and
 * closes; buffered failures detected only at caller flush remain caller-owned.
 * TF_HOST_ERROR reports invalid options/arguments or observed write errors;
 * stream output may be partial on failure. Rendering never mutates bag/sources.
 * No ambient terminal detection: callers explicitly select NEVER or ALWAYS.
 * JSON ignores color and never emits ANSI sequences.
 *
 * JSON v2 exact top-level fields: schema_version(2), diagnostics(array),
 * suppressed(integer), has_errors(bool). Records: code,severity,message,labels,
 * notes,help,edits. Label: primary,message,span. Edit: replacement,span.
 * Span: source(name),start_byte,end_byte,start,end,source_text,raw.
 * start/end are null for invalid sources; otherwise objects with line,column,
 * display_column (all one-based; column counts scalars, tabs use width 4).
 * Byte ranges are original half-open offsets. source_text is the exact selected
 * UTF-8 slice; invalid-source bytes encode independently as \u00XX, raw=true.
 * Strings escape controls including NUL; invalid name bytes also use \u00XX.
 * Array order is insertion order; labels render separately, preserving overlaps.
 * Text prints every touched line, expands tabs at width 4, treats each Unicode
 * scalar as one display cell (no terminal wcwidth/grapheme claim), escapes
 * controls, marks primary with ^ and secondary with -, zero-width with one mark.
 */
TfStatus tf_diagnostic_bag_write(FILE *stream, const TfDiagnosticBag *bag,
                                TfDiagnosticFormat format, TfDiagnosticColor color);

#endif
