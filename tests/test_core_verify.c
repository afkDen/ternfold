#include "ternfold/core_verify.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(e) do { if (!(e)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); exit(EXIT_FAILURE); \
} } while (0)

enum {
    INT = 1, FLOAT, BOOL, TEXT, CHAR, UNIT, INTS, TEXTS, OPT_INT, OPT_TEXT,
    RECORD, ENUM, MACHINE, DECISION, TAG, MAP_FN, FOLD_FN, ENTRY_FN
};

static const TfCoreTypeId int_arg[] = {INT};
static const TfCoreTypeId fold_args[] = {TEXT, INT};
static const TfCoreTypeId entry_args[] = {TEXTS};
static const TfCoreField record_fields[] = {{.type = INT}, {.type = TEXT}};
static const TfCoreField machine_cells[] = {{.type = RECORD}};
static const TfCoreVariant variants[] = {{{0}, {int_arg, 1}, 0}, {0}};
static const TfCoreType baseline[] = {
    {.kind = TF_CORE_TYPE_INT}, {.kind = TF_CORE_TYPE_FLOAT},
    {.kind = TF_CORE_TYPE_BOOL}, {.kind = TF_CORE_TYPE_TEXT},
    {.kind = TF_CORE_TYPE_CHAR}, {.kind = TF_CORE_TYPE_UNIT},
    {.kind = TF_CORE_TYPE_LIST, .as.element = INT},
    {.kind = TF_CORE_TYPE_LIST, .as.element = TEXT},
    {.kind = TF_CORE_TYPE_OPTION, .as.element = INT},
    {.kind = TF_CORE_TYPE_OPTION, .as.element = TEXT},
    {.kind = TF_CORE_TYPE_RECORD, .as.record = {record_fields, 2}},
    {.kind = TF_CORE_TYPE_ENUM, .as.enumeration = {variants, 2}},
    {.kind = TF_CORE_TYPE_MACHINE, .as.machine = {machine_cells, 1, INT, TEXT, ENUM}},
    {.kind = TF_CORE_TYPE_DECISION, .as.decision = {MACHINE, TEXT, ENUM, TEXTS}},
    {.kind = TF_CORE_TYPE_TAG, .as.tagged = OPT_INT},
    {.kind = TF_CORE_TYPE_FUNCTION, .as.function = {{int_arg, 1}, TEXT}},
    {.kind = TF_CORE_TYPE_FUNCTION, .as.function = {{fold_args, 2}, TEXT}},
    {.kind = TF_CORE_TYPE_FUNCTION, .as.function = {{entry_args, 1}, TEXTS}}
};

static const TfCoreConstantId record_children[] = {1, 4}, list_children[] = {1, 1};
static const TfCoreConstantId variant_children[] = {1};
static const unsigned char unicode[] = {0, 0xf4, 0x8f, 0xbf, 0xbf};
static const TfCoreConstant constants[] = {
    {.kind = TF_CORE_CONSTANT_INT, .type = INT, .as.integer = INT64_MIN},
    {.kind = TF_CORE_CONSTANT_FLOAT, .type = FLOAT, .as.float_bits = 1},
    {.kind = TF_CORE_CONSTANT_BOOL, .type = BOOL, .as.boolean = 1},
    {.kind = TF_CORE_CONSTANT_TEXT, .type = TEXT, .as.text = {unicode, sizeof(unicode)}},
    {.kind = TF_CORE_CONSTANT_CHAR, .type = CHAR, .as.character = UINT32_C(0x10ffff)},
    {.kind = TF_CORE_CONSTANT_UNIT, .type = UNIT},
    {.kind = TF_CORE_CONSTANT_LIST, .type = INTS, .as.children = {list_children, 2}},
    {.kind = TF_CORE_CONSTANT_RECORD, .type = RECORD, .as.children = {record_children, 2}},
    {.kind = TF_CORE_CONSTANT_VARIANT, .type = ENUM, .as.variant = {0, {variant_children, 1}}}
};

#define COUNT(xs) (sizeof(xs) / sizeof(*(xs)))
#define VALUES(...) ((TfCoreValueIds){(const TfCoreValueId[]){__VA_ARGS__}, \
                                     sizeof((const TfCoreValueId[]){__VA_ARGS__}) / sizeof(TfCoreValueId)})
#define TYPES(...) ((TfCoreTypeIds){(const TfCoreTypeId[]){__VA_ARGS__}, \
                                   sizeof((const TfCoreTypeId[]){__VA_ARGS__}) / sizeof(TfCoreTypeId)})

static TfCoreProgramData program(void) {
    return (TfCoreProgramData){.types = baseline, .type_count = COUNT(baseline),
                               .constants = constants, .constant_count = COUNT(constants)};
}

static TfCoreLet bind(TfCoreValueId value, TfCoreTypeId type, TfCoreOperation op) {
    return (TfCoreLet){value, type, op, 0};
}

static TfCoreBlock block(const TfCoreLet *lets, size_t count, TfCoreValueId yield) {
    return (TfCoreBlock){lets, count, {TF_CORE_YIELD, yield, 0}, 0};
}

static TfCoreBlock dead(void) {
    return (TfCoreBlock){.terminator = {TF_CORE_UNREACHABLE, 0, 0}};
}

static TfCoreFunction ordinary(const TfCoreParameter *p, size_t count, TfCoreTypeId result,
                               const TfCoreBlock *blocks, size_t blocks_count, size_t values) {
    return (TfCoreFunction){.kind = TF_CORE_FUNCTION_ORDINARY, .parameters = p,
        .parameter_count = count, .result = result, .blocks = blocks, .block_count = blocks_count,
        .body = 1, .value_count = values};
}

static TfCoreVerifyIssue expect(TfCoreProgramData data, TfCoreVerifyCode wanted) {
    TfCoreProgram *owner = NULL;
    TfVerifiedCore *verified = NULL;
    TfCoreVerifyIssue issue = {0};
    TfStatus status;
    CHECK(tf_core_program_copy(&owner, &data, NULL) == TF_OK);
    status = tf_core_verify(&verified, owner, NULL, &issue);
    if (issue.code != wanted)
        fprintf(stderr, "wanted %d got %d (status %d, type %zu, constant %zu, fn %zu, "
                        "block %zu, value %zu, span %zu)\n", (int)wanted, (int)issue.code,
                (int)status, issue.type, issue.constant, issue.function, issue.block,
                issue.value, issue.span);
    CHECK(issue.code == wanted);
    if (wanted == TF_CORE_VERIFY_NONE) {
        CHECK(status == TF_OK && verified != NULL);
        CHECK(tf_verified_core_data(verified) == tf_core_program_data(owner));
    } else {
        CHECK(status == TF_SOURCE_ERROR && verified == NULL);
    }
    tf_verified_core_destroy(verified);
    tf_core_program_destroy(owner);
    return issue;
}

static void metadata(void) {
    TfCoreType types[COUNT(baseline)];
    TfCoreConstant values[COUNT(constants)];
    TfCoreProgramData data = program();
    const TfCoreField bad_cell = {.type = MAP_FN};
    const TfCoreField own_machine = {.type = MACHINE};
    const unsigned char invalid_utf8[] = {0xed, 0xa0, 0x80};
    TfSource source;
    TfSpan span;
    for (size_t i = 0; i < COUNT(types); ++i) types[i] = baseline[i];
    for (size_t i = 0; i < COUNT(values); ++i) values[i] = constants[i];
    data.types = types;
    data.constants = values;
    expect(data, TF_CORE_VERIFY_NONE);
    types[INTS - 1].as.element = OPT_INT;
    types[OPT_INT - 1].as.element = INTS;
    CHECK(expect(data, TF_CORE_VERIFY_TYPE_CYCLE).type == OPT_INT);
    types[INTS - 1] = baseline[INTS - 1];
    types[OPT_INT - 1].as.element = TAG;
    expect(data, TF_CORE_VERIFY_TYPE);
    types[OPT_INT - 1] = baseline[OPT_INT - 1];
    types[DECISION - 1].as.decision.emissions = INTS;
    expect(data, TF_CORE_VERIFY_TYPE);
    types[DECISION - 1] = baseline[DECISION - 1];
    types[MACHINE - 1].as.machine.input = DECISION;
    expect(data, TF_CORE_VERIFY_TYPE_CYCLE);
    types[MACHINE - 1] = baseline[MACHINE - 1];
    types[MACHINE - 1].as.machine.cells = &bad_cell;
    expect(data, TF_CORE_VERIFY_TYPE);
    types[MACHINE - 1].as.machine.cells = &own_machine;
    expect(data, TF_CORE_VERIFY_TYPE_CYCLE);
    types[MACHINE - 1] = baseline[MACHINE - 1];
    types[MACHINE - 1].as.machine.input = MAP_FN;
    expect(data, TF_CORE_VERIFY_NONE); /* a header type is not stored state */
    types[MACHINE - 1] = baseline[MACHINE - 1];
    types[TAG - 1].as.tagged = INT;
    expect(data, TF_CORE_VERIFY_TYPE);
    types[TAG - 1] = baseline[TAG - 1];
    types[ENTRY_FN - 1] = types[MAP_FN - 1];
    CHECK(expect(data, TF_CORE_VERIFY_TYPE).type == ENTRY_FN);
    types[ENTRY_FN - 1] = types[INTS - 1];
    expect(data, TF_CORE_VERIFY_TYPE);
    types[ENTRY_FN - 1] = types[INT - 1];
    expect(data, TF_CORE_VERIFY_TYPE);
    types[ENTRY_FN - 1] = types[RECORD - 1];
    expect(data, TF_CORE_VERIFY_NONE); /* same display/layout, distinct nominal IDs */
    types[ENTRY_FN - 1] = baseline[ENTRY_FN - 1];

    values[1].as.float_bits = UINT64_C(0x8000000000000000);
    CHECK(expect(data, TF_CORE_VERIFY_CONSTANT).constant == 2);
    values[1].as.float_bits = UINT64_C(0x7ff0000000000000);
    expect(data, TF_CORE_VERIFY_CONSTANT);
    values[1].as.float_bits = UINT64_C(0x7ff8000000000001);
    expect(data, TF_CORE_VERIFY_CONSTANT);
    values[1].as.float_bits = UINT64_C(0xffefffffffffffff);
    expect(data, TF_CORE_VERIFY_NONE);
    values[2].as.boolean = 2;
    expect(data, TF_CORE_VERIFY_CONSTANT);
    values[2] = constants[2];
    values[3].as.text = (TfCoreText){invalid_utf8, COUNT(invalid_utf8)};
    expect(data, TF_CORE_VERIFY_CONSTANT);
    values[3] = constants[3];
    values[4].as.character = UINT32_C(0xd800);
    expect(data, TF_CORE_VERIFY_CONSTANT);
    values[4] = constants[4];
    values[6].as.children = (TfCoreConstantIds){(const TfCoreConstantId[]){7}, 1};
    expect(data, TF_CORE_VERIFY_CONSTANT);
    values[6] = constants[6];
    values[7].as.children = (TfCoreConstantIds){(const TfCoreConstantId[]){4, 1}, 2};
    expect(data, TF_CORE_VERIFY_CONSTANT);
    values[7] = constants[7];
    values[8].as.variant.tag = 1;
    expect(data, TF_CORE_VERIFY_CONSTANT);
    values[8] = constants[8];
    values[8].as.variant.payloads = (TfCoreConstantIds){(const TfCoreConstantId[]){99}, 1};
    expect(data, TF_CORE_VERIFY_CONSTANT);
    values[8] = constants[8];

    CHECK(tf_source_init(&source, "origin.tfd", "\xc3\xb1", 2, NULL) == TF_OK);
    CHECK(tf_source_span(&source, 0, 2, &span));
    data.spans = &span;
    data.span_count = 1;
    values[0].span = 1;
    expect(data, TF_CORE_VERIFY_NONE);
    span.start = 1;
    CHECK(expect(data, TF_CORE_VERIFY_SPAN).span == 1);
    span.start = 0;
    values[0].span = 2;
    CHECK(expect(data, TF_CORE_VERIFY_SPAN).span == 2);
    tf_source_destroy(&source);
}

static void ordinary_operations(void) {
    TfCoreProgramData data = program();
    TfCoreLet lets[] = {
        bind(1, INT, (TfCoreOperation){.kind = TF_CORE_OP_CONST, .as.constant = 1}),
        bind(2, FLOAT, (TfCoreOperation){TF_CORE_OP_CONST, {0}, {.constant = 2}}),
        bind(3, TEXT, (TfCoreOperation){TF_CORE_OP_CONST, {0}, {.constant = 4}}),
        bind(4, INT, (TfCoreOperation){TF_CORE_OP_UNARY, VALUES(1), {.unary = TF_CORE_UNARY_NEGATE}}),
        bind(5, FLOAT, (TfCoreOperation){TF_CORE_OP_BINARY, VALUES(1, 2), {.binary = TF_CORE_BINARY_REAL_DIVIDE}}),
        bind(6, FLOAT, (TfCoreOperation){TF_CORE_OP_BINARY, VALUES(5, 1), {.binary = TF_CORE_BINARY_POWER}}),
        bind(7, INT, (TfCoreOperation){TF_CORE_OP_BINARY, VALUES(4, 1), {.binary = TF_CORE_BINARY_FLOOR_DIVIDE}}),
        bind(8, RECORD, (TfCoreOperation){TF_CORE_OP_RECORD, VALUES(7, 3), {.constructed_type = RECORD}}),
        bind(9, MACHINE, (TfCoreOperation){TF_CORE_OP_MACHINE_VALUE, VALUES(8), {.constructed_type = MACHINE}}),
        bind(10, RECORD, (TfCoreOperation){TF_CORE_OP_PROJECT, VALUES(9), {.field = 0}}),
        bind(11, INT, (TfCoreOperation){TF_CORE_OP_PROJECT, VALUES(10), {.field = 0}}),
        bind(12, OPT_INT, (TfCoreOperation){TF_CORE_OP_VARIANT, VALUES(11), {.variant = {OPT_INT, 0}}}),
        bind(13, TAG, (TfCoreOperation){TF_CORE_OP_TAG, VALUES(12), {0}}),
        bind(14, TEXTS, (TfCoreOperation){TF_CORE_OP_LIST, VALUES(3), {.element_type = TEXT}}),
        bind(15, DECISION, (TfCoreOperation){TF_CORE_OP_VARIANT, VALUES(9, 14), {.variant = {DECISION, 0}}}),
        bind(16, BOOL, (TfCoreOperation){TF_CORE_OP_BINARY, VALUES(15, 15), {.binary = TF_CORE_BINARY_EQUAL}})
    };
    TfCoreBlock root = block(lets, COUNT(lets), 16);
    const TfCoreFunction fn = ordinary(NULL, 0, BOOL, &root, 1, COUNT(lets));
    const struct { size_t index; TfCoreVerifyCode error; TfCoreOperation op; } bad[] = {
        {0, TF_CORE_VERIFY_OPERATION, {TF_CORE_OP_CONST, {0}, {.constant = 0}}},
        {3, TF_CORE_VERIFY_VALUE, {TF_CORE_OP_UNARY, VALUES(4), {.unary = TF_CORE_UNARY_NEGATE}}},
        {3, TF_CORE_VERIFY_OPERATION, {TF_CORE_OP_UNARY, VALUES(1), {.unary = TF_CORE_UNARY_NOT}}},
        {4, TF_CORE_VERIFY_OPERATION, {TF_CORE_OP_BINARY, VALUES(1, 2), {.binary = TF_CORE_BINARY_ADD}}},
        {5, TF_CORE_VERIFY_OPERATION, {TF_CORE_OP_BINARY, VALUES(5, 2), {.binary = TF_CORE_BINARY_POWER}}},
        {6, TF_CORE_VERIFY_OPERATION, {TF_CORE_OP_BINARY, VALUES(4, 1), {.binary = (TfCoreBinary)99}}},
        {7, TF_CORE_VERIFY_OPERATION, {TF_CORE_OP_RECORD, VALUES(3, 7), {.constructed_type = RECORD}}},
        {8, TF_CORE_VERIFY_OPERATION, {TF_CORE_OP_MACHINE_VALUE, VALUES(7), {.constructed_type = MACHINE}}},
        {9, TF_CORE_VERIFY_OPERATION, {TF_CORE_OP_PROJECT, VALUES(9), {.field = SIZE_MAX}}},
        {11, TF_CORE_VERIFY_OPERATION, {TF_CORE_OP_VARIANT, VALUES(11), {.variant = {OPT_INT, 1}}}},
        {12, TF_CORE_VERIFY_OPERATION, {TF_CORE_OP_TAG, VALUES(11), {0}}},
        {13, TF_CORE_VERIFY_OPERATION, {TF_CORE_OP_LIST, VALUES(1), {.element_type = TEXT}}},
        {14, TF_CORE_VERIFY_OPERATION, {TF_CORE_OP_VARIANT, VALUES(9, 14), {.variant = {DECISION, 1}}}},
        {15, TF_CORE_VERIFY_OPERATION, {(TfCoreOperationKind)99, {0}, {0}}}
    };
    data.functions = &fn;
    data.function_count = 1;
    expect(data, TF_CORE_VERIFY_NONE);
    for (size_t i = 0; i < COUNT(bad); ++i) {
        TfCoreLet *let = &lets[bad[i].index];
        const TfCoreOperation saved = let->operation;
        let->operation = bad[i].op;
        CHECK(expect(data, bad[i].error).value == let->value);
        let->operation = saved;
    }
    root.terminator.value = 13; /* internal Tag cannot escape as Bool */
    expect(data, TF_CORE_VERIFY_TERMINATOR);
}

static void calls_and_builtins(void) {
    TfCoreProgramData data = program();
    const TfCoreParameter p[] = {{.value = 1, .type = INT}};
    const TfCoreParameter capture = {.value = 2, .type = TEXT};
    const TfCoreParameter fold[] = {{.value = 1, .type = TEXT}, {.value = 2, .type = INT}};
    const TfCoreBlock map_body = block(NULL, 0, 2);
    const TfCoreBlock fold_body = block(NULL, 0, 1);
    const TfCoreLet ordinary_let = bind(2, TEXT, (TfCoreOperation){.kind = TF_CORE_OP_CONST, .as.constant = 4});
    const TfCoreBlock ordinary_body = block(&ordinary_let, 1, 2);
    const TfCoreParameter root_params[] = {
        {.value = 1, .type = INT}, {.value = 2, .type = FLOAT},
        {.value = 3, .type = BOOL}, {.value = 4, .type = TEXT}, {.value = 5, .type = INTS}
    };
    TfCoreLet lets[] = {
        bind(6, MAP_FN, (TfCoreOperation){TF_CORE_OP_CLOSURE, VALUES(4), {.function = 2}}),
        bind(7, FOLD_FN, (TfCoreOperation){.kind = TF_CORE_OP_CLOSURE, .as.function = 3}),
        bind(8, TEXT, (TfCoreOperation){TF_CORE_OP_CALL, VALUES(1), {.function = 4}}),
        bind(9, TEXT, (TfCoreOperation){.kind = TF_CORE_OP_CALL_CLOSURE, .operands = VALUES(6, 1)}),
        bind(10, INT, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(5), {.builtin = {TF_CORE_BUILTIN_LEN, TYPES(INT)}}}),
        bind(11, OPT_INT, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(5, 1), {.builtin = {TF_CORE_BUILTIN_AT, TYPES(INT)}}}),
        bind(12, INTS, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(5, 1), {.builtin = {TF_CORE_BUILTIN_APPEND, TYPES(INT)}}}),
        bind(13, TEXTS, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(5, 6), {.builtin = {TF_CORE_BUILTIN_MAP, TYPES(INT, TEXT)}}}),
        bind(14, TEXT, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(5, 4, 7), {.builtin = {TF_CORE_BUILTIN_FOLD, TYPES(INT, TEXT)}}}),
        bind(15, INTS, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(1, 1), {.builtin = {TF_CORE_BUILTIN_RANGE, {0}}}}),
        bind(16, TEXT, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(1), {.builtin = {TF_CORE_BUILTIN_INT_TEXT, {0}}}}),
        bind(17, FLOAT, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(1), {.builtin = {TF_CORE_BUILTIN_INT_FLOAT, {0}}}}),
        bind(18, OPT_INT, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(2), {.builtin = {TF_CORE_BUILTIN_FLOAT_INT, {0}}}}),
        bind(19, TEXT, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(2), {.builtin = {TF_CORE_BUILTIN_FLOAT_TEXT, {0}}}}),
        bind(20, TEXT, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(3), {.builtin = {TF_CORE_BUILTIN_BOOL_TEXT, {0}}}}),
        bind(21, OPT_INT, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(4), {.builtin = {TF_CORE_BUILTIN_PARSE_INT, {0}}}}),
        bind(22, INT, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(4), {.builtin = {TF_CORE_BUILTIN_TEXT_LEN, {0}}}}),
        bind(23, OPT_TEXT, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(4, 1), {.builtin = {TF_CORE_BUILTIN_TEXT_AT, {0}}}}),
        bind(24, OPT_TEXT, (TfCoreOperation){TF_CORE_OP_BUILTIN, VALUES(4, 1, 1), {.builtin = {TF_CORE_BUILTIN_TEXT_SLICE, {0}}}})
    };
    const TfCoreBlock root = block(lets, COUNT(lets), 24);
    TfCoreFunction fns[] = {
        ordinary(root_params, COUNT(root_params), OPT_TEXT, &root, 1, 24),
        ordinary(p, 1, TEXT, &map_body, 1, 2),
        ordinary(fold, 2, TEXT, &fold_body, 1, 2),
        ordinary(p, 1, TEXT, &ordinary_body, 1, 2)
    };
    const struct { size_t index; TfCoreOperation op; } bad[] = {
        {0, {TF_CORE_OP_CLOSURE, VALUES(1), {.function = 2}}},
        {2, {TF_CORE_OP_CALL, VALUES(1), {.function = 2}}},
        {2, {TF_CORE_OP_CALL, VALUES(2), {.function = 4}}},
        {2, {TF_CORE_OP_CALL, VALUES(1), {.function = SIZE_MAX}}},
        {3, {TF_CORE_OP_CALL_CLOSURE, VALUES(6, 2), {0}}},
        {3, {TF_CORE_OP_CALL_CLOSURE, VALUES(6), {0}}},
        {3, {TF_CORE_OP_BINARY, VALUES(6, 6), {.binary = TF_CORE_BINARY_EQUAL}}},
        {4, {TF_CORE_OP_BUILTIN, VALUES(5), {.builtin = {TF_CORE_BUILTIN_LEN, TYPES(FLOAT)}}}},
        {7, {TF_CORE_OP_BUILTIN, VALUES(5, 7), {.builtin = {TF_CORE_BUILTIN_MAP, TYPES(INT, TEXT)}}}},
        {8, {TF_CORE_OP_BUILTIN, VALUES(5, 4, 6), {.builtin = {TF_CORE_BUILTIN_FOLD, TYPES(INT, TEXT)}}}},
        {13, {TF_CORE_OP_BUILTIN, VALUES(2), {.builtin = {TF_CORE_BUILTIN_FLOAT_TEXT, TYPES(FLOAT)}}}},
        {18, {TF_CORE_OP_BUILTIN, VALUES(4, 1), {.builtin = {TF_CORE_BUILTIN_TEXT_SLICE, {0}}}}}
    };
    fns[1].captures = &capture;
    fns[1].capture_count = 1;
    data.functions = fns;
    data.function_count = COUNT(fns);
    expect(data, TF_CORE_VERIFY_NONE);
    for (size_t i = 0; i < COUNT(bad); ++i) {
        const TfCoreOperation saved = lets[bad[i].index].operation;
        lets[bad[i].index].operation = bad[i].op;
        CHECK(expect(data, TF_CORE_VERIFY_OPERATION).value == lets[bad[i].index].value);
        lets[bad[i].index].operation = saved;
    }
    lets[0].type = FOLD_FN;
    expect(data, TF_CORE_VERIFY_OPERATION);
    lets[0].type = MAP_FN;
    lets[3].type = BOOL;
    lets[3].operation = (TfCoreOperation){TF_CORE_OP_BINARY, VALUES(6, 6), {.binary = TF_CORE_BINARY_EQUAL}};
    expect(data, TF_CORE_VERIFY_OPERATION); /* correct Bool result, but no equality for closures */
}

static void scopes(void) {
    TfCoreProgramData data = program();
    const TfCoreParameter p[] = {{.value = 1, .type = BOOL}, {.value = 2, .type = INTS}};
    TfCoreLet root[] = {
        bind(3, INTS, (TfCoreOperation){TF_CORE_OP_LIST_MAP, VALUES(2), {.list_map = {{.value = 4, .type = INT}, 2}}})
    };
    TfCoreLet map[] = {
        bind(5, INT, (TfCoreOperation){TF_CORE_OP_IF, VALUES(1), {.branch = {3, 4}}})
    };
    TfCoreLet yes[] = {
        bind(6, INT, (TfCoreOperation){TF_CORE_OP_UNARY, VALUES(4), {.unary = TF_CORE_UNARY_NEGATE}})
    };
    TfCoreBlock blocks[] = {
        block(root, 1, 3), block(map, 1, 5), block(yes, 1, 6), block(NULL, 0, 4)
    };
    TfCoreFunction fn = ordinary(p, COUNT(p), INTS, blocks, COUNT(blocks), 6);
    data.functions = &fn;
    data.function_count = 1;
    expect(data, TF_CORE_VERIFY_NONE);
    blocks[3].terminator.value = 6;
    CHECK(expect(data, TF_CORE_VERIFY_TERMINATOR).block == 4); /* sibling-local */
    blocks[3].terminator.value = 5;
    expect(data, TF_CORE_VERIFY_TERMINATOR); /* enclosing result not yet bound */
    blocks[3].terminator.value = 1;
    expect(data, TF_CORE_VERIFY_TERMINATOR); /* wrong result */
    blocks[3].terminator.value = 4;
    blocks[0].terminator.value = 4;
    expect(data, TF_CORE_VERIFY_TERMINATOR); /* map element does not escape */
    blocks[0].terminator.value = 3;
    root[0].operation.as.list_map.element.value = 2;
    expect(data, TF_CORE_VERIFY_VALUE);
    root[0].operation.as.list_map.element.value = 4;
    yes[0].value = 5;
    blocks[2].terminator.value = 5;
    expect(data, TF_CORE_VERIFY_VALUE); /* inner def and delayed enclosing def clash */
    yes[0].value = 6;
    blocks[2].terminator.value = 6;
    map[0].operation.as.branch.else_block = 3;
    expect(data, TF_CORE_VERIFY_BLOCK); /* shared block */
    map[0].operation.as.branch.else_block = 1;
    expect(data, TF_CORE_VERIFY_BLOCK); /* ancestor cycle */
    map[0].operation.as.branch.else_block = 0;
    expect(data, TF_CORE_VERIFY_BLOCK);
    map[0].operation.as.branch.else_block = 4;
    fn.value_count = 7;
    CHECK(expect(data, TF_CORE_VERIFY_VALUE).value == 7);
    fn.value_count = 6;
    fn.body = 4;
    expect(data, TF_CORE_VERIFY_TERMINATOR); /* no map binding at function entry */
}

static void tag_proofs(void) {
    TfCoreProgramData data = program();
    const TfCoreParameter p[] = {
        {.value = 1, .type = OPT_INT}, {.value = 2, .type = OPT_INT}, {.value = 3, .type = INT}
    };
    TfCoreTagCase cases[] = {{TF_CORE_OPTION_NONE, 2}, {TF_CORE_OPTION_SOME, 3}};
    TfCoreLet root = bind(4, INT, (TfCoreOperation){TF_CORE_OP_SWITCH_TAG, VALUES(1),
                                       {.switch_tag = {cases, 1, 3}}});
    TfCoreLet payload = bind(5, INT, (TfCoreOperation){TF_CORE_OP_PAYLOAD, VALUES(1),
                                       {.payload = {TF_CORE_OPTION_SOME, 0}}});
    TfCoreBlock blocks[] = {block(&root, 1, 4), block(NULL, 0, 3), block(&payload, 1, 5), dead()};
    const TfCoreBlock proved = blocks[2];
    TfCoreFunction fn = ordinary(p, COUNT(p), INT, blocks, 3, 5);
    data.functions = &fn;
    data.function_count = 1;
    expect(data, TF_CORE_VERIFY_NONE); /* default subtracts None, leaving Some */
    payload.operation.operands = VALUES(2);
    CHECK(expect(data, TF_CORE_VERIFY_TAG_PROOF).block == 3); /* same type, unrelated ID */
    payload.operation.operands = VALUES(1);
    payload.operation.as.payload.index = 1;
    expect(data, TF_CORE_VERIFY_OPERATION);
    payload.operation.as.payload.index = 0;
    root.operation.as.switch_tag.case_count = 0;
    expect(data, TF_CORE_VERIFY_TAG_PROOF); /* no refinement */
    root.operation.as.switch_tag.case_count = 1;
    root.operation.as.switch_tag.default_block = 0;
    expect(data, TF_CORE_VERIFY_TAG_PROOF); /* missing Some */
    root.operation.as.switch_tag.default_block = 3;
    blocks[2] = dead();
    expect(data, TF_CORE_VERIFY_TERMINATOR); /* explicit unreachable cannot hide Some */
    blocks[2] = proved;
    root.operation.as.switch_tag.case_count = 2;
    root.operation.as.switch_tag.default_block = 4;
    fn.block_count = 4;
    expect(data, TF_CORE_VERIFY_NONE); /* exhaustive, explicit dead default */
    root.operation.as.switch_tag.default_block = 0;
    fn.block_count = 3;
    expect(data, TF_CORE_VERIFY_NONE); /* exhaustive, implicit dead default */
    cases[1].tag = TF_CORE_OPTION_NONE;
    expect(data, TF_CORE_VERIFY_TAG_PROOF);
    cases[1].tag = 2;
    expect(data, TF_CORE_VERIFY_TAG_PROOF);

    /* A singleton layout still requires a dominating branch proof. */
    {
        TfCoreType types[COUNT(baseline)];
        const TfCoreParameter single = {.value = 1, .type = ENUM};
        const TfCoreLet project = bind(2, INT, (TfCoreOperation){TF_CORE_OP_PAYLOAD,
                                                VALUES(1), {.payload = {0, 0}}});
        const TfCoreBlock no_proof = block(&project, 1, 2);
        for (size_t i = 0; i < COUNT(types); ++i) types[i] = baseline[i];
        types[ENUM - 1].as.enumeration.variant_count = 1;
        data.types = types;
        fn = ordinary(&single, 1, INT, &no_proof, 1, 2);
        expect(data, TF_CORE_VERIFY_TAG_PROOF);
    }
}

static void bool_proofs(void) {
    TfCoreProgramData data = program();
    const TfCoreParameter p[] = {
        {.value = 1, .type = BOOL}, {.value = 2, .type = BOOL}, {.value = 3, .type = INT}
    };
    const TfCoreLet root = bind(4, INT, (TfCoreOperation){TF_CORE_OP_IF, VALUES(1), {.branch = {2, 3}}});
    TfCoreLet nested = bind(5, INT, (TfCoreOperation){TF_CORE_OP_IF, VALUES(1), {.branch = {4, 5}}});
    TfCoreBlock blocks[] = {
        block(&root, 1, 4), block(&nested, 1, 5), block(NULL, 0, 3), block(NULL, 0, 3), dead()
    };
    const TfCoreFunction fn = ordinary(p, COUNT(p), INT, blocks, COUNT(blocks), 5);
    data.functions = &fn;
    data.function_count = 1;
    expect(data, TF_CORE_VERIFY_NONE);
    nested.operation.operands = VALUES(2);
    CHECK(expect(data, TF_CORE_VERIFY_TERMINATOR).block == 5);
    nested.operation.operands = VALUES(1);
    blocks[4].terminator.value = 3;
    expect(data, TF_CORE_VERIFY_TERMINATOR);
    blocks[4] = dead();
    blocks[0].terminator = dead().terminator;
    CHECK(expect(data, TF_CORE_VERIFY_TERMINATOR).block == 1);
}

static void entry_protocol(void) {
    TfCoreProgramData data = program();
    const TfCoreParameter snapshot = {.value = 1, .type = TEXTS};
    TfCoreLet entry[] = {
        bind(2, TEXT, (TfCoreOperation){TF_CORE_OP_INPUT_AT, VALUES(1), {.input_index = 0}}),
        bind(3, TEXTS, (TfCoreOperation){TF_CORE_OP_LIST, VALUES(2), {.element_type = TEXT}})
    };
    TfCoreBlock entry_body = block(entry, COUNT(entry), 3);
    TfCoreLet caller = bind(2, TEXTS, (TfCoreOperation){TF_CORE_OP_CALL, VALUES(1), {.function = 1}});
    TfCoreBlock caller_body = block(&caller, 1, 2);
    TfCoreFunction fns[] = {
        ordinary(&snapshot, 1, TEXTS, &entry_body, 1, 3),
        ordinary(&snapshot, 1, TEXTS, &caller_body, 1, 2)
    };
    data.entry = 1;
    fns[0].kind = TF_CORE_FUNCTION_ENTRY_ONLY;
    data.functions = fns;
    data.function_count = 1;
    expect(data, TF_CORE_VERIFY_NONE);
    data.entry = 2;
    expect(data, TF_CORE_VERIFY_ENTRY);
    data.entry = 1;
    fns[0].capture_count = 1;
    fns[0].captures = &snapshot;
    expect(data, TF_CORE_VERIFY_ENTRY);
    fns[0].capture_count = 0;
    data.function_count = 2;
    CHECK(expect(data, TF_CORE_VERIFY_OPERATION).function == 2);
    caller.type = ENTRY_FN;
    caller.operation = (TfCoreOperation){.kind = TF_CORE_OP_CLOSURE, .as.function = 1};
    fns[1].result = ENTRY_FN;
    CHECK(expect(data, TF_CORE_VERIFY_OPERATION).function == 2);

    data.function_count = 1;
    data.entry = 0;
    fns[0].kind = TF_CORE_FUNCTION_ORDINARY;
    CHECK(expect(data, TF_CORE_VERIFY_ENTRY).value == 2);
    data.entry = 1;
    fns[0].kind = TF_CORE_FUNCTION_ENTRY_ONLY;
    entry[0].operation.operands = VALUES(2);
    expect(data, TF_CORE_VERIFY_VALUE);
    entry[0].operation.operands = VALUES(1);
    {
        const TfCoreLet old_input = entry[0];
        const TfCoreLet nested[] = {old_input};
        const TfCoreLet parent[] = {
            bind(4, BOOL, (TfCoreOperation){.kind = TF_CORE_OP_CONST, .as.constant = 3}),
            bind(6, TEXT, (TfCoreOperation){.kind = TF_CORE_OP_CONST, .as.constant = 4}),
            bind(5, TEXT, (TfCoreOperation){TF_CORE_OP_IF, VALUES(4), {.branch = {2, 3}}}),
            bind(3, TEXTS, (TfCoreOperation){TF_CORE_OP_LIST, VALUES(5), {.element_type = TEXT}})
        };
        const TfCoreBlock blocks[] = {
            block(parent, COUNT(parent), 3), block(nested, 1, 2), block(NULL, 0, 6)
        };
        fns[0].blocks = blocks;
        fns[0].block_count = COUNT(blocks);
        fns[0].value_count = 6;
        CHECK(expect(data, TF_CORE_VERIFY_ENTRY).block == 2);
    }
    {
        const TfCoreLet pure = bind(1, TEXTS, (TfCoreOperation){.kind = TF_CORE_OP_LIST, .as.element_type = TEXT});
        const TfCoreBlock pure_body = block(&pure, 1, 1);
        caller = bind(1, TEXTS, (TfCoreOperation){.kind = TF_CORE_OP_CALL, .as.function = 1});
        caller_body = block(&caller, 1, 1);
        fns[0] = ordinary(NULL, 0, TEXTS, &pure_body, 1, 1);
        fns[1] = ordinary(NULL, 0, TEXTS, &caller_body, 1, 1);
        data.function_count = 2;
        expect(data, TF_CORE_VERIFY_NONE); /* pure main is callable */
    }
}

typedef struct { size_t calls, fail, live; } Memory;

static void *allocate(void *context, size_t bytes) {
    Memory *m = context;
    void *p;
    if (++m->calls == m->fail) return NULL;
    p = malloc(bytes);
    CHECK(p != NULL);
    ++m->live;
    return p;
}

static void deallocate(void *context, void *p) {
    Memory *m = context;
    CHECK(p != NULL && m->live != 0);
    --m->live;
    free(p);
}

static void allocation_failures(void) {
    TfCoreProgramData data = program();
    const TfCoreParameter p = {.value = 1, .type = INT};
    const TfCoreBlock root = block(NULL, 0, 1);
    TfCoreFunction fn = ordinary(&p, 1, INT, &root, 1, 1);
    TfCoreProgram *owner = NULL;
    TfVerifiedCore *verified = NULL;
    TfCoreVerifyIssue issue = {0};
    Memory m = {0};
    const TfAllocator allocator = {&m, allocate, deallocate};
    size_t total;
    data.functions = &fn;
    data.function_count = 1;
    CHECK(tf_core_program_copy(&owner, &data, NULL) == TF_OK);
    CHECK(tf_core_verify(&verified, owner, &allocator, &issue) == TF_OK);
    CHECK(m.live == 1);
    total = m.calls;
    CHECK(total > 2);
    tf_verified_core_destroy(verified);
    for (size_t i = 1; i <= total; ++i) {
        verified = NULL;
        issue.code = TF_CORE_VERIFY_TYPE;
        m = (Memory){.fail = i};
        CHECK(tf_core_verify(&verified, owner, &allocator, &issue) == TF_HOST_ERROR);
        CHECK(verified == NULL && issue.code == TF_CORE_VERIFY_NONE && m.live == 0);
    }
    tf_core_program_destroy(owner);
    owner = NULL;
    fn.value_count = SIZE_MAX;
    CHECK(tf_core_program_copy(&owner, &data, NULL) == TF_OK);
    CHECK(tf_core_verify(&verified, owner, NULL, &issue) == TF_HOST_ERROR);
    CHECK(verified == NULL && issue.code == TF_CORE_VERIFY_NONE);
    tf_core_program_destroy(owner);
    CHECK(tf_core_verify(&verified, NULL, NULL, &issue) == TF_HOST_ERROR);
    CHECK(verified == NULL);
    CHECK(tf_verified_core_data(NULL) == NULL);
    tf_verified_core_destroy(NULL);
}

static void deep_blocks(void) {
    enum { DEPTH = 4096 };
    TfCoreProgramData data = program();
    TfCoreBlock *blocks = calloc(2 * DEPTH + 1, sizeof(*blocks));
    TfCoreLet *lets = calloc(DEPTH, sizeof(*lets));
    const TfCoreParameter p[] = {{.value = 1, .type = BOOL}, {.value = 2, .type = INT}};
    const TfCoreValueId condition[] = {1};
    const TfCoreFunction fn = ordinary(p, COUNT(p), INT, blocks, 2 * DEPTH + 1, DEPTH + 2);
    CHECK(blocks != NULL && lets != NULL);
    for (size_t i = 0; i < DEPTH; ++i) {
        lets[i] = bind(i + 3, INT, (TfCoreOperation){TF_CORE_OP_IF, {condition, 1},
                                                         {.branch = {i + 2, DEPTH + i + 2}}});
        blocks[i] = block(&lets[i], 1, i + 3);
        blocks[DEPTH + i + 1] = block(NULL, 0, 2);
    }
    blocks[DEPTH] = block(NULL, 0, 2);
    data.functions = &fn;
    data.function_count = 1;
    expect(data, TF_CORE_VERIFY_NONE);
    lets[DEPTH - 1].operation.as.branch.then_block = 1;
    expect(data, TF_CORE_VERIFY_BLOCK);
    free(lets);
    free(blocks);
}

static void deep_types(void) {
    enum { DEPTH = 8192 };
    TfCoreType *types = calloc(DEPTH, sizeof(*types));
    const TfCoreProgramData data = {.types = types, .type_count = DEPTH};
    CHECK(types != NULL);
    for (size_t i = 0; i + 1 < DEPTH; ++i)
        types[i] = (TfCoreType){.kind = TF_CORE_TYPE_LIST, .as.element = i + 2};
    types[DEPTH - 1].kind = TF_CORE_TYPE_INT;
    expect(data, TF_CORE_VERIFY_NONE);
    types[DEPTH - 1] = (TfCoreType){.kind = TF_CORE_TYPE_LIST, .as.element = 1};
    expect(data, TF_CORE_VERIFY_TYPE_CYCLE);
    free(types);
}

int main(void) {
    metadata();
    ordinary_operations();
    calls_and_builtins();
    scopes();
    tag_proofs();
    bool_proofs();
    entry_protocol();
    allocation_failures();
    deep_blocks();
    deep_types();
    puts("Core verifier: metadata, all operations, lexical scopes, tag facts, entry, OOM and depth passed");
    return 0;
}
