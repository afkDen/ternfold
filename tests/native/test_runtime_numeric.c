#include "ternfold_rt_numeric.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32) && defined(__MINGW32__) && defined(__clang__) && defined(__x86_64__)
#include <xmmintrin.h>
#define TEST_MXCSR 1
#endif

static size_t checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "test_runtime_numeric:%d: %s\n", __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static const tf_rt_type types[] = {
    { .kind = TF_RT_TYPE_UNIT }, { .kind = TF_RT_TYPE_INT },
    { .kind = TF_RT_TYPE_FLOAT }, { .kind = TF_RT_TYPE_BOOL },
    { .kind = TF_RT_TYPE_FUNCTION }, { .kind = TF_RT_TYPE_TEXT }
};
static const tf_rt_function functions[] = {{5, {NULL, 0}}};
static const tf_rt_program program = {types, sizeof types / sizeof *types, functions, 1};

static void *deny_allocation(void *user, size_t bytes) {
    (void)bytes;
    ++*(size_t *)user;
    return NULL;
}

static void unexpected_deallocation(void *user, void *pointer) {
    (void)user;
    (void)pointer;
    CHECK(false);
}

static void init(tf_context *ctx, size_t *allocations) {
    const tf_rt_config config = {{deny_allocation, unexpected_deallocation, allocations},
                                 TF_RT_DEFAULT_BYTE_BUDGET, 2};
    CHECK(tf_context_init(ctx, &program, &config) == TF_OK);
}

#define I(n) {TF_VALUE_INT, {.integer = (n)}}
#define F(n) {TF_VALUE_FLOAT, {.float_bits = (n)}}
#define B(n) {TF_VALUE_BOOL, {.boolean = (n)}}
#define U {0}
#define SIGN UINT64_C(0x8000000000000000)
#define ONE UINT64_C(0x3ff0000000000000)
#define TWO UINT64_C(0x4000000000000000)
#define HALF UINT64_C(0x3fe0000000000000)
#define MAX_FINITE UINT64_C(0x7fefffffffffffff)
#define MIN_NORMAL UINT64_C(0x0010000000000000)

static void expect_value(tf_value actual, tf_value expected) {
    CHECK(actual.tag == expected.tag);
    switch (expected.tag) {
        case TF_VALUE_UNIT: break;
        case TF_VALUE_INT: CHECK(actual.as.integer == expected.as.integer); break;
        case TF_VALUE_FLOAT: CHECK(actual.as.float_bits == expected.as.float_bits); break;
        case TF_VALUE_BOOL: CHECK(actual.as.boolean == expected.as.boolean); break;
        default: CHECK(false); break;
    }
}

static void expect_fault(const tf_context *ctx, tf_fault_code code, tf_span_id span) {
    const tf_fault *fault = tf_context_fault(ctx);
    CHECK(fault != NULL);
    CHECK(fault->code == code);
    CHECK(fault->span == span);
}

static void binary_cases(void) {
    static const struct {
        tf_numeric_op op;
        tf_value a, b, result;
        tf_fault_code fault;
    } cases[] = {
        {TF_RT_NUM_ADD, I(INT64_MAX - 1), I(1), I(INT64_MAX), 0},
        {TF_RT_NUM_ADD, I(INT64_MAX), I(1), U, TF_R001},
        {TF_RT_NUM_SUBTRACT, I(INT64_MIN + 1), I(1), I(INT64_MIN), 0},
        {TF_RT_NUM_SUBTRACT, I(INT64_MIN), I(1), U, TF_R001},
        {TF_RT_NUM_MULTIPLY, I(INT64_MIN / 2), I(2), I(INT64_MIN), 0},
        {TF_RT_NUM_MULTIPLY, I(INT64_MIN), I(-1), U, TF_R001},
        {TF_RT_NUM_FLOOR_DIVIDE, I(-7), I(3), I(-3), 0},
        {TF_RT_NUM_FLOOR_DIVIDE, I(7), I(-3), I(-3), 0},
        {TF_RT_NUM_FLOOR_DIVIDE, I(INT64_MIN), I(-1), U, TF_R001},
        {TF_RT_NUM_FLOOR_DIVIDE, I(1), I(0), U, TF_R002},
        {TF_RT_NUM_REMAINDER, I(-7), I(3), I(-1), 0},
        {TF_RT_NUM_REMAINDER, I(7), I(-3), I(1), 0},
        {TF_RT_NUM_REMAINDER, I(INT64_MIN), I(-1), U, TF_R001},
        {TF_RT_NUM_REMAINDER, I(1), I(0), U, TF_R002},
        {TF_RT_NUM_POWER, I(0), I(0), I(1), 0},
        {TF_RT_NUM_POWER, I(-2), I(63), I(INT64_MIN), 0},
        {TF_RT_NUM_POWER, I(2), I(63), U, TF_R001},
        {TF_RT_NUM_POWER, I(0), I(INT64_MIN), U, TF_R005},
        /* Nearest/even, cancellation, and gradual underflow. */
        {TF_RT_NUM_ADD, F(ONE + 1), F(UINT64_C(0x3ca0000000000000)), F(ONE + 2), 0},
        {TF_RT_NUM_ADD, F(MAX_FINITE), F(MAX_FINITE), U, TF_R001},
        {TF_RT_NUM_SUBTRACT, F(MIN_NORMAL), F(MIN_NORMAL - 1), F(1), 0},
        {TF_RT_NUM_SUBTRACT, F(SIGN | ONE), F(SIGN | ONE), F(0), 0},
        {TF_RT_NUM_MULTIPLY, F(MIN_NORMAL), F(HALF), F(MIN_NORMAL / 2), 0},
        {TF_RT_NUM_MULTIPLY, F(SIGN | 1), F(HALF), F(0), 0},
        {TF_RT_NUM_MULTIPLY, F(MAX_FINITE), F(TWO), U, TF_R001},
        /* All four real division signatures; Int rounding precedes division. */
        {TF_RT_NUM_REAL_DIVIDE, I(7), I(2), F(UINT64_C(0x400c000000000000)), 0},
        {TF_RT_NUM_REAL_DIVIDE, I(INT64_MIN), I(-1), F(UINT64_C(0x43e0000000000000)), 0},
        {TF_RT_NUM_REAL_DIVIDE, I(INT64_C(9007199254740993)), F(TWO),
                               F(UINT64_C(0x4330000000000000)), 0},
        {TF_RT_NUM_REAL_DIVIDE, F(UINT64_C(0x4340000000000000)),
                               I(INT64_C(9007199254740993)), F(ONE), 0},
        {TF_RT_NUM_REAL_DIVIDE, F(ONE), F(TWO), F(HALF), 0},
        {TF_RT_NUM_REAL_DIVIDE, F(1), F(1), F(ONE), 0},
        {TF_RT_NUM_REAL_DIVIDE, F(ONE), I(0), U, TF_R002},
        {TF_RT_NUM_REAL_DIVIDE, I(1), F(0), U, TF_R002},
        {TF_RT_NUM_REAL_DIVIDE, F(ONE), F(1), U, TF_R001},
        {TF_RT_NUM_POWER, F(0), I(0), F(ONE), 0},
        {TF_RT_NUM_POWER, F(0), I(-1), U, TF_R002},
        {TF_RT_NUM_POWER, F(1), I(-1), U, TF_R001},
        {TF_RT_NUM_POWER, F(MAX_FINITE), I(1), F(MAX_FINITE), 0},
        {TF_RT_NUM_POWER, F(MAX_FINITE), I(2), U, TF_R001},
        {TF_RT_NUM_POWER, F(SIGN | ONE), I(INT64_MIN), F(ONE), 0},
        /* (Rounded 1/5)^2 is one ULP above rounded 1/25. */
        {TF_RT_NUM_POWER, F(UINT64_C(0x4014000000000000)), I(-2),
                          F(UINT64_C(0x3fa47ae147ae147c)), 0},
        {TF_RT_NUM_LESS, I(INT64_MIN), I(INT64_MAX), B(1), 0},
        {TF_RT_NUM_LESS, F(SIGN | TWO), F(SIGN | ONE), B(1), 0},
        {TF_RT_NUM_LESS, F(ONE), F(ONE), B(0), 0},
        {TF_RT_NUM_LESS_EQUAL, F(ONE), F(ONE), B(1), 0},
        {TF_RT_NUM_LESS_EQUAL, I(2), I(1), B(0), 0},
        {TF_RT_NUM_GREATER, F(1), F(0), B(1), 0},
        {TF_RT_NUM_GREATER, I(2), I(2), B(0), 0},
        {TF_RT_NUM_GREATER_EQUAL, I(2), I(2), B(1), 0},
        {TF_RT_NUM_GREATER_EQUAL, F(SIGN | 1), F(0), B(0), 0},
        {TF_RT_NUM_EQUAL, F(0), F(0), B(1), 0},
        {TF_RT_NUM_EQUAL, I(0), I(1), B(0), 0},
        {TF_RT_NUM_NOT_EQUAL, F(ONE), F(ONE + 1), B(1), 0},
        {TF_RT_NUM_NOT_EQUAL, I(INT64_MIN), I(INT64_MIN), B(0), 0},
        {TF_RT_NUM_ADD, I(1), F(ONE), U, TF_R004},
        {TF_RT_NUM_EQUAL, F(ONE), I(1), U, TF_R004},
        {TF_RT_NUM_POWER, F(ONE), F(ONE), U, TF_R004},
        {TF_RT_NUM_FLOOR_DIVIDE, F(ONE), F(0), U, TF_R004},
        {TF_RT_NUM_REAL_DIVIDE, B(0), I(0), U, TF_R004},
        {(tf_numeric_op)0, I(1), I(1), U, TF_R004}
    };
    size_t allocations = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        const tf_span_id span = 100 + i;
        tf_context ctx;
        tf_value out = tf_int(123);
        init(&ctx, &allocations);
        const tf_status status = tf_numeric_binary(&ctx, span, cases[i].op,
                                                   cases[i].a, cases[i].b, &out);
        if (cases[i].fault != 0) {
            CHECK(status == TF_FAULT);
            expect_fault(&ctx, cases[i].fault, span);
        } else {
            CHECK(status == TF_OK);
            CHECK(tf_context_fault(&ctx) == NULL);
        }
        expect_value(out, cases[i].result);
        CHECK(tf_context_live_bytes(&ctx) == 0);
        tf_release(&out);
    }
    CHECK(allocations == 0);
}

static void unary_cases(void) {
    static const struct {
        tf_status (*op)(tf_context *, tf_span_id, tf_value, tf_value *);
        tf_value operand, result;
        tf_fault_code fault;
    } cases[] = {
        {tf_numeric_negate, I(INT64_MAX), I(-INT64_MAX), 0},
        {tf_numeric_negate, I(INT64_MIN), U, TF_R001},
        {tf_numeric_negate, F(1), F(SIGN | 1), 0},
        {tf_numeric_negate, F(SIGN | MAX_FINITE), F(MAX_FINITE), 0},
        {tf_numeric_negate, F(0), F(0), 0},
        {tf_numeric_negate, B(1), U, TF_R004},
        {tf_numeric_not, B(0), B(1), 0},
        {tf_numeric_not, B(1), B(0), 0},
        {tf_numeric_not, I(0), U, TF_R004}
    };
    size_t allocations = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        const tf_span_id span = 200 + i;
        tf_context ctx;
        tf_value out = tf_int(123);
        init(&ctx, &allocations);
        CHECK(cases[i].op(&ctx, span, cases[i].operand, &out) ==
              (cases[i].fault ? TF_FAULT : TF_OK));
        if (cases[i].fault) expect_fault(&ctx, cases[i].fault, span);
        else CHECK(tf_context_fault(&ctx) == NULL);
        expect_value(out, cases[i].result);
        CHECK(tf_context_live_bytes(&ctx) == 0);
        tf_release(&out);
    }
    CHECK(allocations == 0);
}

static void first_fault(void) {
    size_t allocations = 0;
    tf_context ctx;
    tf_value out = tf_int(123);
    init(&ctx, &allocations);
    CHECK(tf_context_enter(&ctx, 1, 10) == TF_OK);
    CHECK(tf_context_enter(&ctx, 1, 20) == TF_OK);
    CHECK(tf_numeric_binary(&ctx, 30, TF_RT_NUM_REAL_DIVIDE, tf_int(7), tf_int(0), &out) ==
          TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT);
    tf_context_leave(&ctx);
    out = tf_int(123);
    CHECK(tf_numeric_binary(&ctx, 40, TF_RT_NUM_POWER, tf_int(1), tf_int(-1), &out) == TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT);
    out = tf_int(123);
    CHECK(tf_numeric_negate(&ctx, 50, tf_int(INT64_MIN), &out) == TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT);
    out = tf_int(123);
    CHECK(tf_numeric_not(&ctx, 60, tf_bool(true), &out) == TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT);
    tf_context_leave(&ctx);
    expect_fault(&ctx, TF_R002, 30);
    const tf_fault *fault = tf_context_fault(&ctx);
    CHECK(fault->frame_count == 2);
    CHECK(fault->frames[0].function == 1 && fault->frames[0].call_span == 10);
    CHECK(fault->frames[1].function == 1 && fault->frames[1].call_span == 20);
    CHECK(allocations == 0);
}

static void borrowed_box_on_failure(void) {
    static const unsigned char bytes[] = {'x'};
    tf_context ctx;
    tf_value text, out = tf_int(123);
    CHECK(tf_context_init(&ctx, &program, NULL) == TF_OK);
    CHECK(tf_text(&ctx, 80, 6, bytes, sizeof bytes, &text) == TF_OK);
    const size_t live = tf_context_live_bytes(&ctx);
    CHECK(live > 0);
    CHECK(tf_numeric_binary(&ctx, 81, TF_RT_NUM_ADD, tf_int(1), text, &out) == TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT);
    expect_fault(&ctx, TF_R004, 81);
    CHECK(tf_context_live_bytes(&ctx) == live);
    tf_release(&out);
    tf_release(&text);
    CHECK(tf_context_live_bytes(&ctx) == 0);
}

#ifdef TEST_MXCSR
static void rejected_fp_environment(void) {
    size_t allocations = 0;
    tf_context ctx;
    tf_value out = tf_int(123);
    init(&ctx, &allocations);

    const unsigned saved = _mm_getcsr();
    const unsigned flush_to_zero = saved | 0x8000u;
    _mm_setcsr(flush_to_zero);
    const bool supported = tf_numeric_float_supported();
    const unsigned after_probe = _mm_getcsr();
    /* Simulate loss of support after a successful startup probe. This would
     * otherwise be divide by zero, and must not create a language fault. */
    const tf_status status = tf_numeric_binary(&ctx, 70, TF_RT_NUM_REAL_DIVIDE,
                                               tf_int(1), tf_int(0), &out);
    const unsigned after_op = _mm_getcsr();
    _mm_setcsr(saved);

    CHECK(!supported);
    CHECK(after_probe == flush_to_zero && after_op == flush_to_zero);
    CHECK(status == TF_FAULT);
    CHECK(out.tag == TF_VALUE_UNIT);
    CHECK(tf_context_fault(&ctx) == NULL);
    CHECK(tf_numeric_float_supported());
    CHECK(allocations == 0);
}
#endif

int main(void) {
    if (!tf_numeric_float_supported()) {
        fprintf(stderr, "test_runtime_numeric: unsupported host floating-point environment\n");
        return EXIT_FAILURE;
    }
    binary_cases();
    unary_cases();
    first_fault();
    borrowed_box_on_failure();
#ifdef TEST_MXCSR
    rejected_fp_environment();
#endif
    printf("test_runtime_numeric: %zu checks passed\n", checks);
    return EXIT_SUCCESS;
}
