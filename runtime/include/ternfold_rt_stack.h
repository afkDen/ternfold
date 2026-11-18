#ifndef TERNFOLD_RT_STACK_H
#define TERNFOLD_RT_STACK_H

#include "ternfold_rt_calls.h"

/* Internal generated-body ABI 2; the original borrowed-input/owned-output
 * C function ABI remains the host entry facade. See ADR 0012. */
typedef struct tf_stack tf_stack;
typedef struct tf_stack_frame tf_stack_frame;
typedef void (*tf_stack_frame_cleanup)(tf_stack *,tf_stack_frame *);
typedef tf_status (*tf_stack_resume)(tf_stack *, tf_stack_frame *);

typedef struct {
    tf_stack_resume resume;
    tf_rt_call_kind kind;
    tf_rt_shape parameters;
    tf_type_id result;
    size_t slot_count;
} tf_stack_body;

typedef struct {
    const tf_stack_body *bodies;
    size_t count;
    tf_function_id entry;
} tf_stack_program;

/* Stack-allocated handles, not movable/copyable while initialized. All fields
 * except frame.pc and frame.slots are read-only to generated code. Borrowed
 * arguments/captures are backed by retained, frame-owned copies. */
struct tf_stack_frame {
    size_t pc;
    tf_value *slots;
    const tf_value *args;
    const tf_value *captures;
    size_t argc, capture_count, slot_count;
    tf_function_id function;
    tf_span_id span;
    /* Private dispatcher state. */
    tf_stack_frame *parent;
    size_t bytes, destination, returned;
    unsigned state;
    /* Optional runtime-owned auxiliary state; released by pop/unwind before
     * slot teardown. Generated bodies leave both fields untouched. */
    void *auxiliary;
    tf_stack_frame_cleanup cleanup;
    tf_value values[];
};

struct tf_stack {
    tf_context *ctx;
    const tf_stack_program *program;
    size_t call_limit;
    /* Private dispatcher state. */
    tf_stack_frame *top;
    tf_call_frame *trace;
    size_t depth, trace_bytes;
    tf_fault fault;
    bool running;
};

/* Borrows ctx and immutable generated tables until destroy. Zero call_limit
 * is valid; callers normally pass ctx->config.call_limit (default 1024).
 * Larger limits do not consume host stack. Every allocation, including the
 * preallocated full trace, uses ctx's allocator and shared live byte budget.
 * Init does not allocate. A failed init remains destroyable. */
tf_status tf_stack_init(tf_stack *stack, tf_context *ctx,
                        const tf_stack_program *program, size_t call_limit);
void tf_stack_destroy(tf_stack *stack);

/* Host facade only: no dispatcher may run inside an active context call.
 * Borrowed inputs; fresh/disjoint out receives an owner or Unit on fault.
 * Captures permit a facade with the original ABI for lifted functions too.
 * Entry-only target must equal program.entry; prelude wrappers are internal.
 * Success permits reuse. A fault freezes the full trace until destroy;
 * ctx retains its original first fault and fixed-capacity trace prefix.
 * TF_FAULT without a context/stack fault is a host capability failure (e.g.
 * unsupported Float), also unwound with Unit output, never converted to R004. */
tf_status tf_stack_run(tf_stack *stack, tf_span_id span, tf_function_id target,
                       const tf_value *captures, size_t capture_count,
                       const tf_value *args, size_t argc, tf_value *out);
const tf_fault *tf_stack_fault(const tf_stack *stack);

/* Only the currently executing resume may call these. A successful resume
 * must perform exactly one control action: return one of these helper results,
 * optionally releasing independent local owners before returning. No further
 * fallible/language operation may follow a successful action. Calls retain
 * inputs synchronously, so C-local argument arrays
 * are safe. Parent destination must be Unit and in range. All other owners stay
 * live until return/unwind. Child entry failures leave pc/destination unchanged.
 * Static calls require ordinary bodies; closure calls also permit prelude
 * wrappers. No generated resume may call a facade or legacy direct adapter.
 * Builtin callback loops must use the same continuation protocol. */
tf_status tf_stack_call(tf_stack *stack, tf_stack_frame *frame, tf_span_id span,
                        tf_function_id target, const tf_value *args, size_t argc,
                        size_t destination, size_t resume_pc);
tf_status tf_stack_call_closure(tf_stack *stack, tf_stack_frame *frame, tf_span_id span,
                                tf_value closure, const tf_value *args, size_t argc,
                                size_t destination, size_t resume_pc);
/* map/fold adapters are resume functions, not synchronous C callback loops:
 * keep input/closure/index/accumulator and partial results in owned slots;
 * schedule exactly one callback with call_closure(..., frame->span, ...);
 * resume after its owned result arrives, consume that result, advance the
 * index, then schedule the next callback. Empty inputs return immediately.
 * The outer invocation span is the effective origin for synthetic adapters.
 * Return only the final aggregate/accumulator. A fault drops partial results
 * through normal frame cleanup and publishes no host output. */
/* Moves one slot to the parent/host after validating its declared result type.
 * On any fault no result is published; every owner is released iteratively. */
tf_status tf_stack_return(tf_stack *stack, tf_stack_frame *frame, size_t slot);

#endif
