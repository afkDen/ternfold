#include "../src/check_internal.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#define PROCESS_ID _getpid
#else
#include <unistd.h>
#define PROCESS_ID getpid
#endif

static size_t checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { fprintf(stderr,"test_check_constants:%d: %s\n",__LINE__,#condition); exit(1); } \
} while (0)

typedef struct { size_t calls, fail_at, live; } Counter;
static void *allocate(void *context, size_t size) {
    Counter *c = context;
    void *p;
    if (++c->calls == c->fail_at) return NULL;
    p = malloc(size);
    if (p != NULL) ++c->live;
    return p;
}
static void deallocate(void *context, void *p) {
    Counter *c = context;
    CHECK(p != NULL && c->live != 0);
    --c->live;
    free(p);
}

static void diagnostic(const TfDiagnosticBag *bag, size_t index, size_t line,
                       const char *source_text, const char *message) {
    const TfDiagnosticRecord *d = tf_diagnostic_bag_get(bag,index);
    TfLocation location;
    const TfSpan *span;
    CHECK(d != NULL && strcmp(d->code,"E306") == 0);
    CHECK(d->label_count == 1 && d->labels[0].primary);
    CHECK(d->message.length == strlen(message) && memcmp(d->message.data,message,d->message.length) == 0);
    span = &d->labels[0].span;
    CHECK(tf_source_location(span->source,span->start,&location));
    CHECK(location.line == line);
    CHECK(span->end-span->start == strlen(source_text));
    CHECK(memcmp(span->source->bytes+span->start,source_text,span->end-span->start) == 0);
}

static void pipeline(void) {
    TfSourceManager *manager = NULL;
    TfResolution *resolution = NULL;
    TfTypedSignatures *signatures = NULL;
    TfTypedProgram *program = NULL;
    size_t binders = 0;
    Counter c = {0};
    const TfAllocator allocator = {&c,allocate,deallocate};
    size_t trials;
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    CHECK(tf_resolve(&resolution,manager,"tests/fixtures/types/pipeline.tfd",NULL,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&signatures,resolution,100,NULL) == TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(signatures)));
    CHECK(tf_check(&program,resolution,signatures,100,&allocator) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_typed_program_phase(program)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_typed_program_pending(program) == 0);
    for (size_t i=0; i<tf_resolution_symbol_count(resolution); ++i) {
        const TfResolvedSymbol *s = tf_resolution_symbol_at(resolution,i);
        if (s->kind == TF_SYMBOL_PATTERN) {
            const TfTypedNode *typed = tf_typed_program_node(program,s->declaration);
            const TfConcreteType *type;
            CHECK(typed != NULL && typed->target.owner == s->id.owner &&
                  typed->target.index == s->id.index);
            type = tf_type_store_get(tf_typed_program_store(program),typed->type);
            CHECK(type != NULL && type->kind == TF_CONCRETE_BOOL);
            CHECK(tf_concrete_type_id_equal(type->id,tf_typed_program_symbol(program,s->id)));
            ++binders;
        }
    }
    CHECK(binders == 2);
    trials = c.calls;
    tf_typed_program_destroy(program);
    CHECK(c.live == 0 && trials != 0);
    for (size_t i=1; i<=trials; ++i) {
        c = (Counter){.fail_at=i};
        CHECK(tf_check(&program,resolution,signatures,100,&allocator) == TF_HOST_ERROR);
        CHECK(program == NULL && c.live == 0);
    }
    tf_signatures_destroy(signatures);
    tf_resolution_destroy(resolution);
    tf_source_manager_destroy(manager);
}

static void failed_probes(size_t depth, bool missing) {
    char path[96];
    const int length = snprintf(path,sizeof path,"build/test-failed-probes-%ld.tfd",(long)PROCESS_ID());
    FILE *f;
    TfSourceManager *manager = NULL;
    TfResolution *resolution = NULL;
    TfTypedSignatures *signatures = NULL;
    TfTypedProgram *program = NULL;
    Counter c = {0};
    const TfAllocator allocator = {&c,allocate,deallocate};
    const TfDiagnosticRecord *d;
    CHECK(length > 0 && (size_t)length < sizeof path);
    f = fopen(path,"wb");
    CHECK(f != NULL && fputs("fn f() -> Int { ",f) != EOF);
    for (size_t i=0; i<depth; ++i) CHECK(fputs("len([",f) != EOF);
    CHECK(fputs(missing ? "len([])" : "len(1)",f) != EOF);
    for (size_t i=0; i<depth; ++i) CHECK(fputs("])",f) != EOF);
    CHECK(fputs(" }",f) != EOF && fclose(f) == 0);
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    CHECK(tf_resolve(&resolution,manager,path,NULL,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&signatures,resolution,100,NULL) == TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(signatures)));
    CHECK(tf_check(&program,resolution,signatures,100,&allocator) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_typed_program_phase(program)) == TF_DIAG_PHASE_ERROR);
    CHECK(tf_diagnostic_bag_count(tf_typed_program_diagnostics(program)) == 1);
    d = tf_diagnostic_bag_get(tf_typed_program_diagnostics(program),0);
    CHECK(strcmp(d->code,missing ? "E304" : "E302") == 0);
    if (!missing) CHECK(d->labels[0].span.end-d->labels[0].span.start == 1 &&
                       d->labels[0].span.source->bytes[d->labels[0].span.start] == '1');
    CHECK(c.calls < 4000);
    tf_typed_program_destroy(program);
    CHECK(c.live == 0);
    tf_signatures_destroy(signatures);
    tf_resolution_destroy(resolution);
    tf_source_manager_destroy(manager);
    CHECK(remove(path) == 0);
}

int main(void) {
    TfSourceManager *manager = NULL;
    TfResolution *resolution = NULL;
    TfTypedSignatures *signatures = NULL;
    TfTypedProgram *program = NULL;
    const TfDiagnosticBag *bag;
    Counter c = {0};
    const TfAllocator allocator = {&c,allocate,deallocate};
    size_t trials;
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    CHECK(tf_resolve(&resolution,manager,"tests/fixtures/types/constants.tfd",NULL,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&signatures,resolution,100,NULL) == TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(signatures)));
    CHECK(tf_check(&program,resolution,signatures,100,&allocator) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_typed_program_phase(program)) == TF_DIAG_PHASE_ERROR);
    CHECK(tf_typed_program_pending(program) == 0);
    bag = tf_typed_program_diagnostics(program);
    CHECK(tf_diagnostic_bag_count(bag) == 6);
    diagnostic(bag,0,16,"div","division by zero in state initializer");
    diagnostic(bag,1,17,"+","numeric overflow in state initializer");
    diagnostic(bag,2,18,"^","negative Int power in state initializer");
    diagnostic(bag,3,19,"helper()","state initializer requires a closed constant expression");
    diagnostic(bag,4,20,"/","division by zero in state initializer");
    diagnostic(bag,5,22,"div","division by zero in state initializer");
    tf_typed_program_destroy(program);
    CHECK(c.live == 0 && c.calls != 0);
    trials = c.calls;
    for (size_t i=1; i<=trials; ++i) {
        c = (Counter){0,i,0};
        CHECK(tf_check(&program,resolution,signatures,100,&allocator) == TF_HOST_ERROR);
        CHECK(program == NULL && c.live == 0);
    }
    tf_signatures_destroy(signatures);
    tf_resolution_destroy(resolution);
    tf_source_manager_destroy(manager);
    pipeline();
    for (size_t depth=14; depth<=24; depth+=2) {
        failed_probes(depth,false);
        failed_probes(depth,true);
    }
    printf("typed constants: %zu checks passed, %zu injected allocation failures\n",checks,trials);
    return 0;
}
