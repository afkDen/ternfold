#include "../src/typed_patterns.h"

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
    if (!(condition)) { fprintf(stderr,"test_typed_patterns:%d: %s\n",__LINE__,#condition); exit(1); } \
} while (0)

static bool text_is(TfAstText text, const char *expected) {
    return text.length == strlen(expected) && memcmp(text.data,expected,text.length) == 0;
}

static const TfResolvedSymbol *named(const TfResolution *r, const char *name, TfSymbolKind kind) {
    for (size_t i=0; i<tf_resolution_symbol_count(r); ++i) {
        const TfResolvedSymbol *s = tf_resolution_symbol_at(r,i);
        if (s->kind == kind && text_is(s->name,name)) return s;
    }
    CHECK(false);
    return NULL;
}

static void inspect(TfResolution *r, TfTypedSignatures *s, const char *name,
                    const char *code, const char *span_text, const char *expected_note, bool bind) {
    const TfResolvedSymbol *symbol = named(r,name,TF_SYMBOL_FUNCTION);
    const TfAstNode *fn = tf_ast_get(symbol->declaration.owner,symbol->declaration);
    const TfAstNode *body = tf_ast_get(fn->id.owner,fn->as.function.body);
    const TfAstNode *match = tf_ast_get(fn->id.owner,body->as.block.final_expression);
    const TfConcreteType *sig = tf_type_store_get(tf_signatures_store(s),tf_signatures_symbol(s,symbol->id));
    TfDiagnosticBag *bag = NULL;
    TfTypedPatterns *patterns = NULL;
    CHECK(sig != NULL && sig->kind == TF_CONCRETE_FUNCTION && sig->as.function.arity == 1);
    CHECK(tf_diagnostic_bag_create(&bag,100,NULL) == TF_OK);
    CHECK(tf_typed_patterns(&patterns,r,s,match,sig->as.function.parameters[0],bag,NULL) == TF_OK);
    CHECK(patterns != NULL && tf_diagnostic_bag_count(bag) == (code == NULL ? 0u : 1u));
    if (code != NULL) {
        const TfDiagnosticRecord *d = tf_diagnostic_bag_get(bag,0);
        CHECK(d != NULL && strcmp(d->code,code) == 0 && d->label_count != 0 && d->labels[0].primary);
        const TfSpan span = d->labels[0].span;
        CHECK(span.source == match->span.source && span.start >= match->span.start && span.end <= match->span.end);
        if (span_text == NULL) {
            CHECK(span.start == match->span.start && span.end == match->span.end);
        } else {
            CHECK(span.end-span.start == strlen(span_text));
            CHECK(memcmp(span.source->bytes+span.start,span_text,span.end-span.start) == 0);
        }
        if (expected_note != NULL) {
            CHECK(d->note_count == 1);
            CHECK(d->notes[0].length == strlen(expected_note) &&
                  memcmp(d->notes[0].data,expected_note,d->notes[0].length) == 0);
        }
    }
    if (bind) {
        TfSymbolId id = named(r,"value",TF_SYMBOL_PATTERN)->id;
        const TfNodeId binder = tf_resolution_symbol(r,id)->declaration;
        const TfTypedPatternNode *record = tf_typed_pattern_node(patterns,binder);
        const TfConcreteType *type = tf_type_store_get(tf_signatures_store(s),
                                      tf_typed_pattern_binding(patterns,id));
        CHECK(type != NULL && type->kind == TF_CONCRETE_BOOL);
        CHECK(record != NULL && record->type.index == type->id.index && !record->has_literal &&
              record->target.owner == id.owner && record->target.index == id.index);
        CHECK(tf_typed_pattern_binding(patterns,(TfSymbolId){NULL,id.index}).index == 0);
    }
    tf_typed_patterns_destroy(patterns);
    tf_diagnostic_bag_destroy(bag);
}

typedef struct { size_t calls, fail_at, live, bytes; } Counter;
static void *counted_allocate(void *context, size_t size) {
    Counter *c = context;
    void *p;
    if (++c->calls == c->fail_at) return NULL;
    CHECK(size <= SIZE_MAX-c->bytes);
    c->bytes += size;
    p = malloc(size);
    if (p != NULL) ++c->live;
    return p;
}
static void counted_free(void *context, void *p) {
    Counter *c = context;
    CHECK(c->live != 0);
    --c->live;
    free(p);
}

static size_t allocation_failures(TfResolution *r, TfTypedSignatures *s) {
    const TfResolvedSymbol *symbol = named(r,"missing",TF_SYMBOL_FUNCTION);
    const TfAstNode *fn = tf_ast_get(symbol->declaration.owner,symbol->declaration);
    const TfAstNode *body = tf_ast_get(fn->id.owner,fn->as.function.body);
    const TfAstNode *match = tf_ast_get(fn->id.owner,body->as.block.final_expression);
    const TfConcreteType *signature = tf_type_store_get(tf_signatures_store(s),
                                                        tf_signatures_symbol(s,symbol->id));
    TfTypedPatterns *patterns = NULL;
    TfDiagnosticBag *bag = NULL;
    Counter c = {0};
    const TfAllocator a = {&c,counted_allocate,counted_free};
    size_t count;
    CHECK(tf_diagnostic_bag_create(&bag,100,NULL) == TF_OK);
    CHECK(tf_typed_patterns(&patterns,r,s,match,signature->as.function.parameters[0],bag,&a) == TF_OK);
    count = c.calls;
    tf_typed_patterns_destroy(patterns);
    tf_diagnostic_bag_destroy(bag);
    CHECK(c.live == 0 && count != 0);
    for (size_t i=1; i<=count; ++i) {
        c = (Counter){.fail_at=i};
        CHECK(tf_diagnostic_bag_create(&bag,100,NULL) == TF_OK);
        CHECK(tf_typed_patterns(&patterns,r,s,match,signature->as.function.parameters[0],bag,&a) ==
              TF_HOST_ERROR);
        CHECK(patterns == NULL && c.live == 0);
        tf_diagnostic_bag_destroy(bag);
    }
    return count;
}

/* Allocate signatures afresh for each retry so Decision::Accept must intern
 * its List[Char] payload again. Include the diagnostic bag's allocator. */
static size_t decision_failures(TfResolution *r) {
    const TfResolvedSymbol *fn_symbol = named(r,"decision",TF_SYMBOL_FUNCTION);
    const TfResolvedSymbol *binder = named(r,"emissions",TF_SYMBOL_PATTERN);
    const TfAstNode *fn = tf_ast_get(fn_symbol->declaration.owner,fn_symbol->declaration);
    const TfAstNode *body = tf_ast_get(fn->id.owner,fn->as.function.body);
    const TfAstNode *match = tf_ast_get(fn->id.owner,body->as.block.final_expression);
    size_t trials = 0;
    for (size_t i=0; i<=trials; ++i) {
        TfTypedSignatures *signatures = NULL;
        TfDiagnosticBag *bag = NULL;
        TfTypedPatterns *patterns = NULL;
        Counter c = {0};
        const TfAllocator a = {&c,counted_allocate,counted_free};
        const TfConcreteType *signature;
        TfTypeStore *store;
        size_t first;
        TfStatus status;
        CHECK(tf_signatures_build(&signatures,r,100,&a) == TF_OK);
        CHECK(!tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(signatures)));
        store = tf_signatures_store(signatures);
        signature = tf_type_store_get(store,tf_signatures_symbol(signatures,fn_symbol->id));
        CHECK(signature != NULL && signature->kind == TF_CONCRETE_FUNCTION);
        first = c.calls;
        if (i != 0) c.fail_at = first+i;
        status = tf_diagnostic_bag_create(&bag,100,&a);
        if (status == TF_OK)
            status = tf_typed_patterns(&patterns,r,signatures,match,signature->as.function.parameters[0],bag,&a);
        if (i == 0) {
            const char note[] = "missing pattern: Decision::Reject(_, _)";
            const TfDiagnosticRecord *d = tf_diagnostic_bag_get(bag,0);
            const TfConcreteType *list = tf_type_store_get(store,tf_typed_pattern_binding(patterns,binder->id));
            CHECK(status == TF_OK && patterns != NULL && d != NULL && strcmp(d->code,"E310") == 0);
            CHECK(d->labels[0].span.source == match->span.source &&
                  d->labels[0].span.start == match->span.start && d->labels[0].span.end == match->span.end);
            CHECK(d->note_count == 1 && d->notes[0].length == sizeof note-1 &&
                  memcmp(d->notes[0].data,note,sizeof note-1) == 0);
            CHECK(list != NULL && list->kind == TF_CONCRETE_LIST &&
                  tf_type_store_get(store,list->as.element)->kind == TF_CONCRETE_CHAR);
            trials = c.calls-first;
            CHECK(trials != 0);
        } else CHECK(status == TF_HOST_ERROR && patterns == NULL);
        tf_typed_patterns_destroy(patterns);
        tf_diagnostic_bag_destroy(bag);
        tf_signatures_destroy(signatures);
        CHECK(c.live == 0);
    }
    return trials;
}

static void large_match(TfResolution *r, TfTypedSignatures *s, const char *name, bool unused) {
    const TfResolvedSymbol *symbol = named(r,name,TF_SYMBOL_FUNCTION);
    const TfAstNode *fn = tf_ast_get(symbol->declaration.owner,symbol->declaration);
    const TfAstNode *body = tf_ast_get(fn->id.owner,fn->as.function.body);
    const TfAstNode *match = tf_ast_get(fn->id.owner,body->as.block.final_expression);
    const TfConcreteType *sig = tf_type_store_get(tf_signatures_store(s),tf_signatures_symbol(s,symbol->id));
    Counter c = {0};
    const TfAllocator a = {&c,counted_allocate,counted_free};
    TfDiagnosticBag *bag = NULL;
    TfTypedPatterns *patterns = NULL;
    CHECK(tf_diagnostic_bag_create(&bag,4,NULL) == TF_OK);
    CHECK(tf_typed_patterns(&patterns,r,s,match,sig->as.function.parameters[0],bag,&a) == TF_OK);
    CHECK(tf_diagnostic_bag_count(bag) == (unused ? 4u : 0u));
    CHECK(tf_diagnostic_bag_has_errors(bag) == unused);
    for (size_t i=0; i<tf_diagnostic_bag_count(bag); ++i)
        CHECK(strcmp(tf_diagnostic_bag_get(bag,i)->code,"E311") == 0);
    CHECK(c.bytes < (size_t)32*1024*1024);
    tf_typed_patterns_destroy(patterns);
    CHECK(c.live == 0);
    tf_diagnostic_bag_destroy(bag);
}

static void large_source(void) {
    enum { PAYLOAD=540, LITERALS=20000, VARIANTS=3200 };
    char path[96];
    const int length = snprintf(path,sizeof path,"build/test-typed-patterns-%ld.tfd",(long)PROCESS_ID());
    TfSourceManager *manager = NULL;
    TfResolution *r = NULL;
    TfTypedSignatures *s = NULL;
    FILE *f;
    CHECK(length > 0 && (size_t)length < sizeof path);
    f = fopen(path,"wb");
    CHECK(f != NULL && fputs("enum Wide { Row(",f) != EOF);
    for (size_t i=0; i<PAYLOAD; ++i) CHECK(fputs(i == 0 ? "Bool" : ", Bool",f) != EOF);
    CHECK(fputs(") } fn wide(v: Wide) -> Int { match v { Wide::Row(",f) != EOF);
    for (size_t i=0; i<PAYLOAD; ++i) CHECK(fputs(i == 0 ? "false" : ", false",f) != EOF);
    CHECK(fputs(") => { 0 }, Wide::Row(",f) != EOF);
    for (size_t i=0; i<PAYLOAD-1; ++i) CHECK(fputs(i == 0 ? "false" : ", false",f) != EOF);
    CHECK(fputs(", true) => { 0 }, _ => { 1 } } } fn many(v: Int) -> Int { match v {",f) != EOF);
    for (size_t i=0; i<LITERALS; ++i) CHECK(fprintf(f,"%zu => { 0 },",i) > 0);
    CHECK(fputs("_ => { 1 } } } fn hidden(v: Int) -> Int { match v { _ => { 1 },",f) != EOF);
    for (size_t i=0; i<LITERALS; ++i) CHECK(fprintf(f,"%zu => { 0 },",i) > 0);
    CHECK(fputs("} } enum Flat {",f) != EOF);
    for (size_t i=0; i<VARIANTS; ++i) CHECK(fprintf(f,"V%zu,",i) > 0);
    CHECK(fputs("} fn flat(v: Flat) -> Int { match v {",f) != EOF);
    for (size_t i=0; i<VARIANTS; ++i) CHECK(fprintf(f,"Flat::V%zu => { 1 },",i) > 0);
    CHECK(fputs("} } enum Mixed { Box(Bool),",f) != EOF);
    for (size_t i=0; i<VARIANTS; ++i) CHECK(fprintf(f,"V%zu,",i) > 0);
    CHECK(fputs("} fn mixed(v: Mixed) -> Int { match v {",f) != EOF);
    for (size_t i=0; i<VARIANTS; ++i) CHECK(fprintf(f,"Mixed::V%zu => { 1 },",i) > 0);
    CHECK(fputs("Mixed::Box(false) => { 2 }, Mixed::Box(true) => { 3 } } } enum One { Row(",f) != EOF);
    for (size_t i=0; i<PAYLOAD; ++i) CHECK(fputs(i == 0 ? "Unit" : ", Unit",f) != EOF);
    CHECK(fputs(") } fn single(v: One) -> Int { match v { One::Row(",f) != EOF);
    for (size_t i=0; i<PAYLOAD; ++i) CHECK(fputs(i == 0 ? "()" : ", ()",f) != EOF);
    CHECK(fputs(") => { 1 } } }",f) != EOF && fclose(f) == 0);
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    CHECK(tf_resolve(&r,manager,path,NULL,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(r)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&s,r,100,NULL) == TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(s)));
    large_match(r,s,"wide",false);
    large_match(r,s,"many",false);
    large_match(r,s,"hidden",true);
    large_match(r,s,"flat",false);
    large_match(r,s,"mixed",false);
    large_match(r,s,"single",false);
    tf_signatures_destroy(s);
    tf_resolution_destroy(r);
    tf_source_manager_destroy(manager);
    CHECK(remove(path) == 0);
}

int main(void) {
    TfSourceManager *manager = NULL;
    TfResolution *r = NULL;
    TfTypedSignatures *s = NULL;
    size_t injected;
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    CHECK(tf_resolve(&r,manager,"tests/fixtures/types/patterns.tfd",NULL,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(r)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&s,r,100,NULL) == TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(s)));
    inspect(r,s,"complete",NULL,NULL,NULL,false);
    inspect(r,s,"missing","E310",NULL,"missing pattern: Option::Some(false)",false);
    inspect(r,s,"binder",NULL,NULL,NULL,true);
    inspect(r,s,"bad_arity","E312","Option::Some(true, false)",NULL,false);
    inspect(r,s,"wrong_payload","E312","5",NULL,false);
    inspect(r,s,"wide_int","E301","- 9223372036854775808",NULL,false);
    inspect(r,s,"rounded_float","E311","0e10",NULL,false);
    inspect(r,s,"open_char","E310",NULL,"missing pattern: _",false);
    injected = allocation_failures(r,s);
    tf_signatures_destroy(s);
    injected += decision_failures(r);
    tf_resolution_destroy(r);
    tf_source_manager_destroy(manager);
    large_source();
    printf("typed patterns: %zu checks passed, %zu injected allocation failures\n",checks,injected);
    return 0;
}
