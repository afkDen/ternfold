#ifndef TERNFOLD_TRACE_ARTIFACT_H
#define TERNFOLD_TRACE_ARTIFACT_H
#include "ternfold/trace_io.h"
/* Exact source map copied from the verified Core used by this execution.
 * The companion is descriptive metadata, not authenticated provenance. */
typedef struct tf_generated_origin {
    const unsigned char *path;
    size_t path_length, start, end;
    size_t start_line, start_column, end_line, end_column;
} TfTraceOrigin;
/* Writes PATH and PATH.origins, requiring both to be new files. No existing
 * file is overwritten. Failure can leave a newly created partial artifact.
 * Serializing/writing happens only after execution; never call from capture. */
TfTraceIoStatus tf_trace_artifact_write(const char *path, const TfTraceRecorder *recorder,
                                       const TfTraceOrigin *origins, size_t count);
/* Inspect records without executing code. Prints escaped previews and, when
 * present, the bounded companion source map. Missing metadata is explicit. */
TfTraceIoStatus tf_trace_artifact_replay(FILE *output, const char *path);
#endif
