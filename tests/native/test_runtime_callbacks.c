#include "ternfold_rt_callbacks.h"
#include "ternfold_rt_numeric.h"

#include <stdio.h>
#include <stdlib.h>

static size_t checks;
#define CHECK(ok) do { ++checks; if (!(ok)) { \
    fprintf(stderr,"test_runtime_callbacks:%d: %s\n",__LINE__,#ok); exit(1); \
} } while (0)

typedef struct { size_t calls,fail,live; } Counts;
static void *allocate(void *user,size_t bytes) {
    Counts *c=user; if (++c->calls==c->fail) return NULL;
    void *ptr=malloc(bytes); if (ptr!=NULL) ++c->live; return ptr;
}
static void deallocate(void *user,void *ptr) {
    Counts *c=user; CHECK(ptr!=NULL && c->live!=0); --c->live; free(ptr);
}

enum { UNIT=1, INT, INTS, FUNCTION };
static const tf_rt_type types[]={ {.kind=TF_RT_TYPE_UNIT},{.kind=TF_RT_TYPE_INT},
                                  {.kind=TF_RT_TYPE_LIST,.as.element=INT},{.kind=TF_RT_TYPE_FUNCTION} };
static const tf_rt_function functions[]={ {FUNCTION,{0}},{FUNCTION,{0}},{FUNCTION,{0}},
                                         {FUNCTION,{0}},{FUNCTION,{0}} };
static const tf_rt_program values={types,4,functions,5};
static const tf_type_id map_parameters[]={INTS,FUNCTION},fold_parameters[]={INTS,INT,FUNCTION};
static const tf_type_id int_parameters[]={INT},pair_parameters[]={INT,INT};

static tf_status double_value(tf_stack *s,tf_stack_frame *f) {
    if (tf_numeric_binary(s->ctx,f->span,TF_RT_NUM_MULTIPLY,f->args[0],tf_int(2),&f->slots[0])!=TF_OK)
        return TF_FAULT;
    return tf_stack_return(s,f,0);
}
static tf_status digits(tf_stack *s,tf_stack_frame *f) {
    if (tf_numeric_binary(s->ctx,f->span,TF_RT_NUM_MULTIPLY,f->args[0],tf_int(10),&f->slots[0])!=TF_OK)
        return TF_FAULT;
    if (tf_numeric_binary(s->ctx,f->span,TF_RT_NUM_ADD,f->slots[0],f->args[1],&f->slots[1])!=TF_OK)
        return TF_FAULT;
    return tf_stack_return(s,f,1);
}
static tf_status fault_second(tf_stack *s,tf_stack_frame *f) {
    if (f->args[0].as.integer==2) return tf_context_fail(s->ctx,TF_R002,f->span);
    if (tf_retain(s->ctx,f->span,f->args[0],&f->slots[0])!=TF_OK) return TF_FAULT;
    return tf_stack_return(s,f,0);
}
static const tf_stack_body bodies[]={
    {tf_stack_map_resume,TF_RT_ORDINARY,{map_parameters,2},INTS,4},
    {tf_stack_fold_resume,TF_RT_ORDINARY,{fold_parameters,3},INT,4},
    {double_value,TF_RT_ORDINARY,{int_parameters,1},INT,1},
    {digits,TF_RT_ORDINARY,{pair_parameters,2},INT,2},
    {fault_second,TF_RT_PRELUDE_WRAPPER,{int_parameters,1},INT,1}
};
static const tf_stack_program program={bodies,5,0};

static size_t run(bool fail_test,size_t fail) {
    Counts counts={0};
    const tf_rt_config config={{allocate,deallocate,&counts},100000,32};
    tf_context ctx;
    tf_stack stack;
    tf_value list=tf_unit(),closure=tf_unit(),out=tf_unit();
    const tf_value items[]={tf_int(1),tf_int(2),tf_int(3)};
    CHECK(tf_context_init(&ctx,&values,&config)==TF_OK);
    CHECK(tf_list(&ctx,1,INTS,items,3,&list)==TF_OK);
    CHECK(tf_closure(&ctx,2,fail_test ? 5 : 3,NULL,0,&closure)==TF_OK);
    CHECK(tf_stack_init(&stack,&ctx,&program,32)==TF_OK);
    const size_t base=counts.calls;
    if (fail!=0) counts.fail=base+fail;
    const tf_value args[]={list,closure};
    const tf_status status=tf_stack_run(&stack,88,1,NULL,0,args,2,&out);
    const size_t allocation_points=counts.calls-base;
    if (fail_test || fail!=0) {
        CHECK(status==TF_FAULT && out.tag==TF_VALUE_UNIT);
        const tf_fault *fault=tf_stack_fault(&stack);
        CHECK(fault!=NULL && fault->code==(fail!=0 ? TF_R003 : TF_R002));
        if (fail==0) CHECK(fault->span==88 && fault->frame_count==2);
    } else {
        CHECK(status==TF_OK);
        size_t count;
        CHECK(tf_list_length(&ctx,0,out,&count)==TF_OK && count==3);
        for (size_t i=0;i<3;++i) {
            tf_value item=tf_unit();
            CHECK(tf_list_at(&ctx,0,out,i,&item)==TF_OK && item.as.integer==(int64_t)(i+1)*2);
            tf_release(&item);
        }
    }
    tf_release(&out); tf_release(&closure); tf_release(&list); tf_stack_destroy(&stack);
    CHECK(counts.live==0 && tf_context_live_bytes(&ctx)==0);
    return allocation_points;
}

int main(void) {
    const size_t failures=run(false,0);
    for (size_t i=1;i<=failures;++i) (void)run(false,i);
    (void)run(true,0);
    tf_context ctx;
    tf_stack stack;
    tf_value list=tf_unit(),closure=tf_unit(),out=tf_unit();
    CHECK(tf_context_init(&ctx,&values,NULL)==TF_OK);
    const tf_value items[]={tf_int(1),tf_int(2),tf_int(3)};
    CHECK(tf_list(&ctx,0,INTS,items,3,&list)==TF_OK);
    CHECK(tf_closure(&ctx,0,4,NULL,0,&closure)==TF_OK);
    CHECK(tf_stack_init(&stack,&ctx,&program,32)==TF_OK);
    const tf_value args[]={list,tf_int(0),closure};
    CHECK(tf_stack_run(&stack,99,2,NULL,0,args,3,&out)==TF_OK && out.as.integer==123);
    tf_release(&out); tf_release(&list); tf_release(&closure); tf_stack_destroy(&stack);
    CHECK(tf_context_live_bytes(&ctx)==0);

    CHECK(tf_list(&ctx,0,INTS,NULL,0,&list)==TF_OK);
    CHECK(tf_closure(&ctx,0,5,NULL,0,&closure)==TF_OK);
    CHECK(tf_stack_init(&stack,&ctx,&program,32)==TF_OK);
    const tf_value empty_map[]={list,closure};
    CHECK(tf_stack_run(&stack,100,1,NULL,0,empty_map,2,&out)==TF_OK);
    size_t count=1;
    CHECK(tf_list_length(&ctx,0,out,&count)==TF_OK && count==0);
    tf_release(&out); tf_release(&closure);
    CHECK(tf_closure(&ctx,0,4,NULL,0,&closure)==TF_OK);
    const tf_value empty_fold[]={list,tf_int(7),closure};
    CHECK(tf_stack_run(&stack,101,2,NULL,0,empty_fold,3,&out)==TF_OK && out.as.integer==7);
    tf_release(&out); tf_release(&list); tf_release(&closure); tf_stack_destroy(&stack);
    CHECK(tf_context_live_bytes(&ctx)==0 && tf_context_fault(&ctx)==NULL);
    printf("Native callbacks: %zu checks, %zu allocation failures\n",checks,failures);
    return 0;
}
