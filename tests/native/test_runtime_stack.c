#include "ternfold_rt_stack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); exit(EXIT_FAILURE); } } while (0)
enum { T_UNIT = 1, T_INT, T_TEXT, T_FN };
enum { SOURCE = 1, CALLBACK, WRAPPER, ENTRY, BAD };
enum { NORMAL, FAULT_BOTTOM, MIXED, BAD_RETURN, NO_ACTION, BAD_DESTINATION,
       ENTRY_CALL, DOUBLE_ACTION, REENTER, BAD_TARGET, BAD_ARG, BAD_CAPTURE,
       MIXED_PRELUDE, MIXED_FAULT, HOST_FAILURE, INVALID_STATUS };
enum { OWNED_SLOTS = 256, FRAME_SLOTS = OWNED_SLOTS + 1 };
static const tf_type_id parameters[] = {T_INT, T_INT};
static const tf_type_id capture_types[] = {T_TEXT, T_INT};
static const tf_rt_type types[] = {
    {.kind = TF_RT_TYPE_UNIT}, {.kind = TF_RT_TYPE_INT},
    {.kind = TF_RT_TYPE_TEXT}, {.kind = TF_RT_TYPE_FUNCTION}
};
static const tf_rt_function functions[] = {
    {T_FN, {NULL, 0}}, {T_FN, {capture_types, 2}}, {T_FN, {capture_types, 2}},
    {T_FN, {NULL, 0}}, {T_FN, {NULL, 0}}
};
static const tf_rt_program values = {types, 4, functions, 5};

/* Generated-style bodies: structured C with a switch at call resume points,
 * never a recursive C call. 256 heap owners plus a child-result slot per frame. */
static tf_status body(tf_stack *s, tf_stack_frame *f) {
    tf_context *ctx = s->ctx;
    const int64_t n = f->args[0].as.integer;
    const int64_t mode = f->args[1].as.integer;
    if (f->pc == 1) {
        const unsigned char *bytes;
        size_t length;
        CHECK(tf_text_view(ctx, 920, f->slots[1], &bytes, &length) == TF_OK);
        CHECK(length == 3 && memcmp(bytes, "x\0y", 3) == 0);
        if (f->capture_count != 0) {
            CHECK(f->captures[1].as.integer == n + 1);
            CHECK(tf_text_view(ctx, 921, f->captures[0], &bytes, &length) == TF_OK);
            CHECK(length == 3 && memcmp(bytes, "x\0y", 3) == 0);
        }
        return tf_stack_return(s, f, 0);
    }
    CHECK(f->pc == 0 && f->slot_count == FRAME_SLOTS);
    if (tf_text(ctx, 800, T_TEXT, (const unsigned char *)"x\0y", 3, &f->slots[1]) != TF_OK) return TF_FAULT;
    for (size_t i = 2; i < f->slot_count; ++i)
        if (tf_retain(ctx, 801, f->slots[1], &f->slots[i]) != TF_OK) return TF_FAULT;
    if (mode == NO_ACTION) return TF_OK;
    if (mode == HOST_FAILURE) return TF_FAULT;
    if (mode == INVALID_STATUS) return (tf_status)99;
    if (mode == BAD_RETURN) return tf_stack_return(s, f, 0);
    if (mode == REENTER) {
        tf_value out;
        return tf_stack_run(s, 830, SOURCE, NULL, 0, f->args, f->argc, &out);
    }
    if (mode == BAD_CAPTURE) return tf_stack_call(s, f, 831, CALLBACK, f->args, 2, 0, 1);
    if (n == 0) {
        if (mode == FAULT_BOTTOM || mode == MIXED_FAULT) return tf_context_fail(ctx, TF_R002, 777);
        if (tf_retain(ctx, 802, f->slots[1], &f->slots[0]) != TF_OK) return TF_FAULT;
        return tf_stack_return(s, f, 0);
    }
    /* Local arrays cease to exist on return; dispatcher must copy/retain now. */
    tf_value args[] = {tf_int(n - 1), tf_int(mode)};
    if (mode == BAD_ARG) args[0] = tf_bool(true);
    if ((mode == MIXED || mode == MIXED_PRELUDE || mode == MIXED_FAULT) && f->function == SOURCE) {
        const tf_value captures[] = {f->slots[1], tf_int(n)};
        tf_value closure;
        const tf_function_id target = mode == MIXED_PRELUDE ? WRAPPER : CALLBACK;
        if (tf_closure(ctx, 803, target, captures, 2, &closure) != TF_OK) return TF_FAULT;
        const tf_status status = tf_stack_call_closure(s, f, 901, closure, args, 2, 0, 1);
        /* Only the captured values, not the closure object, must survive. */
        tf_release(&closure);
        return status;
    }
    const tf_function_id target = mode == ENTRY_CALL ? ENTRY : mode == BAD_TARGET ? 999 : SOURCE;
    const size_t destination = mode == BAD_DESTINATION ? 1u : 0u;
    tf_status status = tf_stack_call(s, f, 900, target, args, 2, destination, 1);
    if (mode == DOUBLE_ACTION && status == TF_OK) status = tf_stack_return(s, f, 0);
    return status;
}

static const tf_stack_body bodies[] = {
    {body, TF_RT_ORDINARY, {parameters, 2}, T_TEXT, FRAME_SLOTS},
    {body, TF_RT_ORDINARY, {parameters, 2}, T_TEXT, FRAME_SLOTS},
    {body, TF_RT_PRELUDE_WRAPPER, {parameters, 2}, T_TEXT, FRAME_SLOTS},
    {body, TF_RT_ENTRY_ONLY, {parameters, 2}, T_TEXT, FRAME_SLOTS},
    {NULL, TF_RT_ORDINARY, {parameters, 2}, T_TEXT, FRAME_SLOTS}
};
static const tf_stack_program program = {bodies, 5, ENTRY};

typedef struct {
    size_t attempts, fail_at, outstanding, bytes, peak, releases;
    bool failed;
} tracker;
typedef union { max_align_t alignment; size_t bytes; } prefix;

static void *tracked_allocate(void *user, size_t bytes) {
    tracker *t = user;
    ++t->attempts;
    if (t->attempts == t->fail_at) { t->failed = true; return NULL; }
    CHECK(bytes <= SIZE_MAX - sizeof(prefix));
    prefix *p = malloc(sizeof *p + bytes);
    CHECK(p != NULL);
    p->bytes = bytes;
    ++t->outstanding;
    t->bytes += bytes;
    if (t->bytes > t->peak) t->peak = t->bytes;
    return p + 1;
}
static void tracked_free(void *user, void *ptr) {
    tracker *t = user;
    prefix *p = (prefix *)ptr - 1;
    CHECK(t->outstanding != 0 && t->bytes >= p->bytes);
    --t->outstanding;
    t->bytes -= p->bytes;
    ++t->releases;
    free(p);
}
static void init(tf_context *ctx, tf_stack *s, tracker *t, size_t budget, size_t limit) {
    const tf_rt_config config = {{tracked_allocate, tracked_free, t}, budget, TF_RT_MAX_CALL_DEPTH};
    CHECK(tf_context_init(ctx, &values, &config) == TF_OK);
    CHECK(tf_stack_init(s, ctx, &program, limit) == TF_OK);
}
static void clean(tf_context *ctx, tf_stack *s, tracker *t) {
    CHECK(s->depth == 0 && s->top == NULL && !s->running && ctx->depth == 0);
    CHECK(ctx->live_bytes == t->bytes);
    tf_stack_destroy(s);
    CHECK(ctx->live_bytes == 0 && t->bytes == 0 && t->outstanding == 0);
}
static tf_status run(tf_stack *s, int64_t n, int64_t mode, tf_value *out) {
    const tf_value args[] = {tf_int(n), tf_int(mode)};
    return tf_stack_run(s, 100, SOURCE, NULL, 0, args, 2, out);
}
static void fault_is(tf_stack *s, tf_fault_code code, tf_span_id span, size_t depth) {
    const tf_fault *fault = tf_stack_fault(s);
    CHECK(fault != NULL && fault->code == code && fault->span == span && fault->frame_count == depth);
    const tf_fault *context_fault = tf_context_fault(s->ctx);
    CHECK(context_fault != NULL && context_fault->code == code && context_fault->span == span);
    CHECK(context_fault->frame_count == (depth < TF_RT_MAX_CALL_DEPTH ? depth : TF_RT_MAX_CALL_DEPTH));
    for (size_t i = 0; i < depth; ++i) {
        CHECK(fault->frames[i].function == SOURCE || fault->frames[i].function == CALLBACK);
        CHECK(fault->frames[i].call_span == (i == 0 ? 100u : fault->frames[i].function == CALLBACK ? 901u : 900u));
    }
}

static size_t success(int64_t n, int64_t mode, size_t budget, size_t *attempts) {
    tf_context ctx;
    tf_stack s;
    tracker t = {0};
    init(&ctx, &s, &t, budget, (size_t)n + 1);
    tf_value out;
    CHECK(run(&s, n, mode, &out) == TF_OK && tf_stack_fault(&s) == NULL);
    CHECK(out.tag == TF_VALUE_BOXED);
    const unsigned char *bytes;
    size_t length;
    CHECK(tf_text_view(&ctx, 999, out, &bytes, &length) == TF_OK);
    CHECK(length == 3 && memcmp(bytes, "x\0y", 3) == 0);
    tf_release(&out);
    const size_t peak = t.peak;
    if (attempts != NULL) *attempts = t.attempts;
    /* Successful reuse must neither allocate another trace nor leak owners. */
    CHECK(run(&s, 0, NORMAL, &out) == TF_OK);
    tf_release(&out);
    clean(&ctx, &s, &t);
    return peak;
}

static void test_deep(void) {
    size_t attempts;
    const size_t peak = success(2048, NORMAL, SIZE_MAX, &attempts);
    CHECK(attempts == 1 + 2 * 2049); /* trace, one frame + one Text per invocation */
    CHECK(success(2048, NORMAL, peak, NULL) == peak);
    (void)success(2048, MIXED, SIZE_MAX, NULL);
    (void)success(2048, MIXED_PRELUDE, SIZE_MAX, NULL);
    tf_context ctx;
    tf_stack s;
    tracker t = {0};
    init(&ctx, &s, &t, peak - 1, 2049);
    tf_value out;
    CHECK(run(&s, 2048, NORMAL, &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
    fault_is(&s, TF_R003, 800, 2049);
    CHECK(t.attempts == attempts - 1); /* budget prevents allocator invocation */
    clean(&ctx, &s, &t);

    t = (tracker){0};
    init(&ctx, &s, &t, SIZE_MAX, 2049);
    CHECK(run(&s, 2048, FAULT_BOTTOM, &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
    fault_is(&s, TF_R002, 777, 2049);
    const size_t old_attempts = t.attempts;
    CHECK(run(&s, 0, NORMAL, &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
    CHECK(t.attempts == old_attempts);
    fault_is(&s, TF_R002, 777, 2049);
    clean(&ctx, &s, &t);
    /* The context fault prefix must remain valid after stack trace destruction. */
    CHECK(ctx.fault.frames == ctx.frames && ctx.fault.frame_count == 1024);
    CHECK(ctx.frames[0].call_span == 100 && ctx.frames[1023].call_span == 900);

    t = (tracker){0};
    init(&ctx, &s, &t, SIZE_MAX, 2050);
    CHECK(run(&s, 2049, MIXED_FAULT, &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
    fault_is(&s, TF_R002, 777, 2050);
    CHECK(tf_stack_fault(&s)->frames[2049].function == CALLBACK);
    clean(&ctx, &s, &t);
}

static void test_limits(void) {
    const size_t limits[] = {0, 1, 1024, 1537, 2049};
    for (size_t i = 0; i < sizeof limits / sizeof *limits; ++i) {
        tf_context ctx;
        tf_stack s;
        tracker t = {0};
        init(&ctx, &s, &t, SIZE_MAX, limits[i]);
        tf_value out;
        CHECK(run(&s, 2050, NORMAL, &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
        fault_is(&s, TF_R003, limits[i] == 0 ? 100u : 900u, limits[i]);
        CHECK(t.attempts == (limits[i] == 0 ? 0 : 1 + 2 * limits[i]));
        clean(&ctx, &s, &t);
    }
    tf_context ctx;
    tf_stack s;
    tracker t = {0};
    init(&ctx, &s, &t, SIZE_MAX, SIZE_MAX);
    tf_value out;
    CHECK(run(&s, 0, NORMAL, &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
    fault_is(&s, TF_R003, 100, 0);
    CHECK(t.attempts == 0);
    clean(&ctx, &s, &t);
}

static void test_allocations(void) {
    /* Sweep every allocation of a mixed source/callback recursion, including
     * partial stack entry, frame Text and closure creation. */
    size_t attempts;
    (void)success(12, MIXED, SIZE_MAX, &attempts);
    for (size_t fail_at = 1; fail_at <= attempts; ++fail_at) {
        tf_context ctx;
        tf_stack s;
        tracker t = {.fail_at = fail_at};
        init(&ctx, &s, &t, SIZE_MAX, 13);
        tf_value out;
        CHECK(run(&s, 12, MIXED, &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
        CHECK(t.failed && t.attempts == fail_at && tf_stack_fault(&s)->code == TF_R003);
        clean(&ctx, &s, &t);
    }
    /* A deepest-frame allocation failure after >1024 active source frames. */
    for (size_t fail_at = 4098; fail_at <= 4099; ++fail_at) {
        tf_context ctx;
        tf_stack s;
        tracker t = {.fail_at = fail_at};
        init(&ctx, &s, &t, SIZE_MAX, 2049);
        tf_value out;
        CHECK(run(&s, 2048, NORMAL, &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
        fault_is(&s, TF_R003, fail_at == 4098 ? 900u : 800u, fail_at == 4098 ? 2048u : 2049u);
        CHECK(t.attempts == fail_at && t.failed);
        clean(&ctx, &s, &t);
    }
}

static void test_invalid(void) {
    const int64_t modes[] = {BAD_RETURN, NO_ACTION, BAD_DESTINATION, ENTRY_CALL,
        DOUBLE_ACTION, REENTER, BAD_TARGET, BAD_ARG, BAD_CAPTURE, INVALID_STATUS};
    const tf_span_id spans[] = {100, 100, 900, 900, 0, 830, 900, 900, 831, 100};
    for (size_t i = 0; i < sizeof modes / sizeof *modes; ++i) {
        tf_context ctx;
        tf_stack s;
        tracker t = {0};
        init(&ctx, &s, &t, SIZE_MAX, 4);
        tf_value out;
        CHECK(run(&s, 1, modes[i], &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
        fault_is(&s, TF_R004, spans[i], modes[i] == DOUBLE_ACTION ? 2u : 1u);
        clean(&ctx, &s, &t);
    }
    /* Checked size addition/multiplication, without visiting fabricated arrays. */
    tf_stack_body modified[5];
    memcpy(modified, bodies, sizeof modified);
    tf_stack_program table = {modified, 5, ENTRY};
    tf_context ctx;
    tf_stack s;
    tf_value out;
    const size_t excessive[] = {SIZE_MAX, SIZE_MAX / sizeof(tf_value)};
    for (size_t i = 0; i < sizeof excessive / sizeof *excessive; ++i) {
        tracker t = {0};
        modified[0].slot_count = excessive[i];
        init(&ctx, &s, &t, SIZE_MAX, 2);
        s.program = &table;
        CHECK(run(&s, 0, NORMAL, &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
        fault_is(&s, TF_R003, 100, 0);
        CHECK(t.attempts == 1);
        clean(&ctx, &s, &t);
    }
}

/* Same C signature as the original generated wrapper ABI. Host entry creates
 * a dispatcher; recursive calls inside generated bodies never use this facade.
 * Default-limit faults fit the context's unchanged fixed trace capacity. */
static tf_status facade(tf_context *ctx, const tf_value *captures, size_t capture_count,
                         const tf_value *args, size_t argc, tf_value *out) {
    tf_stack s;
    tf_status status = tf_stack_init(&s, ctx, &program, ctx->config.call_limit);
    *out = tf_unit();
    if (status == TF_OK) status = tf_stack_run(&s, 100, SOURCE, captures, capture_count, args, argc, out);
    tf_stack_destroy(&s);
    return status;
}

static void test_facade_and_root_validation(void) {
    tf_context ctx;
    tf_stack s;
    tracker t = {0};
    init(&ctx, &s, &t, SIZE_MAX, 1024);
    tf_value out;
    const tf_value args[] = {tf_int(1023), tf_int(NORMAL)};
    CHECK(facade(&ctx, NULL, 0, args, 2, &out) == TF_OK);
    tf_release(&out);
    const tf_value too_deep[] = {tf_int(1024), tf_int(NORMAL)};
    CHECK(facade(&ctx, NULL, 0, too_deep, 2, &out) == TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT && ctx.fault.code == TF_R003 && ctx.fault.span == 900);
    CHECK(ctx.fault.frame_count == 1024 && ctx.fault.frames[1023].function == SOURCE);
    clean(&ctx, &s, &t);

    const tf_function_id invalid[] = {0, 999, CALLBACK, WRAPPER, BAD};
    const tf_value zero[] = {tf_int(0), tf_int(NORMAL)};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        t = (tracker){0};
        init(&ctx, &s, &t, SIZE_MAX, 2);
        CHECK(tf_stack_run(&s, 105, invalid[i], NULL, 0, zero, 2, &out) == TF_FAULT);
        CHECK(out.tag == TF_VALUE_UNIT && tf_stack_fault(&s)->code == TF_R004);
        clean(&ctx, &s, &t);
    }
    t = (tracker){0};
    init(&ctx, &s, &t, SIZE_MAX, 2);
    CHECK(tf_stack_run(&s, 106, ENTRY, NULL, 0, zero, 2, &out) == TF_OK);
    tf_release(&out);
    CHECK(tf_context_enter(&ctx, SOURCE, 107) == TF_OK);
    CHECK(tf_stack_run(&s, 108, SOURCE, NULL, 0, zero, 2, &out) == TF_FAULT);
    CHECK(ctx.depth == 1 && ctx.fault.frames[0].call_span == 107 && out.tag == TF_VALUE_UNIT);
    tf_context_leave(&ctx);
    clean(&ctx, &s, &t);

    t = (tracker){0};
    init(&ctx, &s, &t, 0, 2);
    CHECK(run(&s, 0, NORMAL, &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
    fault_is(&s, TF_R003, 100, 0);
    CHECK(t.attempts == 0);
    clean(&ctx, &s, &t);

    t = (tracker){0};
    init(&ctx, &s, &t, SIZE_MAX, 2);
    CHECK(run(&s, 0, HOST_FAILURE, &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
    CHECK(tf_context_fault(&ctx) == NULL && tf_stack_fault(&s) == NULL);
    CHECK(run(&s, 0, NORMAL, &out) == TF_OK);
    tf_release(&out);
    clean(&ctx, &s, &t);
}

static void test_borrowed_and_first_fault(void) {
    tf_context ctx;
    tf_stack s;
    tracker t = {0};
    init(&ctx, &s, &t, SIZE_MAX, 4);
    tf_value text, out;
    CHECK(tf_text(&ctx, 1, T_TEXT, (const unsigned char *)"c", 1, &text) == TF_OK);
    tf_value captures[] = {text, tf_int(1)};
    const tf_value args[] = {tf_int(0), tf_int(NORMAL)};
    CHECK(tf_stack_run(&s, 101, CALLBACK, captures, 2, args, 2, &out) == TF_OK);
    tf_release(&out);
    CHECK(tf_test_swap_refcount(&ctx, text, UINT64_MAX) == 1);
    CHECK(tf_stack_run(&s, 102, CALLBACK, captures, 2, args, 2, &out) == TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT && tf_stack_fault(&s)->code == TF_R003);
    CHECK(tf_test_swap_refcount(&ctx, text, 1) == UINT64_MAX);
    tf_release(&text);
    clean(&ctx, &s, &t);

    /* First retain succeeds, second overflows: release the partially copied
     * argument vector without touching either borrowed caller owner. */
    t = (tracker){0};
    init(&ctx, &s, &t, SIZE_MAX, 4);
    CHECK(tf_text(&ctx, 1, T_TEXT, (const unsigned char *)"r", 1, &text) == TF_OK);
    CHECK(tf_test_swap_refcount(&ctx, text, UINT64_MAX - 1) == 1);
    tf_stack_body changed[5];
    memcpy(changed, bodies, sizeof changed);
    const tf_type_id text_parameters[] = {T_TEXT, T_TEXT};
    changed[0].parameters = (tf_rt_shape){text_parameters, 2};
    const tf_stack_program changed_program = {changed, 5, ENTRY};
    s.program = &changed_program;
    const tf_value repeated[] = {text, text};
    CHECK(tf_stack_run(&s, 104, SOURCE, NULL, 0, repeated, 2, &out) == TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT && tf_stack_fault(&s)->code == TF_R003);
    CHECK(tf_test_swap_refcount(&ctx, text, 1) == UINT64_MAX - 1);
    tf_release(&text);
    clean(&ctx, &s, &t);

    t = (tracker){0};
    const tf_rt_config config = {{tracked_allocate, tracked_free, &t}, SIZE_MAX, 1024};
    CHECK(tf_context_init(&ctx, &values, &config) == TF_OK);
    CHECK(tf_context_enter(&ctx, SOURCE, 43) == TF_OK);
    CHECK(tf_context_fail(&ctx, TF_R001, 44) == TF_FAULT);
    tf_context_leave(&ctx);
    const tf_fault original = ctx.fault;
    CHECK(tf_stack_init(&s, &ctx, &program, 2049) == TF_FAULT);
    CHECK(run(&s, 2048, NORMAL, &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
    CHECK(ctx.fault.code == original.code && ctx.fault.span == original.span &&
          ctx.fault.frames == original.frames && ctx.fault.frame_count == original.frame_count);
    CHECK(tf_stack_fault(&s)->frames[0].call_span == 43 && t.attempts == 0);
    clean(&ctx, &s, &t);
}

int main(void) {
    test_deep();
    test_limits();
    test_allocations();
    test_invalid();
    test_facade_and_root_validation();
    test_borrowed_and_first_fault();
    puts("runtime stack: PASS (2049 frames, 256 slots; source/callback return, fault, limits, allocation/retain failures)");
    return EXIT_SUCCESS;
}
