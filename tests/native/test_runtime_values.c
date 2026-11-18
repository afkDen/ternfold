#include "ternfold_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
    exit(EXIT_FAILURE); \
} } while (0)

enum {
    T_INT = 1, T_BOOL, T_CHAR, T_FLOAT, T_UNIT, T_TEXT, T_LIST, T_RECORD,
    T_MACHINE, T_VARIANT, T_FN, T_OTHER_RECORD, T_UNUSED, T_BAD_LIST, T_BAD_RECORD
};
enum { F_GLOBAL = 1, F_CAPTURE, F_LINK, F_BAD };

static const tf_type_id record_fields[] = {T_TEXT, T_LIST};
static const tf_type_id machine_cells[] = {T_RECORD};
static const tf_type_id pair_payload[] = {T_MACHINE, T_TEXT};
static const tf_type_id variant_capture[] = {T_VARIANT};
static const tf_type_id closure_links[] = {T_FN, T_FN};
static const tf_rt_shape variants[] = {{NULL, 0}, {pair_payload, 2}};
static const tf_rt_type types[] = {
    [T_INT - 1] = {.kind = TF_RT_TYPE_INT},
    [T_BOOL - 1] = {.kind = TF_RT_TYPE_BOOL},
    [T_CHAR - 1] = {.kind = TF_RT_TYPE_CHAR},
    [T_FLOAT - 1] = {.kind = TF_RT_TYPE_FLOAT},
    [T_UNIT - 1] = {.kind = TF_RT_TYPE_UNIT},
    [T_TEXT - 1] = {.kind = TF_RT_TYPE_TEXT},
    [T_LIST - 1] = {TF_RT_TYPE_LIST, {.element = T_TEXT}},
    [T_RECORD - 1] = {TF_RT_TYPE_RECORD, {.fields = {record_fields, 2}}},
    [T_MACHINE - 1] = {TF_RT_TYPE_MACHINE, {.fields = {machine_cells, 1}}},
    [T_VARIANT - 1] = {TF_RT_TYPE_VARIANT, {.variants = {variants, 2}}},
    [T_FN - 1] = {.kind = TF_RT_TYPE_FUNCTION},
    [T_OTHER_RECORD - 1] = {TF_RT_TYPE_RECORD, {.fields = {record_fields, 2}}},
    [T_UNUSED - 1] = {.kind = TF_RT_TYPE_UNUSED},
    [T_BAD_LIST - 1] = {TF_RT_TYPE_LIST, {.element = T_UNUSED}},
    [T_BAD_RECORD - 1] = {TF_RT_TYPE_RECORD, {.fields = {NULL, 1}}}
};
static const tf_rt_function functions[] = {
    [F_GLOBAL - 1] = {T_FN, {NULL, 0}},
    [F_CAPTURE - 1] = {T_FN, {variant_capture, 1}},
    [F_LINK - 1] = {T_FN, {closure_links, 2}},
    [F_BAD - 1] = {T_RECORD, {NULL, 0}}
};
static const tf_rt_program program = {
    types, sizeof types / sizeof *types, functions, sizeof functions / sizeof *functions
};

typedef struct { size_t attempts, fail_at, outstanding; } tracker;

static void *tracked_allocate(void *user, size_t bytes) {
    tracker *state = user;
    ++state->attempts;
    if (state->attempts == state->fail_at) return NULL;
    void *ptr = malloc(bytes);
    if (ptr) ++state->outstanding;
    return ptr;
}

static void tracked_free(void *user, void *ptr) {
    tracker *state = user;
    CHECK(ptr && state->outstanding != 0);
    --state->outstanding;
    free(ptr);
}

static void init(tf_context *ctx, tracker *state, size_t budget, size_t call_limit) {
    const tf_rt_config config = {{tracked_allocate, tracked_free, state}, budget, call_limit};
    CHECK(tf_context_init(ctx, &program, &config) == TF_OK);
    CHECK(tf_context_fault(ctx) == NULL);
    CHECK(tf_context_live_bytes(ctx) == 0);
}

static void check_fault(const tf_context *ctx, tf_fault_code code, tf_span_id span) {
    const tf_fault *fault = tf_context_fault(ctx);
    CHECK(fault && fault->code == code && fault->span == span);
}

static void check_empty(const tf_context *ctx, const tracker *state) {
    CHECK(tf_context_live_bytes(ctx) == 0 && state->outstanding == 0);
}

static void test_scalars(void) {
    tf_context ctx;
    tracker state = {0};
    init(&ctx, &state, 0, 0);
    tf_value value;
    CHECK(tf_value_type(&ctx, tf_unit()) == T_UNIT);
    CHECK(tf_retain(&ctx, 1, tf_int(INT64_MIN), &value) == TF_OK);
    CHECK(value.tag == TF_VALUE_INT && value.as.integer == INT64_MIN);
    CHECK(tf_value_type(&ctx, value) == T_INT);
    CHECK(tf_retain(&ctx, 1, tf_bool(true), &value) == TF_OK);
    CHECK(value.tag == TF_VALUE_BOOL && value.as.boolean == 1);
    CHECK(tf_value_type(&ctx, value) == T_BOOL);
    CHECK(tf_bool(false).as.boolean == 0);
    const uint32_t scalars[] = {0, 0xd7ff, 0xe000, 0x10ffff};
    for (size_t i = 0; i < sizeof scalars / sizeof *scalars; ++i) {
        CHECK(tf_char(&ctx, 2, scalars[i], &value) == TF_OK);
        CHECK(value.tag == TF_VALUE_CHAR && value.as.character == scalars[i]);
        CHECK(tf_value_type(&ctx, value) == T_CHAR);
    }
    const uint64_t finite[] = {
        0, 1, UINT64_C(0x7fefffffffffffff), UINT64_C(0xffefffffffffffff),
        UINT64_C(0x8000000000000000)
    };
    for (size_t i = 0; i < sizeof finite / sizeof *finite; ++i) {
        CHECK(tf_float(&ctx, 3, finite[i], &value) == TF_OK);
        CHECK(value.tag == TF_VALUE_FLOAT);
        CHECK(value.as.float_bits == (i == 4 ? 0 : finite[i]));
        CHECK(tf_value_type(&ctx, value) == T_FLOAT);
    }
    const uint64_t nonfinite[] = {
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff0000000000001), UINT64_C(0x7ff8000000000000)
    };
    for (size_t i = 0; i < sizeof nonfinite / sizeof *nonfinite; ++i) {
        init(&ctx, &state, 0, 0);
        CHECK(tf_float(&ctx, 4, nonfinite[i], &value) == TF_FAULT);
        CHECK(value.tag == TF_VALUE_UNIT);
        check_fault(&ctx, TF_R001, 4);
    }
    const uint32_t invalid[] = {0xd800, 0xdfff, 0x110000, UINT32_MAX};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        init(&ctx, &state, 0, 0);
        value = tf_int(42);
        CHECK(tf_char(&ctx, 5, invalid[i], &value) == TF_FAULT);
        CHECK(value.tag == TF_VALUE_UNIT);
        check_fault(&ctx, TF_R004, 5);
    }
    CHECK(state.attempts == 0);
    check_empty(&ctx, &state);
}

static void test_text(void) {
    /* NUL, ASCII, then boundary code points in every UTF-8 width. */
    static const unsigned char expected[] = {
        0, 0x7f, 0xc2, 0x80, 0xdf, 0xbf, 0xe0, 0xa0, 0x80, 0xed, 0x9f, 0xbf,
        0xee, 0x80, 0x80, 0xef, 0xbf, 0xbf, 0xf0, 0x90, 0x80, 0x80, 0xf4, 0x8f, 0xbf, 0xbf
    };
    tf_context ctx;
    tracker state = {0};
    init(&ctx, &state, 4096, 0);
    unsigned char input[sizeof expected];
    memcpy(input, expected, sizeof expected);
    tf_value text, copy;
    CHECK(tf_text(&ctx, 11, T_TEXT, input, sizeof input, &text) == TF_OK);
    CHECK(tf_retain(&ctx, 12, text, &copy) == TF_OK);
    memset(input, 0xff, sizeof input);
    tf_release(&text);
    CHECK(text.tag == TF_VALUE_UNIT && tf_value_type(&ctx, copy) == T_TEXT);
    const unsigned char *view;
    size_t length;
    CHECK(tf_text_view(&ctx, 13, copy, &view, &length) == TF_OK);
    CHECK(length == sizeof expected && memcmp(view, expected, length) == 0);
    tf_release(&copy);
    CHECK(tf_text(&ctx, 14, T_TEXT, NULL, 0, &text) == TF_OK);
    CHECK(tf_text_view(&ctx, 15, text, &view, &length) == TF_OK);
    CHECK(length == 0 && view == NULL);
    tf_release(&text);
    check_empty(&ctx, &state);

    static const struct { unsigned char bytes[4]; size_t length; } invalid[] = {
        {{0x80}, 1}, {{0xc0, 0x80}, 2}, {{0xc2}, 1}, {{0xc2, 0x00}, 2},
        {{0xe0, 0x9f, 0xbf}, 3}, {{0xed, 0xa0, 0x80}, 3},
        {{0xf0, 0x8f, 0xbf, 0xbf}, 4}, {{0xf4, 0x90, 0x80, 0x80}, 4},
        {{0xf4, 0x8f, 0xbf}, 3}, {{0xf5, 0x80, 0x80, 0x80}, 4}
    };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        init(&ctx, &state, 4096, 0);
        CHECK(tf_text(&ctx, 16, T_TEXT, invalid[i].bytes, invalid[i].length, &text) == TF_FAULT);
        CHECK(text.tag == TF_VALUE_UNIT);
        check_fault(&ctx, TF_R004, 16);
        check_empty(&ctx, &state);
    }
    CHECK(state.attempts == 2);
}

/* Build a graph with sharing across a list, nominal aggregates and a closure.
 * One cleanup block owns every initialized slot, as generated C will. */
static tf_status build_graph(tf_context *ctx, tf_value *out) {
    const unsigned char bytes[] = {0x61, 0, 0x62};
    tf_value text = tf_unit(), list = tf_unit(), record = tf_unit();
    tf_value machine = tf_unit(), variant = tf_unit();
    tf_status status = TF_FAULT;
    *out = tf_unit();
    if (tf_text(ctx, 21, T_TEXT, bytes, sizeof bytes, &text) != TF_OK) goto done;
    if (tf_list(ctx, 22, T_LIST, (tf_value[]){text, text}, 2, &list) != TF_OK) goto done;
    if (tf_record(ctx, 23, T_RECORD, (tf_value[]){text, list}, 2, &record) != TF_OK) goto done;
    if (tf_machine(ctx, 24, T_MACHINE, &record, 1, &machine) != TF_OK) goto done;
    if (tf_variant(ctx, 25, T_VARIANT, 1, (tf_value[]){machine, text}, 2, &variant) != TF_OK) {
        goto done;
    }
    status = tf_closure(ctx, 26, F_CAPTURE, &variant, 1, out);
done:
    tf_release(&variant);
    tf_release(&machine);
    tf_release(&record);
    tf_release(&list);
    tf_release(&text);
    return status;
}

static void test_graph_and_allocation_failures(void) {
    tf_context ctx;
    tracker state = {0};
    tf_value closure;
    init(&ctx, &state, 4096, 0);
    CHECK(build_graph(&ctx, &closure) == TF_OK);
    CHECK(state.attempts == 6 && state.outstanding == 6);
    CHECK(tf_value_type(&ctx, closure) == T_FN);
    size_t count, tag;
    tf_function_id function;
    const tf_value *captures;
    CHECK(tf_closure_view(&ctx, 27, closure, &function, &captures, &count) == TF_OK);
    CHECK(function == F_CAPTURE && count == 1 && tf_value_type(&ctx, captures[0]) == T_VARIANT);
    tf_value variant, machine, record, list, item, other;
    CHECK(tf_retain(&ctx, 28, captures[0], &variant) == TF_OK);
    tf_release(&closure);
    CHECK(state.outstanding == 5);
    CHECK(tf_variant_tag(&ctx, 29, variant, &tag) == TF_OK && tag == 1);
    CHECK(tf_payload(&ctx, 30, variant, 1, 0, &machine) == TF_OK);
    CHECK(tf_payload(&ctx, 30, variant, 1, 1, &other) == TF_OK);
    tf_release(&variant);
    CHECK(tf_value_type(&ctx, machine) == T_MACHINE);
    CHECK(tf_project(&ctx, 31, machine, 0, &record) == TF_OK);
    tf_release(&machine);
    CHECK(tf_value_type(&ctx, record) == T_RECORD);
    CHECK(tf_project(&ctx, 32, record, 1, &list) == TF_OK);
    tf_release(&record);
    CHECK(tf_list_length(&ctx, 33, list, &count) == TF_OK && count == 2);
    CHECK(tf_list_at(&ctx, 34, list, 1, &item) == TF_OK);
    tf_release(&list);
    tf_release(&other);
    CHECK(state.outstanding == 1 && state.attempts == 6);
    const unsigned char *bytes;
    CHECK(tf_text_view(&ctx, 35, item, &bytes, &count) == TF_OK);
    CHECK(count == 3 && bytes[0] == 0x61 && bytes[1] == 0 && bytes[2] == 0x62);
    tf_release(&item);
    check_empty(&ctx, &state);

    for (size_t failure = 1; failure <= 6; ++failure) {
        state = (tracker){.fail_at = failure};
        init(&ctx, &state, 4096, 0);
        CHECK(build_graph(&ctx, &closure) == TF_FAULT);
        CHECK(closure.tag == TF_VALUE_UNIT && state.attempts == failure);
        check_fault(&ctx, TF_R003, 20 + failure);
        check_empty(&ctx, &state);
    }
    state = (tracker){0};
    init(&ctx, &state, 4096, 0);
    CHECK(build_graph(&ctx, &closure) == TF_OK);
    CHECK(tf_closure_view(&ctx, 36, closure, &function, &captures, &count) == TF_OK);
    CHECK(tf_payload(&ctx, 37, captures[0], 0, 0, &item) == TF_FAULT);
    CHECK(item.tag == TF_VALUE_UNIT);
    check_fault(&ctx, TF_R004, 37);
    tf_release(&closure);
    check_empty(&ctx, &state);
}

static void test_retain_rollback(void) {
    tf_context ctx;
    tracker state = {0};
    init(&ctx, &state, 4096, 0);
    tf_value first, full, out;
    CHECK(tf_text(&ctx, 40, T_TEXT, NULL, 0, &first) == TF_OK);
    CHECK(tf_text(&ctx, 40, T_TEXT, NULL, 0, &full) == TF_OK);
    size_t prior = tf_context_live_bytes(&ctx);
    CHECK(tf_test_swap_refcount(&ctx, full, UINT64_MAX - 1) == 1);
    /* A distinct child and the first edge to full succeed; the repeat fails. */
    const tf_value children[] = {first, full, full};
    CHECK(tf_list(&ctx, 41, T_LIST, children, 3, &out) == TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT && state.attempts == 3 && state.outstanding == 2);
    CHECK(tf_context_live_bytes(&ctx) == prior);
    check_fault(&ctx, TF_R003, 41);
    CHECK(tf_test_swap_refcount(&ctx, full, 1) == UINT64_MAX - 1);
    tf_release(&full);
    tf_release(&first);
    check_empty(&ctx, &state);

    for (int project = 0; project <= 1; ++project) {
        init(&ctx, &state, 4096, 0);
        tf_value list;
        CHECK(tf_text(&ctx, 42, T_TEXT, NULL, 0, &first) == TF_OK);
        CHECK(tf_list(&ctx, 42, T_LIST, &first, 1, &list) == TF_OK);
        CHECK(tf_test_swap_refcount(&ctx, first, UINT64_MAX) == 2);
        CHECK((project ? tf_list_at(&ctx, 43, list, 0, &out)
                       : tf_retain(&ctx, 43, first, &out)) == TF_FAULT);
        CHECK(out.tag == TF_VALUE_UNIT);
        check_fault(&ctx, TF_R003, 43);
        CHECK(tf_test_swap_refcount(&ctx, first, 2) == UINT64_MAX);
        tf_release(&list);
        tf_release(&first);
        check_empty(&ctx, &state);
    }
}

static void test_invalid_shapes(void) {
    tf_context ctx, other_ctx;
    tracker state = {0}, other_state = {0};
    init(&other_ctx, &other_state, 4096, 0);
    tf_value foreign;
    CHECK(tf_text(&other_ctx, 50, T_TEXT, NULL, 0, &foreign) == TF_OK);
    for (size_t kind = 0; kind < 14; ++kind) {
        init(&ctx, &state, 4096, 0);
        tf_value text, list, record, variant, out = tf_int(42);
        CHECK(tf_text(&ctx, 50, T_TEXT, NULL, 0, &text) == TF_OK);
        CHECK(tf_list(&ctx, 50, T_LIST, NULL, 0, &list) == TF_OK);
        CHECK(tf_record(&ctx, 50, T_OTHER_RECORD, (tf_value[]){text, list}, 2, &record) == TF_OK);
        CHECK(tf_variant(&ctx, 50, T_VARIANT, 0, NULL, 0, &variant) == TF_OK);
        CHECK(tf_value_type(&ctx, record) == T_OTHER_RECORD);
        size_t tag, count;
        CHECK(tf_variant_tag(&ctx, 50, variant, &tag) == TF_OK && tag == 0);
        CHECK(tf_list_length(&ctx, 50, list, &count) == TF_OK && count == 0);
        size_t attempts = state.attempts;
        tf_status status;
        switch (kind) {
            case 0: status = tf_list(&ctx, 51, T_LIST, NULL, 1, &out); break;
            case 1: status = tf_list(&ctx, 51, T_BAD_LIST, NULL, 0, &out); break;
            case 2: status = tf_list(&ctx, 51, T_LIST, &foreign, 1, &out); break;
            case 3: status = tf_list(&ctx, 51, T_LIST, (tf_value[]){tf_int(1)}, 1, &out); break;
            case 4: status = tf_record(&ctx, 51, T_RECORD, &text, 1, &out); break;
            case 5: status = tf_record(&ctx, 51, T_BAD_RECORD, &text, 1, &out); break;
            case 6: status = tf_record(&ctx, 51, T_RECORD, (tf_value[]){list, text}, 2, &out); break;
            case 7: status = tf_machine(&ctx, 51, T_MACHINE, &record, 1, &out); break;
            case 8: status = tf_variant(&ctx, 51, T_VARIANT, 2, NULL, 0, &out); break;
            case 9: status = tf_variant(&ctx, 51, T_VARIANT, 0, &text, 1, &out); break;
            case 10: status = tf_closure(&ctx, 51, F_CAPTURE, &text, 1, &out); break;
            case 11: status = tf_closure(&ctx, 51, F_BAD, NULL, 0, &out); break;
            case 12: status = tf_list_at(&ctx, 51, list, 0, &out); break;
            default: status = tf_project(&ctx, 51, record, 2, &out); break;
        }
        CHECK(status == TF_FAULT && out.tag == TF_VALUE_UNIT && state.attempts == attempts);
        check_fault(&ctx, TF_R004, 51);
        tf_release(&variant);
        tf_release(&record);
        tf_release(&list);
        tf_release(&text);
        check_empty(&ctx, &state);
        CHECK(other_state.outstanding == 1);
    }
    tf_release(&foreign);
    check_empty(&other_ctx, &other_state);
}

static void test_budget_and_frames(void) {
    tf_context ctx;
    tracker state = {0};
    tf_value text, out;
    const unsigned char one[] = {0x61};
    init(&ctx, &state, 4096, 2);
    CHECK(tf_text(&ctx, 60, T_TEXT, one, sizeof one, &text) == TF_OK);
    size_t one_box = tf_context_live_bytes(&ctx);
    CHECK(one_box > sizeof one);
    tf_release(&text);
    init(&ctx, &state, one_box, 2);
    for (size_t i = 0; i < 2; ++i) {
        CHECK(tf_text(&ctx, 61, T_TEXT, one, sizeof one, &text) == TF_OK);
        CHECK(tf_context_live_bytes(&ctx) == one_box);
        tf_release(&text);
        check_empty(&ctx, &state);
    }
    CHECK(tf_text(&ctx, 61, T_TEXT, one, sizeof one, &text) == TF_OK);
    CHECK(tf_context_enter(&ctx, F_GLOBAL, 62) == TF_OK);
    CHECK(tf_context_enter(&ctx, F_CAPTURE, 63) == TF_OK);
    size_t attempts = state.attempts;
    CHECK(tf_text(&ctx, 64, T_TEXT, NULL, 0, &out) == TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT && state.attempts == attempts);
    CHECK(tf_context_enter(&ctx, F_LINK, 65) == TF_FAULT);
    CHECK(tf_context_fail(&ctx, TF_R002, 66) == TF_FAULT);
    tf_context_leave(&ctx);
    tf_context_leave(&ctx);
    check_fault(&ctx, TF_R003, 64);
    const tf_fault *fault = tf_context_fault(&ctx);
    CHECK(fault->frame_count == 2);
    CHECK(fault->frames[0].function == F_GLOBAL && fault->frames[0].call_span == 62);
    CHECK(fault->frames[1].function == F_CAPTURE && fault->frames[1].call_span == 63);
    const unsigned char *view = one;
    size_t length = 7;
    CHECK(tf_text_view(&ctx, 67, text, &view, &length) == TF_FAULT);
    CHECK(view == NULL && length == 0);
    CHECK(tf_retain(&ctx, 67, text, &out) == TF_FAULT && out.tag == TF_VALUE_UNIT);
    CHECK(tf_value_type(&ctx, text) == T_TEXT);
    tf_release(&text);
    check_empty(&ctx, &state);

    init(&ctx, &state, 0, 0);
    CHECK(tf_context_enter(&ctx, F_GLOBAL, 68) == TF_FAULT);
    check_fault(&ctx, TF_R003, 68);
    CHECK(tf_context_fault(&ctx)->frame_count == 0);
    CHECK(tf_context_init(&ctx, &program, NULL) == TF_OK);
    CHECK(tf_text(&ctx, 68, T_TEXT, NULL, 0, &text) == TF_OK);
    tf_release(&text);
    CHECK(tf_context_live_bytes(&ctx) == 0);
    for (size_t i = 0; i < TF_RT_MAX_CALL_DEPTH; ++i) {
        CHECK(tf_context_enter(&ctx, F_GLOBAL, i + 1) == TF_OK);
    }
    CHECK(tf_context_enter(&ctx, F_GLOBAL, 9000) == TF_FAULT);
    check_fault(&ctx, TF_R003, 9000);
    for (size_t i = 0; i < TF_RT_MAX_CALL_DEPTH; ++i) tf_context_leave(&ctx);
    fault = tf_context_fault(&ctx);
    CHECK(fault->frame_count == TF_RT_MAX_CALL_DEPTH);
    CHECK(fault->frames[TF_RT_MAX_CALL_DEPTH - 1].call_span == TF_RT_MAX_CALL_DEPTH);

    const tf_rt_config bad = {{tracked_allocate, NULL, &state}, 0, 0};
    CHECK(tf_context_init(&ctx, &program, &bad) == TF_FAULT);
    check_fault(&ctx, TF_R004, 0);

    init(&ctx, &state, SIZE_MAX, 0);
    CHECK(tf_text(&ctx, 69, T_TEXT, one, SIZE_MAX, &out) == TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT);
    check_fault(&ctx, TF_R003, 69);
    init(&ctx, &state, SIZE_MAX, 0);
    tf_value scalar = tf_int(1);
    CHECK(tf_list(&ctx, 70, T_LIST, &scalar, SIZE_MAX / sizeof(tf_value) + 1, &out) == TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT && state.attempts == attempts);
    check_fault(&ctx, TF_R003, 70);
    check_empty(&ctx, &state);
}

static void test_deep_shared_graph(void) {
    const size_t depth = 100000;
    tf_context ctx;
    tracker state = {0};
    init(&ctx, &state, 32u * 1024u * 1024u, 0);
    tf_value root, leaf;
    CHECK(tf_closure(&ctx, 71, F_GLOBAL, NULL, 0, &root) == TF_OK);
    const tf_value *captures;
    size_t count;
    tf_function_id id;
    CHECK(tf_closure_view(&ctx, 72, root, &id, &captures, &count) == TF_OK);
    CHECK(id == F_GLOBAL && captures == NULL && count == 0);
    CHECK(tf_retain(&ctx, 73, root, &leaf) == TF_OK);
    for (size_t i = 0; i < depth; ++i) {
        tf_value next;
        CHECK(tf_closure(&ctx, 74, F_LINK, (tf_value[]){root, root}, 2, &next) == TF_OK);
        tf_release(&root);
        root = next;
        next = tf_unit(); /* move */
    }
    CHECK(state.outstanding == depth + 1);
    state.fail_at = state.attempts + 1;
    CHECK(tf_context_fail(&ctx, TF_R004, 75) == TF_FAULT);
    tf_release(&root);
    CHECK(root.tag == TF_VALUE_UNIT && state.outstanding == 1 && state.attempts == depth + 1);
    CHECK(tf_value_type(&ctx, leaf) == T_FN);
    tf_release(&leaf);
    check_empty(&ctx, &state);
}

int main(void) {
    test_scalars();
    test_text();
    test_graph_and_allocation_failures();
    test_retain_rollback();
    test_invalid_shapes();
    test_budget_and_frames();
    test_deep_shared_graph();
    puts("runtime values: all tests passed (6 allocation failpoints, 100000 shared closure levels)");
    return EXIT_SUCCESS;
}
