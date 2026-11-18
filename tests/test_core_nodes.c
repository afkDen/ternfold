#include "ternfold/core.h"

#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "core_nodes:%d: %s\n", __LINE__, #expr); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

typedef struct { size_t calls, fail_at, live; } Counter;

static void *count_allocate(void *context, size_t bytes) {
    Counter *counter = context;
    void *result;
    CHECK(bytes != 0);
    if (++counter->calls == counter->fail_at) return NULL;
    result = malloc(bytes);
    if (result != NULL) ++counter->live;
    return result;
}

static void count_deallocate(void *context, void *pointer) {
    Counter *counter = context;
    CHECK(pointer != NULL && counter->live != 0);
    --counter->live;
    free(pointer);
}

enum {
    T_INT = 1, T_FLOAT, T_BOOL, T_TEXT, T_CHAR, T_UNIT, T_LIST_TEXT, T_LIST_INT,
    T_OPTION, T_RECORD, T_ENUM, T_MACHINE, T_DECISION, T_TAG, T_FN
};

enum {
    C_INT = 1, C_TEXT, C_LONG_TEXT, C_FLOAT_SUBNORMAL, C_FLOAT_MAX, C_FLOAT_ZERO,
    C_CHAR, C_BOOL, C_UNIT, C_LIST, C_RECORD, C_VARIANT
};

enum { F_HELPER = 1, F_ENTRY, F_INSPECT };

static TfStatus fixture(TfCoreProgram **out, const TfSource *source,
                         const TfAllocator *allocator) {
    unsigned char name[] = "owned";
    unsigned char text[] = {'A', 0, 0xc3, 0xa9};
    unsigned char long_text[9000];
    TfCoreText label = {name, sizeof(name) - 1};
    TfSpan spans[] = {{source, 0, 1}, {source, 1, 3}};
    TfCoreField fields[] = {{label, T_INT, 1}};
    TfCoreField cells[] = {{label, T_INT, 2}};
    TfCoreTypeId int_type[] = {T_INT};
    TfCoreVariant variants[] = {{label, {int_type, 1}, 2}, {label, {0}, 1}};
    TfCoreType types[] = {
        {.kind = TF_CORE_TYPE_INT}, {.kind = TF_CORE_TYPE_FLOAT},
        {.kind = TF_CORE_TYPE_BOOL}, {.kind = TF_CORE_TYPE_TEXT},
        {.kind = TF_CORE_TYPE_CHAR}, {.kind = TF_CORE_TYPE_UNIT},
        {.kind = TF_CORE_TYPE_LIST, .as.element = T_TEXT},
        {.kind = TF_CORE_TYPE_LIST, .as.element = T_INT},
        {.kind = TF_CORE_TYPE_OPTION, .as.element = T_INT},
        {.kind = TF_CORE_TYPE_RECORD, .name = label, .module = label, .span = 2,
         .as.record = {fields, COUNT(fields)}},
        {.kind = TF_CORE_TYPE_ENUM, .name = label,
         .as.enumeration = {variants, COUNT(variants)}},
        {.kind = TF_CORE_TYPE_MACHINE, .name = label,
         .as.machine = {cells, COUNT(cells), T_INT, T_TEXT, T_ENUM}},
        {.kind = TF_CORE_TYPE_DECISION, .as.decision = {T_MACHINE, T_TEXT, T_ENUM, T_LIST_TEXT}},
        {.kind = TF_CORE_TYPE_TAG, .as.tagged = T_ENUM},
        {.kind = TF_CORE_TYPE_FUNCTION, .as.function = {{int_type, 1}, T_INT}}
    };
    TfCoreConstantId text_children[] = {C_TEXT, C_LONG_TEXT}, int_children[] = {C_INT};
    TfCoreConstant constants[] = {
        {TF_CORE_CONSTANT_INT, T_INT, 1, {.integer = INT64_MIN}},
        {TF_CORE_CONSTANT_TEXT, T_TEXT, 2, {.text = {text, COUNT(text)}}},
        {TF_CORE_CONSTANT_TEXT, T_TEXT, 2, {.text = {long_text, COUNT(long_text)}}},
        {TF_CORE_CONSTANT_FLOAT, T_FLOAT, 2, {.float_bits = UINT64_C(1)}},
        {TF_CORE_CONSTANT_FLOAT, T_FLOAT, 2, {.float_bits = UINT64_C(0x7fefffffffffffff)}},
        {TF_CORE_CONSTANT_FLOAT, T_FLOAT, 2, {.float_bits = 0}},
        {TF_CORE_CONSTANT_CHAR, T_CHAR, 2, {.character = UINT32_C(0x10ffff)}},
        {TF_CORE_CONSTANT_BOOL, T_BOOL, 2, {.boolean = 1}},
        {TF_CORE_CONSTANT_UNIT, T_UNIT, 2, {0}},
        {TF_CORE_CONSTANT_LIST, T_LIST_TEXT, 2, {.children = {text_children, 2}}},
        {TF_CORE_CONSTANT_RECORD, T_RECORD, 2, {.children = {int_children, 1}}},
        {TF_CORE_CONSTANT_VARIANT, T_ENUM, 2, {.variant = {0, {int_children, 1}}}}
    };
    TfCoreValueId one[] = {1}, two[] = {2}, five[] = {5}, seven[] = {7}, eight[] = {8};
    TfCoreValueId helper_args[] = {1, 2}, joined[] = {3, 5}, closure_args[] = {6, 5};
    TfCoreParameter helper_param[] = {{1, T_INT, label, 1}};
    TfCoreParameter helper_capture[] = {{2, T_INT, label, 1}};
    TfCoreParameter input_param[] = {{1, T_LIST_TEXT, label, 1}};
    TfCoreParameter enum_param[] = {{1, T_ENUM, label, 1}};
    TfCoreLet helper[] = {
        {3, T_INT, {TF_CORE_OP_BINARY, {helper_args, 2}, {.binary = TF_CORE_BINARY_ADD}}, 2}
    };
    TfCoreBlock helper_blocks[] = {{helper, 1, {TF_CORE_YIELD, 3, 2}, 1}};
    TfCoreLet entry[] = {
        {2, T_TEXT, {TF_CORE_OP_INPUT_AT, {one, 1}, {.input_index = 0}}, 1},
        {3, T_LIST_TEXT, {TF_CORE_OP_LIST, {two, 1}, {.element_type = T_TEXT}}, 1},
        {5, T_LIST_TEXT, {TF_CORE_OP_LIST_MAP, {one, 1},
                         {.list_map = {{4, T_TEXT, label, 1}, 2}}}, 1},
        {6, T_LIST_TEXT, {TF_CORE_OP_BINARY, {joined, 2}, {.binary = TF_CORE_BINARY_CONCAT}}, 2}
    };
    TfCoreBlock entry_blocks[] = {
        {entry, COUNT(entry), {TF_CORE_YIELD, 6, 2}, 1},
        {NULL, 0, {TF_CORE_YIELD, 4, 2}, 2}
    };
    TfCoreTagCase cases[] = {{0, 2}, {1, 3}};
    TfCoreLet inspect[] = {
        {2, T_TAG, {.kind = TF_CORE_OP_TAG, .operands = {one, 1}}, 1},
        {5, T_INT, {TF_CORE_OP_SWITCH_TAG, {one, 1}, {.switch_tag = {cases, 2, 0}}}, 1},
        {6, T_FN, {TF_CORE_OP_CLOSURE, {five, 1}, {.function = F_HELPER}}, 1},
        {7, T_INT, {.kind = TF_CORE_OP_CALL_CLOSURE, .operands = {closure_args, 2}}, 1},
        {8, T_LIST_INT, {TF_CORE_OP_LIST, {seven, 1}, {.element_type = T_INT}}, 1},
        {9, T_INT, {TF_CORE_OP_BUILTIN, {eight, 1}, {.builtin = {TF_CORE_BUILTIN_LEN, {int_type, 1}}}}, 2}
    };
    TfCoreLet some[] = {{3, T_INT, {TF_CORE_OP_PAYLOAD, {one, 1}, {.payload = {0, 0}}}, 1}};
    TfCoreLet none[] = {{4, T_INT, {TF_CORE_OP_CONST, {NULL, 0}, {.constant = C_INT}}, 2}};
    TfCoreBlock inspect_blocks[] = {
        {inspect, COUNT(inspect), {TF_CORE_YIELD, 9, 2}, 1},
        {some, 1, {TF_CORE_YIELD, 3, 1}, 1},
        {none, 1, {TF_CORE_YIELD, 4, 2}, 2}
    };
    TfCoreFunction functions[] = {
        {TF_CORE_FUNCTION_ORDINARY, label, label, 1, helper_param, helper_capture,
         1, 1, T_INT, helper_blocks, 1, 1, 3},
        {TF_CORE_FUNCTION_ENTRY_ONLY, label, label, 1, input_param, NULL,
         1, 0, T_LIST_TEXT, entry_blocks, COUNT(entry_blocks), 1, 6},
        {TF_CORE_FUNCTION_ORDINARY, label, label, 2, enum_param, NULL,
         1, 0, T_INT, inspect_blocks, COUNT(inspect_blocks), 1, 9}
    };
    TfCoreProgramData input = {types, COUNT(types), constants, COUNT(constants),
                               functions, COUNT(functions), spans, COUNT(spans), F_ENTRY};
    TfStatus status;
    memset(long_text, 'x', sizeof(long_text));
    long_text[sizeof(long_text) - 1] = 'z';
    status = tf_core_program_copy(out, &input, allocator);
    if (status == TF_OK) {
        const TfCoreProgramData *frozen = tf_core_program_data(*out);
        CHECK(frozen != &input && frozen->types != types && frozen->constants != constants);
        CHECK(frozen->functions != functions && frozen->spans != spans);
        CHECK(frozen->types[T_RECORD - 1].as.record.fields != fields);
        CHECK(frozen->types[T_ENUM - 1].as.enumeration.variants != variants);
        CHECK(frozen->constants[C_TEXT - 1].as.text.bytes != text);
        CHECK(frozen->functions[F_ENTRY - 1].blocks != entry_blocks);
        name[0] = '!';
        text[2] = '!';
        long_text[0] = '!';
        spans[1].end = 0;
        fields[0].type = 0;
        int_type[0] = 0;
        variants[0].payloads.count = 0;
        text_children[0] = 0;
        int_children[0] = 0;
        input_param[0].value = 0;
        helper_capture[0].type = 0;
        cases[1].block = 0;
        one[0] = 0;
        entry[2].operation.as.list_map.body = 0;
        inspect_blocks[0].let_count = 0;
    }
    return status;
}

static bool label(TfCoreText text) {
    return text.length == 5 && text.bytes != NULL && memcmp(text.bytes, "owned", 5) == 0;
}

static void ownership(const TfSource *source) {
    TfCoreProgram *program = NULL, *second = NULL;
    const TfCoreProgramData *data;
    const TfCoreType *t;
    const TfCoreConstant *c;
    const TfCoreFunction *fn;
    const TfCoreBlock *b;
    const TfSpan *span;
    CHECK(fixture(&program, source, NULL) == TF_OK && program != NULL);
    CHECK(tf_core_program_copy(&second, tf_core_program_data(program), NULL) == TF_OK);
    tf_core_program_destroy(program);
    data = tf_core_program_data(second);

    CHECK(data->type_count == T_FN && data->constant_count == C_VARIANT);
    CHECK(data->function_count == F_INSPECT && data->span_count == 2 && data->entry == F_ENTRY);
    span = tf_core_span(data, 2);
    CHECK(span != NULL && span->source == source && span->start == 1 && span->end == 3);
    t = tf_core_type(data, T_RECORD);
    CHECK(t != NULL && t->kind == TF_CORE_TYPE_RECORD && t->span == 2);
    CHECK(label(t->name) && label(t->module));
    CHECK(t->as.record.field_count == 1 && t->as.record.fields[0].type == T_INT);
    CHECK(label(t->as.record.fields[0].name));
    t = tf_core_type(data, T_ENUM);
    CHECK(t->as.enumeration.variant_count == 2);
    CHECK(label(t->as.enumeration.variants[0].name));
    CHECK(t->as.enumeration.variants[0].payloads.count == 1 &&
          t->as.enumeration.variants[0].payloads.items[0] == T_INT);
    CHECK(t->as.enumeration.variants[1].payloads.items == NULL);
    t = tf_core_type(data, T_MACHINE);
    CHECK(t->as.machine.cell_count == 1 && t->as.machine.cells[0].type == T_INT);
    CHECK(label(t->as.machine.cells[0].name));
    CHECK(t->as.machine.input == T_INT && t->as.machine.output == T_TEXT &&
          t->as.machine.rejection == T_ENUM);
    t = tf_core_type(data, T_DECISION);
    CHECK(t->as.decision.machine == T_MACHINE && t->as.decision.output == T_TEXT &&
          t->as.decision.rejection == T_ENUM && t->as.decision.emissions == T_LIST_TEXT);
    CHECK(tf_core_type(data, T_OPTION)->as.element == T_INT &&
          tf_core_type(data, T_TAG)->as.tagged == T_ENUM);
    t = tf_core_type(data, T_FN);
    CHECK(t->as.function.result == T_INT && t->as.function.parameters.items[0] == T_INT);

    c = tf_core_constant(data, C_TEXT);
    CHECK(c != NULL && c->kind == TF_CORE_CONSTANT_TEXT && c->type == T_TEXT && c->span == 2);
    CHECK(c->as.text.length == 4 && memcmp(c->as.text.bytes, "A\0\xc3\xa9", 4) == 0);
    c = tf_core_constant(data, C_LONG_TEXT);
    CHECK(c->as.text.length == 9000 && c->as.text.bytes[0] == 'x' && c->as.text.bytes[8999] == 'z');
    CHECK(tf_core_constant(data, C_INT)->as.integer == INT64_MIN);
    CHECK(tf_core_constant(data, C_FLOAT_SUBNORMAL)->as.float_bits == UINT64_C(1));
    CHECK(tf_core_constant(data, C_FLOAT_MAX)->as.float_bits == UINT64_C(0x7fefffffffffffff));
    CHECK(tf_core_constant(data, C_FLOAT_ZERO)->as.float_bits == 0);
    CHECK(tf_core_constant(data, C_CHAR)->as.character == UINT32_C(0x10ffff));
    CHECK(tf_core_constant(data, C_BOOL)->as.boolean == 1);
    CHECK(tf_core_constant(data, C_UNIT)->kind == TF_CORE_CONSTANT_UNIT);
    c = tf_core_constant(data, C_LIST);
    CHECK(c->as.children.count == 2 && c->as.children.items[0] == C_TEXT &&
          c->as.children.items[1] == C_LONG_TEXT);
    CHECK(tf_core_constant(data, C_RECORD)->as.children.items[0] == C_INT);
    c = tf_core_constant(data, C_VARIANT);
    CHECK(c->as.variant.tag == 0 && c->as.variant.payloads.items[0] == C_INT);

    fn = tf_core_function(data, F_HELPER);
    CHECK(fn->kind == TF_CORE_FUNCTION_ORDINARY && label(fn->name) && label(fn->module));
    CHECK(fn->captures[0].value == 2 && fn->captures[0].type == T_INT && label(fn->captures[0].name));
    CHECK(tf_core_block(fn, fn->body)->lets[0].operation.operands.items[1] == 2);
    fn = tf_core_function(data, data->entry);
    CHECK(fn->kind == TF_CORE_FUNCTION_ENTRY_ONLY && fn->capture_count == 0);
    CHECK(fn->parameter_count == 1 && fn->parameters[0].value == 1 &&
          fn->parameters[0].type == T_LIST_TEXT && label(fn->parameters[0].name));
    CHECK(fn->value_count == 6 && fn->block_count == 2 && fn->result == T_LIST_TEXT);
    b = tf_core_block(fn, fn->body);
    CHECK(b->let_count == 4 && b->terminator.kind == TF_CORE_YIELD && b->terminator.value == 6);
    CHECK(b->lets[0].operation.kind == TF_CORE_OP_INPUT_AT && b->lets[0].operation.operands.items[0] == 1);
    CHECK(b->lets[2].operation.kind == TF_CORE_OP_LIST_MAP &&
          b->lets[2].operation.as.list_map.element.value == 4 &&
          label(b->lets[2].operation.as.list_map.element.name));
    CHECK(tf_core_block(fn, b->lets[2].operation.as.list_map.body)->terminator.value == 4);
    fn = tf_core_function(data, F_INSPECT);
    b = tf_core_block(fn, fn->body);
    CHECK(b->let_count == 6 && b->lets[1].operation.as.switch_tag.case_count == 2 &&
          b->lets[1].operation.as.switch_tag.cases[1].block == 3);
    CHECK(b->lets[2].operation.as.function == F_HELPER &&
          b->lets[3].operation.operands.items[0] == 6);
    CHECK(b->lets[5].operation.as.builtin.type_arguments.items[0] == T_INT);
    CHECK(tf_core_block(fn, 2)->lets[0].operation.as.payload.index == 0);
    CHECK(tf_core_block(fn, 3)->lets[0].operation.as.constant == C_INT);

    CHECK(tf_core_type(data, 0) == NULL && tf_core_type(data, T_FN + 1) == NULL);
    CHECK(tf_core_constant(data, 0) == NULL && tf_core_constant(data, SIZE_MAX) == NULL);
    CHECK(tf_core_function(data, F_INSPECT + 1) == NULL && tf_core_function(data, 0) == NULL);
    CHECK(tf_core_span(data, 0) == NULL && tf_core_span(data, 3) == NULL);
    CHECK(tf_core_block(fn, 0) == NULL && tf_core_block(fn, 4) == NULL);
    tf_core_program_destroy(second);
}

static void allocation_failures(const TfSource *source) {
    Counter counter = {0};
    TfAllocator allocator = {&counter, count_allocate, count_deallocate};
    TfCoreProgram *program = NULL;
    size_t positions;
    CHECK(fixture(&program, source, &allocator) == TF_OK);
    positions = counter.calls;
    CHECK(positions >= 4 && counter.live == positions);
    tf_core_program_destroy(program);
    CHECK(counter.live == 0);
    for (size_t i = 1; i <= positions; ++i) {
        counter = (Counter){.fail_at = i};
        program = NULL;
        CHECK(fixture(&program, source, &allocator) == TF_HOST_ERROR);
        CHECK(program == NULL && counter.live == 0 && counter.calls == i);
        tf_core_program_destroy(program);
    }
    printf("core_nodes: exercised %zu allocation failure positions\n", positions);
}

static void storage_rejections(void) {
    TfCoreProgram *program = NULL;
    TfCoreType type = {.kind = TF_CORE_TYPE_ENUM};
    TfCoreTypeId bad_payload[] = {1};
    TfCoreVariant variant = {.payloads = {bad_payload, SIZE_MAX}};
    TfCoreConstant constant = {.kind = TF_CORE_CONSTANT_TEXT,
                               .as.text = {(const unsigned char *)"x", SIZE_MAX}};
    TfCoreProgramData bad = {.types = &type, .type_count = SIZE_MAX};
    TfAllocator invalid = {0};
    TfCoreBlock block = {0};
    TfCoreFunction fn = {.blocks = &block, .block_count = 1};
    TfCoreLet let = {.operation = {.kind = TF_CORE_OP_IF, .operands = {bad_payload, SIZE_MAX}}};

    CHECK(tf_core_program_copy(NULL, &bad, NULL) == TF_HOST_ERROR);
    CHECK(tf_core_program_copy(&program, NULL, NULL) == TF_HOST_ERROR && program == NULL);
    CHECK(tf_core_program_copy(&program, &bad, &invalid) == TF_HOST_ERROR && program == NULL);
    CHECK(tf_core_program_copy(&program, &bad, NULL) == TF_HOST_ERROR && program == NULL);
    bad = (TfCoreProgramData){.span_count = 1};
    CHECK(tf_core_program_copy(&program, &bad, NULL) == TF_HOST_ERROR && program == NULL);
    type.as.enumeration.variants = &variant;
    type.as.enumeration.variant_count = 1;
    bad = (TfCoreProgramData){.types = &type, .type_count = 1};
    CHECK(tf_core_program_copy(&program, &bad, NULL) == TF_HOST_ERROR && program == NULL);
    variant.payloads = (TfCoreTypeIds){NULL, 1};
    CHECK(tf_core_program_copy(&program, &bad, NULL) == TF_HOST_ERROR && program == NULL);
    bad = (TfCoreProgramData){.constants = &constant, .constant_count = 1};
    CHECK(tf_core_program_copy(&program, &bad, NULL) == TF_HOST_ERROR && program == NULL);
    block.lets = &let;
    block.let_count = 1;
    bad = (TfCoreProgramData){.functions = &fn, .function_count = 1};
    CHECK(tf_core_program_copy(&program, &bad, NULL) == TF_HOST_ERROR && program == NULL);
    let.operation.operands = (TfCoreValueIds){NULL, 1};
    CHECK(tf_core_program_copy(&program, &bad, NULL) == TF_HOST_ERROR && program == NULL);

    CHECK(tf_core_program_copy(&program, &(TfCoreProgramData){.types = &type}, NULL) == TF_OK);
    CHECK(tf_core_program_data(program)->types == NULL);
    tf_core_program_destroy(program);
    CHECK(tf_core_program_data(NULL) == NULL && tf_core_type(NULL, 1) == NULL &&
          tf_core_constant(NULL, 1) == NULL && tf_core_function(NULL, 1) == NULL &&
          tf_core_span(NULL, 1) == NULL && tf_core_block(NULL, 1) == NULL);
}

static void malformed_constructible(void) {
    TfCoreType types[] = {{.kind = (TfCoreTypeKind)99}, {.kind = TF_CORE_TYPE_LIST, .as.element = 2}};
    TfCoreConstantId cycle[] = {3};
    TfCoreConstant constants[] = {
        {.kind = TF_CORE_CONSTANT_FLOAT, .type = 99, .as.float_bits = UINT64_C(0x8000000000000000)},
        {.kind = TF_CORE_CONSTANT_FLOAT, .as.float_bits = UINT64_C(0x7ff8000000000000)},
        {.kind = TF_CORE_CONSTANT_LIST, .type = 2, .as.children = {cycle, 1}},
        {.kind = (TfCoreConstantKind)99},
        {.kind = TF_CORE_CONSTANT_CHAR, .as.character = UINT32_C(0xd800)},
        {.kind = TF_CORE_CONSTANT_BOOL, .as.boolean = 2}
    };
    TfCoreValueId invalid_operands[] = {3, 3, SIZE_MAX};
    TfCoreLet lets[] = {
        {1, 1, {TF_CORE_OP_IF, {invalid_operands, 3}, {.branch = {1, 99}}}, 20},
        {1, 2, {.kind = (TfCoreOperationKind)99}, 20}
    };
    TfCoreBlock block = {lets, COUNT(lets), {TF_CORE_UNREACHABLE, 55, 1}, 20};
    TfCoreFunction function = {.kind = TF_CORE_FUNCTION_ENTRY_ONLY, .body = 1,
                               .value_count = SIZE_MAX, .blocks = &block, .block_count = 1};
    TfCoreProgramData input = {types, COUNT(types), constants, COUNT(constants),
                               &function, 1, NULL, 0, 99};
    TfCoreProgram *program = NULL;
    const TfCoreProgramData *data;
    const TfCoreBlock *frozen;
    CHECK(tf_core_program_copy(&program, &input, NULL) == TF_OK);
    data = tf_core_program_data(program);
    CHECK(data->entry == 99 && tf_core_function(data, 1)->value_count == SIZE_MAX);
    CHECK(tf_core_type(data, 1)->kind == (TfCoreTypeKind)99 &&
          tf_core_type(data, 2)->as.element == 2);
    CHECK(tf_core_constant(data, 1)->type == 99 &&
          tf_core_constant(data, 1)->as.float_bits == UINT64_C(0x8000000000000000));
    CHECK(tf_core_constant(data, 2)->as.float_bits == UINT64_C(0x7ff8000000000000));
    CHECK(tf_core_constant(data, 3)->as.children.items[0] == 3);
    CHECK(tf_core_constant(data, 4)->kind == (TfCoreConstantKind)99);
    CHECK(tf_core_constant(data, 5)->as.character == UINT32_C(0xd800) &&
          tf_core_constant(data, 6)->as.boolean == 2);
    frozen = tf_core_block(tf_core_function(data, 1), 1);
    CHECK(frozen->span == 20 && frozen->terminator.value == 55);
    CHECK(frozen->lets[0].value == frozen->lets[1].value &&
          frozen->lets[0].operation.operands.items[2] == SIZE_MAX);
    CHECK(frozen->lets[0].operation.as.branch.then_block == 1 &&
          frozen->lets[0].operation.as.branch.else_block == 99);
    CHECK(frozen->lets[1].operation.kind == (TfCoreOperationKind)99);
    tf_core_program_destroy(program);
}

int main(void) {
    TfSource source = {0};
    CHECK(tf_source_init(&source, "core-nodes.tfd", "abc", 3, NULL) == TF_OK);
    ownership(&source);
    allocation_failures(&source);
    storage_rejections();
    malformed_constructible();
    tf_source_destroy(&source);
    printf("core_nodes: %zu checks passed\n", checks);
    return EXIT_SUCCESS;
}
