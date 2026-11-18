#ifndef TERNFOLD_EDITOR_H
#define TERNFOLD_EDITOR_H
#include "ternfold/check.h"
#include <stdint.h>

typedef struct TfEditorSnapshot TfEditorSnapshot;
typedef struct { size_t line, character; } TfEditorPosition; /* zero-based UTF-16 */
typedef struct { TfEditorPosition start, end; } TfEditorRange;

/* Reject CRLF interior, non-scalar byte offsets and UTF-16 surrogate interior.
 * Line-end positions precede CR/LF; EOF on final empty line is valid. */
bool tf_editor_position(const TfSource *source, size_t byte, TfEditorPosition *out);
bool tf_editor_byte(const TfSource *source, TfEditorPosition position, size_t *out);

/* Fresh owning immutable snapshot; same compiler check phases as `check`.
 * root is a borrowed native directory during construction (NULL = cwd).
 * Imports resolve from root; this bounded virtual document is not a file
 * overlay. TF_OK includes diagnosed errors. Invalid UTF-8 retains source
 * diagnostics. HOST_ERROR clears *out; all objects use supplied allocator.
 * Snapshot/allocator context must outlive diagnostics, source and type borrows.
 * Versions are nonnegative; cancelled publication never exposes hover/results.
 * Cancellation changes publication state only; no concurrent use is allowed. */
TfStatus tf_editor_snapshot_create(TfEditorSnapshot **out, const char *name,
    const void *bytes, size_t length, int64_t version, const char *root,
    const TfAllocator *allocator);
void tf_editor_snapshot_destroy(TfEditorSnapshot *snapshot);
const TfSource *tf_editor_source(const TfEditorSnapshot *snapshot);
const TfDiagnosticBag *tf_editor_diagnostics(const TfEditorSnapshot *snapshot, int64_t current_version);
TfConcreteTypeId tf_editor_hover(const TfEditorSnapshot *snapshot, int64_t current_version,
                                TfEditorPosition position, TfEditorRange *range);
const TfTypeStore *tf_editor_types(const TfEditorSnapshot *snapshot);
void tf_editor_cancel(TfEditorSnapshot *snapshot);

/* Blocking, single-document, full-sync LSP subset; binary borrowed streams.
 * Caller owns binary mode, streams and final flush. No ambient stdio/logging.
 * Bounds: 1 MiB frame/source, 4096 header bytes, 1024 JSON tokens, depth 32,
 * 2 MiB response. EOF between frames returns OK; truncated/malformed framing,
 * I/O/OOM/compiler host errors return HOST_ERROR. No subprocesses/downloads.
 * See docs/editor-tooling.md for lifecycle/cancellation/import limitations. */
TfStatus tf_editor_serve(FILE *input, FILE *output, const char *root,
                         const TfAllocator *allocator);
#endif
