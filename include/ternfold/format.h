#ifndef TERNFOLD_FORMAT_H
#define TERNFOLD_FORMAT_H
#include "ternfold/parser.h"

typedef struct {
    unsigned char *bytes;
    size_t length;
    TfAllocator allocator;
} TfFormatResult;

/* Fresh output, cleared on failure. Source is borrowed only during the call.
 * NULL options use the parser defaults. SOURCE_ERROR includes lexical/syntax
 * errors; HOST_ERROR includes resource limits/OOM. Successful bytes are owned,
 * NUL terminated for convenience; length excludes the terminator. No I/O.
 * Tokens/comments retain exact lexemes; only inter-token whitespace changes.
 * Destroy once; allocator/context must outlive result. */
TfStatus tf_format(TfFormatResult *out, const TfSource *source,
                   const TfParseOptions *options, const TfAllocator *allocator);
void tf_format_destroy(TfFormatResult *result);
#endif
