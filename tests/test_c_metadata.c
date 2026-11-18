/* Two-stage harness. The default build verifies Core and emits fixtures.
 * Pass EMPTY.inc LIBRARY.inc to also save them for the native builds.
 * Compile this file again with TF_TEST_METADATA_GENERATED, and for the empty
 * fixture also TF_TEST_METADATA_EMPTY. The include directory supplies
 * c_metadata_empty.inc or c_metadata_library.inc from the first stage. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "test_c_metadata:%d: %s\n", __LINE__, #test); exit(EXIT_FAILURE); \
} } while (0)
#define COUNT(array) (sizeof(array) / sizeof(*(array)))

#ifndef TF_TEST_METADATA_EMPTY
enum {
    INT = 1, FLOAT, BOOL, TEXT, CHAR, UNIT, TEXT_LIST, OPTION, PAIR, CHOICE,
    MACHINE, DECISION, FUNCTION, TAG, EMPTY_RECORD, MARKER
};
static const char path[] = "dir\\\"\n" "\xc3\xa9" "\?\?/ #include.tfd";
static const unsigned char text[] = {'"', '\\', 0, 'f', '\n', 0xc3, 0xa9};
#endif

#ifdef TF_TEST_METADATA_GENERATED

#include "ternfold_rt_constants.h"

#ifdef TF_TEST_METADATA_EMPTY
#include "c_metadata_empty.inc"
#else
#include "c_metadata_library.inc"

static tf_value literal(tf_constant_pool *pool, size_t id) {
    tf_value value = tf_unit();
    CHECK(tf_constant_get(pool, 1, id, &value) == TF_OK);
    return value;
}

static void library(tf_context *ctx, tf_constant_pool *pool) {
    CHECK(tf_type_count == MARKER && tf_types[FUNCTION - 1].kind == TF_RT_TYPE_FUNCTION);
    CHECK(tf_types[MARKER - 1].kind == TF_RT_TYPE_FUNCTION);
    CHECK(tf_types[TAG - 1].kind == TF_RT_TYPE_UNUSED);
    CHECK(tf_types[TEXT_LIST - 1].kind == TF_RT_TYPE_LIST &&
          tf_types[TEXT_LIST - 1].as.element == TEXT);
    CHECK(tf_types[OPTION - 1].kind == TF_RT_TYPE_VARIANT &&
          tf_types[OPTION - 1].as.variants.count == 2);
    const tf_rt_shape *option = tf_types[OPTION - 1].as.variants.tags;
    CHECK(option[0].count == 1 && option[0].types[0] == INT);
    CHECK(option[1].count == 0 && option[1].types == NULL);
    CHECK(tf_types[DECISION - 1].kind == TF_RT_TYPE_VARIANT &&
          tf_types[DECISION - 1].as.variants.count == 2);
    const tf_rt_shape *decision = tf_types[DECISION - 1].as.variants.tags;
    CHECK(decision[0].count == 2 && decision[0].types[0] == MACHINE &&
          decision[0].types[1] == TEXT_LIST);
    CHECK(decision[1].count == 2 && decision[1].types[0] == MACHINE &&
          decision[1].types[1] == CHOICE);
    CHECK(tf_types[MACHINE - 1].kind == TF_RT_TYPE_MACHINE &&
          tf_types[MACHINE - 1].as.fields.count == 1 &&
          tf_types[MACHINE - 1].as.fields.types[0] == INT);
    CHECK(tf_types[EMPTY_RECORD - 1].kind == TF_RT_TYPE_RECORD &&
          tf_types[EMPTY_RECORD - 1].as.fields.count == 0 &&
          tf_types[EMPTY_RECORD - 1].as.fields.types == NULL);

    CHECK(tf_literal_count == 16);
    CHECK(tf_literal_id_4 < tf_literal_id_2 && tf_literal_id_2 < tf_literal_id_1 &&
          tf_literal_id_3 < tf_literal_id_1 && tf_literal_id_4 < tf_literal_id_3);
    CHECK(tf_literals[tf_literal_id_4 - 1].span == 2);
    tf_value minimum = literal(pool, tf_literal_id_4), maximum = literal(pool, tf_literal_id_7);
    tf_value negative = literal(pool, tf_literal_id_8), subnormal = literal(pool, tf_literal_id_9);
    tf_value finite = literal(pool, tf_literal_id_10), boolean = literal(pool, tf_literal_id_11);
    tf_value character = literal(pool, tf_literal_id_12), unit = literal(pool, tf_literal_id_13);
    CHECK(minimum.tag == TF_VALUE_INT && minimum.as.integer == INT64_MIN);
    CHECK(maximum.tag == TF_VALUE_INT && maximum.as.integer == INT64_MAX);
    CHECK(negative.tag == TF_VALUE_INT && negative.as.integer == -INT64_C(42));
    CHECK(subnormal.tag == TF_VALUE_FLOAT && subnormal.as.float_bits == UINT64_C(1));
    CHECK(finite.tag == TF_VALUE_FLOAT && finite.as.float_bits == UINT64_C(0xffefffffffffffff));
    CHECK(boolean.tag == TF_VALUE_BOOL && boolean.as.boolean == 1);
    CHECK(character.tag == TF_VALUE_CHAR && character.as.character == UINT32_C(0x10ffff));
    CHECK(unit.tag == TF_VALUE_UNIT);

    tf_value choice = literal(pool, tf_literal_id_1), item = tf_unit(), pair = tf_unit();
    size_t tag = 0;
    CHECK(tf_variant_tag(ctx, 1, choice, &tag) == TF_OK && tag == 1);
    CHECK(tf_payload(ctx, 1, choice, 1, 0, &pair) == TF_OK && tf_value_type(ctx, pair) == PAIR);
    CHECK(tf_project(ctx, 1, pair, 0, &item) == TF_OK &&
          item.tag == TF_VALUE_INT && item.as.integer == INT64_MIN);
    tf_release(&item);
    const unsigned char *actual = NULL;
    size_t length = 0;
    CHECK(tf_project(ctx, 1, pair, 1, &item) == TF_OK &&
          tf_text_view(ctx, 1, item, &actual, &length) == TF_OK &&
          length == COUNT(text) && memcmp(actual, text, length) == 0);
    tf_release(&item);
    tf_release(&pair);
    CHECK(tf_payload(ctx, 1, choice, 1, 1, &item) == TF_OK &&
          tf_variant_tag(ctx, 1, item, &tag) == TF_OK && tag == 0);
    tf_release(&item);
    tf_release(&choice);

    tf_value list = literal(pool, tf_literal_id_6);
    CHECK(tf_list_length(ctx, 1, list, &length) == TF_OK && length == 2);
    CHECK(tf_list_at(ctx, 1, list, 1, &item) == TF_OK &&
          tf_text_view(ctx, 1, item, &actual, &length) == TF_OK && length == 0);
    tf_release(&item);
    tf_release(&list);
    tf_value none = literal(pool, tf_literal_id_14);
    CHECK(tf_variant_tag(ctx, 1, none, &tag) == TF_OK && tag == 1);
    tf_release(&none);
    tf_value empty = literal(pool, tf_literal_id_15);
    CHECK(tf_value_type(ctx, empty) == EMPTY_RECORD);
    tf_release(&empty);

    CHECK(tf_origin_count == 4);
    const struct tf_generated_origin *origin = &tf_origins[0];
    CHECK(origin->path_length == sizeof path - 1 &&
          memcmp(origin->path, path, origin->path_length) == 0);
    CHECK(origin->start == 1 && origin->end == 9 && origin->start_line == 1 &&
          origin->start_column == 2 && origin->end_line == 2 && origin->end_column == 1);
    origin = &tf_origins[1];
    CHECK(origin->path == tf_origins[0].path && origin->path_length == sizeof path - 1 &&
          origin->start == 3 && origin->end == 7 && origin->start_line == 1 &&
          origin->start_column == 3 && origin->end_line == 1 && origin->end_column == 4);
    origin = &tf_origins[2];
    CHECK(origin->start == 11 && origin->end == 11 && origin->start_line == 3 &&
          origin->end_line == 3 && origin->start_column == 1 && origin->end_column == 1);
    origin = &tf_origins[3];
    CHECK(origin->path_length == 0 && origin->path == NULL && origin->start == 0 &&
          origin->end == 0 && origin->start_line == 1 && origin->end_line == 1 &&
          origin->start_column == 1 && origin->end_column == 1);
}
#endif

int main(void) {
    const tf_rt_program program = {tf_types, tf_type_count, NULL, 0};
    tf_context ctx;
    tf_constant_pool *pool = NULL;
    CHECK(tf_context_init(&ctx, &program, NULL) == TF_OK);
    CHECK(tf_constants_create(&ctx, tf_literals, tf_literal_count, &pool) == TF_OK);
#ifdef TF_TEST_METADATA_EMPTY
    CHECK(tf_type_count == 1 && tf_types[0].kind == TF_RT_TYPE_FUNCTION);
    CHECK(tf_literal_count == 0 && tf_origin_count == 0 && tf_origins[0].path == NULL);
#else
    library(&ctx, pool);
#endif
    tf_constants_destroy(pool);
    CHECK(tf_context_live_bytes(&ctx) == 0);
    puts("Compiled C metadata: PASS");
    return EXIT_SUCCESS;
}

#else

#include "../src/backend/c_metadata.h"

#ifdef TF_TEST_METADATA_ALLOC
/* Link the emitter object compiled with -Dcalloc=tf_metadata_test_calloc
 * -Dmalloc=tf_metadata_test_malloc -Dfree=tf_metadata_test_free. */
static size_t fail_at, calls, live;

void *tf_metadata_test_calloc(size_t count, size_t width) {
    if (++calls == fail_at) return NULL;
    void *ptr = calloc(count, width);
    if (ptr != NULL) ++live;
    return ptr;
}

void *tf_metadata_test_malloc(size_t bytes) {
    if (++calls == fail_at) return NULL;
    void *ptr = malloc(bytes);
    if (ptr != NULL) ++live;
    return ptr;
}

void tf_metadata_test_free(void *ptr) {
    if (ptr != NULL) {
        CHECK(live != 0);
        --live;
        free(ptr);
    }
}
#endif

#define NAME(value) {(const unsigned char *)(value), sizeof(value) - 1}

static void fixture(const TfCoreProgramData *data, const char *filename, bool forward) {
    TfCoreProgram *core = NULL;
    TfVerifiedCore *verified = NULL;
    TfCoreVerifyIssue issue = {0};
    CHECK(tf_core_program_copy(&core, data, NULL) == TF_OK);
    const TfStatus status = tf_core_verify(&verified, core, NULL, &issue);
    if (status != TF_OK)
        fprintf(stderr, "Core verify: status=%d code=%d span=%zu type=%zu constant=%zu\n",
                status, issue.code, issue.span, issue.type, issue.constant);
    CHECK(status == TF_OK);

    FILE *first = filename != NULL ? fopen(filename, "w+b") : tmpfile();
    FILE *second = tmpfile();
    CHECK(first != NULL && second != NULL);
    CHECK(tf_c_metadata_write(NULL, verified) == TF_HOST_ERROR);
    CHECK(tf_c_metadata_write(first, NULL) == TF_HOST_ERROR && ftell(first) == 0);
#ifdef TF_TEST_METADATA_ALLOC
    if (forward) {
        for (size_t i = 1; i <= 2; ++i) {
            calls = 0;
            fail_at = i;
            CHECK(tf_c_metadata_write(first, verified) == TF_HOST_ERROR);
            CHECK(ftell(first) == 0 && live == 0 && calls == i);
        }
        fail_at = 0;
    } else {
        calls = 0;
        fail_at = 1;
    }
#else
    (void)forward;
#endif
    CHECK(fputs("#include \"ternfold_rt_constants.h\"\n", first) != EOF);
    CHECK(fputs("#include \"ternfold_rt_constants.h\"\n", second) != EOF);
    CHECK(tf_c_metadata_write(first, verified) == TF_OK);
    CHECK(tf_c_metadata_write(second, verified) == TF_OK);
    CHECK(fflush(first) == 0 && fflush(second) == 0);
    CHECK(fseek(first, 0, SEEK_SET) == 0 && fseek(second, 0, SEEK_SET) == 0);
    size_t size = 0;
    int byte;
    while ((byte = fgetc(first)) != EOF) {
        CHECK(byte == fgetc(second));
        CHECK(byte == '\n' || (byte >= 0x20 && byte <= 0x7e));
        ++size;
    }
    CHECK(size != 0 && fgetc(second) == EOF && !ferror(first) && !ferror(second));
    CHECK(fclose(first) == 0 && fclose(second) == 0);
    if (filename != NULL) {
        FILE *read_only = fopen(filename, "rb");
        CHECK(read_only != NULL && setvbuf(read_only, NULL, _IONBF, 0) == 0);
        CHECK(tf_c_metadata_write(read_only, verified) == TF_HOST_ERROR);
        CHECK(fclose(read_only) == 0);
    }
#ifdef TF_TEST_METADATA_ALLOC
    CHECK(live == 0 && (forward || calls == 0));
    fail_at = 0;
#endif
    tf_verified_core_destroy(verified);
    tf_core_program_destroy(core);
}

static void library_fixture(const char *filename) {
    const unsigned char source_bytes[] = {'\t', 0xc3, 0xa9, 0xf0, 0x9f, 0x98, 0x80,
                                           '\r', '\n', 'x', '\n'};
    TfSource source = {0}, unnamed = {0};
    CHECK(tf_source_init(&source, path, source_bytes, COUNT(source_bytes), NULL) == TF_OK);
    CHECK(tf_source_init(&unnamed, "", NULL, 0, NULL) == TF_OK);
    const TfSpan spans[] = {{&source, 1, 9}, {&source, 3, 7},
                            {&source, 11, 11}, {&unnamed, 0, 0}};
    const TfCoreField fields[] = {{NAME("number"), INT, 1}, {NAME("label"), TEXT, 2}};
    const TfCoreTypeId pair_and_option[] = {PAIR, OPTION}, int_type[] = {INT};
    const TfCoreVariant choices[] = {{NAME("Empty"), {NULL, 0}, 0},
                                     {NAME("Present"), {pair_and_option, 2}, 1}};
    const TfCoreType types[] = {
        {.kind = TF_CORE_TYPE_INT}, {.kind = TF_CORE_TYPE_FLOAT}, {.kind = TF_CORE_TYPE_BOOL},
        {.kind = TF_CORE_TYPE_TEXT}, {.kind = TF_CORE_TYPE_CHAR}, {.kind = TF_CORE_TYPE_UNIT},
        {.kind = TF_CORE_TYPE_LIST, .as.element = TEXT},
        {.kind = TF_CORE_TYPE_OPTION, .as.element = INT},
        {.kind = TF_CORE_TYPE_RECORD, .name = NAME("Pair"), .as.record = {fields, 2}},
        {.kind = TF_CORE_TYPE_ENUM, .name = NAME("Choice"), .as.enumeration = {choices, 2}},
        {.kind = TF_CORE_TYPE_MACHINE, .name = NAME("Counter"),
         .as.machine = {fields, 1, INT, TEXT, CHOICE}},
        {.kind = TF_CORE_TYPE_DECISION, .as.decision = {MACHINE, TEXT, CHOICE, TEXT_LIST}},
        {.kind = TF_CORE_TYPE_FUNCTION, .as.function = {{int_type, 1}, INT}},
        {.kind = TF_CORE_TYPE_TAG, .as.tagged = CHOICE},
        {.kind = TF_CORE_TYPE_RECORD, .name = NAME("Empty"), .as.record = {NULL, 0}}
    };
    /* The first root points forward to a diamond sharing one Int literal.
     * Keep both declaration-order children and stable source origins. */
    const TfCoreConstantId pair_and_some[] = {2, 3}, pair[] = {4, 5}, some[] = {4},
                           list_items[] = {5, 16};
    const TfCoreConstant constants[] = {
        {.kind = TF_CORE_CONSTANT_VARIANT, .type = CHOICE, .as.variant = {1, {pair_and_some, 2}}},
        {.kind = TF_CORE_CONSTANT_RECORD, .type = PAIR, .as.children = {pair, 2}},
        {.kind = TF_CORE_CONSTANT_VARIANT, .type = OPTION, .as.variant = {0, {some, 1}}},
        {.kind = TF_CORE_CONSTANT_INT, .type = INT, .span = 2, .as.integer = INT64_MIN},
        {.kind = TF_CORE_CONSTANT_TEXT, .type = TEXT, .as.text = {text, COUNT(text)}},
        {.kind = TF_CORE_CONSTANT_LIST, .type = TEXT_LIST, .as.children = {list_items, 2}},
        {.kind = TF_CORE_CONSTANT_INT, .type = INT, .as.integer = INT64_MAX},
        {.kind = TF_CORE_CONSTANT_INT, .type = INT, .as.integer = -42},
        {.kind = TF_CORE_CONSTANT_FLOAT, .type = FLOAT, .as.float_bits = UINT64_C(1)},
        {.kind = TF_CORE_CONSTANT_FLOAT, .type = FLOAT, .as.float_bits = UINT64_C(0xffefffffffffffff)},
        {.kind = TF_CORE_CONSTANT_BOOL, .type = BOOL, .as.boolean = 1},
        {.kind = TF_CORE_CONSTANT_CHAR, .type = CHAR, .as.character = UINT32_C(0x10ffff)},
        {.kind = TF_CORE_CONSTANT_UNIT, .type = UNIT},
        {.kind = TF_CORE_CONSTANT_VARIANT, .type = OPTION, .as.variant = {1, {NULL, 0}}},
        {.kind = TF_CORE_CONSTANT_RECORD, .type = EMPTY_RECORD, .as.children = {NULL, 0}},
        {.kind = TF_CORE_CONSTANT_TEXT, .type = TEXT, .as.text = {NULL, 0}}
    };
    /* A genuine library function, with no designated entry point. Metadata
     * must not copy its name, parameter names, or any code. */
    const TfCoreParameter parameter[] = {{1, INT, NAME("parameter"), 0}};
    const TfCoreBlock body[] = {{.terminator = {TF_CORE_YIELD, 1, 0}}};
    const TfCoreFunction functions[] = {
        {.kind = TF_CORE_FUNCTION_ORDINARY, .name = NAME("private"), .parameters = parameter,
         .parameter_count = 1, .result = INT, .blocks = body, .block_count = 1, .body = 1,
         .value_count = 1}
    };
    fixture(&(TfCoreProgramData){types, COUNT(types), constants, COUNT(constants),
            functions, COUNT(functions), spans, COUNT(spans), 0}, filename, true);
    tf_source_destroy(&unnamed);
    tf_source_destroy(&source);
}

int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 3);
    fixture(&(TfCoreProgramData){0}, argc == 3 ? argv[1] : NULL, false);
    const TfCoreType types[] = {{.kind = TF_CORE_TYPE_INT},
                                {.kind = TF_CORE_TYPE_LIST, .as.element = 1}};
    const TfCoreConstantId elements[] = {1};
    const TfCoreConstant constants[] = {
        {.kind = TF_CORE_CONSTANT_INT, .type = 1, .as.integer = 7},
        {.kind = TF_CORE_CONSTANT_LIST, .type = 2, .as.children = {elements, 1}}
    };
    fixture(&(TfCoreProgramData){.types = types, .type_count = COUNT(types),
            .constants = constants, .constant_count = COUNT(constants)}, NULL, false);
    library_fixture(argc == 3 ? argv[2] : NULL);
    puts("Verified C metadata: PASS");
    return EXIT_SUCCESS;
}
#endif
