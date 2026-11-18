#ifndef TERNFOLD_REPL_VALUE_H
#define TERNFOLD_REPL_VALUE_H

#include "ternfold/interpreter.h"

enum { TF_REPL_VALUE_CAPACITY = 4096 };
typedef struct {
    unsigned char bytes[TF_REPL_VALUE_CAPACITY];
    size_t length;
} TfReplValue;

/* Execute a verified ordinary, non-entry function with no parameters or
 * captures. The caller selects the wrapper ID returned by tf_core_lower_repl.
 * No host I/O is performed. Success publishes one NUL-terminated debug line
 * (without LF) into caller storage; failure leaves an empty line. The optional
 * fault borrows its ID metadata from program as in tf_interpret_entry. */
TfStatus tf_interpret_repl(TfReplValue *out, const TfVerifiedCore *program,
                           TfCoreFunctionId function, const TfInterpreterLimits *limits,
                           const TfAllocator *allocator, TfRuntimeFault *fault);

struct TfExecValue;
/* VM-private: render a completed value while its arena is still alive. */
void tf_repl_value_render(TfReplValue *out, const TfCoreProgramData *program,
                          const struct TfExecValue *value);

#endif
