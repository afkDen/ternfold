#include "../src/check_builtins.h"

#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { fprintf(stderr,"test_check_builtins:%d: %s\n",__LINE__,#condition); exit(1); } \
} while (0)

static const TfCheckAdapters adapters = {.builtin = tf_check_builtin};

static TfResolution *resolve_fixture(TfSourceManager *manager, const char *path) {
    TfResolution *resolution = NULL;
    CHECK(tf_resolve(&resolution,manager,path,NULL,NULL) == TF_OK);
    if (tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) != TF_DIAG_PHASE_VALID)
        (void)tf_diagnostic_bag_write(stderr,tf_resolution_diagnostics(resolution),
                                      TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) == TF_DIAG_PHASE_VALID);
    return resolution;
}

static const TfAst *root_ast(const TfResolution *resolution) {
    return tf_resolution_module(resolution,tf_resolution_root(resolution))->ast;
}

static const TfConcreteType *type(const TfTypedProgram *program, TfNodeId id) {
    const TfTypedNode *typed = tf_typed_program_node(program,id);
    CHECK(typed != NULL);
    CHECK(typed->type.owner == tf_typed_program_store(program));
    const TfConcreteType *concrete = tf_type_store_get(typed->type.owner,typed->type);
    CHECK(concrete != NULL);
    return concrete;
}

static void inspect_calls(const TfTypedProgram *program) {
    const TfResolution *resolution = tf_typed_program_resolution(program);
    const TfAst *ast = root_ast(resolution);
    size_t seen[4] = {0};
    bool seen_builtins[16] = {0};
    size_t count = 0, calls = 0;
    for (size_t i = 0; i < tf_resolution_symbol_count(resolution); ++i) {
        const TfResolvedSymbol *symbol = tf_resolution_symbol_at(resolution,i);
        if (symbol->kind == TF_SYMBOL_BUILTIN) ++count;
    }
    CHECK(count == sizeof seen_builtins / sizeof *seen_builtins);
    for (size_t i = 0; i < tf_ast_count(ast); ++i) {
        const TfAstNode *n = tf_ast_at(ast,i);
        if (n->kind != TF_AST_CALL) continue;
        const TfResolvedReference *reference = tf_resolution_reference(resolution,n->as.call.callee);
        if (reference == NULL || reference->symbol.index == 0) continue;
        const TfResolvedSymbol *symbol = tf_resolution_symbol(resolution,reference->symbol);
        if (symbol->declaration.index != 0) continue;
        const TfConcreteType *signature = type(program,n->as.call.callee);
        const TfConcreteType *result = type(program,n->id);
        CHECK(signature->kind == TF_CONCRETE_FUNCTION);
        CHECK(signature->as.function.arity == n->as.call.arguments.count);
        CHECK(tf_concrete_type_id_equal(signature->as.function.result,result->id));
        for (size_t j = 0; j < n->as.call.arguments.count; ++j) {
            const TfAstNode *arg = tf_ast_get(ast,n->as.call.arguments.items[j]);
            CHECK(tf_concrete_type_id_equal(signature->as.function.parameters[j],type(program,arg->id)->id));
            CHECK(tf_concrete_type_id_equal(signature->as.function.parameters[j],
                                             type(program,arg->as.argument.value)->id));
        }
        if (symbol->kind == TF_SYMBOL_BUILTIN) {
            /* Mark the ordinal among resolved prelude functions, without
             * depending on their absolute symbol-table indexes. */
            size_t ordinal = 0;
            for (size_t j = 0; j < tf_resolution_symbol_count(resolution); ++j) {
                const TfResolvedSymbol *candidate = tf_resolution_symbol_at(resolution,j);
                if (candidate == symbol) break;
                if (candidate->kind == TF_SYMBOL_BUILTIN) ++ordinal;
            }
            CHECK(ordinal < count);
            seen_builtins[ordinal] = true;
        }
        if (result->kind == TF_CONCRETE_DECISION) {
            CHECK(tf_concrete_type_id_equal(result->as.decision.machine,signature->as.function.parameters[0]));
            CHECK(tf_type_store_get(result->id.owner,result->as.decision.output)->kind == TF_CONCRETE_CHAR);
            CHECK(tf_type_store_get(result->id.owner,result->as.decision.rejection)->kind == TF_CONCRETE_TEXT);
            ++seen[0];
        } else if (result->kind == TF_CONCRETE_OPTION) ++seen[1];
        else if (result->kind == TF_CONCRETE_LIST) ++seen[2];
        else ++seen[3];
        ++calls;
    }
    for (size_t i = 0; i < count; ++i) CHECK(seen_builtins[i]);
    CHECK(seen[0] == 3 && seen[1] >= 7 && seen[2] >= 10 && seen[3] >= 7);
    CHECK(calls >= 30);
}

static void inspect_errors(const TfTypedProgram *program) {
    static const struct { const char *code, *bytes; } expected[] = {
        {"E302","false"}, {"E302","fn() -> Int { 0 }"},
        {"E302","fn(acc: Int, x: Bool) -> Bool { x }"},
        {"E302","1.0"}, {"E303","len()"}, {"E303","first: 1"}, {"E303","second: 2"},
        {"E303","Option::None"}, {"E303","Option::Some"},
        {"E302","at"}, {"E304","map([], len)"}, {"E304","append([], Option::None)"},
        {"E304","len"}, {"E304","Option::None"},
        {"E302","true"}, {"E302","1"}, {"E302","1"}, {"E302","false"},
        {"E307","1"}, {"E302","Other()"}, {"E302","step"}, {"E301","1e999"}
    };
    const TfDiagnosticBag *bag = tf_typed_program_diagnostics(program);
    const TfAst *ast = root_ast(tf_typed_program_resolution(program));
    const TfSource *source = tf_ast_source(ast);
    size_t previous = 0;
    if (tf_diagnostic_bag_count(bag) != sizeof expected / sizeof *expected)
        (void)tf_diagnostic_bag_write(stderr,bag,TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
    CHECK(tf_diagnostic_bag_count(bag) == sizeof expected / sizeof *expected);
    for (size_t i = 0; i < sizeof expected / sizeof *expected; ++i) {
        const TfDiagnosticRecord *d = tf_diagnostic_bag_get(bag,i);
        const TfSpan primary = d->labels[0].span;
        CHECK(strcmp(d->code,expected[i].code) == 0);
        CHECK(d->severity == TF_DIAG_ERROR && d->label_count != 0 && d->labels[0].primary);
        CHECK(primary.source == source && primary.start >= previous && primary.end <= source->length);
        CHECK(primary.end - primary.start == strlen(expected[i].bytes));
        CHECK(memcmp(source->bytes + primary.start,expected[i].bytes,primary.end - primary.start) == 0);
        previous = primary.end;
    }
}

typedef struct { size_t calls, fail_at, live; } Failure;

static void *allocate(void *context, size_t bytes) {
    Failure *failure = context;
    if (++failure->calls == failure->fail_at) return NULL;
    void *pointer = malloc(bytes);
    if (pointer != NULL) ++failure->live;
    return pointer;
}

static void deallocate(void *context, void *pointer) {
    Failure *failure = context;
    CHECK(pointer != NULL && failure->live != 0);
    --failure->live;
    free(pointer);
}

static size_t check_fixture(const TfResolution *resolution, bool valid, size_t cap, size_t fail_at) {
    Failure failure = {0};
    const TfAllocator allocator = {&failure,allocate,deallocate};
    TfTypedSignatures *signatures = NULL;
    TfTypedProgram *program = NULL;
    CHECK(tf_signatures_build(&signatures,resolution,100,&allocator) == TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(signatures)));
    failure.calls = 0;
    failure.fail_at = fail_at;
    const TfStatus status = tf_check_with_adapters(&program,resolution,signatures,cap,&allocator,&adapters);
    const size_t calls = failure.calls;
    if (fail_at != 0) {
        CHECK(calls >= fail_at && status == TF_HOST_ERROR && program == NULL);
    } else {
        CHECK(status == TF_OK && program != NULL);
        const TfDiagnosticBag *bag = tf_typed_program_diagnostics(program);
        /* The fixture exercises machine signatures; constants are deliberately
         * left to the independent M06 adapter. */
        CHECK(tf_typed_program_pending(program) == TF_CHECK_PENDING_CONSTANTS);
        if (valid && tf_diagnostic_bag_has_errors(bag))
            (void)tf_diagnostic_bag_write(stderr,bag,TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
        CHECK(tf_diagnostic_bag_has_errors(bag) == !valid);
        CHECK(tf_diagnostic_phase_kind(tf_typed_program_phase(program)) ==
              (valid ? TF_DIAG_PHASE_INVALID : TF_DIAG_PHASE_ERROR));
        if (valid) inspect_calls(program);
        else if (cap != 0) inspect_errors(program);
        else {
            CHECK(tf_diagnostic_bag_count(bag) == 0);
            CHECK(tf_diagnostic_bag_suppressed(bag) == 22);
        }
    }
    tf_typed_program_destroy(program);
    tf_signatures_destroy(signatures);
    CHECK(failure.live == 0);
    return calls;
}

static size_t recovery_fixture(const TfResolution *resolution, size_t cap, size_t fail_at) {
    static const struct { const char *function, *code, *bytes; } expected[] = {
        {"shape_none","E302","Option::None"},
        {"shape_some","E302","Option::Some([])"},
        {"shape_machine","E307","Option::None"},
        {"shape_machine","E302","Decision::Accept(Option::None, [])"},
        {"hidden","E302","1"},
        {"hidden","E302","true"},
        {"ordered","E302","true"},
        {"ordered","E302","fn() -> Int { true }"},
        {"ordered","E302","fn(x: Int) -> Bool { true }"},
        {"arity","E302","true"},
        {"arity","E303","map(fn() -> Int { true })"},
        {"named","E302","true"},
        {"named","E303","items: fn() -> Int { true }"},
        {"named","E302","true"},
        {"named","E303","callback: fn(x: Int) -> Int { true }"},
        {"result","E302","1"},
        {"result","E302","true"},
        {"result","E302","map(1, fn(x: Int) -> Int { true })"},
        {"ambiguous","E304","map([], len)"},
        {"nullary","E302","true"},
        {"nullary","E303","Option::None"},
        {"probe_ordered","E302","true"},
        {"probe_ordered","E302","fn(x: Int) -> Bool { true }"}
    };
    Failure failure = {0};
    const TfAllocator allocator = {&failure,allocate,deallocate};
    TfTypedSignatures *signatures = NULL;
    TfTypedProgram *program = NULL;
    CHECK(tf_signatures_build(&signatures,resolution,100,&allocator) == TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(signatures)));
    failure.calls = 0;
    failure.fail_at = fail_at;
    const TfStatus status = tf_check_with_adapters(&program,resolution,signatures,cap,&allocator,&adapters);
    const size_t calls = failure.calls;
    if (fail_at != 0) {
        CHECK(calls >= fail_at && status == TF_HOST_ERROR && program == NULL);
    } else {
        CHECK(status == TF_OK && program != NULL);
        CHECK(tf_diagnostic_phase_kind(tf_typed_program_phase(program)) == TF_DIAG_PHASE_ERROR);
        CHECK(tf_typed_program_pending(program) == 0);
        const TfDiagnosticBag *bag = tf_typed_program_diagnostics(program);
        const size_t count = sizeof expected / sizeof *expected;
        CHECK(tf_diagnostic_bag_has_errors(bag));
        if (cap != 0) {
            if (tf_diagnostic_bag_count(bag) != count)
                (void)tf_diagnostic_bag_write(stderr,bag,TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
            CHECK(tf_diagnostic_bag_count(bag) == count);
            const TfAst *ast = root_ast(resolution);
            for (size_t i = 0; i < count; ++i) {
                const TfDiagnosticRecord *d = tf_diagnostic_bag_get(bag,i);
                const TfAstNode *function = NULL;
                for (size_t j = 0; j < tf_ast_count(ast); ++j) {
                    const TfAstNode *n = tf_ast_at(ast,j);
                    if (n->kind == TF_AST_FUNCTION &&
                        n->as.function.name.length == strlen(expected[i].function) &&
                        memcmp(n->as.function.name.data,expected[i].function,strlen(expected[i].function)) == 0)
                        function = n;
                }
                CHECK(function != NULL);
                CHECK(strcmp(d->code,expected[i].code) == 0);
                CHECK(d->label_count != 0 && d->labels[0].primary);
                const TfSpan span = d->labels[0].span;
                CHECK(span.source == function->span.source && span.start >= function->span.start &&
                      span.end <= function->span.end);
                CHECK(span.end - span.start == strlen(expected[i].bytes));
                CHECK(memcmp(span.source->bytes + span.start,expected[i].bytes,strlen(expected[i].bytes)) == 0);
                /* Both named closures use true; distinguish the second body
                 * by the preceding argument's end, rather than spelling alone. */
                if (i == 13) CHECK(span.start > tf_diagnostic_bag_get(bag,12)->labels[0].span.end);
            }
        } else {
            CHECK(tf_diagnostic_bag_count(bag) == 0);
            CHECK(tf_diagnostic_bag_suppressed(bag) == count);
        }
    }
    tf_typed_program_destroy(program);
    tf_signatures_destroy(signatures);
    CHECK(failure.live == 0);
    return calls;
}
int main(void) {
    TfSourceManager *manager = NULL;
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    TfResolution *valid = resolve_fixture(manager,"tests/fixtures/types/builtins-valid.tfd");
    TfResolution *invalid = resolve_fixture(manager,"tests/fixtures/types/builtins-invalid.tfd");
    TfResolution *recovery = resolve_fixture(manager,"tests/fixtures/types/builtins-recovery.tfd");
    const size_t recovery_allocations = recovery_fixture(recovery,100,0);
    (void)recovery_fixture(recovery,0,0);
    for (size_t i = 1; i <= recovery_allocations; ++i) (void)recovery_fixture(recovery,100,i);
    tf_resolution_destroy(recovery);
    const size_t valid_allocations = check_fixture(valid,true,100,0);
    const size_t invalid_allocations = check_fixture(invalid,false,100,0);
    (void)check_fixture(invalid,false,0,0);
    for (size_t i = 1; i <= valid_allocations; ++i) (void)check_fixture(valid,true,100,i);
    for (size_t i = 1; i <= invalid_allocations; ++i) (void)check_fixture(invalid,false,100,i);
    tf_resolution_destroy(invalid);
    tf_resolution_destroy(valid);
    tf_source_manager_destroy(manager);
    printf("builtin checker: %zu checks; %zu injected allocation failures\n",
           checks,valid_allocations + invalid_allocations + recovery_allocations);
    return 0;
}
