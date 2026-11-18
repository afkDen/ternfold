#ifndef TERNFOLD_INTERPRETER_H
#define TERNFOLD_INTERPRETER_H

#include "ternfold/core_verify.h"
#include "ternfold/trace.h"

typedef struct TfInterpreterResult TfInterpreterResult;

typedef enum {
    TF_RUNTIME_NONE = 0,
    TF_RUNTIME_OVERFLOW = 1,           /* R001 */
    TF_RUNTIME_ZERO_DIVISOR = 2,       /* R002 */
    TF_RUNTIME_RESOURCE = 3,           /* R003 */
    TF_RUNTIME_INVARIANT = 4,          /* R004 */
    TF_RUNTIME_NEGATIVE_EXPONENT = 5,  /* R005 */
    TF_RUNTIME_INSUFFICIENT_INPUT = 6  /* R006 */
} TfRuntimeCode;

typedef struct {
    TfCoreFunctionId function;
    TfCoreSpanId invocation; /* zero for the host entry */
} TfRuntimeFrame;

/* Caller-owned fault record; ordered outermost to innermost. At most the
 * first and last frames are retained when the stack exceeds the fixed limit. */
enum { TF_INTERPRETER_FAULT_FRAMES = 32 };
typedef struct {
    TfRuntimeCode code;
    TfCoreSpanId span;
    TfRuntimeFrame frames[TF_INTERPRETER_FAULT_FRAMES];
    size_t frame_count, omitted_frames;
} TfRuntimeFault;

typedef struct {
    size_t max_bytes;       /* live interpreter-owned allocations */
    size_t max_call_depth;  /* includes the entry frame */
    uint64_t max_steps;     /* charges operations and bounded inner work */
} TfInterpreterLimits;

/* Default limits: 256 MiB, 1024 language calls, 10 million steps. */
TfInterpreterLimits tf_interpreter_default_limits(void);

/* Execute the entry of verified Core. Lines are counted UTF-8 Text values,
 * already split by the host. The call borrows the program and input for its
 * duration. A pure entry ignores the input. On success the result owns the
 * completed output lines and borrows only the allocator's context.
 *
 * TF_SOURCE_ERROR reports a language fault (including resource exhaustion).
 * TF_HOST_ERROR reports malformed API arguments, missing entry, or a host
 * capability failure. On either failure the fresh result stays NULL. The
 * optional fault is cleared on entry; it contains only IDs, whose metadata
 * is meaningful while the caller retains the original verified Core. */
TfStatus tf_interpret_entry(TfInterpreterResult **out, const TfVerifiedCore *program,
                           const TfCoreText *lines, size_t line_count,
                           const TfInterpreterLimits *limits, const TfAllocator *allocator,
                           TfRuntimeFault *fault);
/* Optional observation store is borrowed for this call. It owns copied
 * previews only. Capacity exhaustion cannot change evaluation or its budget. */
TfStatus tf_interpret_entry_traced(TfInterpreterResult **out, const TfVerifiedCore *program,
                           const TfCoreText *lines, size_t line_count,
                           const TfInterpreterLimits *limits, const TfAllocator *allocator,
                           TfRuntimeFault *fault, TfTraceRecorder *trace);

size_t tf_interpreter_result_count(const TfInterpreterResult *result);
/* Out-of-range returns false without writing *line. Bytes borrow result. */
bool tf_interpreter_result_line(const TfInterpreterResult *result, size_t index, TfCoreText *line);
void tf_interpreter_result_destroy(TfInterpreterResult *result);

#endif
