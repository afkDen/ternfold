/* Rename the runtime's independent success enumerator within this test TU. */
#define TF_OK TF_RT_OK
#include "ternfold_rt_stack.h"
#undef TF_OK
#include "../src/backend/lifetimes.h"
#include "ternfold/core_lower.h"
#include "../src/typed_signatures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(e) do { ++checks; if (!(e)) { \
    fprintf(stderr,"lifetimes:%d: %s\n",__LINE__,#e); exit(1); } } while (0)
typedef struct { size_t calls,fail,live; } Allocation;
static void *allocate(void *context,size_t bytes) {
    Allocation *a=context;
    if(++a->calls==a->fail) return NULL;
    void *p=malloc(bytes); if(p!=NULL) ++a->live; return p;
}
static void deallocate(void *context,void *p) {
    Allocation *a=context; CHECK(p!=NULL && a->live!=0); --a->live; free(p);
}

static const TfCoreTypeId arg_types[]={1};
static const TfCoreType types[]={
    {.kind=TF_CORE_TYPE_TEXT}, {.kind=TF_CORE_TYPE_LIST,.as.element=1},
    {.kind=TF_CORE_TYPE_FUNCTION,.as.function={{arg_types,1},1}}
};
static const TfCoreParameter parameter={.value=1,.type=1};
static const TfCoreParameter child_arg={.value=2,.type=1};
static const TfCoreConstant text={.kind=TF_CORE_CONSTANT_TEXT,.type=1,
    .as.text={(const unsigned char *)"a\0b",3}};
static const TfCoreValueId capture[]={2}, invocation[]={3,2}, child[]={4};
static const TfCoreLet lets[]={
    {.value=2,.type=1,.operation={.kind=TF_CORE_OP_CONST,.as.constant=1}},
    {.value=3,.type=3,.operation={.kind=TF_CORE_OP_CLOSURE,.operands={capture,1},.as.function=2}},
    {.value=4,.type=1,.operation={.kind=TF_CORE_OP_CALL_CLOSURE,.operands={invocation,2}}},
    {.value=5,.type=2,.operation={.kind=TF_CORE_OP_LIST,.operands={child,1},.as.element_type=1}}
};
static const TfCoreBlock block={lets,4,{TF_CORE_YIELD,5,0},0};
static const TfCoreBlock child_block={NULL,0,{TF_CORE_YIELD,1,0},0};
static const TfCoreFunction functions[]={
    {.kind=TF_CORE_FUNCTION_ORDINARY,.parameters=&parameter,.parameter_count=1,.result=2,
     .blocks=&block,.block_count=1,.body=1,.value_count=5},
    {.kind=TF_CORE_FUNCTION_ORDINARY,.parameters=&child_arg,.parameter_count=1,
     .captures=&parameter,.capture_count=1,.result=1,.blocks=&child_block,.block_count=1,.body=1,.value_count=2}
};

static void expect(const TfNativeLifetimes *p,size_t boundary,const size_t *ids,size_t count) {
    const TfCoreValueIds actual=tf_native_lifetimes_releases(p,boundary);
    CHECK(actual.count==count);
    for(size_t i=0;i<count;++i) CHECK(actual.items[i]==ids[i]);
}
static void plan_failures(const TfVerifiedCore *v) {
    Allocation a={0}; const TfAllocator allocator={&a,allocate,deallocate};
    TfNativeLifetimes *p=NULL;
    CHECK(tf_native_lifetimes(&p,v,1,&allocator)==TF_OK);
    tf_native_lifetimes_destroy(p); CHECK(a.live==0);
    const size_t count=a.calls;
    for(size_t i=1;i<=count;++i) {
        a=(Allocation){.fail=i}; p=NULL;
        CHECK(tf_native_lifetimes(&p,v,1,&allocator)==TF_HOST_ERROR);
        CHECK(p==NULL && a.live==0);
    }
    printf("lifetimes: %zu analysis allocation failpoints\n",count);
}

/* Handwritten structured resume bodies execute the analyzed single-block Core
 * above. No emitter changes or generated-source rewriting is used. Plans belong
 * to each invocation's stack wrapper, not mutable process-global test state. */
typedef struct { tf_stack stack; const TfNativeLifetimes *plan; bool release; unsigned fault; } Stack;
static void boundary(Stack *s,tf_stack_frame *f,size_t after) {
    if(!s->release) return;
    const TfCoreValueIds ids=tf_native_lifetimes_releases(s->plan,after);
    for(size_t i=0;i<ids.count;++i) tf_release(&f->slots[ids.items[i]-1]);
}
static tf_status resume_child(tf_stack *s,tf_stack_frame *f) {
    Stack *test=(Stack *)s;
    if(test->fault==1) return tf_context_fail(s->ctx,TF_R002,7);
    if(tf_retain(s->ctx,0,f->captures[0],&f->slots[0])!=TF_RT_OK) return TF_FAULT;
    return tf_stack_return(s,f,0);
}
static tf_status resume_root(tf_stack *s,tf_stack_frame *f) {
    Stack *test=(Stack *)s; tf_value *v=f->slots;
    if(f->pc==1) goto resumed;
    CHECK(f->pc==0);
    if(tf_retain(s->ctx,0,f->args[0],&v[0])!=TF_RT_OK) return TF_FAULT;
    boundary(test,f,0);
    if(tf_text(s->ctx,0,1,(const unsigned char *)"a\0b",3,&v[1])!=TF_RT_OK) return TF_FAULT;
    boundary(test,f,1);
    if(tf_closure(s->ctx,0,2,&v[1],1,&v[2])!=TF_RT_OK) return TF_FAULT;
    boundary(test,f,2);
    /* No release at scheduling. Inputs/results remain live until resumption. */
    return tf_stack_call_closure(s,f,0,v[2],&v[1],1,3,1);
resumed:
    CHECK(v[1].tag==TF_VALUE_BOXED && v[2].tag==TF_VALUE_BOXED && v[3].tag==TF_VALUE_BOXED);
    boundary(test,f,3);
    if(test->release) CHECK(v[1].tag==TF_VALUE_UNIT && v[2].tag==TF_VALUE_UNIT);
    if(test->fault==2) {
        const uint64_t old=tf_test_swap_refcount(s->ctx,v[3],UINT64_MAX);
        CHECK(old!=0);
        const tf_status status=tf_list(s->ctx,9,2,&v[3],1,&v[4]);
        CHECK(tf_test_swap_refcount(s->ctx,v[3],old)==UINT64_MAX);
        CHECK(status==TF_FAULT && v[4].tag==TF_VALUE_UNIT);
        return status;
    }
    if(tf_list(s->ctx,0,2,&v[3],1,&v[4])!=TF_RT_OK) return TF_FAULT;
    boundary(test,f,4);
    return tf_stack_return(s,f,4);
}
static const tf_type_id rt_args[]={1};
static const tf_rt_type rt_types[]={
    {.kind=TF_RT_TYPE_TEXT},{.kind=TF_RT_TYPE_LIST,.as.element=1},{.kind=TF_RT_TYPE_FUNCTION}
};
static const tf_rt_function rt_functions[]={{3,{NULL,0}},{3,{rt_args,1}}};
static const tf_rt_program rt_program={rt_types,3,rt_functions,2};
static const tf_stack_body bodies[]={
    {resume_root,TF_RT_ORDINARY,{rt_args,1},2,5},
    {resume_child,TF_RT_ORDINARY,{rt_args,1},1,2}
};
static const tf_stack_program stack_program={bodies,2,1};

static size_t native_run(const TfNativeLifetimes *plan,bool release,unsigned fault,size_t fail) {
    Allocation a={.fail=fail};
    tf_context ctx;
    tf_rt_config config={.allocator={allocate,deallocate,&a},
        .byte_budget=(size_t)TF_RT_DEFAULT_BYTE_BUDGET,.call_limit=8};
    CHECK(tf_context_init(&ctx,&rt_program,&config)==TF_RT_OK);
    Stack s={.plan=plan,.release=release,.fault=fault};
    CHECK(tf_stack_init(&s.stack,&ctx,&stack_program,8)==TF_RT_OK);
    tf_value input=tf_unit(),out=tf_unit();
    tf_status status=tf_text(&ctx,0,1,(const unsigned char *)"input",5,&input);
    if(status==TF_RT_OK) status=tf_stack_run(&s.stack,0,1,NULL,0,&input,1,&out);
    if(fail!=0 || fault) {
        CHECK(status==TF_FAULT && out.tag==TF_VALUE_UNIT);
        CHECK(ctx.fault.code==(fail!=0 || fault==2 ? TF_R003 : TF_R002));
        if(fault!=0 && fail==0) CHECK(ctx.fault.span==(fault==1 ? 7u:9u));
        if(fail!=0) CHECK(a.calls==fail); /* no allocations during unwind */
    } else {
        CHECK(status==TF_RT_OK && out.tag==TF_VALUE_BOXED);
        size_t count=0; tf_value item=tf_unit();
        CHECK(tf_list_length(&ctx,0,out,&count)==TF_RT_OK && count==1);
        CHECK(tf_list_at(&ctx,0,out,0,&item)==TF_RT_OK);
        const unsigned char *bytes=NULL; size_t length=0;
        CHECK(tf_text_view(&ctx,0,item,&bytes,&length)==TF_RT_OK && length==3 && memcmp(bytes,"a\0b",3)==0);
        tf_release(&item);
    }
    tf_release(&out); tf_release(&input); tf_stack_destroy(&s.stack);
    CHECK(ctx.live_bytes==0 && a.live==0);
    return a.calls;
}

static void observed_source(void) {
    TfSourceManager *sources=NULL; TfResolution *resolution=NULL; TfTypedSignatures *sig=NULL;
    TfTypedProgram *typed=NULL; TfMachineProgram *machines=NULL; TfCoreProgram *owner=NULL; TfVerifiedCore *v=NULL;
    CHECK(tf_source_manager_create(&sources,NULL)==TF_OK);
    CHECK(tf_resolve(&resolution,sources,"benchmarks/lifetimes-fixture.tfd",NULL,NULL)==TF_OK);
    CHECK(tf_signatures_build(&sig,resolution,100,NULL)==TF_OK);
    CHECK(tf_check(&typed,resolution,sig,100,NULL)==TF_OK);
    CHECK(tf_machine_program_build(&machines,typed,100,NULL)==TF_OK);
    CHECK(tf_core_lower(&owner,machines,NULL,NULL)==TF_OK && tf_core_verify(&v,owner,NULL,NULL)==TF_OK);
    const TfCoreProgramData *data=tf_verified_core_data(v);
    CHECK(data->trace_point_count!=0);
    size_t observed=0,unsupported=0;
    for(size_t f=1;f<=data->function_count;++f) {
        TfNativeLifetimes *p=NULL; CHECK(tf_native_lifetimes(&p,v,f,NULL)==TF_OK);
        if(!tf_native_lifetimes_eligible(p)) { ++unsupported; tf_native_lifetimes_destroy(p); continue; }
        const TfCoreFunction *fn=&data->functions[f-1];
        for(size_t j=0;j<=fn->blocks[0].let_count;++j) {
            const TfCoreValueIds ids=tf_native_lifetimes_releases(p,j);
            for(size_t i=0;i<ids.count;++i) {
                CHECK(ids.items[i]!=fn->blocks[0].terminator.value);
                for(size_t k=0;k<data->trace_point_count;++k)
                    if(data->trace_points[k].function==f && data->trace_points[k].value==ids.items[i]) {
                        CHECK(j>=data->trace_points[k].after_let); ++observed;
                    }
            }
        }
        tf_native_lifetimes_destroy(p);
    }
    CHECK(observed!=0 && unsupported!=0);
    /* Delay all points in the straight-line machine to the final boundary.
     * These verified metadata reads must keep their slots live after their
     * computational last uses, regardless of whether capture is enabled. */
    TfCoreProgramData delayed=*data;
    TfCoreTracePoint *points=malloc(data->trace_point_count*sizeof(*points)); CHECK(points!=NULL);
    memcpy(points,data->trace_points,data->trace_point_count*sizeof(*points));
    delayed.trace_points=points;
    const TfCoreFunctionId machine=data->trace_machines[0].function;
    const size_t end=data->functions[machine-1].blocks[0].let_count;
    CHECK(data->functions[machine-1].block_count==1);
    for(size_t i=0;i<data->trace_point_count;++i) if(points[i].function==machine) points[i].after_let=end;
    TfCoreProgram *delayed_owner=NULL; TfVerifiedCore *delayed_v=NULL; TfNativeLifetimes *delayed_plan=NULL;
    CHECK(tf_core_program_copy(&delayed_owner,&delayed,NULL)==TF_OK);
    CHECK(tf_core_verify(&delayed_v,delayed_owner,NULL,NULL)==TF_OK);
    CHECK(tf_native_lifetimes(&delayed_plan,delayed_v,machine,NULL)==TF_OK && tf_native_lifetimes_eligible(delayed_plan));
    for(size_t i=0;i<data->trace_point_count;++i) if(points[i].function==machine) {
        bool found=false;
        for(size_t j=0;j<=end;++j) {
            const TfCoreValueIds ids=tf_native_lifetimes_releases(delayed_plan,j);
            for(size_t k=0;k<ids.count;++k) if(ids.items[k]==points[i].value) { CHECK(j==end); found=true; }
        }
        CHECK(found);
    }
    tf_native_lifetimes_destroy(delayed_plan); tf_verified_core_destroy(delayed_v);
    tf_core_program_destroy(delayed_owner); free(points);
    tf_verified_core_destroy(v); tf_core_program_destroy(owner); tf_machine_program_destroy(machines);
    tf_typed_program_destroy(typed); tf_signatures_destroy(sig); tf_resolution_destroy(resolution);
    tf_source_manager_destroy(sources);
}

int main(void) {
    const TfCoreProgramData data={.types=types,.type_count=3,.constants=&text,.constant_count=1,
        .functions=functions,.function_count=2};
    TfCoreProgram *owner=NULL; TfVerifiedCore *v=NULL; TfNativeLifetimes *p=NULL;
    CHECK(tf_core_program_copy(&owner,&data,NULL)==TF_OK && tf_core_verify(&v,owner,NULL,NULL)==TF_OK);
    CHECK(tf_native_lifetimes(&p,v,1,NULL)==TF_OK && tf_native_lifetimes_eligible(p));
    expect(p,0,(const size_t[]){1},1); expect(p,1,NULL,0); expect(p,2,NULL,0);
    expect(p,3,(const size_t[]){2,3},2); expect(p,4,(const size_t[]){4},1); expect(p,SIZE_MAX,NULL,0);
    plan_failures(v);
    tf_verified_core_destroy(v); tf_core_program_destroy(owner); /* plan has no borrowed Core */
    for(unsigned release=0;release<=1;++release) {
        const size_t calls=native_run(p,release!=0,0,0);
        (void)native_run(p,release!=0,1,0);
        (void)native_run(p,release!=0,2,0);
        for(size_t i=1;i<=calls;++i) (void)native_run(p,release!=0,0,i);
        printf("lifetimes native release=%u: %zu allocation failpoints\n",release,calls);
    }
    tf_native_lifetimes_destroy(p);
    CHECK(tf_native_lifetimes(NULL,NULL,0,NULL)==TF_HOST_ERROR);
    p=NULL; CHECK(tf_native_lifetimes(&p,NULL,0,NULL)==TF_HOST_ERROR && p==NULL);
    CHECK(!tf_native_lifetimes_eligible(NULL) && tf_native_lifetimes_releases(NULL,0).count==0);
    tf_native_lifetimes_destroy(NULL);
    observed_source();
    printf("lifetimes: %zu checks passed\n",checks); return 0;
}
