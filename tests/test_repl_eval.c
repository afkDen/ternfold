#include "../src/core/repl_lower.h"
#include "../src/execution/repl_value.h"
#include "../src/typed_signatures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(ok) do { ++checks; if (!(ok)) { \
    fprintf(stderr,"test_repl_eval:%d: %s\n",__LINE__,#ok); exit(1); \
} } while (0)

typedef struct {
    TfSourceManager *manager;
    TfResolution *resolution;
    TfTypedSignatures *signatures;
    TfTypedProgram *typed;
    TfMachineProgram *machines;
} Front;

static Front prepare(void) {
    Front f={0};
    CHECK(tf_source_manager_create(&f.manager,NULL)==TF_OK);
    CHECK(tf_resolve(&f.resolution,f.manager,"tests/fixtures/types/repl-eval.tfd",NULL,NULL)==TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(f.resolution))==TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&f.signatures,f.resolution,100,NULL)==TF_OK);
    CHECK(tf_check(&f.typed,f.resolution,f.signatures,100,NULL)==TF_OK);
    const TfDiagnosticBag *bag=tf_typed_program_diagnostics(f.typed);
    if (tf_diagnostic_bag_has_errors(bag))
        (void)tf_diagnostic_bag_write(stderr,bag,TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
    CHECK(tf_machine_program_build(&f.machines,f.typed,100,NULL)==TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_machine_program_phase(f.machines))==TF_DIAG_PHASE_VALID);
    return f;
}

static void release(Front f) {
    tf_machine_program_destroy(f.machines);
    tf_typed_program_destroy(f.typed);
    tf_signatures_destroy(f.signatures);
    tf_resolution_destroy(f.resolution);
    tf_source_manager_destroy(f.manager);
}

static TfSymbolId symbol(Front f,const char *name) {
    const size_t length=strlen(name);
    for (size_t i=0; i<tf_resolution_symbol_count(f.resolution); ++i) {
        const TfResolvedSymbol *s=tf_resolution_symbol_at(f.resolution,i);
        if (s->kind==TF_SYMBOL_FUNCTION && s->name.length==length &&
            memcmp(s->name.data,name,length)==0) return s->id;
    }
    CHECK(false);
    return (TfSymbolId){0};
}

static TfNodeList lets(Front f,TfSymbolId fn) {
    const TfNodeId id=tf_resolution_symbol(f.resolution,fn)->declaration;
    const TfAstNode *n=tf_ast_get(id.owner,id);
    return tf_ast_get(id.owner,n->as.function.body)->as.block.statements;
}

static void text(TfReplValue view,const char *expected) {
    CHECK(view.length==strlen(expected) && memcmp(view.bytes,expected,view.length+1)==0);
}

static void entry_unchanged(const TfVerifiedCore *verified,TfReplValue *view) {
    const TfCoreProgramData *data=tf_verified_core_data(verified);
    CHECK(tf_core_function(data,data->entry)->kind==TF_CORE_FUNCTION_ENTRY_ONLY);
    CHECK(tf_interpret_repl(view,verified,data->entry,NULL,NULL,NULL)==TF_HOST_ERROR);
    text(*view,"");
    const TfCoreText input={(const unsigned char *)"visitor",7};
    TfInterpreterResult *result=NULL;
    CHECK(tf_interpret_entry(&result,verified,&input,1,NULL,NULL,NULL)==TF_OK);
    TfCoreText line={0};
    CHECK(tf_interpreter_result_count(result)==2 && tf_interpreter_result_line(result,0,&line));
    CHECK(line.length==12 && memcmp(line.bytes,"main:visitor",12)==0);
    CHECK(tf_interpreter_result_line(result,1,&line) && line.length==4 && memcmp(line.bytes,"done",4)==0);
    tf_interpreter_result_destroy(result);
}

static void cases(Front f,TfSymbolId wrapper,TfNodeList statements) {
    static const struct { size_t index; const char *expected; } expected[]={
        {1,"-9223372036854775808"}, {2,"5.0e-1"}, {3,"true"},
        {4,"\"a\\u{0}\\n\\r\\t\\\"\\\\\\u{1f642}\""}, {5,"'\\u{1f642}'"},
        {6,"()"}, {7,"Pair {x: 7, label: \"hi\"}"},
        {8,"Mark::Labeled(\"tag\")"}, {9,"Mark::Plain"}, {10,"Some(5)"}, {11,"None"},
        {12,"[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, ...]"}, {13,"<closure>"},
        {15,"Accept(Bin {doubled: 6, count: 3}, [6, 4])"},
        {16,"Reject(Bin {doubled: 4, count: 2}, \"negative\")"},
        {17,"Bin {doubled: 4, count: 2}"},
        {24,"\"012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345...\""},
        {25,"[[[[[[...]]]]]]"}, {27,NULL}
    };
    for (size_t i=0; i<sizeof expected/sizeof *expected; ++i) {
        TfCoreProgram *core=NULL;
        TfCoreFunctionId id=0;
        CHECK(expected[i].index<statements.count);
        CHECK(tf_core_lower_repl(&core,&id,f.machines,wrapper,statements.items[expected[i].index],NULL,NULL)==TF_OK);
        TfVerifiedCore *verified=NULL;
        CHECK(tf_core_verify(&verified,core,NULL,NULL)==TF_OK);
        const TfCoreProgramData *data=tf_verified_core_data(verified);
        const TfCoreFunction *fn=tf_core_function(data,id);
        CHECK(id!=data->entry && fn!=NULL && fn->kind==TF_CORE_FUNCTION_ORDINARY &&
              fn->parameter_count==0 && fn->capture_count==0);
        TfReplValue view;
        TfRuntimeFault fault;
        CHECK(tf_interpret_repl(&view,verified,id,NULL,NULL,&fault)==TF_OK && fault.code==TF_RUNTIME_NONE);
        if (i==0) {
            const TfCoreBlock *body=tf_core_block(fn,fn->body);
            CHECK(body->let_count>2 && body->terminator.value==body->lets[0].value);
            CHECK(tf_core_type(data,fn->result)->kind==TF_CORE_TYPE_INT);
            entry_unchanged(verified,&view);
            CHECK(tf_interpret_repl(&view,verified,id,NULL,NULL,&fault)==TF_OK);
        }
        tf_verified_core_destroy(verified);
        tf_core_program_destroy(core); /* Debug data is independent of the VM and Core. */
        if (expected[i].expected!=NULL) text(view,expected[i].expected);
        else {
            CHECK(view.length>4000 && view.length<sizeof view.bytes);
            CHECK(view.bytes[view.length]==0 && memcmp(view.bytes+view.length-3,"...",3)==0);
        }
    }
}

static void bad_lower(Front f,TfSymbolId wrapper,TfNodeId id) {
    TfCoreProgram *core=NULL;
    TfCoreFunctionId lowered=123;
    CHECK(tf_core_lower_repl(&core,&lowered,f.machines,wrapper,id,NULL,NULL)==TF_HOST_ERROR);
    CHECK(core==NULL && lowered==0);
}

static void invalid_and_fault(Front f,TfSymbolId wrapper,TfNodeList statements) {
    const TfSymbolId fault_fn=symbol(f,"session_fault"), main_fn=symbol(f,"main");
    const TfSymbolId parameters=symbol(f,"takes_argument"), non_unit=symbol(f,"non_unit");
    const TfNodeList fault_lets=lets(f,fault_fn);
    bad_lower(f,main_fn,lets(f,main_fn).items[0]);
    bad_lower(f,parameters,lets(f,parameters).items[0]);
    bad_lower(f,non_unit,lets(f,non_unit).items[0]);
    bad_lower(f,wrapper,fault_lets.items[0]);
    bad_lower(f,wrapper,(TfNodeId){0});
    const TfNodeId outer=statements.items[18];
    const TfNodeId inner=tf_ast_get(outer.owner,outer)->as.binding.value;
    bad_lower(f,wrapper,tf_ast_get(inner.owner,inner)->as.block.statements.items[0]);

    TfCoreProgram *core=NULL;
    TfCoreFunctionId id=0;
    TfVerifiedCore *verified=NULL;
    CHECK(tf_core_lower_repl(&core,&id,f.machines,fault_fn,fault_lets.items[0],NULL,NULL)==TF_OK);
    CHECK(tf_core_verify(&verified,core,NULL,NULL)==TF_OK);
    TfReplValue view={.bytes="old",.length=3};
    TfRuntimeFault fault;
    CHECK(tf_interpret_repl(&view,NULL,id,NULL,NULL,&fault)==TF_HOST_ERROR);
    text(view,"");
    CHECK(tf_interpret_repl(&view,verified,0,NULL,NULL,&fault)==TF_HOST_ERROR);
    CHECK(tf_interpret_repl(&view,verified,id,NULL,NULL,&fault)==TF_SOURCE_ERROR);
    text(view,"");
    CHECK(fault.code==TF_RUNTIME_ZERO_DIVISOR && fault.frame_count==1 && fault.frames[0].function==id);
    const TfSpan *span=tf_core_span(tf_verified_core_data(verified),fault.span);
    CHECK(span!=NULL && span->end-span->start==3 && memcmp(span->source->bytes+span->start,"div",3)==0);
    const TfInterpreterLimits no_calls={SIZE_MAX,0,UINT64_MAX};
    CHECK(tf_interpret_repl(&view,verified,id,&no_calls,NULL,&fault)==TF_SOURCE_ERROR);
    CHECK(fault.code==TF_RUNTIME_RESOURCE);
    text(view,"");
    tf_verified_core_destroy(verified);
    tf_core_program_destroy(core);
}

typedef struct { size_t calls, fail, live; } Count;
static void *allocate(void *context,size_t bytes) {
    Count *c=context;
    if (++c->calls==c->fail) return NULL;
    void *ptr=malloc(bytes);
    if (ptr!=NULL) ++c->live;
    return ptr;
}
static void deallocate(void *context,void *ptr) {
    Count *c=context;
    CHECK(ptr!=NULL && c->live>0);
    --c->live;
    free(ptr);
}

static size_t allocation_failures(Front f,TfSymbolId wrapper,TfNodeId selected) {
    Count count={0};
    const TfAllocator allocator={&count,allocate,deallocate};
    TfCoreProgram *core=NULL;
    TfCoreFunctionId id=0;
    CHECK(tf_core_lower_repl(&core,&id,f.machines,wrapper,selected,&allocator,NULL)==TF_OK);
    const size_t lowering=count.calls;
    tf_core_program_destroy(core);
    CHECK(count.live==0);
    for (size_t i=1; i<=lowering; ++i) {
        count=(Count){.fail=i};
        CHECK(tf_core_lower_repl(&core,&id,f.machines,wrapper,selected,&allocator,NULL)==TF_HOST_ERROR);
        CHECK(count.live==0 && core==NULL && id==0);
    }

    CHECK(tf_core_lower_repl(&core,&id,f.machines,wrapper,selected,NULL,NULL)==TF_OK);
    TfVerifiedCore *verified=NULL;
    CHECK(tf_core_verify(&verified,core,NULL,NULL)==TF_OK);
    TfReplValue view;
    TfRuntimeFault fault;
    count=(Count){0};
    CHECK(tf_interpret_repl(&view,verified,id,NULL,&allocator,&fault)==TF_OK);
    const size_t execution=count.calls;
    CHECK(count.live==0);
    for (size_t i=1; i<=execution; ++i) {
        count=(Count){.fail=i};
        CHECK(tf_interpret_repl(&view,verified,id,NULL,&allocator,&fault)==TF_SOURCE_ERROR);
        CHECK(count.live==0 && fault.code==TF_RUNTIME_RESOURCE);
        text(view,"");
    }
    tf_verified_core_destroy(verified);
    tf_core_program_destroy(core);
    return lowering+execution;
}

int main(void) {
    Front f=prepare();
    const TfSymbolId wrapper=symbol(f,"session_eval");
    const TfNodeList statements=lets(f,wrapper);
    CHECK(statements.count==28);
    cases(f,wrapper,statements);
    invalid_and_fault(f,wrapper,statements);
    const size_t allocations=allocation_failures(f,wrapper,statements.items[15]);
    release(f);
    printf("REPL evaluator: %zu checks, %zu injected allocation failures\n",checks,allocations);
    return 0;
}
