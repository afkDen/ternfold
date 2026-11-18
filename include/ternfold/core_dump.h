#ifndef TERNFOLD_CORE_DUMP_H
#define TERNFOLD_CORE_DUMP_H

#include "ternfold/core_verify.h"

/* Human-readable inspection of verified Core. Keep the verified handle,
 * its frozen owner and borrowed sources alive for the call.
 *
 * Table and list order is preserved. t/c/f/s IDs refer to program tables;
 * b and % IDs are local to a function. Zero denotes an absent reference.
 * Spans use half-open byte offsets. Quoted strings are ASCII, with quote,
 * backslash, LF, CR and tab escaped and all other nonprintable/non-ASCII
 * bytes written as \xHH (exactly two hex digits per byte, including NUL).
 *
 * The caller owns the writable byte-oriented or unoriented binary stream
 * and handles flush/close errors.
 * An observed write error returns TF_HOST_ERROR and can leave a prefix.
 * No working storage is allocated by the renderer. */
TfStatus tf_core_dump_write(FILE *stream, const TfVerifiedCore *verified);

#endif
