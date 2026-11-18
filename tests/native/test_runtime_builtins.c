#include "ternfold_rt_builtins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(test) do { ++checks; if (!(test)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #test); exit(1); } } while (0)

enum {
    T_UNIT = 1, T_INT, T_FLOAT, T_BOOL, T_CHAR, T_TEXT, T_INTS, T_TEXTS,
    T_OPT_INT, T_OPT_TEXT, T_RECORD, T_RECORDS, T_OPT_RECORD, T_BAD_OPTION
};
static const tf_type_id int_field[] = {T_INT};
static const tf_type_id text_field[] = {T_TEXT};
static const tf_type_id record_field[] = {T_RECORD};
static const tf_rt_shape opt_int[] = {{int_field, 1}, {NULL, 0}};
static const tf_rt_shape opt_text[] = {{text_field, 1}, {NULL, 0}};
static const tf_rt_shape opt_record[] = {{record_field, 1}, {NULL, 0}};
static const tf_rt_shape bad_option[] = {{NULL, 0}, {int_field, 1}};
static const tf_rt_type types[] = {
    {TF_RT_TYPE_UNIT, {0}}, {TF_RT_TYPE_INT, {0}}, {TF_RT_TYPE_FLOAT, {0}},
    {TF_RT_TYPE_BOOL, {0}}, {TF_RT_TYPE_CHAR, {0}}, {TF_RT_TYPE_TEXT, {0}},
    {TF_RT_TYPE_LIST, {.element = T_INT}}, {TF_RT_TYPE_LIST, {.element = T_TEXT}},
    {TF_RT_TYPE_VARIANT, {.variants = {opt_int, 2}}},
    {TF_RT_TYPE_VARIANT, {.variants = {opt_text, 2}}},
    {TF_RT_TYPE_RECORD, {.fields = {text_field, 1}}},
    {TF_RT_TYPE_LIST, {.element = T_RECORD}},
    {TF_RT_TYPE_VARIANT, {.variants = {opt_record, 2}}},
    {TF_RT_TYPE_VARIANT, {.variants = {bad_option, 2}}}
};
static const tf_rt_program program = {types, sizeof(types) / sizeof(types[0]), NULL, 0};

typedef struct { size_t calls, fail, live; } Allocator;
static void *allocate(void *user, size_t bytes) {
    Allocator *a = user;
    ++a->calls;
    if (a->calls == a->fail) return NULL;
    void *p = malloc(bytes);
    if (p) ++a->live;
    return p;
}
static void deallocate(void *user, void *p) {
    Allocator *a = user;
    CHECK(p != NULL && a->live != 0);
    --a->live;
    free(p);
}
static void init(tf_context *ctx, tf_builtin_env *env, Allocator *a) {
    const tf_rt_allocator allocator = {allocate, deallocate, a};
    const tf_rt_config config = {allocator, 1024 * 1024, TF_RT_MAX_CALL_DEPTH};
    CHECK(tf_context_init(ctx, &program, &config) == TF_OK);
    *env = (tf_builtin_env){&program, allocator, 1024 * 1024};
}
static tf_status call(tf_context *ctx, const tf_builtin_env *env, tf_builtin op,
                       tf_type_id result, const tf_value *args, size_t count, tf_value *out) {
    return tf_builtin_call(ctx, env, 71, op, result, args, count, out);
}
static tf_value text(tf_context *ctx, const unsigned char *bytes, size_t length) {
    tf_value out = tf_unit();
    CHECK(tf_text(ctx, 1, T_TEXT, bytes, length, &out) == TF_OK);
    return out;
}
static void expect_text(tf_context *ctx, tf_value value, const void *expected, size_t count) {
    const unsigned char *bytes;
    size_t length;
    CHECK(tf_text_view(ctx, 1, value, &bytes, &length) == TF_OK);
    CHECK(length == count);
    CHECK(count == 0 || memcmp(bytes, expected, count) == 0);
}
static size_t tag(tf_context *ctx, tf_value value) {
    size_t result;
    CHECK(tf_variant_tag(ctx, 1, value, &result) == TF_OK);
    return result;
}
static tf_value payload(tf_context *ctx, tf_value value) {
    tf_value out = tf_unit();
    CHECK(tag(ctx, value) == 0);
    CHECK(tf_payload(ctx, 1, value, 0, 0, &out) == TF_OK);
    return out;
}
static void clean(tf_context *ctx, const Allocator *a) {
    CHECK(tf_context_live_bytes(ctx) == 0);
    CHECK(a->live == 0);
}

static void lists(void) {
    tf_context ctx;
    tf_builtin_env env;
    Allocator a = {0};
    init(&ctx, &env, &a);
    tf_value out = tf_unit(), list = tf_unit();
    const tf_value original[] = {tf_int(4), tf_int(9)};
    CHECK(tf_list(&ctx, 1, T_INTS, original, 2, &list) == TF_OK);
    tf_value args[2] = {list, tf_int(17)}; /* borrowed copies */
    CHECK(call(&ctx, &env, TF_BUILTIN_APPEND, T_INTS, args, 2, &out) == TF_OK);
    size_t length;
    CHECK(tf_list_length(&ctx, 1, out, &length) == TF_OK && length == 3);
    CHECK(tf_list_length(&ctx, 1, list, &length) == TF_OK && length == 2);
    tf_value item = tf_unit();
    CHECK(tf_list_at(&ctx, 1, out, 2, &item) == TF_OK && item.as.integer == 17);
    tf_release(&item);
    tf_release(&out);
    CHECK(call(&ctx, &env, TF_BUILTIN_LEN, T_INT, args, 1, &out) == TF_OK && out.as.integer == 2);
    tf_release(&out);
    const int64_t indices[] = {INT64_MIN, -1, 0, 1, 2, INT64_MAX};
    for (size_t i = 0; i < sizeof(indices) / sizeof(indices[0]); ++i) {
        args[1] = tf_int(indices[i]);
        CHECK(call(&ctx, &env, TF_BUILTIN_AT, T_OPT_INT, args, 2, &out) == TF_OK);
        const bool some = indices[i] == 0 || indices[i] == 1;
        CHECK(tag(&ctx, out) == (some ? 0u : 1u));
        if (some) {
            item = payload(&ctx, out);
            CHECK(item.as.integer == (indices[i] == 0 ? 4 : 9));
            tf_release(&item);
        }
        tf_release(&out);
    }
    tf_release(&list);
    CHECK(tf_list(&ctx, 1, T_INTS, NULL, 0, &list) == TF_OK);
    args[0] = list; args[1] = tf_int(0);
    CHECK(call(&ctx, &env, TF_BUILTIN_AT, T_OPT_INT, args, 2, &out) == TF_OK);
    CHECK(tag(&ctx, out) == 1); tf_release(&out);
    CHECK(call(&ctx, &env, TF_BUILTIN_APPEND, T_INTS, args, 2, &out) == TF_OK);
    CHECK(tf_list_length(&ctx, 1, out, &length) == TF_OK && length == 1);
    tf_release(&out); tf_release(&list);
    /* Generic record payloads retain their own Text children. */
    tf_value name = text(&ctx, (const unsigned char *)"x", 1), record = tf_unit();
    CHECK(tf_record(&ctx, 1, T_RECORD, &name, 1, &record) == TF_OK);
    CHECK(tf_list(&ctx, 1, T_RECORDS, &record, 1, &list) == TF_OK);
    args[0] = list; args[1] = record;
    CHECK(call(&ctx, &env, TF_BUILTIN_APPEND, T_RECORDS, args, 2, &out) == TF_OK);
    tf_release(&out);
    args[1] = tf_int(0);
    CHECK(call(&ctx, &env, TF_BUILTIN_AT, T_OPT_RECORD, args, 2, &out) == TF_OK);
    tf_release(&list); tf_release(&record); tf_release(&name);
    item = payload(&ctx, out);
    CHECK(tf_project(&ctx, 1, item, 0, &name) == TF_OK);
    expect_text(&ctx, name, "x", 1);
    tf_release(&name); tf_release(&item); tf_release(&out);
    clean(&ctx, &a);
}

static void ranges(void) {
    static const int64_t pairs[][2] = {
        {0, 0}, {5, -5}, {-2, 3}, {INT64_MIN, INT64_MIN + 3}, {INT64_MAX - 3, INT64_MAX}
    };
    tf_context ctx;
    tf_builtin_env env;
    Allocator a = {0};
    init(&ctx, &env, &a);
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
        const tf_value args[] = {tf_int(pairs[i][0]), tf_int(pairs[i][1])};
        tf_value out = tf_unit();
        CHECK(call(&ctx, &env, TF_BUILTIN_RANGE, T_INTS, args, 2, &out) == TF_OK);
        size_t length;
        CHECK(tf_list_length(&ctx, 1, out, &length) == TF_OK);
        CHECK(length == (pairs[i][0] >= pairs[i][1] ? 0 :
            (size_t)((uint64_t)pairs[i][1] - (uint64_t)pairs[i][0])));
        for (size_t j = 0; j < length; ++j) {
            tf_value item = tf_unit();
            CHECK(tf_list_at(&ctx, 1, out, j, &item) == TF_OK);
            CHECK(item.as.integer == pairs[i][0] + (int64_t)j);
            tf_release(&item);
        }
        tf_release(&out);
    }
    clean(&ctx, &a);
}

static void unicode(void) {
    /* NUL, ASCII, two/three/four-byte scalars, combining acute accent. */
    static const unsigned char bytes[] = {0, 'A', 0xc3, 0xa9, 0xe4, 0xb8, 0xad,
        0xf0, 0x9f, 0x98, 0x80, 0xcc, 0x81};
    static const size_t offsets[] = {0, 1, 2, 4, 7, 11, 13};
    tf_context ctx;
    tf_builtin_env env;
    Allocator a = {0};
    init(&ctx, &env, &a);
    tf_value source = text(&ctx, bytes, sizeof(bytes)), out = tf_unit();
    tf_value args[] = {source, tf_int(0), tf_int(0)};
    CHECK(call(&ctx, &env, TF_BUILTIN_TEXT_LEN, T_INT, args, 1, &out) == TF_OK);
    CHECK(out.as.integer == 6); tf_release(&out);
    for (int64_t i = -1; i <= 7; ++i) {
        args[1] = tf_int(i);
        CHECK(call(&ctx, &env, TF_BUILTIN_TEXT_AT, T_OPT_TEXT, args, 2, &out) == TF_OK);
        CHECK(tag(&ctx, out) == (i >= 0 && i < 6 ? 0u : 1u));
        if (i >= 0 && i < 6) {
            tf_value one = payload(&ctx, out);
            expect_text(&ctx, one, bytes + offsets[i], offsets[i + 1] - offsets[i]);
            tf_release(&one);
        }
        tf_release(&out);
        for (int64_t j = -1; j <= 7; ++j) {
            args[2] = tf_int(j);
            CHECK(call(&ctx, &env, TF_BUILTIN_TEXT_SLICE, T_OPT_TEXT, args, 3, &out) == TF_OK);
            const bool some = i >= 0 && i <= j && j <= 6;
            CHECK(tag(&ctx, out) == (some ? 0u : 1u));
            if (some) {
                tf_value slice = payload(&ctx, out);
                expect_text(&ctx, slice, bytes + offsets[i], offsets[j] - offsets[i]);
                tf_release(&slice);
            }
            tf_release(&out);
        }
    }
    const int64_t extreme[] = {INT64_MIN, INT64_MAX};
    for (size_t i = 0; i < 2; ++i) {
        args[1] = tf_int(extreme[i]); args[2] = tf_int(INT64_MAX);
        CHECK(call(&ctx, &env, TF_BUILTIN_TEXT_AT, T_OPT_TEXT, args, 2, &out) == TF_OK);
        CHECK(tag(&ctx, out) == 1); tf_release(&out);
        CHECK(call(&ctx, &env, TF_BUILTIN_TEXT_SLICE, T_OPT_TEXT, args, 3, &out) == TF_OK);
        CHECK(tag(&ctx, out) == 1); tf_release(&out);
    }
    tf_release(&source);
    source = text(&ctx, NULL, 0); args[0] = source; args[1] = tf_int(0); args[2] = tf_int(0);
    CHECK(call(&ctx, &env, TF_BUILTIN_TEXT_SLICE, T_OPT_TEXT, args, 3, &out) == TF_OK);
    tf_value empty = payload(&ctx, out);
    expect_text(&ctx, empty, NULL, 0);
    tf_release(&source); tf_release(&out); tf_release(&empty);
    clean(&ctx, &a);
}

static void conversions(void) {
    static const struct { int64_t value; const char *text; } ints[] = {
        {INT64_MIN, "-9223372036854775808"}, {INT64_MAX, "9223372036854775807"},
        {0, "0"}, {-19, "-19"}, {100, "100"}
    };
    static const struct { uint64_t bits; const char *text; bool some; int64_t integer; } floats[] = {
        {0, "0.0e0", true, 0}, {UINT64_C(0x3fb999999999999a), "1.0000000000000001e-1", true, 0},
        {UINT64_C(0xc004000000000000), "-2.5e0", true, -2},
        {1, "4.9406564584124654e-324", true, 0},
        {UINT64_C(0x7fefffffffffffff), "1.7976931348623157e308", false, 0},
        {UINT64_C(0x43e0000000000000), "9.2233720368547758e18", false, 0},
        {UINT64_C(0xc3e0000000000000), "-9.2233720368547758e18", true, INT64_MIN}
    };
    tf_context ctx;
    tf_builtin_env env;
    Allocator a = {0};
    init(&ctx, &env, &a);
    tf_value out = tf_unit();
    for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); ++i) {
        const tf_value arg = tf_int(ints[i].value);
        CHECK(call(&ctx, &env, TF_BUILTIN_INT_TEXT, T_TEXT, &arg, 1, &out) == TF_OK);
        expect_text(&ctx, out, ints[i].text, strlen(ints[i].text));
        tf_value parsed = tf_unit();
        CHECK(call(&ctx, &env, TF_BUILTIN_PARSE_INT, T_OPT_INT, &out, 1, &parsed) == TF_OK);
        tf_value value = payload(&ctx, parsed);
        CHECK(value.as.integer == ints[i].value);
        tf_release(&value); tf_release(&parsed); tf_release(&out);
    }
    for (size_t i = 0; i < sizeof(floats) / sizeof(floats[0]); ++i) {
        tf_value arg = tf_unit();
        CHECK(tf_float(&ctx, 1, floats[i].bits, &arg) == TF_OK);
        CHECK(call(&ctx, &env, TF_BUILTIN_FLOAT_TEXT, T_TEXT, &arg, 1, &out) == TF_OK);
        expect_text(&ctx, out, floats[i].text, strlen(floats[i].text)); tf_release(&out);
        CHECK(call(&ctx, &env, TF_BUILTIN_FLOAT_INT, T_OPT_INT, &arg, 1, &out) == TF_OK);
        CHECK(tag(&ctx, out) == (floats[i].some ? 0u : 1u));
        if (floats[i].some) {
            tf_value value = payload(&ctx, out);
            CHECK(value.as.integer == floats[i].integer); tf_release(&value);
        }
        tf_release(&out);
    }
    static const struct { int64_t integer; uint64_t bits; } rounded[] = {
        {INT64_MAX, UINT64_C(0x43e0000000000000)}, {INT64_MIN, UINT64_C(0xc3e0000000000000)},
        {INT64_C(9007199254740993), UINT64_C(0x4340000000000000)},
        {INT64_C(9007199254740995), UINT64_C(0x4340000000000002)}
    };
    for (size_t i = 0; i < sizeof(rounded) / sizeof(rounded[0]); ++i) {
        const tf_value arg = tf_int(rounded[i].integer);
        CHECK(call(&ctx, &env, TF_BUILTIN_INT_FLOAT, T_FLOAT, &arg, 1, &out) == TF_OK);
        CHECK(out.tag == TF_VALUE_FLOAT && out.as.float_bits == rounded[i].bits); tf_release(&out);
    }
    for (size_t i = 0; i < 2; ++i) {
        const tf_value arg = tf_bool(i != 0);
        CHECK(call(&ctx, &env, TF_BUILTIN_BOOL_TEXT, T_TEXT, &arg, 1, &out) == TF_OK);
        expect_text(&ctx, out, i ? "true" : "false", i ? 4u : 5u); tf_release(&out);
    }
    static const char *const invalid[] = {"", "-", "+1", "01", "-01", " 1", "1 ",
        "9223372036854775808", "-9223372036854775809", "1.0", "\xc3\xa9"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        tf_value arg = text(&ctx, (const unsigned char *)invalid[i], strlen(invalid[i]));
        CHECK(call(&ctx, &env, TF_BUILTIN_PARSE_INT, T_OPT_INT, &arg, 1, &out) == TF_OK);
        CHECK(tag(&ctx, out) == 1); tf_release(&out); tf_release(&arg);
    }
    static const unsigned char nul[] = {'1', 0, '2'};
    tf_value arg = text(&ctx, nul, sizeof(nul));
    CHECK(call(&ctx, &env, TF_BUILTIN_PARSE_INT, T_OPT_INT, &arg, 1, &out) == TF_OK);
    CHECK(tag(&ctx, out) == 1); tf_release(&out); tf_release(&arg);
    arg = text(&ctx, (const unsigned char *)"-0", 2);
    CHECK(call(&ctx, &env, TF_BUILTIN_PARSE_INT, T_OPT_INT, &arg, 1, &out) == TF_OK);
    tf_value value = payload(&ctx, out); CHECK(value.as.integer == 0);
    tf_release(&value); tf_release(&arg); tf_release(&out);
    clean(&ctx, &a);
}

/* One allocating representative per builtin path; sweep every allocation
 * including scratch, child text, final Option/list, and None construction. */
static void allocation_failures(void) {
    for (int which = 0; which < 14; ++which) {
        size_t failures = 0;
        for (size_t fail = 1; fail <= 6; ++fail) {
            tf_context ctx;
            tf_builtin_env env;
            Allocator a = {0};
            init(&ctx, &env, &a);
            tf_value source = text(&ctx, (const unsigned char *)"12", 2), list = tf_unit();
            CHECK(tf_list(&ctx, 1, T_TEXTS, &source, 1, &list) == TF_OK);
            const size_t baseline = tf_context_live_bytes(&ctx), live = a.live;
            tf_value args[] = {source, tf_int(0), tf_int(1)}, out = tf_int(99);
            tf_builtin op;
            tf_type_id result;
            size_t argc = 1;
            switch (which) {
                case 0: op = TF_BUILTIN_AT; result = T_OPT_TEXT; args[0] = list; argc = 2; break;
                case 1: op = TF_BUILTIN_AT; result = T_OPT_TEXT; args[0] = list; args[1] = tf_int(-1); argc = 2; break;
                case 2: op = TF_BUILTIN_APPEND; result = T_TEXTS; args[0] = list; args[1] = source; argc = 2; break;
                case 3: op = TF_BUILTIN_RANGE; result = T_INTS; args[0] = tf_int(-2); args[1] = tf_int(2); argc = 2; break;
                case 4: op = TF_BUILTIN_RANGE; result = T_INTS; args[0] = tf_int(0); args[1] = tf_int(0); argc = 2; break;
                case 5: op = TF_BUILTIN_INT_TEXT; result = T_TEXT; args[0] = tf_int(INT64_MIN); break;
                case 6: op = TF_BUILTIN_FLOAT_TEXT; result = T_TEXT;
                    CHECK(tf_float(&ctx, 1, 1, &args[0]) == TF_OK); break;
                case 7: op = TF_BUILTIN_BOOL_TEXT; result = T_TEXT; args[0] = tf_bool(true); break;
                case 8: op = TF_BUILTIN_FLOAT_INT; result = T_OPT_INT;
                    CHECK(tf_float(&ctx, 1, 0, &args[0]) == TF_OK); break;
                case 9: op = TF_BUILTIN_PARSE_INT; result = T_OPT_INT; break;
                case 10: op = TF_BUILTIN_TEXT_AT; result = T_OPT_TEXT; argc = 2; break;
                case 11: op = TF_BUILTIN_TEXT_AT; result = T_OPT_TEXT; args[1] = tf_int(-1); argc = 2; break;
                case 12: op = TF_BUILTIN_TEXT_SLICE; result = T_OPT_TEXT; argc = 3; break;
                default: op = TF_BUILTIN_TEXT_SLICE; result = T_OPT_TEXT; args[2] = tf_int(-1); argc = 3; break;
            }
            a.fail = a.calls + fail;
            const tf_status status = call(&ctx, &env, op, result, args, argc, &out);
            if (status != TF_OK) {
                ++failures;
                CHECK(out.tag == TF_VALUE_UNIT);
                CHECK(tf_context_fault(&ctx)->code == TF_R003 && tf_context_fault(&ctx)->span == 71);
                /* Prior fault must win even over malformed requests. */
                CHECK(call(&ctx, NULL, (tf_builtin)999, 0, NULL, 0, &out) == TF_FAULT);
                CHECK(out.tag == TF_VALUE_UNIT && tf_context_fault(&ctx)->code == TF_R003);
            } else tf_release(&out);
            CHECK(tf_context_live_bytes(&ctx) == baseline && a.live == live);
            tf_release(&list); tf_release(&source); clean(&ctx, &a);
            if (status == TF_OK) break;
        }
        CHECK(failures != 0);
    }
}

static void retain_failures(void) {
    for (int which = 0; which < 6; ++which) {
        tf_context ctx;
        tf_builtin_env env;
        Allocator a = {0};
        init(&ctx, &env, &a);
        tf_value x = text(&ctx, (const unsigned char *)"x", 1), y = text(&ctx, (const unsigned char *)"y", 1);
        const tf_value children[] = {x, y};
        tf_value list = tf_unit();
        CHECK(tf_list(&ctx, 1, T_TEXTS, children, 2, &list) == TF_OK);
        const size_t baseline = tf_context_live_bytes(&ctx), live = a.live;
        const tf_value victim = which == 1 || which == 5 ? y : x;
        const uint64_t injected = which >= 3 ? UINT64_MAX - 1 : UINT64_MAX;
        const uint64_t old = tf_test_swap_refcount(&ctx, victim, injected);
        CHECK(old == 2);
        tf_value args[] = {list, which >= 4 ? x : tf_int(0)}, out = tf_int(5);
        const tf_builtin op = which == 0 || which == 3 ? TF_BUILTIN_AT : TF_BUILTIN_APPEND;
        if (op == TF_BUILTIN_APPEND && which < 4) args[1] = y;
        CHECK(call(&ctx, &env, op, op == TF_BUILTIN_AT ? T_OPT_TEXT : T_TEXTS, args, 2, &out) == TF_FAULT);
        CHECK(out.tag == TF_VALUE_UNIT && tf_context_fault(&ctx)->code == TF_R003);
        CHECK(tf_test_swap_refcount(&ctx, victim, old) == injected);
        CHECK(tf_context_live_bytes(&ctx) == baseline && a.live == live);
        tf_release(&x); tf_release(&list); tf_release(&y); clean(&ctx, &a);
    }
}

static void invalid_and_limits(void) {
    for (int which = 0; which < 15; ++which) {
        tf_context ctx;
        tf_builtin_env env;
        Allocator a = {0};
        init(&ctx, &env, &a);
        tf_value args[] = {tf_int(0), tf_int(1), tf_int(0)}, out = tf_int(9);
        tf_builtin op = TF_BUILTIN_RANGE;
        tf_type_id result = T_INTS;
        size_t argc = 2;
        tf_fault_code expected = TF_R004;
        switch (which) {
            case 0: op = (tf_builtin)999; break;
            case 1: argc = 3; break;
            case 2: result = 0; break;
            case 3: result = T_TEXTS; break;
            case 4: args[1] = tf_bool(true); break;
            case 5: op = TF_BUILTIN_FLOAT_INT; result = T_OPT_INT; argc = 1;
                args[0] = (tf_value){TF_VALUE_FLOAT, {.float_bits = UINT64_C(0x7ff0000000000000)}}; break;
            case 6: op = TF_BUILTIN_FLOAT_INT; result = T_BAD_OPTION; argc = 1;
                CHECK(tf_float(&ctx, 1, 0, &args[0]) == TF_OK); break;
            case 7: env.scratch_allocator.deallocate = NULL; break;
            case 8: env.scratch_byte_limit = 0; expected = TF_R003; break;
            case 9: args[0] = tf_int(INT64_MIN); args[1] = tf_int(INT64_MAX); expected = TF_R003; break;
            case 10: args[0] = tf_int(0); args[1] = tf_int(INT64_MAX); expected = TF_R003; break;
            case 11: op = TF_BUILTIN_BOOL_TEXT; args[0] = (tf_value){TF_VALUE_BOOL, {.boolean = 2}};
                argc = 1; result = T_TEXT; break;
            case 12: op = TF_BUILTIN_AT; result = T_OPT_INT; break;
            case 13: op = TF_BUILTIN_LEN; result = T_INT; argc = 1; break;
            default: op = TF_BUILTIN_TEXT_SLICE; result = T_OPT_TEXT; argc = 3; break;
        }
        const size_t before = a.calls;
        CHECK(call(&ctx, &env, op, result, args, argc, &out) == TF_FAULT);
        CHECK(out.tag == TF_VALUE_UNIT);
        CHECK(tf_context_fault(&ctx)->code == expected && tf_context_fault(&ctx)->span == 71);
        CHECK(a.calls == before); clean(&ctx, &a);
    }
    /* Default scratch allocator and empty range with zero scratch budget. */
    tf_context ctx;
    CHECK(tf_context_init(&ctx, &program, NULL) == TF_OK);
    tf_builtin_env env = {&program, {0}, 1024};
    tf_value out = tf_unit();
    const tf_value args[] = {tf_int(-1), tf_int(1)};
    CHECK(call(&ctx, &env, TF_BUILTIN_RANGE, T_INTS, args, 2, &out) == TF_OK);
    tf_release(&out);
    env.scratch_byte_limit = 0;
    const tf_value empty_args[] = {tf_int(0), tf_int(0)};
    CHECK(call(&ctx, &env, TF_BUILTIN_RANGE, T_INTS, empty_args, 2, &out) == TF_OK);
    tf_release(&out);
    CHECK(tf_context_live_bytes(&ctx) == 0);

    /* Scratch succeeds, final box fails the context budget: scratch is freed. */
    Allocator a = {0};
    const tf_rt_allocator allocator = {allocate, deallocate, &a};
    const tf_rt_config config = {allocator, 2 * sizeof(tf_value), TF_RT_MAX_CALL_DEPTH};
    CHECK(tf_context_init(&ctx, &program, &config) == TF_OK);
    env = (tf_builtin_env){&program, allocator, 1024};
    CHECK(call(&ctx, &env, TF_BUILTIN_RANGE, T_INTS, args, 2, &out) == TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT && tf_context_fault(&ctx)->code == TF_R003);
    CHECK(a.calls == 1); clean(&ctx, &a);
}

static void shared_quota(void) {
    /* Measure real box extents via public accounting, without object layout
     * duplication. Keep both Text and its input list live in every quota case. */
    tf_context probe;
    tf_builtin_env env;
    Allocator a = {0};
    init(&probe, &env, &a);
    tf_value source = text(&probe, (const unsigned char *)"x", 1), list = tf_unit(), two = tf_unit();
    CHECK(tf_list(&probe, 1, T_TEXTS, &source, 1, &list) == TF_OK);
    const size_t baseline = tf_context_live_bytes(&probe);
    const tf_value children[] = {source, source};
    CHECK(tf_list(&probe, 1, T_TEXTS, children, 2, &two) == TF_OK);
    const size_t result_bytes = tf_context_live_bytes(&probe) - baseline;
    tf_release(&two); tf_release(&list); tf_release(&source); clean(&probe, &a);
    const size_t scratch_bytes = 2 * sizeof(tf_value);
    for (int range_case = 0; range_case < 2; ++range_case) {
        for (int boundary = 0; boundary < 4; ++boundary) {
            const size_t budget = boundary == 0 ? baseline + scratch_bytes - 1 :
                boundary == 1 ? baseline + scratch_bytes + result_bytes - 1 :
                baseline + scratch_bytes + result_bytes;
            tf_context ctx;
            a = (Allocator){0};
            const tf_rt_allocator allocator = {allocate, deallocate, &a};
            const tf_rt_config config = {allocator, budget, TF_RT_MAX_CALL_DEPTH};
            CHECK(tf_context_init(&ctx, &program, &config) == TF_OK);
            env = (tf_builtin_env){&program, allocator,
                boundary == 3 ? scratch_bytes - 1 : scratch_bytes};
            source = text(&ctx, (const unsigned char *)"x", 1);
            CHECK(tf_list(&ctx, 1, T_TEXTS, &source, 1, &list) == TF_OK);
            CHECK(tf_context_live_bytes(&ctx) == baseline);
            const size_t calls_before = a.calls, live_before = a.live;
            const tf_value args[] = {range_case ? tf_int(0) : list,
                range_case ? tf_int(2) : source};
            tf_value out = tf_int(7);
            const tf_status status = call(&ctx, &env,
                range_case ? TF_BUILTIN_RANGE : TF_BUILTIN_APPEND,
                range_case ? T_INTS : T_TEXTS, args, 2, &out);
            if (boundary == 2) {
                CHECK(status == TF_OK);
                CHECK(a.calls == calls_before + 2);
                CHECK(tf_context_live_bytes(&ctx) == baseline + result_bytes);
                tf_release(&out);
            } else {
                CHECK(status == TF_FAULT && out.tag == TF_VALUE_UNIT);
                CHECK(tf_context_fault(&ctx)->code == TF_R003 && tf_context_fault(&ctx)->span == 71);
                CHECK(a.calls == calls_before + (boundary == 1 ? 1u : 0u));
            }
            CHECK(tf_context_live_bytes(&ctx) == baseline && a.live == live_before);
            tf_release(&source); tf_release(&list); clean(&ctx, &a);
        }
    }
}

int main(void) {
    lists(); ranges(); unicode(); conversions(); allocation_failures(); retain_failures(); invalid_and_limits();
    shared_quota();
    printf("runtime builtins: %zu checks passed\n", checks);
    return 0;
}
