#include "../src/check_constants.h"
#include "../src/core/lower_constants.h"

#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(ok) do { ++checks; if (!(ok)) { \
    fprintf(stderr,"test_core_initializers:%d: %s\n",__LINE__,#ok); exit(1); \
} } while (0)

typedef struct { size_t calls, fail_at, live; } Counter;
static void *allocate(void *context, size_t size) {
    Counter *c = context;
    if (++c->calls == c->fail_at) return NULL;
    void *p = malloc(size);
    if (p != NULL) ++c->live;
    return p;
}
static void deallocate(void *context, void *p) {
    Counter *c = context;
    CHECK(p != NULL && c->live != 0);
    --c->live;
    free(p);
}

typedef struct {
    const TfTypedProgram *program;
    TfConcreteTypeId expected;
    size_t cell, visits;
    bool reject;
} Inspect;

static void variant(const Inspect *probe, const TfConstantValue *value, const char *name) {
    const TfResolvedSymbol *symbol = tf_resolution_symbol(tf_typed_program_resolution(probe->program),value->variant);
    CHECK(symbol != NULL && symbol->kind == TF_SYMBOL_VARIANT);
    CHECK(symbol->name.length == strlen(name) && memcmp(symbol->name.data,name,symbol->name.length) == 0);
}

static TfStatus inspect(void *context, const TfConstantValue *value) {
    Inspect *p = context;
    const TfTypeStore *store = tf_typed_program_store(p->program);
    CHECK(p->visits++ == 0);
    CHECK(tf_concrete_type_id_equal(value->type,p->expected));
    const TfConcreteType *type = tf_type_store_get(store,value->type);
    CHECK(type != NULL && type->kind == value->kind);
    if (p->cell == 0) {
        const unsigned char text[] = {'a',0,'b'};
        CHECK(value->kind == TF_CONCRETE_RECORD && value->as.aggregate.count == 3);
        const TfConstantValue *amount = value->as.aggregate.items[0];
        const TfConstantValue *some = value->as.aggregate.items[1];
        const TfConstantValue *label = value->as.aggregate.items[2];
        CHECK(amount->kind == TF_CONCRETE_FLOAT && amount->as.bits == UINT64_C(0x400c000000000000));
        CHECK(some->kind == TF_CONCRETE_OPTION && some->as.aggregate.count == 1);
        variant(p,some,"Some");
        const TfConstantValue *number = some->as.aggregate.items[0];
        CHECK(number->kind == TF_CONCRETE_INT && number->as.integer == -4);
        CHECK(tf_concrete_type_id_equal(tf_type_store_get(store,some->type)->as.element,number->type));
        CHECK(label->kind == TF_CONCRETE_TEXT && label->as.text.length == sizeof text &&
              memcmp(label->as.text.bytes,text,sizeof text) == 0);
    } else if (p->cell == 1) {
        CHECK(value->kind == TF_CONCRETE_LIST && value->as.aggregate.count == 0 && value->as.aggregate.items == NULL);
        CHECK(tf_type_store_get(store,type->as.element)->kind == TF_CONCRETE_CHAR);
    } else if (p->cell == 2) {
        CHECK(value->kind == TF_CONCRETE_BOOL && value->as.boolean);
    } else {
        CHECK(value->kind == TF_CONCRETE_OPTION && value->as.aggregate.count == 0 && value->as.aggregate.items == NULL);
        variant(p,value,"None");
    }
    return p->reject ? TF_HOST_ERROR : TF_OK;
}

static const TfCoreConstant *constant(const TfCoreProgramData *data, TfCoreConstantId id,
                                      TfCoreConstantKind kind) {
    CHECK(id != 0 && id <= data->constant_count && data->constants[id-1].kind == kind);
    return &data->constants[id-1];
}

static void pool(const TfTypedProgram *program, const TfNodeId *states) {
    TfMachineProgram *plan = NULL;
    TfCoreLowerTypes *types = NULL;
    TfCoreLowerConstants *constants = NULL;
    TfCoreProgramData view = {0};
    TfCoreConstantId root, sentinel;
    Counter c = {0};
    const TfAllocator a = {&c,allocate,deallocate};
    size_t trials, before;
    CHECK(tf_machine_program_build(&plan,program,100,NULL) == TF_OK);
    CHECK(tf_core_lower_types_create(&types,plan,NULL) == TF_OK);
    CHECK(tf_core_lower_constants_create(&constants,types,&a) == TF_OK);
    TfCoreConstant seed = {.kind=TF_CORE_CONSTANT_INT,.as.integer=9};
    CHECK(tf_core_lower_constant_append(constants,seed,&sentinel) == TF_OK && sentinel == 1);
    before = c.calls;
    CHECK(tf_core_lower_initializer_constant(constants,states[0],&root) == TF_OK && root == 6);
    trials = c.calls-before;
    tf_core_lower_type_view(types,&view);
    tf_core_lower_constant_view(constants,&view);
    const TfCoreConstant *record = constant(&view,root,TF_CORE_CONSTANT_RECORD);
    CHECK(record->type == tf_typed_program_node(program,states[0])->type.index);
    CHECK(record->span != 0 && record->span <= view.span_count);
    CHECK(record->as.children.count == 3);
    const TfCoreConstantId *ids = record->as.children.items;
    CHECK(ids[0] == 2 && ids[1] == 4 && ids[2] == 5);
    CHECK(constant(&view,ids[0],TF_CORE_CONSTANT_FLOAT)->as.float_bits == UINT64_C(0x400c000000000000));
    const TfCoreConstant *option = constant(&view,ids[1],TF_CORE_CONSTANT_VARIANT);
    CHECK(option->as.variant.tag == TF_CORE_OPTION_SOME && option->as.variant.payloads.count == 1);
    CHECK(constant(&view,option->as.variant.payloads.items[0],TF_CORE_CONSTANT_INT)->as.integer == -4);
    const TfCoreText text = constant(&view,ids[2],TF_CORE_CONSTANT_TEXT)->as.text;
    CHECK(text.length == 3 && memcmp(text.bytes,"a\0b",3) == 0);
    CHECK(tf_core_lower_initializer_constant(constants,states[1],&root) == TF_OK && root == 7);
    CHECK(tf_core_lower_initializer_constant(constants,states[3],&root) == TF_OK && root == 8);
    tf_core_lower_constant_view(constants,&view);
    CHECK(constant(&view,7,TF_CORE_CONSTANT_LIST)->as.children.count == 0);
    CHECK(constant(&view,8,TF_CORE_CONSTANT_VARIANT)->as.variant.tag == TF_CORE_OPTION_NONE);
    tf_core_lower_constants_destroy(constants);
    CHECK(c.live == 0 && trials != 0);
    for (size_t fail=1; fail<=trials; ++fail) {
        c = (Counter){0};
        CHECK(tf_core_lower_constants_create(&constants,types,&a) == TF_OK);
        CHECK(tf_core_lower_constant_append(constants,seed,&sentinel) == TF_OK && sentinel == 1);
        c.fail_at = c.calls+fail;
        CHECK(tf_core_lower_initializer_constant(constants,states[0],&root) == TF_HOST_ERROR && root == 0);
        tf_core_lower_constant_view(constants,&view);
        CHECK(view.constant_count == 1 && constant(&view,1,TF_CORE_CONSTANT_INT)->as.integer == 9);
        CHECK(tf_core_lower_initializer_constant(constants,states[0],&root) == TF_OK && root == 6);
        tf_core_lower_constants_destroy(constants);
        CHECK(c.live == 0);
    }
    tf_core_lower_types_destroy(types);
    tf_machine_program_destroy(plan);
    printf("Core constant pool: %zu injected materialization allocation failures\n",trials);
}

int main(void) {
    TfSourceManager *manager = NULL;
    TfResolution *resolution = NULL;
    TfTypedSignatures *signatures = NULL;
    TfTypedProgram *program = NULL;
    TfNodeId states[4], non_state = {0};
    size_t count = 0, injections = 0;
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    CHECK(tf_resolve(&resolution,manager,"tests/fixtures/types/core-initializers.tfd",NULL,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&signatures,resolution,100,NULL) == TF_OK);
    CHECK(tf_check(&program,resolution,signatures,100,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_typed_program_phase(program)) == TF_DIAG_PHASE_VALID);
    const TfAst *ast = tf_resolution_module(resolution,tf_resolution_root(resolution))->ast;
    for (size_t i=0; i<tf_ast_count(ast); ++i) {
        const TfAstNode *n = tf_ast_at(ast,i);
        if (n->kind == TF_AST_STATE) { CHECK(count<4); states[count++] = n->id; }
        else if (n->kind == TF_AST_WRITER) non_state = n->id;
    }
    CHECK(count == 4 && non_state.index != 0);
    for (size_t i=0; i<count; ++i) {
        const TfTypedNode *state = tf_typed_program_node(program,states[i]);
        Counter c = {0};
        const TfAllocator a = {&c,allocate,deallocate};
        size_t trials;
        CHECK(state != NULL);
        Inspect probe = {.program=program,.expected=state->type,.cell=i};
        CHECK(tf_visit_checked_initializer(program,states[i],&a,inspect,&probe) == TF_OK);
        CHECK(probe.visits == 1 && c.live == 0);
        trials = c.calls;
        for (size_t fail=1; fail<=trials; ++fail) {
            c = (Counter){.fail_at=fail};
            probe.visits = 0;
            CHECK(tf_visit_checked_initializer(program,states[i],&a,inspect,&probe) == TF_HOST_ERROR);
            CHECK(probe.visits == 0 && c.live == 0);
        }
        injections += trials;
        c = (Counter){0};
        probe.visits = 0; probe.reject = true;
        CHECK(tf_visit_checked_initializer(program,states[i],&a,inspect,&probe) == TF_HOST_ERROR);
        CHECK(probe.visits == 1 && c.live == 0);
        CHECK(tf_visit_checked_initializer(program,non_state,&a,inspect,&probe) == TF_HOST_ERROR);
        CHECK(tf_visit_checked_initializer(program,(TfNodeId){0},&a,inspect,&probe) == TF_HOST_ERROR);
        CHECK(tf_visit_checked_initializer(program,states[i],&a,NULL,NULL) == TF_HOST_ERROR);
        CHECK(tf_visit_checked_initializer(NULL,states[i],&a,inspect,&probe) == TF_HOST_ERROR);
        CHECK(probe.visits == 1 && c.live == 0);
    }
    pool(program,states);
    tf_typed_program_destroy(program);
    tf_signatures_destroy(signatures);
    tf_resolution_destroy(resolution);
    tf_source_manager_destroy(manager);
    printf("Core initializer bridge: %zu checks, %zu injected allocation failures\n",checks,injections);
    return 0;
}
