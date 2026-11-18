#include "../src/typed_signatures.h"
#include "../src/type_operators.h"

#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { fprintf(stderr,"test_typed_signatures:%d: %s\n",__LINE__,#condition); exit(1); } \
} while (0)

static const TfResolvedSymbol *symbol(const TfResolution *r, const char *name, TfSymbolKind kind) {
    size_t length = strlen(name);
    for (size_t i=0; i<tf_resolution_symbol_count(r); ++i) {
        const TfResolvedSymbol *s = tf_resolution_symbol_at(r,i);
        if (s->kind == kind && s->name.length == length && memcmp(name,s->name.data,length) == 0)
            return s;
    }
    CHECK(false);
    return NULL;
}

static const TfConcreteType *check_type(const TfTypeStore *store, TfConcreteTypeId id,
                                        TfConcreteTypeKind kind) {
    const TfConcreteType *type = tf_type_store_get(store,id);
    CHECK(type != NULL && type->kind == kind);
    return type;
}

static void operators(TfTypedSignatures *s, TfConcreteTypeId function) {
    TfTypeStore *types = tf_signatures_store(s);
    TfConcreteTypeId integer, floating, boolean, text, char_type, list, closure_list, result;
    const char *error;
    CHECK(tf_type_primitive(types,TF_CONCRETE_INT,&integer) == TF_OK);
    CHECK(tf_type_primitive(types,TF_CONCRETE_FLOAT,&floating) == TF_OK);
    CHECK(tf_type_primitive(types,TF_CONCRETE_BOOL,&boolean) == TF_OK);
    CHECK(tf_type_primitive(types,TF_CONCRETE_TEXT,&text) == TF_OK);
    CHECK(tf_type_primitive(types,TF_CONCRETE_CHAR,&char_type) == TF_OK);
    CHECK(tf_type_list(types,integer,&list) == TF_OK);
    CHECK(tf_type_list(types,function,&closure_list) == TF_OK);
    const struct { TfTokenKind op; TfConcreteTypeId left,right,expected; } valid[] = {
        {TF_PLUS,integer,integer,integer}, {TF_MINUS,floating,floating,floating},
        {TF_STAR,floating,floating,floating}, {TF_SLASH,integer,floating,floating},
        {TF_SLASH,integer,integer,floating}, {TF_SLASH,floating,integer,floating},
        {TF_SLASH,floating,floating,floating}, {TF_DIV,integer,integer,integer},
        {TF_PERCENT,integer,integer,integer}, {TF_CARET,floating,integer,floating},
        {TF_CARET,integer,integer,integer}, {TF_AND_AND,boolean,boolean,boolean},
        {TF_OR_OR,boolean,boolean,boolean}, {TF_BANG,boolean,{0},boolean},
        {TF_MINUS,floating,{0},floating}, {TF_LESS,integer,integer,boolean},
        {TF_LESS_EQUAL,floating,floating,boolean}, {TF_GREATER,integer,integer,boolean},
        {TF_GREATER_EQUAL,floating,floating,boolean}, {TF_EQUAL_EQUAL,char_type,char_type,boolean},
        {TF_BANG_EQUAL,list,list,boolean}, {TF_PLUS_PLUS,text,text,text},
        {TF_PLUS_PLUS,list,list,list}
    };
    for (size_t i=0; i<sizeof valid/sizeof *valid; ++i) {
        CHECK(tf_type_operator(s,valid[i].op,valid[i].left,valid[i].right,&result,&error) == TF_OK);
        CHECK(error == NULL && tf_concrete_type_id_equal(result,valid[i].expected));
    }
    CHECK(tf_type_operator(s,TF_PLUS,integer,floating,&result,&error) == TF_SOURCE_ERROR);
    CHECK(result.index == 0 && strcmp(error,"E302") == 0);
    CHECK(tf_type_operator(s,TF_DIV,floating,integer,&result,&error) == TF_SOURCE_ERROR);
    CHECK(result.index == 0 && strcmp(error,"E302") == 0);
    CHECK(tf_type_operator(s,TF_CARET,floating,floating,&result,&error) == TF_SOURCE_ERROR);
    CHECK(result.index == 0 && strcmp(error,"E302") == 0);
    CHECK(tf_type_operator(s,TF_EQUAL_EQUAL,closure_list,closure_list,&result,&error) == TF_SOURCE_ERROR);
    CHECK(result.index == 0 && strcmp(error,"E305") == 0);
}

static void signatures(void) {
    TfSourceManager *manager = NULL;
    TfResolution *r = NULL;
    TfTypedSignatures *s = NULL;
    const TfTypeStore *types;
    const TfResolvedSymbol *ctor, *field, *variant;
    const TfConcreteType *type, *param;
    TfMachineSignature machine;
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    CHECK(tf_resolve(&r,manager,"examples/showcase/branching_inventory.tfd",NULL,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(r)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&s,r,100,NULL) == TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(s)));
    types = tf_signatures_store(s);
    ctor = symbol(r,"Shelf",TF_SYMBOL_CONSTRUCTOR);
    type = check_type(types,tf_signatures_symbol(s,ctor->id),TF_CONCRETE_FUNCTION);
    CHECK(type->as.function.arity == 0);
    check_type(types,type->as.function.result,TF_CONCRETE_MACHINE);
    CHECK(tf_signatures_machine(s,ctor->type,&machine));
    check_type(types,machine.input,TF_CONCRETE_RECORD);
    check_type(types,machine.output,TF_CONCRETE_TEXT);
    check_type(types,machine.rejection,TF_CONCRETE_ENUM);
    ctor = symbol(r,"Change",TF_SYMBOL_CONSTRUCTOR);
    type = check_type(types,tf_signatures_symbol(s,ctor->id),TF_CONCRETE_FUNCTION);
    CHECK(type->as.function.arity == 1);
    param = check_type(types,type->as.function.parameters[0],TF_CONCRETE_INT);
    field = symbol(r,"delta",TF_SYMBOL_FIELD);
    CHECK(tf_concrete_type_id_equal(param->id,tf_signatures_symbol(s,field->id)));
    variant = symbol(r,"Underflow",TF_SYMBOL_VARIANT);
    CHECK(tf_concrete_type_id_equal(machine.rejection,tf_signatures_symbol(s,variant->id)));
    type = check_type(types,tf_signatures_symbol(s,symbol(r,"main",TF_SYMBOL_FUNCTION)->id),
                      TF_CONCRETE_FUNCTION);
    CHECK(type->as.function.arity == 0);
    operators(s,type->id);
    type = check_type(types,type->as.function.result,TF_CONCRETE_LIST);
    check_type(types,type->as.element,TF_CONCRETE_TEXT);
    CHECK(tf_signatures_symbol(s,symbol(r,"step",TF_SYMBOL_BUILTIN)->id).index == 0);
    CHECK(tf_signatures_symbol(s,(TfSymbolId){NULL,ctor->id.index}).index == 0);
    tf_signatures_destroy(s);
    tf_resolution_destroy(r);
    tf_source_manager_destroy(manager);
}

static void diagnostic_case(const char *path, const char *code) {
    TfSourceManager *manager = NULL;
    TfResolution *r = NULL;
    TfTypedSignatures *s = NULL;
    const TfDiagnosticRecord *d;
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    CHECK(tf_resolve(&r,manager,path,NULL,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(r)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&s,r,100,NULL) == TF_OK);
    CHECK(tf_diagnostic_bag_count(tf_signatures_diagnostics(s)) == (code != NULL ? 1u : 0u));
    if (code != NULL) {
        d = tf_diagnostic_bag_get(tf_signatures_diagnostics(s),0);
        CHECK(d != NULL && strcmp(d->code,code) == 0 && d->label_count != 0);
        CHECK(d->labels[0].primary && d->labels[0].span.source != NULL);
        CHECK(d->labels[0].span.end > d->labels[0].span.start);
        if (strcmp(code,"E303") == 0) CHECK(d->labels[0].span.start == 16);
        tf_signatures_destroy(s);
        CHECK(tf_signatures_build(&s,r,0,NULL) == TF_OK);
        CHECK(tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(s)));
        CHECK(tf_diagnostic_bag_count(tf_signatures_diagnostics(s)) == 0);
        CHECK(tf_diagnostic_bag_suppressed(tf_signatures_diagnostics(s)) == 1);
    }
    tf_signatures_destroy(s);
    tf_resolution_destroy(r);
    tf_source_manager_destroy(manager);
}

typedef struct { size_t calls, fail_at, live; } Counter;
static void *counted_allocate(void *context, size_t size) {
    Counter *c = context;
    void *p;
    if (++c->calls == c->fail_at) return NULL;
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
static size_t failure_sweep(const char *path) {
    TfSourceManager *manager = NULL;
    TfResolution *r = NULL;
    TfTypedSignatures *s = NULL;
    Counter c = {0};
    const TfAllocator a = {&c,counted_allocate,counted_free};
    size_t count;
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    CHECK(tf_resolve(&r,manager,path,NULL,NULL) == TF_OK);
    CHECK(tf_signatures_build(&s,r,100,&a) == TF_OK);
    count = c.calls;
    tf_signatures_destroy(s);
    CHECK(c.live == 0 && count != 0);
    for (size_t i=1; i<=count; ++i) {
        c = (Counter){0,i,0};
        CHECK(tf_signatures_build(&s,r,100,&a) == TF_HOST_ERROR);
        CHECK(s == NULL && c.live == 0);
    }
    tf_resolution_destroy(r);
    tf_source_manager_destroy(manager);
    return count;
}

int main(void) {
    size_t injected;
    signatures();
    diagnostic_case("tests/fixtures/types/invalid-signature.tfd","E303");
    diagnostic_case("tests/fixtures/types/recursive-nominal.tfd","E307");
    diagnostic_case("tests/fixtures/types/recursive-machine.tfd","E307");
    diagnostic_case("tests/fixtures/types/non-data-cell.tfd","E407");
    diagnostic_case("tests/fixtures/types/machine-signature-is-not-state.tfd",NULL);
    injected = failure_sweep("examples/showcase/branching_inventory.tfd");
    injected += failure_sweep("tests/fixtures/types/recursive-nominal.tfd");
    printf("typed signatures: %zu checks passed, %zu injected allocation failures\n",checks,injected);
    return 0;
}
