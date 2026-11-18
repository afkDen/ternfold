#ifndef TERNFOLD_EXEC_HOST_IO_H
#define TERNFOLD_EXEC_HOST_IO_H

#include "ternfold/interpreter.h"

/* Owned host snapshot. Streams must be byte-oriented and binary, or not
 * yet oriented. File opening, closing and flushing are caller-owned. */
typedef struct {
    unsigned char *bytes;
    TfCoreText *lines;
    size_t count;
} TfInputSnapshot;

/* Fresh out. Strict UTF-8, split at LF, trim the preceding CR, preserve NUL.
 * No terminal empty line is appended. The host budget includes both owned
 * bytes and line descriptors. Either failure clears the output. */
TfStatus tf_input_snapshot_read(TfInputSnapshot *out, FILE *input, size_t max_bytes);
void tf_input_snapshot_destroy(TfInputSnapshot *snapshot);

/* Write completed output only; exact counted bytes plus LF per line. */
TfStatus tf_interpreter_output_write(FILE *stream, const TfInterpreterResult *result);

#endif
