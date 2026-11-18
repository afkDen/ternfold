#include "ternfold/core_dump.h"

#include <stdlib.h>
#include <string.h>

static void expect(bool ok, const char *what, int line) {
    if (!ok) {
        (void)fprintf(stderr, "test_core_dump:%d: %s\n", line, what);
        exit(EXIT_FAILURE);
    }
}
#define CHECK(test) expect((test), #test, __LINE__)
#define TEXT(literal) { (const unsigned char *)(literal), sizeof(literal) - 1 }
#define COUNT(array) (sizeof(array) / sizeof(*(array)))

static TfCoreProgram *freeze(const TfCoreProgramData *data) {
    TfCoreProgram *program = NULL;
    CHECK(tf_core_program_copy(&program, data, NULL) == TF_OK);
    return program;
}

static TfVerifiedCore *verify(const TfCoreProgram *program) {
    TfVerifiedCore *verified = NULL;
    TfCoreVerifyIssue issue = {0};
    const TfStatus status = tf_core_verify(&verified, program, NULL, &issue);
    if (status != TF_OK)
        (void)fprintf(stderr, "test_core_dump: verifier status=%d code=%d type=%zu fn=%zu block=%zu value=%zu\n",
                      status, issue.code, issue.type, issue.function, issue.block, issue.value);
    CHECK(status == TF_OK && verified != NULL);
    return verified;
}

static char *render(const TfCoreProgram *program) {
    TfVerifiedCore *verified = verify(program);
    FILE *out = tmpfile();
    long length;
    char *text;
    CHECK(out != NULL);
    CHECK(tf_core_dump_write(out, verified) == TF_OK);
    tf_verified_core_destroy(verified);
    CHECK(fflush(out) == 0);
    CHECK(fseek(out, 0, SEEK_END) == 0);
    length = ftell(out);
    CHECK(length >= 0 && (uintmax_t)length < SIZE_MAX);
    text = malloc((size_t)length + 1);
    CHECK(text != NULL);
    CHECK(fseek(out, 0, SEEK_SET) == 0);
    CHECK(fread(text, 1, (size_t)length, out) == (size_t)length);
    text[(size_t)length] = '\0';
    for (size_t i = 0; i < (size_t)length; ++i)
        CHECK(text[i] == '\n' || (text[i] >= 0x20 && text[i] <= 0x7e));
    CHECK(fclose(out) == 0);
    return text;
}

/* Exercise all operation spellings without relying on a source lowerer. */
static TfCoreProgram *example(TfSource *root, TfSource *other) {
    const TfSpan spans[] = {{root, 2, 6}, {other, 0, 0}, {root, 6, 6}};
    const TfCoreTypeId int_type[] = {1};
    const TfCoreField fields[] = {{TEXT("number"), 1, 1}, {TEXT("label"), 4, 2}};
    const TfCoreVariant variants[] = {{TEXT("Empty"), {0}, 2},
                                      {TEXT("Full"), {int_type, 1}, 1}};
    const TfCoreType types[] = {
        {.kind = TF_CORE_TYPE_INT},
        {.kind = TF_CORE_TYPE_FLOAT},
        {.kind = TF_CORE_TYPE_BOOL},
        {.kind = TF_CORE_TYPE_TEXT},
        {.kind = TF_CORE_TYPE_CHAR},
        {.kind = TF_CORE_TYPE_UNIT},
        {.kind = TF_CORE_TYPE_LIST, .as.element = 4},
        {.kind = TF_CORE_TYPE_OPTION, .as.element = 1},
        {.kind = TF_CORE_TYPE_RECORD, .name = TEXT("Pair"), .module = TEXT("lib"), .span = 2,
         .as.record = {fields, COUNT(fields)}},
        {.kind = TF_CORE_TYPE_ENUM, .name = TEXT("Choice"), .module = TEXT("lib"), .span = 2,
         .as.enumeration = {variants, COUNT(variants)}},
        {.kind = TF_CORE_TYPE_MACHINE, .name = TEXT("Counter"), .module = TEXT("root"), .span = 1,
         .as.machine = {fields, 1, 1, 4, 10}},
        {.kind = TF_CORE_TYPE_DECISION, .as.decision = {11, 4, 10, 7}},
        {.kind = TF_CORE_TYPE_FUNCTION, .as.function = {{int_type, 1}, 1}},
        {.kind = TF_CORE_TYPE_TAG, .as.tagged = 10}
    };
    /* Counted UTF-8 without a trailing NUL. The f after NUL must not become
     * part of its hexadecimal escape. */
    unsigned char text[] = {'"', '\\', '\n', '\r', '\t', 0x7f, 0xc3, 0xa9, 0, 'f'};
    const TfCoreConstantId texts[] = {4}, pair[] = {1, 4}, payload[] = {1};
    const TfCoreConstant constants[] = {
        {.kind = TF_CORE_CONSTANT_INT, .type = 1, .span = 1, .as.integer = INT64_MIN},
        {.kind = TF_CORE_CONSTANT_FLOAT, .type = 2, .as.float_bits = UINT64_C(1)},
        {.kind = TF_CORE_CONSTANT_BOOL, .type = 3, .as.boolean = 0},
        {.kind = TF_CORE_CONSTANT_TEXT, .type = 4, .span = 3, .as.text = {text, sizeof(text)}},
        {.kind = TF_CORE_CONSTANT_CHAR, .type = 5, .as.character = UINT32_C(0x10ffff)},
        {.kind = TF_CORE_CONSTANT_UNIT, .type = 6},
        {.kind = TF_CORE_CONSTANT_LIST, .type = 7, .as.children = {texts, COUNT(texts)}},
        {.kind = TF_CORE_CONSTANT_RECORD, .type = 9, .as.children = {pair, COUNT(pair)}},
        {.kind = TF_CORE_CONSTANT_VARIANT, .type = 10, .as.variant = {1, {payload, 1}}}
    };
    const TfCoreValueId one[] = {1}, two[] = {2}, four[] = {4}, five[] = {5},
                        seven[] = {7}, nine[] = {9}, choice[] = {13},
                        apply_args[] = {6, 3, 2}, pair_args[] = {10, 11}, accept[] = {8, 9},
                        closure_args[] = {7, 3}, binary_args[] = {2, 1}, concat_args[] = {5, 4};
    const TfCoreParameter args[] = {{1, 8, TEXT("choice"), 1}, {2, 3, TEXT("use"), 0},
                                    {3, 1, TEXT("fallback"), 0}};
    /* Case order and block table order intentionally differ from graph order. */
    const TfCoreTagCase cases[] = {{1, 3}, {0, 4}};
    const TfCoreLet dispatch[] = {
        {4, 1, {.kind = TF_CORE_OP_SWITCH_TAG, .operands = {one, 1},
                .as.switch_tag = {cases, COUNT(cases), 2}}, 1}
    };
    const TfCoreLet some[] = {
        {5, 1, {.kind = TF_CORE_OP_PAYLOAD, .operands = {one, 1}, .as.payload = {0, 0}}, 1},
        {6, 1, {.kind = TF_CORE_OP_IF, .operands = {two, 1}, .as.branch = {6, 1}}, 3}
    };
    const TfCoreLet call[] = {
        {7, 13, {.kind = TF_CORE_OP_CLOSURE, .operands = {five, 1}, .as.function = 2}, 1},
        {8, 1, {.kind = TF_CORE_OP_CALL_CLOSURE, .operands = {closure_args, 2}}, 3}
    };
    const TfCoreBlock blocks[] = {
        {.terminator = {TF_CORE_YIELD, 3, 0}, .span = 1},
        {.terminator = {TF_CORE_UNREACHABLE, 0, 3}, .span = 3},
        {.terminator = {TF_CORE_YIELD, 3, 0}},
        {some, COUNT(some), {TF_CORE_YIELD, 6, 3}, 1},
        {dispatch, COUNT(dispatch), {TF_CORE_YIELD, 4, 1}, 1},
        {call, COUNT(call), {TF_CORE_YIELD, 8, 3}, 3}
    };
    const TfCoreParameter lifted_arg[] = {{1, 1, TEXT("divisor"), 2}};
    const TfCoreParameter lifted_capture[] = {{2, 1, TEXT("number"), 2}};
    const TfCoreLet lifted[] = {
        {3, 1, {.kind = TF_CORE_OP_BINARY, .operands = {binary_args, 2},
                .as.binary = TF_CORE_BINARY_FLOOR_DIVIDE}, 2},
        {4, 2, {.kind = TF_CORE_OP_BINARY, .operands = {binary_args, 2},
                .as.binary = TF_CORE_BINARY_REAL_DIVIDE}, 2}
    };
    const TfCoreBlock lifted_body[] = {{lifted, COUNT(lifted), {TF_CORE_YIELD, 3, 2}, 2}};
    const TfCoreParameter snapshot[] = {{1, 7, TEXT("stdin"), 1}};
    const TfCoreLet staged[] = {
        {2, 4, {.kind = TF_CORE_OP_INPUT_AT, .operands = {one, 1}, .as.input_index = 0}, 1},
        {5, 7, {.kind = TF_CORE_OP_LIST, .operands = {two, 1}, .as.element_type = 4}, 1},
        {4, 7, {.kind = TF_CORE_OP_LIST_MAP, .operands = {five, 1},
                .as.list_map = {{3, 4, TEXT("line"), 3}, 1}}, 3},
        {6, 7, {.kind = TF_CORE_OP_BINARY, .operands = {concat_args, 2},
                .as.binary = TF_CORE_BINARY_CONCAT}, 3}
    };
    const TfCoreBlock adapter[] = {
        {.terminator = {TF_CORE_YIELD, 3, 3}, .span = 3},
        {staged, COUNT(staged), {TF_CORE_YIELD, 6, 3}, 1}
    };
    const TfCoreParameter old[] = {{1, 11, TEXT("old"), 1}};
    const TfCoreTypeId text_type[] = {4};
    const TfCoreLet step[] = {
        {2, 1, {.kind = TF_CORE_OP_CONST, .as.constant = 1}, 1},
        {3, 3, {.kind = TF_CORE_OP_CONST, .as.constant = 3}, 1},
        {4, 1, {.kind = TF_CORE_OP_PROJECT, .operands = {one, 1}, .as.field = 0}, 1},
        {5, 1, {.kind = TF_CORE_OP_UNARY, .operands = {four, 1}, .as.unary = TF_CORE_UNARY_NEGATE}, 1},
        {6, 8, {.kind = TF_CORE_OP_VARIANT, .operands = {five, 1}, .as.variant = {8, 0}}, 1},
        {7, 1, {.kind = TF_CORE_OP_CALL, .operands = {apply_args, 3}, .as.function = 1}, 1},
        {8, 11, {.kind = TF_CORE_OP_MACHINE_VALUE, .operands = {seven, 1}, .as.constructed_type = 11}, 1},
        {9, 7, {.kind = TF_CORE_OP_CONST, .as.constant = 7}, 1},
        {10, 1, {.kind = TF_CORE_OP_BUILTIN, .operands = {nine, 1},
                 .as.builtin = {TF_CORE_BUILTIN_LEN, {text_type, 1}}}, 1},
        {11, 4, {.kind = TF_CORE_OP_CONST, .as.constant = 4}, 1},
        {12, 9, {.kind = TF_CORE_OP_RECORD, .operands = {pair_args, 2}, .as.constructed_type = 9}, 1},
        {13, 10, {.kind = TF_CORE_OP_VARIANT, .operands = {seven, 1}, .as.variant = {10, 1}}, 1},
        {14, 14, {.kind = TF_CORE_OP_TAG, .operands = {choice, 1}}, 1},
        {15, 12, {.kind = TF_CORE_OP_VARIANT, .operands = {accept, 2}, .as.variant = {12, 0}}, 1}
    };
    const TfCoreBlock step_body[] = {{step, COUNT(step), {TF_CORE_YIELD, 15, 1}, 1}};
    const TfCoreFunction functions[] = {
        {.kind = TF_CORE_FUNCTION_ORDINARY, .name = TEXT("apply"), .module = TEXT("root"),
         .span = 1, .parameters = args, .parameter_count = COUNT(args), .result = 1,
         .blocks = blocks, .block_count = COUNT(blocks), .body = 5, .value_count = 8},
        {.kind = TF_CORE_FUNCTION_ORDINARY, .name = TEXT("$lambda"), .module = TEXT("lib"),
         .span = 2, .parameters = lifted_arg, .captures = lifted_capture,
         .parameter_count = 1, .capture_count = 1, .result = 1,
         .blocks = lifted_body, .block_count = 1, .body = 1, .value_count = 4},
        {.kind = TF_CORE_FUNCTION_ENTRY_ONLY, .name = TEXT("$entry"), .module = TEXT("root"),
         .span = 1, .parameters = snapshot, .parameter_count = 1, .result = 7,
         .blocks = adapter, .block_count = COUNT(adapter), .body = 2, .value_count = 6},
        {.kind = TF_CORE_FUNCTION_ORDINARY, .name = TEXT("$step"), .module = TEXT("root"),
         .span = 1, .parameters = old, .parameter_count = 1, .result = 12,
         .blocks = step_body, .block_count = 1, .body = 1, .value_count = 15}
    };
    TfCoreProgram *program = freeze(&(TfCoreProgramData){
        types, COUNT(types), constants, COUNT(constants), functions, COUNT(functions),
        spans, COUNT(spans), 3
    });
    memset(text, '!', sizeof(text));
    return program;
}

static void exact_dump(void) {
    const char golden[] =
        "Core entry=f3\n"
        "span s1 source=\"root\\\"\\xc3\\xa9.tfd\" bytes=2..6\n"
        "span s2 source=\"lib\\\\math.tfd\" bytes=0..0\n"
        "span s3 source=\"root\\\"\\xc3\\xa9.tfd\" bytes=6..6\n"
        "type t1 Int name=\"\" module=\"\" span=s0\n"
        "type t2 Float name=\"\" module=\"\" span=s0\n"
        "type t3 Bool name=\"\" module=\"\" span=s0\n"
        "type t4 Text name=\"\" module=\"\" span=s0\n"
        "type t5 Char name=\"\" module=\"\" span=s0\n"
        "type t6 Unit name=\"\" module=\"\" span=s0\n"
        "type t7 List name=\"\" module=\"\" span=s0 element=t4\n"
        "type t8 Option name=\"\" module=\"\" span=s0 element=t1\n"
        "type t9 Record name=\"Pair\" module=\"lib\" span=s2\n"
        "  field 0 name=\"number\" type=t1 span=s1\n"
        "  field 1 name=\"label\" type=t4 span=s2\n"
        "type t10 Enum name=\"Choice\" module=\"lib\" span=s2\n"
        "  tag 0 name=\"Empty\" span=s2 payloads=[]\n"
        "  tag 1 name=\"Full\" span=s1 payloads=[t1]\n"
        "type t11 Machine name=\"Counter\" module=\"root\" span=s1 input=t1 output=t4 rejection=t10\n"
        "  cell 0 name=\"number\" type=t1 span=s1\n"
        "type t12 Decision name=\"\" module=\"\" span=s0 machine=t11 output=t4 rejection=t10 emissions=t7\n"
        "type t13 Function name=\"\" module=\"\" span=s0 parameters=[t1] result=t1\n"
        "type t14 Tag name=\"\" module=\"\" span=s0 tagged=t10\n"
        "const c1:t1 Int -9223372036854775808 span=s1\n"
        "const c2:t2 Float bits=0x0000000000000001 span=s0\n"
        "const c3:t3 Bool false span=s0\n"
        "const c4:t4 Text \"\\\"\\\\\\n\\r\\t\\x7f\\xc3\\xa9\\x00f\" span=s3\n"
        "const c5:t5 Char U+10FFFF span=s0\n"
        "const c6:t6 Unit span=s0\n"
        "const c7:t7 List [c4] span=s0\n"
        "const c8:t9 Record [c1, c4] span=s0\n"
        "const c9:t10 Variant tag=1 payloads=[c1] span=s0\n"
        "function f1 ordinary name=\"apply\" module=\"root\" result=t1 body=b5 values=8 span=s1\n"
        "  param %1:t8 name=\"choice\" span=s1\n"
        "  param %2:t3 name=\"use\" span=s0\n"
        "  param %3:t1 name=\"fallback\" span=s0\n"
        "  block b1 span=s1\n"
        "    Yield %3 span=s0\n"
        "  block b2 span=s3\n"
        "    Unreachable span=s3\n"
        "  block b3 span=s0\n"
        "    Yield %3 span=s0\n"
        "  block b4 span=s1\n"
        "    %5:t1 = Payload tag=0 index=0 operands=[%1] span=s1\n"
        "    %6:t1 = If then=b6 else=b1 operands=[%2] span=s3\n"
        "    Yield %6 span=s3\n"
        "  block b5 span=s1\n"
        "    %4:t1 = SwitchTag cases=[1:b3, 0:b4] default=b2 operands=[%1] span=s1\n"
        "    Yield %4 span=s1\n"
        "  block b6 span=s3\n"
        "    %7:t13 = Closure f2 operands=[%5] span=s1\n"
        "    %8:t1 = CallClosure operands=[%7, %3] span=s3\n"
        "    Yield %8 span=s3\n"
        "function f2 ordinary name=\"$lambda\" module=\"lib\" result=t1 body=b1 values=4 span=s2\n"
        "  param %1:t1 name=\"divisor\" span=s2\n"
        "  capture %2:t1 name=\"number\" span=s2\n"
        "  block b1 span=s2\n"
        "    %3:t1 = Binary div operands=[%2, %1] span=s2\n"
        "    %4:t2 = Binary / operands=[%2, %1] span=s2\n"
        "    Yield %3 span=s2\n"
        "function f3 entry-only name=\"$entry\" module=\"root\" result=t7 body=b2 values=6 span=s1\n"
        "  param %1:t7 name=\"stdin\" span=s1\n"
        "  block b1 span=s3\n"
        "    Yield %3 span=s3\n"
        "  block b2 span=s1\n"
        "    %2:t4 = InputAt index=0 operands=[%1] span=s1\n"
        "    %5:t7 = List element=t4 operands=[%2] span=s1\n"
        "    %4:t7 = ListMap element=%3:t4 name=\"line\" span=s3 body=b1 operands=[%5] span=s3\n"
        "    %6:t7 = Binary concat operands=[%5, %4] span=s3\n"
        "    Yield %6 span=s3\n"
        "function f4 ordinary name=\"$step\" module=\"root\" result=t12 body=b1 values=15 span=s1\n"
        "  param %1:t11 name=\"old\" span=s1\n"
        "  block b1 span=s1\n"
        "    %2:t1 = Const c1 operands=[] span=s1\n"
        "    %3:t3 = Const c3 operands=[] span=s1\n"
        "    %4:t1 = Project field=0 operands=[%1] span=s1\n"
        "    %5:t1 = Unary negate operands=[%4] span=s1\n"
        "    %6:t8 = Variant type=t8 tag=0 operands=[%5] span=s1\n"
        "    %7:t1 = Call f1 operands=[%6, %3, %2] span=s1\n"
        "    %8:t11 = MachineValue type=t11 operands=[%7] span=s1\n"
        "    %9:t7 = Const c7 operands=[] span=s1\n"
        "    %10:t1 = Builtin len types=[t4] operands=[%9] span=s1\n"
        "    %11:t4 = Const c4 operands=[] span=s1\n"
        "    %12:t9 = Record type=t9 operands=[%10, %11] span=s1\n"
        "    %13:t10 = Variant type=t10 tag=1 operands=[%7] span=s1\n"
        "    %14:t14 = Tag operands=[%13] span=s1\n"
        "    %15:t12 = Variant type=t12 tag=0 operands=[%8, %9] span=s1\n"
        "    Yield %15 span=s1\n";
    const unsigned char bytes[] = {0xc3, 0xa9, 'a', 'b', 'c', 'd'};
    TfSource root, other;
    CHECK(tf_source_init(&root, "root\"\xc3\xa9.tfd", bytes, sizeof(bytes), NULL) == TF_OK);
    CHECK(tf_source_init(&other, "lib\\math.tfd", NULL, 0, NULL) == TF_OK);
    TfCoreProgram *a = example(&root, &other);
    TfCoreProgram *b = example(&root, &other);
    char *first = render(a);
    char *second = render(b);
    if (strcmp(first, golden) != 0) (void)fputs(first, stderr);
    CHECK(strcmp(first, golden) == 0);
    CHECK(strcmp(second, first) == 0);
    free(second);
    free(first);
    tf_core_program_destroy(b);
    tf_core_program_destroy(a);
    tf_source_destroy(&other);
    tf_source_destroy(&root);
}

static void deep_blocks(void) {
    enum { DEPTH = 8192, BLOCKS = DEPTH * 2 + 1 };
    TfCoreLet *lets = calloc(DEPTH, sizeof(*lets));
    TfCoreBlock *blocks = calloc(BLOCKS, sizeof(*blocks));
    const TfCoreValueId test[] = {1};
    const TfCoreType boolean = {.kind = TF_CORE_TYPE_BOOL};
    const TfCoreParameter param = {1, 1, TEXT("condition"), 0};
    const TfCoreFunction fn = {
        .kind = TF_CORE_FUNCTION_ORDINARY, .name = TEXT("deep"), .parameters = &param,
        .parameter_count = 1, .result = 1, .blocks = blocks, .block_count = BLOCKS,
        .body = 1, .value_count = DEPTH + 1
    };
    CHECK(lets != NULL && blocks != NULL);
    for (size_t i = 0; i < DEPTH; ++i) {
        lets[i] = (TfCoreLet){i + 2, 1, {
            .kind = TF_CORE_OP_IF, .operands = {test, 1}, .as.branch = {i + 2, DEPTH + 2 + i}
        }, 0};
        blocks[i] = (TfCoreBlock){&lets[i], 1, {TF_CORE_YIELD, i + 2, 0}, 0};
        blocks[DEPTH + 1 + i].terminator = i == 0 ? (TfCoreTerminator){TF_CORE_YIELD, 1, 0}
                                                  : (TfCoreTerminator){TF_CORE_UNREACHABLE, 0, 0};
    }
    blocks[DEPTH].terminator = (TfCoreTerminator){TF_CORE_YIELD, 1, 0};
    TfCoreProgram *program = freeze(&(TfCoreProgramData){
        .types = &boolean, .type_count = 1, .functions = &fn, .function_count = 1
    });
    free(blocks);
    free(lets);
    char *text = render(program);
    const char *last_if = strstr(text, "    %8193:t1 = If then=b8193 else=b16385 operands=[%1] span=s0\n");
    CHECK(last_if != NULL);
    CHECK(strstr(last_if, "  block b8193 span=s0\n    Yield %1 span=s0\n") != NULL);
    const char *last_block = strstr(last_if, "  block b16385 span=s0\n");
    CHECK(last_block != NULL && strcmp(last_block, "  block b16385 span=s0\n    Unreachable span=s0\n") == 0);
    free(text);
    tf_core_program_destroy(program);
}

static void empty_and_write_failure(void) {
    TfCoreProgram *program = freeze(&(TfCoreProgramData){0});
    TfVerifiedCore *verified = verify(program);
    char *text = render(program);
    FILE *out = tmpfile();
    CHECK(strcmp(text, "Core entry=f0\n") == 0);
    CHECK(out != NULL);
    CHECK(tf_core_dump_write(out, NULL) == TF_HOST_ERROR);
    CHECK(ftell(out) == 0);
    CHECK(tf_core_dump_write(NULL, verified) == TF_HOST_ERROR);
    CHECK(fclose(out) == 0);

    out = fopen("tests/test_core_dump.c", "rb");
    CHECK(out != NULL);
    CHECK(setvbuf(out, NULL, _IONBF, 0) == 0);
    CHECK(tf_core_dump_write(out, verified) == TF_HOST_ERROR);
    CHECK(fclose(out) == 0);
    free(text);
    tf_verified_core_destroy(verified);
    tf_core_program_destroy(program);
}

int main(void) {
    exact_dump();
    deep_blocks();
    empty_and_write_failure();
    (void)puts("Core dump: exact text, determinism, deep blocks and write failures passed");
    return EXIT_SUCCESS;
}
