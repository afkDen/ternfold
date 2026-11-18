#include "ternfold/core_lower.h"
#include "ternfold/core_verify.h"
#include "../src/typed_signatures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(ok) do { ++checks; if (!(ok)) { \
    fprintf(stderr,"test_core_lower:%d: %s\n",__LINE__,#ok); exit(1); \
} } while (0)

typedef struct { TfResolution *resolution; TfTypedSignatures *signatures;
                 TfTypedProgram *typed; TfMachineProgram *machines; } Front;
static Front prepare(TfSourceManager *manager, const char *path) {
    Front f={0};
    CHECK(tf_resolve(&f.resolution,manager,path,NULL,NULL)==TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(f.resolution))==TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&f.signatures,f.resolution,100,NULL)==TF_OK);
    CHECK(tf_check(&f.typed,f.resolution,f.signatures,100,NULL)==TF_OK);
    CHECK(tf_machine_program_build(&f.machines,f.typed,100,NULL)==TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_machine_program_phase(f.machines))==TF_DIAG_PHASE_VALID);
    return f;
}
static void release(Front f) {
    tf_machine_program_destroy(f.machines); tf_typed_program_destroy(f.typed);
    tf_signatures_destroy(f.signatures); tf_resolution_destroy(f.resolution);
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
    --c->live; free(ptr);
}
static bool text_is(TfCoreText text,const char *name) {
    const size_t len=strlen(name);
    return text.length==len && memcmp(text.bytes,name,len)==0;
}
static const TfCoreFunction *find(const TfCoreProgramData *data,const char *name) {
    for (size_t i=0; i<data->function_count; ++i)
        if (text_is(data->functions[i].name,name)) return &data->functions[i];
    CHECK(false); return NULL;
}
static const TfCoreLet *nth(const TfCoreBlock *b,size_t i,TfCoreOperationKind kind) {
    CHECK(b!=NULL && i<b->let_count && b->lets[i].operation.kind==kind);
    return &b->lets[i];
}
static const TfCoreConstant *constant(const TfCoreProgramData *d,const TfCoreBlock *b,size_t i,
                                      TfCoreConstantKind kind) {
    const TfCoreConstant *k=tf_core_constant(d,nth(b,i,TF_CORE_OP_CONST)->operation.as.constant);
    CHECK(k!=NULL && k->kind==kind); return k;
}
static void inspect(const TfCoreProgramData *d) {
    CHECK(d!=NULL && d->function_count==8 && d->entry!=0);
    const TfCoreFunction *main_fn=find(d,"main"), *make=find(d,"make"), *flags=find(d,"flags");
    const TfCoreFunction *add=find(d,"add"), *bounds=find(d,"bounds"), *mark=find(d,"mark");
    const TfCoreFunction *through=find(d,"through"), *pure=find(d,"pure_main_is_callable");
    CHECK(main_fn==tf_core_function(d,d->entry) && main_fn->kind==TF_CORE_FUNCTION_ORDINARY);
    CHECK(text_is(main_fn->module,"core-ordinary.tfd"));
    const TfCoreBlock *b=tf_core_block(pure,pure->body);
    CHECK(nth(b,0,TF_CORE_OP_CALL)->operation.as.function==d->entry);
    CHECK(b->terminator.value==b->lets[0].value);

    b=tf_core_block(make,make->body);
    CHECK(constant(d,b,0,TF_CORE_CONSTANT_INT)->as.integer==1);
    CHECK(constant(d,b,1,TF_CORE_CONSTANT_INT)->as.integer==2);
    const TfCoreLet *second=nth(b,2,TF_CORE_OP_CALL);
    CHECK(tf_core_function(d,second->operation.as.function)==add);
    CHECK(constant(d,b,3,TF_CORE_CONSTANT_INT)->as.integer==3);
    CHECK(constant(d,b,4,TF_CORE_CONSTANT_INT)->as.integer==4);
    const TfCoreLet *first=nth(b,5,TF_CORE_OP_CALL);
    const TfCoreLet *record=nth(b,6,TF_CORE_OP_RECORD);
    CHECK(record->operation.operands.count==2 && record->operation.operands.items[0]==first->value &&
          record->operation.operands.items[1]==second->value && b->terminator.value==record->value);
    CHECK(constant(d,tf_core_block(bounds,bounds->body),0,TF_CORE_CONSTANT_INT)->as.integer==INT64_MIN);

    b=tf_core_block(flags,flags->body);
    const TfCoreLet *branch=nth(b,0,TF_CORE_OP_IF);
    CHECK(b->let_count==1 && branch->operation.operands.items[0]==flags->parameters[0].value);
    const TfCoreBlock *yes=tf_core_block(flags,branch->operation.as.branch.then_block);
    const TfCoreBlock *no=tf_core_block(flags,branch->operation.as.branch.else_block);
    const TfCoreLet *divide=nth(yes,1,TF_CORE_OP_BINARY);
    CHECK(divide->operation.as.binary==TF_CORE_BINARY_FLOOR_DIVIDE);
    const TfSpan *span=tf_core_span(d,divide->span);
    CHECK(span!=NULL && span->end-span->start==3 && memcmp(span->source->bytes+span->start,"div",3)==0);
    CHECK(no->let_count==1 && constant(d,no,0,TF_CORE_CONSTANT_BOOL)->as.boolean==0);

    b=tf_core_block(mark,mark->body);
    branch=nth(b,0,TF_CORE_OP_IF);
    yes=tf_core_block(mark,branch->operation.as.branch.then_block);
    no=tf_core_block(mark,branch->operation.as.branch.else_block);
    CHECK(nth(yes,0,TF_CORE_OP_VARIANT)->operation.as.variant.tag==0);
    CHECK(nth(no,1,TF_CORE_OP_VARIANT)->operation.as.variant.tag==1);

    b=tf_core_block(through,through->body);
    const TfCoreLet *dynamic=nth(b,2,TF_CORE_OP_CALL_CLOSURE);
    CHECK(dynamic->operation.operands.count==3 && dynamic->operation.operands.items[0]==through->parameters[0].value);
    b=tf_core_block(main_fn,main_fn->body);
    CHECK(tf_core_function(d,nth(b,1,TF_CORE_OP_CLOSURE)->operation.as.function)==add);
    const TfCoreLet *len=nth(b,5,TF_CORE_OP_BUILTIN);
    CHECK(len->operation.as.builtin.id==TF_CORE_BUILTIN_LEN && len->operation.as.builtin.type_arguments.count==1);
    const TfCoreLet *map=nth(b,b->let_count-1,TF_CORE_OP_LIST_MAP);
    const TfCoreBlock *body=tf_core_block(main_fn,map->operation.as.list_map.body);
    const TfCoreLet *formatter=nth(body,0,TF_CORE_OP_BUILTIN);
    CHECK(formatter->operation.as.builtin.id==TF_CORE_BUILTIN_INT_TEXT &&
          formatter->operation.operands.items[0]==map->operation.as.list_map.element.value &&
          b->terminator.value==map->value && body->terminator.value==formatter->value);
}

static void inspect_entry(const TfCoreProgramData *d) {
    CHECK(d!=NULL && d->function_count==2 && d->entry!=0);
    const TfCoreFunction *entry=tf_core_function(d,d->entry);
    CHECK(entry!=NULL && entry->kind==TF_CORE_FUNCTION_ENTRY_ONLY && text_is(entry->name,"$entry"));
    CHECK(entry->parameter_count==1 && entry->capture_count==0 && entry->parameters[0].type==entry->result);
    const TfCoreBlock *b=tf_core_block(entry,entry->body);
    const TfCoreLet *first=nth(b,0,TF_CORE_OP_INPUT_AT), *second=nth(b,2,TF_CORE_OP_INPUT_AT);
    CHECK(first->operation.operands.items[0]==entry->parameters[0].value && first->operation.as.input_index==0);
    CHECK(second->operation.operands.items[0]==entry->parameters[0].value && second->operation.as.input_index==1);
    const TfCoreLet *stage_call=nth(b,1,TF_CORE_OP_CALL), *returned=nth(b,3,TF_CORE_OP_LIST);
    CHECK(tf_core_function(d,stage_call->operation.as.function)==find(d,"prefix") &&
          stage_call->operation.operands.items[0]==first->value);
    CHECK(returned->operation.operands.count==2 && returned->operation.operands.items[0]==second->value &&
          returned->operation.operands.items[1]==first->value);
    const TfCoreLet *staged=nth(b,4,TF_CORE_OP_LIST), *result=nth(b,5,TF_CORE_OP_BINARY);
    CHECK(staged->operation.operands.count==2 && staged->operation.operands.items[0]==stage_call->value &&
          staged->operation.operands.items[1]==second->value);
    CHECK(result->operation.as.binary==TF_CORE_BINARY_CONCAT && result->operation.operands.items[0]==staged->value &&
          result->operation.operands.items[1]==returned->value && b->terminator.value==result->value);
}

static void inspect_wrappers(const TfCoreProgramData *d) {
    CHECK(d!=NULL && d->function_count==4 && d->entry==0);
    const TfCoreFunction *fn=find(d,"use_prelude");
    const TfCoreBlock *b=tf_core_block(fn,fn->body);
    const TfCoreFunctionId format=nth(b,0,TF_CORE_OP_CLOSURE)->operation.as.function;
    const TfCoreFunctionId ints=nth(b,2,TF_CORE_OP_CLOSURE)->operation.as.function;
    const TfCoreFunctionId chars=nth(b,3,TF_CORE_OP_CLOSURE)->operation.as.function;
    CHECK(format==nth(b,1,TF_CORE_OP_CLOSURE)->operation.as.function);
    CHECK(ints!=chars && chars!=format && ints!=format);
    CHECK(ints==nth(b,8,TF_CORE_OP_CLOSURE)->operation.as.function);
    const TfCoreFunction *wrapper=tf_core_function(d,format);
    CHECK(wrapper!=NULL && wrapper->capture_count==0 && wrapper->parameter_count==1);
    CHECK(nth(tf_core_block(wrapper,wrapper->body),0,TF_CORE_OP_BUILTIN)->operation.as.builtin.id==TF_CORE_BUILTIN_INT_TEXT);
    wrapper=tf_core_function(d,ints);
    const TfCoreLet *int_len=nth(tf_core_block(wrapper,wrapper->body),0,TF_CORE_OP_BUILTIN);
    wrapper=tf_core_function(d,chars);
    const TfCoreLet *char_len=nth(tf_core_block(wrapper,wrapper->body),0,TF_CORE_OP_BUILTIN);
    CHECK(int_len->operation.as.builtin.id==TF_CORE_BUILTIN_LEN && char_len->operation.as.builtin.id==TF_CORE_BUILTIN_LEN);
    CHECK(int_len->operation.as.builtin.type_arguments.count==1 && char_len->operation.as.builtin.type_arguments.count==1);
    CHECK(tf_core_type(d,int_len->operation.as.builtin.type_arguments.items[0])->kind==TF_CORE_TYPE_INT);
    CHECK(tf_core_type(d,char_len->operation.as.builtin.type_arguments.items[0])->kind==TF_CORE_TYPE_CHAR);
}

static void inspect_closures(const TfCoreProgramData *d) {
    CHECK(d!=NULL && d->function_count==3 && d->entry==0);
    const TfCoreFunction *source=find(d,"capture");
    const TfCoreBlock *b=tf_core_block(source,source->body);
    const TfCoreOperation *outer_op=&nth(b,0,TF_CORE_OP_CLOSURE)->operation;
    CHECK(outer_op->operands.count==2 && outer_op->operands.items[0]==source->parameters[0].value &&
          outer_op->operands.items[1]==source->parameters[1].value);
    CHECK(nth(b,3,TF_CORE_OP_BINARY)->operation.operands.items[1]==source->parameters[0].value);
    const TfCoreFunction *outer=tf_core_function(d,outer_op->as.function);
    CHECK(outer!=NULL && outer->capture_count==2 && outer->parameter_count==1 && text_is(outer->name,"$lambda"));
    CHECK(text_is(outer->captures[0].name,"z") && text_is(outer->captures[1].name,"a"));
    b=tf_core_block(outer,outer->body);
    const TfCoreOperation *inner_op=&nth(b,0,TF_CORE_OP_CLOSURE)->operation;
    CHECK(inner_op->operands.count==3 && inner_op->operands.items[0]==outer->captures[0].value &&
          inner_op->operands.items[1]==outer->captures[1].value && inner_op->operands.items[2]==outer->parameters[0].value);
    CHECK(nth(b,2,TF_CORE_OP_BINARY)->operation.operands.items[1]==outer->captures[1].value);
    const TfCoreFunction *inner=tf_core_function(d,inner_op->as.function);
    CHECK(inner!=NULL && inner->capture_count==3 && inner->parameter_count==0);
    CHECK(text_is(inner->captures[0].name,"z") && text_is(inner->captures[1].name,"a") &&
          text_is(inner->captures[2].name,"delta"));
    b=tf_core_block(inner,inner->body);
    const TfCoreOperation *difference=&nth(b,0,TF_CORE_OP_BINARY)->operation;
    CHECK(difference->as.binary==TF_CORE_BINARY_SUBTRACT && difference->operands.items[0]==inner->captures[1].value &&
          difference->operands.items[1]==inner->captures[0].value);
    CHECK(nth(b,1,TF_CORE_OP_BINARY)->operation.operands.items[1]==inner->captures[2].value);
}

static TfVerifiedCore *verify(TfCoreProgram *program) {
    TfVerifiedCore *verified=NULL;
    TfCoreVerifyIssue issue={0};
    const TfStatus status=tf_core_verify(&verified,program,NULL,&issue);
    if (status!=TF_OK)
        fprintf(stderr,"verify lowered Core: status=%d code=%d type=%zu fn=%zu block=%zu value=%zu\n",
                status,issue.code,issue.type,issue.function,issue.block,issue.value);
    CHECK(status==TF_OK && verified!=NULL && issue.code==TF_CORE_VERIFY_NONE);
    return verified;
}

static size_t failpoints(Front f) {
    TfCoreProgram *core=NULL;
    TfCoreLowerIssue issue;
    Count count={0};
    const TfAllocator allocator={&count,allocate,deallocate};
    CHECK(tf_core_lower(&core,f.machines,&allocator,&issue)==TF_OK);
    CHECK(core!=NULL && issue.code==TF_CORE_LOWER_NONE);
    const size_t allocations=count.calls;
    tf_core_program_destroy(core); CHECK(count.live==0);
    for (size_t i=1; i<=allocations; ++i) {
        count=(Count){.fail=i};
        CHECK(tf_core_lower(&core,f.machines,&allocator,&issue)==TF_HOST_ERROR);
        CHECK(core==NULL && count.live==0 && issue.code==TF_CORE_LOWER_NONE);
    }
    return allocations;
}

int main(void) {
    TfSourceManager *manager=NULL;
    TfCoreProgram *core=NULL;
    TfCoreLowerIssue issue;
    CHECK(tf_source_manager_create(&manager,NULL)==TF_OK);
    Front f=prepare(manager,"tests/fixtures/types/core-ordinary.tfd");
    size_t allocations=failpoints(f);
    CHECK(tf_core_lower(&core,f.machines,NULL,&issue)==TF_OK);
    release(f); /* Only source snapshots must still be alive. */
    TfVerifiedCore *verified=verify(core);
    inspect(tf_verified_core_data(verified));
    tf_verified_core_destroy(verified); tf_core_program_destroy(core);

    f=prepare(manager,"tests/fixtures/types/core-match-pending.tfd");
    CHECK(tf_core_lower(&core,f.machines,NULL,&issue)==TF_HOST_ERROR && core==NULL);
    CHECK(issue.code==TF_CORE_LOWER_UNSUPPORTED_FORM && issue.span.source!=NULL &&
          memcmp(issue.span.source->bytes+issue.span.start,"match",5)==0);
    release(f);

    f=prepare(manager,"tests/fixtures/types/core-entry.tfd");
    allocations+=failpoints(f);
    CHECK(tf_core_lower(&core,f.machines,NULL,&issue)==TF_OK);
    release(f);
    verified=verify(core);
    inspect_entry(tf_verified_core_data(verified));
    tf_verified_core_destroy(verified); tf_core_program_destroy(core);

    f=prepare(manager,"tests/fixtures/types/core-prelude-values.tfd");
    allocations+=failpoints(f);
    CHECK(tf_core_lower(&core,f.machines,NULL,&issue)==TF_OK);
    release(f);
    verified=verify(core);
    inspect_wrappers(tf_verified_core_data(verified));
    tf_verified_core_destroy(verified); tf_core_program_destroy(core);

    f=prepare(manager,"tests/fixtures/types/core-closures.tfd");
    allocations+=failpoints(f);
    CHECK(tf_core_lower(&core,f.machines,NULL,&issue)==TF_OK);
    release(f);
    verified=verify(core);
    inspect_closures(tf_verified_core_data(verified));
    tf_verified_core_destroy(verified); tf_core_program_destroy(core);
    tf_source_manager_destroy(manager);
    printf("Core ordinary lowering: %zu checks, %zu injected allocation failures\n",checks,allocations);
    return 0;
}
