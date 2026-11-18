#include "ternfold_rt_value_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

enum { UNIT = 1, INT, FLOAT, BOOL, CHAR, TEXT, INTS, TEXTS, RECORD,
       MACHINE, ENUM, DECISION, FUNCTION, FUNCTIONS, BAD_RECORD, BAD_ENUM,
       OTHER_RECORD, LISTS };
static const tf_type_id fields[] = {INT, TEXT};
static const tf_type_id cells[] = {INT, TEXTS};
static const tf_type_id accept[] = {MACHINE, TEXTS};
static const tf_type_id reject[] = {MACHINE, TEXT};
static const tf_type_id function_field[] = {FUNCTION};
static const tf_rt_shape tags[] = {{NULL, 0}, {fields, 2}};
static const tf_rt_shape decisions[] = {{accept, 2}, {reject, 2}};
static const tf_rt_shape bad_tags[] = {{NULL, 0}, {function_field, 1}};
static const tf_rt_type types[] = {
    {.kind = TF_RT_TYPE_UNIT}, {.kind = TF_RT_TYPE_INT},
    {.kind = TF_RT_TYPE_FLOAT}, {.kind = TF_RT_TYPE_BOOL},
    {.kind = TF_RT_TYPE_CHAR}, {.kind = TF_RT_TYPE_TEXT},
    {TF_RT_TYPE_LIST, {.element = INT}}, {TF_RT_TYPE_LIST, {.element = TEXT}},
    {TF_RT_TYPE_RECORD, {.fields = {fields, 2}}},
    {TF_RT_TYPE_MACHINE, {.fields = {cells, 2}}},
    {TF_RT_TYPE_VARIANT, {.variants = {tags, 2}}},
    {TF_RT_TYPE_VARIANT, {.variants = {decisions, 2}}},
    {.kind = TF_RT_TYPE_FUNCTION},
    {TF_RT_TYPE_LIST, {.element = FUNCTION}},
    {TF_RT_TYPE_RECORD, {.fields = {function_field, 1}}},
    {TF_RT_TYPE_VARIANT, {.variants = {bad_tags, 2}}},
    {TF_RT_TYPE_RECORD, {.fields = {fields, 2}}},
    {TF_RT_TYPE_LIST, {.element = TEXTS}}
};
static const tf_rt_function functions[] = {{FUNCTION, {NULL, 0}}};
static const tf_rt_program program = {types, sizeof types / sizeof *types, functions, 1};

typedef struct { size_t calls, fail_at, live; } tracker;
static void *allocate(void *user, size_t bytes) {
    tracker *state = user;
    CHECK(bytes != 0);
    if (++state->calls == state->fail_at) return NULL;
    void *memory = malloc(bytes);
    if (memory) ++state->live;
    return memory;
}
static void deallocate(void *user, void *memory) {
    tracker *state = user;
    CHECK(memory && state->live != 0);
    --state->live;
    free(memory);
}
static void init_program(tf_context *ctx, tracker *state, const tf_rt_program *p) {
    tf_rt_config config = {{allocate, deallocate, state}, 64u * 1024u * 1024u, 0};
    CHECK(tf_context_init(ctx, p, &config) == TF_OK);
}
static void init(tf_context *ctx, tracker *state) { init_program(ctx, state, &program); }
static void empty(tf_context *ctx, tracker *state) {
    CHECK(tf_context_live_bytes(ctx) == 0 && state->live == 0);
}
static void fault(tf_context *ctx, tf_fault_code code, tf_span_id span, tf_value out) {
    const tf_fault *f = tf_context_fault(ctx);
    CHECK(f && f->code == code && f->span == span && out.tag == TF_VALUE_UNIT);
}
static tf_value text(tf_context *ctx, const unsigned char *bytes, size_t count) {
    tf_value result;
    CHECK(tf_text(ctx, 1, TEXT, bytes, count, &result) == TF_OK);
    return result;
}
static void equal(tf_context *ctx, tf_value a, tf_value b, bool expected) {
    tf_value result;
    size_t before = tf_context_live_bytes(ctx);
    CHECK(tf_value_equal(ctx, 91, a, b, &result) == TF_OK);
    CHECK(result.tag == TF_VALUE_BOOL && result.as.boolean == (uint8_t)expected);
    CHECK(tf_context_live_bytes(ctx) == before);
}

static void test_scalars(void) {
    tf_context ctx;
    tracker state = {0};
    init(&ctx, &state);
    ctx.config.byte_budget = 0; /* inline equality never allocates */
    equal(&ctx, tf_unit(), tf_unit(), true);
    equal(&ctx, tf_int(INT64_MIN), tf_int(INT64_MIN), true);
    equal(&ctx, tf_int(INT64_MIN), tf_int(INT64_MAX), false);
    equal(&ctx, tf_bool(false), tf_bool(true), false);
    equal(&ctx, tf_bool(true), tf_bool(true), true);
    tf_value a, b;
    CHECK(tf_char(&ctx, 1, 0, &a) == TF_OK);
    CHECK(tf_char(&ctx, 1, 0x10ffff, &b) == TF_OK);
    equal(&ctx, a, b, false); equal(&ctx, b, b, true);
    const uint64_t bits[] = {0, UINT64_C(0x8000000000000000), 1,
        UINT64_C(0x3fb999999999999a), UINT64_C(0x3fb999999999999b),
        UINT64_C(0x7fefffffffffffff), UINT64_C(0xffefffffffffffff)};
    for (size_t i = 0; i < sizeof bits / sizeof *bits; ++i) {
        for (size_t j = 0; j < sizeof bits / sizeof *bits; ++j) {
            CHECK(tf_float(&ctx, 1, bits[i], &a) == TF_OK);
            CHECK(tf_float(&ctx, 1, bits[j], &b) == TF_OK);
            equal(&ctx, a, b, a.as.float_bits == b.as.float_bits);
        }
    }
    tf_value out;
    CHECK(tf_value_equal(&ctx, 17, tf_int(0), tf_bool(false), &out) == TF_FAULT);
    fault(&ctx, TF_R004, 17, out);
    CHECK(tf_text_concat(&ctx, 99, tf_unit(), tf_unit(), &out) == TF_FAULT);
    fault(&ctx, TF_R004, 17, out);
    empty(&ctx, &state);
    const tf_value invalid[] = {
        {TF_VALUE_FLOAT, {.float_bits = UINT64_C(0x7ff0000000000000)}},
        {TF_VALUE_FLOAT, {.float_bits = UINT64_C(0x8000000000000000)}},
        {TF_VALUE_BOOL, {.boolean = 2}}, {TF_VALUE_CHAR, {.character = 0xd800}}
    };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        init(&ctx, &state);
        CHECK(tf_value_equal(&ctx, 18, invalid[i], invalid[i], &out) == TF_FAULT);
        fault(&ctx, TF_R004, 18, out);
        empty(&ctx, &state);
    }
}

static void test_text(void) {
    tf_context ctx;
    tracker state = {0};
    init(&ctx, &state);
    const unsigned char bytes[] = {'A', 0, 0xc3, 0xb1, 0xf0, 0x9f, 0x99, 0x82};
    tf_value a = text(&ctx, bytes, sizeof bytes), b = text(&ctx, bytes, sizeof bytes);
    tf_value e = text(&ctx, NULL, 0), out;
    equal(&ctx, a, b, true); equal(&ctx, a, e, false); equal(&ctx, a, a, true);
    const unsigned char decomposed[] = {'A', 0, 'n', 0xcc, 0x83, 0xf0, 0x9f, 0x99, 0x82};
    tf_value d = text(&ctx, decomposed, sizeof decomposed);
    equal(&ctx, a, d, false);
    unsigned char changed[sizeof bytes];
    memcpy(changed, bytes, sizeof bytes); changed[sizeof bytes - 1] = 0x83;
    tf_value after_nul = text(&ctx, changed, sizeof changed);
    equal(&ctx, a, after_nul, false); tf_release(&after_nul);
    CHECK(tf_text_concat(&ctx, 1, a, b, &out) == TF_OK);
    const unsigned char *view;
    size_t length;
    CHECK(tf_text_view(&ctx, 1, out, &view, &length) == TF_OK);
    CHECK(length == 2 * sizeof bytes && memcmp(view, bytes, sizeof bytes) == 0 &&
          memcmp(view + sizeof bytes, bytes, sizeof bytes) == 0);
    tf_release(&out);
    CHECK(tf_text_concat(&ctx, 1, e, a, &out) == TF_OK); equal(&ctx, a, out, true); tf_release(&out);
    CHECK(tf_text_concat(&ctx, 1, a, e, &out) == TF_OK); equal(&ctx, a, out, true); tf_release(&out);
    CHECK(tf_text_concat(&ctx, 1, e, e, &out) == TF_OK); equal(&ctx, e, out, true); tf_release(&out);
    CHECK(tf_text_concat(&ctx, 1, a, a, &out) == TF_OK);
    tf_release(&a); tf_release(&b); tf_release(&d); tf_release(&e);
    CHECK(tf_text_view(&ctx, 1, out, &view, &length) == TF_OK && length == 2 * sizeof bytes);
    tf_release(&out);
    empty(&ctx, &state);
}

static void test_aggregates(void) {
    tf_context ctx;
    tracker state = {0};
    init(&ctx, &state);
    const unsigned char bytes[] = {0, 'x'};
    tf_value a = text(&ctx, bytes, sizeof bytes), b = text(&ctx, bytes, sizeof bytes);
    tf_value la, lb, ra, rb, ma, mb, ea, eb, da, db, out;
    CHECK(tf_list(&ctx, 1, TEXTS, &a, 1, &la) == TF_OK);
    CHECK(tf_list(&ctx, 1, TEXTS, &b, 1, &lb) == TF_OK);
    tf_value fa[] = {tf_int(7), a}, fb[] = {tf_int(7), b};
    CHECK(tf_record(&ctx, 1, RECORD, fa, 2, &ra) == TF_OK);
    CHECK(tf_record(&ctx, 1, RECORD, fb, 2, &rb) == TF_OK);
    tf_value ca[] = {tf_int(7), la}, cb[] = {tf_int(7), lb};
    CHECK(tf_machine(&ctx, 1, MACHINE, ca, 2, &ma) == TF_OK);
    CHECK(tf_machine(&ctx, 1, MACHINE, cb, 2, &mb) == TF_OK);
    CHECK(tf_variant(&ctx, 1, ENUM, 1, fa, 2, &ea) == TF_OK);
    CHECK(tf_variant(&ctx, 1, ENUM, 1, fb, 2, &eb) == TF_OK);
    tf_value aa[] = {ma, la}, ab[] = {mb, lb};
    CHECK(tf_variant(&ctx, 1, DECISION, 0, aa, 2, &da) == TF_OK);
    CHECK(tf_variant(&ctx, 1, DECISION, 0, ab, 2, &db) == TF_OK);
    equal(&ctx, la, lb, true); equal(&ctx, ra, rb, true); equal(&ctx, ma, mb, true);
    equal(&ctx, ea, eb, true); equal(&ctx, da, db, true);
    tf_value different;
    CHECK(tf_list(&ctx, 1, TEXTS, NULL, 0, &different) == TF_OK);
    equal(&ctx, la, different, false); tf_release(&different);
    tf_value changed_cells[] = {tf_int(-1), lb};
    CHECK(tf_machine(&ctx, 1, MACHINE, changed_cells, 2, &different) == TF_OK);
    equal(&ctx, ma, different, false); tf_release(&different);
    tf_value changed_fields[] = {tf_int(-1), b};
    CHECK(tf_variant(&ctx, 1, ENUM, 1, changed_fields, 2, &different) == TF_OK);
    equal(&ctx, ea, different, false); tf_release(&different);
    tf_release(&db);
    tf_value rb_payload[] = {mb, b};
    CHECK(tf_variant(&ctx, 1, DECISION, 1, rb_payload, 2, &db) == TF_OK);
    equal(&ctx, da, db, false);
    tf_release(&da);
    tf_value ra_payload[] = {ma, a};
    CHECK(tf_variant(&ctx, 1, DECISION, 1, ra_payload, 2, &da) == TF_OK);
    equal(&ctx, da, db, true);
    tf_release(&eb); CHECK(tf_variant(&ctx, 1, ENUM, 0, NULL, 0, &eb) == TF_OK);
    equal(&ctx, ea, eb, false);
    tf_release(&rb); fb[0] = tf_int(8);
    CHECK(tf_record(&ctx, 1, RECORD, fb, 2, &rb) == TF_OK);
    equal(&ctx, ra, rb, false);
    CHECK(tf_list_concat(&ctx, 1, la, lb, &out) == TF_OK);
    size_t length;
    CHECK(tf_list_length(&ctx, 1, la, &length) == TF_OK && length == 1);
    tf_release(&la); tf_release(&lb); tf_release(&ra); tf_release(&rb);
    tf_release(&ma); tf_release(&mb); tf_release(&ea); tf_release(&eb);
    tf_release(&da); tf_release(&db); tf_release(&a); tf_release(&b);
    CHECK(tf_list_length(&ctx, 1, out, &length) == TF_OK && length == 2);
    tf_value projected;
    CHECK(tf_list_at(&ctx, 1, out, 1, &projected) == TF_OK);
    const unsigned char *view;
    CHECK(tf_text_view(&ctx, 1, projected, &view, &length) == TF_OK && length == sizeof bytes);
    CHECK(memcmp(view, bytes, length) == 0);
    tf_release(&projected); tf_release(&out);
    empty(&ctx, &state);
}

static void test_incapable_and_mismatch(void) {
    for (size_t which = 0; which < 7; ++which) {
        tf_context ctx;
        tracker state = {0};
        init(&ctx, &state);
        tf_value fn, a, b, out;
        CHECK(tf_closure(&ctx, 1, 1, NULL, 0, &fn) == TF_OK);
        a = tf_unit(); b = tf_unit();
        if (which == 0) CHECK(tf_retain(&ctx, 1, fn, &a) == TF_OK);
        if (which == 1) CHECK(tf_list(&ctx, 1, FUNCTIONS, NULL, 0, &a) == TF_OK);
        if (which == 2) CHECK(tf_record(&ctx, 1, BAD_RECORD, &fn, 1, &a) == TF_OK);
        if (which == 3) CHECK(tf_variant(&ctx, 1, BAD_ENUM, 0, NULL, 0, &a) == TF_OK);
        if (which >= 4) {
            tf_value t = text(&ctx, NULL, 0), f[] = {tf_int(1), t};
            CHECK(tf_record(&ctx, 1, RECORD, f, 2, &a) == TF_OK);
            CHECK(tf_record(&ctx, 1, OTHER_RECORD, f, 2, &b) == TF_OK);
            tf_release(&t);
        }
        tf_status status;
        if (which == 4) status = tf_value_equal(&ctx, 51, a, b, &out);
        else if (which == 5) status = tf_text_concat(&ctx, 51, a, a, &out);
        else if (which == 6) status = tf_list_concat(&ctx, 51, a, a, &out);
        else status = tf_value_equal(&ctx, 51, a, a, &out);
        CHECK(status == TF_FAULT); fault(&ctx, TF_R004, 51, out);
        tf_release(&a); tf_release(&b); tf_release(&fn); empty(&ctx, &state);
    }
}

static tf_status node(tf_context *ctx, size_t level, const tf_value *items, tf_value *out) {
    return level % 2 != 0 ? tf_list(ctx, 1, level + 2, items, 2, out) :
                            tf_record(ctx, 1, level + 2, items, 2, out);
}

/* Finite, acyclic mixed List/record chain. Each level shares both edges to its predecessor:
 * the unfolded tree has 2^depth leaves, but equality must visit O(depth) pairs.
 * Separate left/right graphs prevent identity shortcuts from masking this. */
static void test_deep_shared(bool failure_sweep) {
    const size_t depth = failure_sweep ? 80 : 12000;
    tf_rt_type *layouts = malloc((depth + 1) * sizeof *layouts);
    tf_type_id (*links)[2] = malloc(depth * sizeof *links);
    CHECK(layouts && links);
    layouts[0] = (tf_rt_type){.kind = TF_RT_TYPE_INT};
    for (size_t i = 0; i < depth; ++i) {
        links[i][0] = i + 1; links[i][1] = i + 1;
        layouts[i + 1] = i % 2 != 0 ?
            (tf_rt_type){TF_RT_TYPE_LIST, {.element = i + 1}} :
            (tf_rt_type){TF_RT_TYPE_RECORD, {.fields = {links[i], 2}}};
    }
    tf_rt_program p = {layouts, depth + 1, NULL, 0};
    bool succeeded = false;
    size_t attempts = failure_sweep ? 100 : 1;
    for (size_t nth = 1; nth <= attempts; ++nth) {
        tf_context ctx;
        tracker state = {0};
        init_program(&ctx, &state, &p);
        tf_value a = tf_int(1), b = tf_int(1), c = tf_int(2), out;
        for (size_t i = 0; i < depth; ++i) {
            tf_value aa[] = {a, a}, bb[] = {b, b}, cc[] = {b, c}, next;
            CHECK(node(&ctx, i, aa, &next) == TF_OK); tf_release(&a); a = next;
            CHECK(node(&ctx, i, cc, &next) == TF_OK); tf_release(&c); c = next;
            CHECK(node(&ctx, i, bb, &next) == TF_OK); tf_release(&b); b = next;
        }
        size_t before = ctx.live_bytes, live = state.live;
        if (failure_sweep) state.fail_at = state.calls + nth;
        tf_status status = tf_value_equal(&ctx, 61, a, b, &out);
        CHECK(ctx.live_bytes == before && state.live == live);
        if (status == TF_OK) {
            CHECK(out.tag == TF_VALUE_BOOL && out.as.boolean == 1);
            state.fail_at = 0;
            equal(&ctx, a, c, false);
            equal(&ctx, a, a, true);
            succeeded = true;
        } else fault(&ctx, TF_R003, 61, out);
        tf_release(&a); tf_release(&b); tf_release(&c); empty(&ctx, &state);
        if (succeeded) break;
    }
    CHECK(succeeded);
    free(links); free(layouts);
}

static void test_concat_failure(void) {
    for (size_t operation = 0; operation < 4; ++operation) {
        bool succeeded = false;
        for (size_t nth = 1; nth < 8; ++nth) {
            tf_context ctx;
            tracker state = {0};
            init(&ctx, &state);
            const unsigned char bytes[] = {0, 'a'};
            tf_value t = text(&ctx, bytes, operation >= 2 ? 0 : sizeof bytes), list, out;
            tf_value items[] = {t, t};
            CHECK(tf_list(&ctx, 1, TEXTS, items, operation >= 2 ? 0 : 2, &list) == TF_OK);
            size_t before = ctx.live_bytes, live = state.live;
            state.fail_at = state.calls + nth;
            tf_status status = operation % 2 == 0 ? tf_text_concat(&ctx, 71, t, t, &out) :
                                               tf_list_concat(&ctx, 71, list, list, &out);
            if (status == TF_OK) { tf_release(&out); succeeded = true; }
            else fault(&ctx, TF_R003, 71, out);
            CHECK(ctx.live_bytes == before && state.live == live);
            tf_release(&t); tf_release(&list); empty(&ctx, &state);
            if (succeeded) break;
        }
        CHECK(succeeded);
    }
}

static void test_retain_failure(void) {
    /* Each side's projection may fail; concat may fail while acquiring its
     * prefix or after projection succeeds but the constructor retain fails. */
    for (size_t which = 0; which < 6; ++which) {
        tf_context ctx;
        tracker state = {0};
        init(&ctx, &state);
        tf_value a = text(&ctx, (const unsigned char *)"a", 1);
        tf_value b = text(&ctx, (const unsigned char *)"a", 1), la, lb, out;
        tf_value ia[] = {which == 5 ? a : b, a}, ib[] = {b, b};
        CHECK(tf_list(&ctx, 1, TEXTS, ia, 2, &la) == TF_OK);
        CHECK(tf_list(&ctx, 1, TEXTS, ib, 2, &lb) == TF_OK);
        tf_value target = which == 0 || which == 5 ? b : a;
        uint64_t injected = which == 4 ? UINT64_MAX - 1 : UINT64_MAX;
        uint64_t old = tf_test_swap_refcount(&ctx, target, injected);
        CHECK(old != 0);
        size_t before = ctx.live_bytes, live = state.live;
        tf_status status;
        if (which < 2 || which == 5) status = tf_value_equal(&ctx, 81, la, lb, &out);
        else status = tf_list_concat(&ctx, 81, la, lb, &out);
        CHECK(status == TF_FAULT); fault(&ctx, TF_R003, 81, out);
        CHECK(ctx.live_bytes == before && state.live == live);
        CHECK(tf_test_swap_refcount(&ctx, target, old) == injected);
        tf_release(&a); tf_release(&b); tf_release(&la); tf_release(&lb); empty(&ctx, &state);
    }
}

static void test_empty_large_budget(void) {
    tf_context ctx;
    tracker state = {0};
    init(&ctx, &state);
    tf_value e, a, b, out;
    CHECK(tf_list(&ctx, 1, INTS, NULL, 0, &e) == TF_OK);
    CHECK(tf_list_concat(&ctx, 1, e, e, &out) == TF_OK); equal(&ctx, e, out, true); tf_release(&out);
    size_t count = 50000;
    tf_value *items = malloc(count * sizeof *items);
    CHECK(items);
    for (size_t i = 0; i < count; ++i) items[i] = tf_int((int64_t)i);
    CHECK(tf_list(&ctx, 1, INTS, items, count, &a) == TF_OK);
    CHECK(tf_list(&ctx, 1, INTS, items, count, &b) == TF_OK);
    equal(&ctx, a, b, true);
    CHECK(tf_list_concat(&ctx, 1, e, a, &out) == TF_OK); equal(&ctx, a, out, true); tf_release(&out);
    CHECK(tf_list_concat(&ctx, 1, a, b, &out) == TF_OK);
    tf_value last;
    CHECK(tf_list_at(&ctx, 1, out, count * 2 - 1, &last) == TF_OK && last.as.integer == 49999);
    tf_release(&out); tf_release(&a); tf_release(&b); tf_release(&e); free(items);
    empty(&ctx, &state);
    size_t byte_count = 1024u * 1024u;
    unsigned char *bytes = malloc(byte_count);
    CHECK(bytes);
    memset(bytes, 'x', byte_count); bytes[byte_count / 2] = 0;
    a = text(&ctx, bytes, byte_count); b = text(&ctx, bytes, byte_count);
    equal(&ctx, a, b, true);
    CHECK(tf_text_concat(&ctx, 1, a, b, &out) == TF_OK);
    tf_release(&a); tf_release(&b);
    const unsigned char *view;
    size_t length;
    CHECK(tf_text_view(&ctx, 1, out, &view, &length) == TF_OK && length == byte_count * 2);
    CHECK(memcmp(view, bytes, byte_count) == 0 && memcmp(view + byte_count, bytes, byte_count) == 0);
    tf_release(&out); free(bytes); empty(&ctx, &state);
    for (size_t operation = 0; operation < 3; ++operation) {
        init(&ctx, &state);
        a = text(&ctx, (const unsigned char *)"a", 1);
        CHECK(tf_list(&ctx, 1, TEXTS, &a, 1, &b) == TF_OK);
        size_t before = ctx.live_bytes;
        ctx.config.byte_budget = before;
        tf_status status = operation == 0 ? tf_value_equal(&ctx, 31, b, b, &out) :
            operation == 1 ? tf_text_concat(&ctx, 31, a, a, &out) :
                             tf_list_concat(&ctx, 31, b, b, &out);
        CHECK(status == TF_FAULT); fault(&ctx, TF_R003, 31, out);
        CHECK(ctx.live_bytes == before);
        tf_release(&a); tf_release(&b); empty(&ctx, &state);
    }
}

static void test_foreign_context_and_first_fault(void) {
    for (size_t operation = 0; operation < 3; ++operation) {
        tf_context ctx, other;
        tracker state = {0}, other_state = {0};
        init(&ctx, &state); init(&other, &other_state);
        tf_value local = text(&ctx, NULL, 0), foreign = text(&other, NULL, 0), out;
        tf_status status = operation == 0 ? tf_value_equal(&ctx, 41, local, foreign, &out) :
            operation == 1 ? tf_text_concat(&ctx, 41, local, foreign, &out) :
                             tf_list_concat(&ctx, 41, local, foreign, &out);
        CHECK(status == TF_FAULT); fault(&ctx, TF_R004, 41, out);
        CHECK(tf_context_fail(&ctx, TF_R002, 99) == TF_FAULT);
        fault(&ctx, TF_R004, 41, out);
        size_t calls = state.calls;
        CHECK(tf_value_equal(&ctx, 42, tf_int(1), tf_int(1), &out) == TF_FAULT);
        CHECK(tf_text_concat(&ctx, 43, local, local, &out) == TF_FAULT);
        CHECK(tf_list_concat(&ctx, 44, local, local, &out) == TF_FAULT);
        CHECK(state.calls == calls); fault(&ctx, TF_R004, 41, out);
        tf_release(&local); tf_release(&foreign);
        empty(&ctx, &state); empty(&other, &other_state);
    }
    tf_context ctx;
    tracker state = {0};
    init(&ctx, &state);
    tf_value t = text(&ctx, NULL, 0);
    uint64_t old = tf_test_swap_refcount(&ctx, t, UINT64_MAX);
    equal(&ctx, t, t, true); /* identity shortcut does not acquire an owner */
    CHECK(tf_test_swap_refcount(&ctx, t, old) == UINT64_MAX);
    tf_release(&t); empty(&ctx, &state);
}

int main(void) {
    test_scalars(); test_text(); test_aggregates(); test_incapable_and_mismatch();
    test_deep_shared(false); test_deep_shared(true); test_concat_failure();
    test_retain_failure(); test_empty_large_budget();
    test_foreign_context_and_first_fault();
    puts("runtime value operations: PASS");
    return 0;
}
