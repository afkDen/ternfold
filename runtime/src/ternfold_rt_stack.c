#include "ternfold_rt_stack.h"

enum { READY, EXECUTING, WAITING, RETURNED };

static void observe(tf_stack *s, tf_stack_frame *f, TfTraceEventKind kind,
                    tf_span_id span, size_t ordinal, const tf_value *value,
                    const tf_value *input) {
    if (!s->observer || !f->trace_invocation) return;
    TfTracePreview preview={0};
    if (kind==TF_TRACE_FAULT) {
        preview.bytes[0]='R'; preview.bytes[1]='0'; preview.bytes[2]='0';
        preview.bytes[3]=(unsigned char)('0'+ordinal); preview.length=4;
    } else if (value && s->observer->count<s->observer->capacity) {
        const TfTraceValueAdapter adapter=tf_runtime_trace_adapter(s->ctx);
        if (input) tf_trace_preview_pair(&preview,adapter,value,input);
        else tf_trace_preview(&preview,adapter,value);
    }
    const TfTraceEvent event={.kind=kind,.invocation_id=f->trace_invocation,
        .parent_invocation_id=f->trace_parent,.function_id=f->function,
        .span_id=span,.ordinal=kind==TF_TRACE_FAULT ? 0 : ordinal};
    (void)tf_trace_capture(s->observer,&event,preview.bytes,preview.length,preview.incomplete);
}
void tf_stack_trace_begin(tf_stack *s, tf_stack_frame *f, tf_span_id origin) {
    if (!s->observer || f->argc!=2 || f->trace_invocation) return;
    if (!tf_trace_next_invocation(s->observer,&f->trace_invocation)) return;
    f->trace_parent=f->trace_active;
    f->trace_active=f->trace_invocation;
    f->trace_origin=origin;
    observe(s,f,TF_TRACE_STEP_BEGIN,origin,0,&f->args[0],&f->args[1]);
}
void tf_stack_trace_value(tf_stack *s, tf_stack_frame *f, TfTraceEventKind kind,
                          tf_span_id origin, size_t ordinal, size_t slot) {
    if (slot<f->slot_count) observe(s,f,kind,origin,ordinal,&f->slots[slot],NULL);
}
typedef enum { ROOT, STATIC, CLOSURE } call_kind;

static tf_status fail(tf_stack *s, tf_fault_code code, tf_span_id span) {
    return tf_context_fail(s->ctx, code, span);
}

static bool shape(tf_context *ctx, tf_rt_shape expected,
                  const tf_value *values, size_t count) {
    if (count != expected.count || count > SIZE_MAX / sizeof *values ||
        count > SIZE_MAX / sizeof *expected.types ||
        (count != 0 && (values == NULL || expected.types == NULL))) return false;
    for (size_t i = 0; i < count; ++i)
        if (expected.types[i] == 0 || tf_value_type(ctx, values[i]) != expected.types[i]) return false;
    return true;
}

static void *allocate(tf_stack *s, tf_span_id span, size_t bytes) {
    tf_context *ctx = s->ctx;
    if (ctx->live_bytes > ctx->config.byte_budget ||
        bytes > ctx->config.byte_budget - ctx->live_bytes) {
        (void)fail(s, TF_R003, span);
        return NULL;
    }
    void *ptr = ctx->config.allocator.allocate(ctx->config.allocator.user, bytes);
    if (ptr == NULL) {
        (void)fail(s, TF_R003, span);
        return NULL;
    }
    ctx->live_bytes += bytes; /* bounded above by byte_budget <= SIZE_MAX */
    return ptr;
}

static void deallocate(tf_stack *s, void *ptr, size_t bytes) {
    s->ctx->live_bytes -= bytes;
    s->ctx->config.allocator.deallocate(s->ctx->config.allocator.user, ptr);
}

static void drop(tf_stack *s, tf_stack_frame *f) {
    if (f->cleanup != NULL) f->cleanup(s,f);
    /* Both additions were checked before allocation. No allocation on unwind. */
    const size_t count = f->slot_count + f->argc + f->capture_count;
    for (size_t i = 0; i < count; ++i) tf_release(&f->values[i]);
    deallocate(s, f, f->bytes);
}

static void pop(tf_stack *s) {
    tf_stack_frame *f = s->top;
    s->top = f->parent;
    --s->depth;
    s->ctx->depth = s->depth < TF_RT_MAX_CALL_DEPTH ? s->depth : TF_RT_MAX_CALL_DEPTH;
    drop(s, f);
}

tf_status tf_stack_init(tf_stack *s, tf_context *ctx,
                        const tf_stack_program *program, size_t call_limit) {
    *s = (tf_stack){.ctx = ctx, .program = program, .call_limit = call_limit};
    if (tf_context_fault(ctx) != NULL) return TF_FAULT;
    if (program == NULL || program->count != ctx->program.function_count ||
        program->count > SIZE_MAX / sizeof *program->bodies ||
        (program->count != 0 && program->bodies == NULL)) return fail(s, TF_R004, 0);
    return TF_OK;
}

void tf_stack_destroy(tf_stack *s) {
    /* Not callable from a resume. A completed run has no live frames. */
    while (s->top != NULL) pop(s);
    if (s->trace != NULL) deallocate(s, s->trace, s->trace_bytes);
    *s = (tf_stack){0};
}

const tf_fault *tf_stack_fault(const tf_stack *s) {
    return s->fault.code != 0 ? &s->fault : tf_context_fault(s->ctx);
}

static tf_status push(tf_stack *s, tf_span_id span, call_kind kind,
                      tf_function_id target, const tf_value *captures, size_t capture_count,
                      const tf_value *args, size_t argc, size_t destination) {
    if (tf_context_fault(s->ctx) != NULL) return TF_FAULT;
    if (target == 0 || target > s->program->count) return fail(s, TF_R004, span);
    const tf_stack_body *b = &s->program->bodies[target - 1];
    const tf_rt_function *fn = &s->ctx->program.functions[target - 1];
    if (b->resume == NULL || b->kind < TF_RT_ORDINARY || b->kind > TF_RT_PRELUDE_WRAPPER ||
        (kind == STATIC && b->kind != TF_RT_ORDINARY) ||
        (kind == CLOSURE && b->kind == TF_RT_ENTRY_ONLY) ||
        (kind == ROOT && (b->kind == TF_RT_PRELUDE_WRAPPER ||
                         (b->kind == TF_RT_ENTRY_ONLY && target != s->program->entry))) ||
        fn->type == 0 || fn->type > s->ctx->program.type_count ||
        s->ctx->program.types[fn->type - 1].kind != TF_RT_TYPE_FUNCTION ||
        b->result == 0 || b->result > s->ctx->program.type_count ||
        s->ctx->program.types[b->result - 1].kind == TF_RT_TYPE_UNUSED ||
        !shape(s->ctx, b->parameters, args, argc) ||
        !shape(s->ctx, fn->captures, captures, capture_count)) return fail(s, TF_R004, span);
    if (s->depth >= s->call_limit) return fail(s, TF_R003, span);
    if (argc > SIZE_MAX - capture_count || b->slot_count > SIZE_MAX - argc - capture_count)
        return fail(s, TF_R003, span);
    const size_t count = b->slot_count + argc + capture_count;
    if (count > (SIZE_MAX - sizeof(tf_stack_frame)) / sizeof(tf_value)) return fail(s, TF_R003, span);
    const size_t bytes = sizeof(tf_stack_frame) + count * sizeof(tf_value);
    tf_stack_frame *f = allocate(s, span, bytes);
    if (f == NULL) return TF_FAULT;
    *f = (tf_stack_frame){.slots = f->values, .args = f->values + b->slot_count,
        .captures = f->values + b->slot_count + argc, .argc = argc,
        .capture_count = capture_count, .slot_count = b->slot_count,
        .function = target, .span = span, .parent = s->top, .bytes = bytes,
        .destination = destination, .state = READY};
    f->trace_active=s->top ? s->top->trace_active : 0;
    for (size_t i = 0; i < count; ++i) f->values[i] = tf_unit();
    for (size_t i = 0; i < argc; ++i) {
        if (tf_retain(s->ctx, span, args[i], &f->values[b->slot_count + i]) != TF_OK) {
            drop(s, f);
            return TF_FAULT;
        }
    }
    for (size_t i = 0; i < capture_count; ++i) {
        if (tf_retain(s->ctx, span, captures[i], &f->values[b->slot_count + argc + i]) != TF_OK) {
            drop(s, f);
            return TF_FAULT;
        }
    }
    s->trace[s->depth] = (tf_call_frame){target, span};
    if (s->depth < TF_RT_MAX_CALL_DEPTH) s->ctx->frames[s->depth] = s->trace[s->depth];
    ++s->depth;
    s->ctx->depth = s->depth < TF_RT_MAX_CALL_DEPTH ? s->depth : TF_RT_MAX_CALL_DEPTH;
    s->top = f;
    return TF_OK;
}

static bool executing(tf_stack *s, tf_stack_frame *f) {
    return s->running && s->top == f && f != NULL && f->state == EXECUTING;
}

static tf_status call(tf_stack *s, tf_stack_frame *f, tf_span_id span, call_kind kind,
                      tf_function_id target, const tf_value *captures, size_t capture_count,
                      const tf_value *args, size_t argc, size_t destination, size_t resume_pc) {
    if (tf_context_fault(s->ctx) != NULL) return TF_FAULT;
    if (!executing(s, f) || destination >= f->slot_count ||
        f->slots[destination].tag != TF_VALUE_UNIT) return fail(s, TF_R004, span);
    if (push(s, span, kind, target, captures, capture_count, args, argc, destination) != TF_OK)
        return TF_FAULT;
    f->pc = resume_pc;
    f->state = WAITING;
    return TF_OK;
}

tf_status tf_stack_call(tf_stack *s, tf_stack_frame *f, tf_span_id span,
                        tf_function_id target, const tf_value *args, size_t argc,
                        size_t destination, size_t resume_pc) {
    return call(s, f, span, STATIC, target, NULL, 0, args, argc, destination, resume_pc);
}

tf_status tf_stack_call_closure(tf_stack *s, tf_stack_frame *f, tf_span_id span,
                                tf_value closure, const tf_value *args, size_t argc,
                                size_t destination, size_t resume_pc) {
    if (tf_context_fault(s->ctx) != NULL) return TF_FAULT;
    if (!executing(s, f)) return fail(s, TF_R004, span);
    tf_function_id target;
    const tf_value *captures;
    size_t capture_count;
    if (tf_closure_view(s->ctx, span, closure, &target, &captures, &capture_count) != TF_OK) return TF_FAULT;
    return call(s, f, span, CLOSURE, target, captures, capture_count, args, argc, destination, resume_pc);
}

tf_status tf_stack_return(tf_stack *s, tf_stack_frame *f, size_t slot) {
    if (tf_context_fault(s->ctx) != NULL) return TF_FAULT;
    if (!executing(s, f)) return fail(s, TF_R004, 0);
    if (slot >= f->slot_count ||
        tf_value_type(s->ctx, f->slots[slot]) != s->program->bodies[f->function - 1].result)
        return fail(s, TF_R004, f->span);
    f->returned = slot;
    f->state = RETURNED;
    return TF_OK;
}

tf_status tf_stack_run(tf_stack *s, tf_span_id span, tf_function_id target,
                       const tf_value *captures, size_t capture_count,
                       const tf_value *args, size_t argc, tf_value *out) {
    *out = tf_unit();
    if (tf_context_fault(s->ctx) != NULL) return TF_FAULT;
    if (s->running || s->ctx->depth != 0 || s->program == NULL) return fail(s, TF_R004, span);
    if (s->call_limit > SIZE_MAX / sizeof *s->trace) return fail(s, TF_R003, span);
    if (s->call_limit != 0 && s->trace == NULL) {
        s->trace_bytes = s->call_limit * sizeof *s->trace;
        s->trace = allocate(s, span, s->trace_bytes);
        if (s->trace == NULL) return TF_FAULT;
    }
    s->running = true;
    tf_status status = push(s, span, ROOT, target, captures, capture_count, args, argc, 0);
    while (status == TF_OK && s->top != NULL) {
        tf_stack_frame *f = s->top;
        f->state = EXECUTING;
        status = s->program->bodies[f->function - 1].resume(s, f);
        if (status != TF_OK || tf_context_fault(s->ctx) != NULL) {
            /* Numeric capability failure is a host failure: TF_FAULT with no
             * context fault. Do not invent a source-language R004 for it. */
            if (status != TF_OK && status != TF_FAULT) (void)fail(s, TF_R004, f->span);
            status = TF_FAULT;
            break;
        }
        if (f->state == WAITING && s->top != f) continue;
        if (f->state != RETURNED || s->top != f) {
            status = fail(s, TF_R004, f->span);
            break;
        }
        tf_value *destination = f->parent != NULL ? &f->parent->slots[f->destination] : out;
        if (destination->tag != TF_VALUE_UNIT) {
            status = fail(s, TF_R004, f->span);
            break;
        }
        if (f->trace_invocation) {
            const tf_value *result=&f->slots[f->returned];
            const TfTraceValueView view=tf_runtime_trace_adapter(s->ctx).view(s->ctx,result);
            observe(s,f,view.tag==0 ? TF_TRACE_ACCEPT : TF_TRACE_REJECT,f->trace_origin,0,result,NULL);
        }
        *destination = f->slots[f->returned]; /* ownership move, never a retain */
        f->slots[f->returned] = tf_unit();
        pop(s);
    }
    if (status != TF_OK) {
        const tf_fault *fault = tf_context_fault(s->ctx);
        if (fault != NULL) {
            s->fault = *fault;
            s->fault.frames = s->trace;
            s->fault.frame_count = s->depth;
        }
        while (s->top != NULL) {
            if (fault) observe(s,s->top,TF_TRACE_FAULT,fault->span,(size_t)fault->code,NULL,NULL);
            pop(s);
        }
        tf_release(out);
    }
    s->running = false;
    return status;
}
