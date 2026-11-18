#include "ternfold_rt_calls.h"

#include <stdio.h>
#include <stdlib.h>

static size_t checks;
#define CHECK(ok) do { ++checks; if (!(ok)) { \
    fprintf(stderr,"test_runtime_calls:%d: %s\n",__LINE__,#ok); exit(1); \
} } while (0)

enum { UNIT=1, INT, TEXT, FN, TEXTS };
static const tf_type_id int_arg[]={INT}, text_list_arg[]={TEXTS};
static const tf_rt_type types[]={
    {.kind=TF_RT_TYPE_UNIT}, {.kind=TF_RT_TYPE_INT}, {.kind=TF_RT_TYPE_TEXT},
    {.kind=TF_RT_TYPE_FUNCTION}, {.kind=TF_RT_TYPE_LIST,.as.element=TEXT}
};
static const tf_rt_function funcs[]={
    {FN,{0}}, {FN,{int_arg,1}}, {FN,{0}}, {FN,{0}}, {FN,{0}}
};
static const tf_rt_program program={types,sizeof types/sizeof *types,funcs,sizeof funcs/sizeof *funcs};

static tf_status same(tf_context *ctx,const tf_value *captures,size_t capture_count,
                      const tf_value *args,size_t count,tf_value *out) {
    CHECK(captures==NULL && capture_count==0 && count==1);
    return tf_retain(ctx,tf_invocation_span(ctx),args[0],out);
}

static tf_status captured(tf_context *ctx,const tf_value *captures,size_t capture_count,
                          const tf_value *args,size_t count,tf_value *out) {
    CHECK(capture_count==1 && count==1 && args[0].tag==TF_VALUE_INT);
    return tf_retain(ctx,tf_invocation_span(ctx),captures[0],out);
}

static tf_status span_fault(tf_context *ctx,const tf_value *captures,size_t capture_count,
                            const tf_value *args,size_t count,tf_value *out) {
    (void)captures; (void)capture_count; (void)args; (void)count; (void)out;
    return tf_context_fail(ctx,TF_R002,tf_invocation_span(ctx));
}

static tf_status wrong_result(tf_context *ctx,const tf_value *captures,size_t capture_count,
                              const tf_value *args,size_t count,tf_value *out) {
    (void)captures; (void)capture_count; (void)args; (void)count;
    return tf_text(ctx,tf_invocation_span(ctx),TEXT,(const unsigned char *)"bad",3,out);
}

static const tf_rt_body bodies[]={
    {same,TF_RT_ORDINARY,{int_arg,1},INT},
    {captured,TF_RT_ORDINARY,{int_arg,1},INT},
    {same,TF_RT_ENTRY_ONLY,{text_list_arg,1},TEXTS},
    {span_fault,TF_RT_PRELUDE_WRAPPER,{int_arg,1},INT},
    {wrong_result,TF_RT_ORDINARY,{0},INT}
};
static const tf_rt_dispatch table={bodies,sizeof bodies/sizeof *bodies,3};

int main(void) {
    tf_context ctx;
    const tf_value arg=tf_int(7), capture=tf_int(13);
    tf_value closure=tf_unit(), out=tf_unit(), snapshot=tf_unit();
    CHECK(tf_context_init(&ctx,&program,NULL)==TF_OK);
    CHECK(tf_call_static(&ctx,&table,17,1,&arg,1,&out)==TF_OK && out.as.integer==7);
    CHECK(tf_invocation_span(&ctx)==0);
    CHECK(tf_closure(&ctx,19,2,&capture,1,&closure)==TF_OK);
    CHECK(tf_call_closure(&ctx,&table,20,closure,&arg,1,&out)==TF_OK && out.as.integer==13);
    tf_release(&closure);
    CHECK(tf_list(&ctx,22,TEXTS,NULL,0,&snapshot)==TF_OK);
    CHECK(tf_call_entry(&ctx,&table,&snapshot,1,&out)==TF_OK);
    tf_release(&snapshot);
    size_t count=1;
    CHECK(tf_list_length(&ctx,0,out,&count)==TF_OK && count==0);
    tf_release(&out);
    CHECK(tf_context_live_bytes(&ctx)==0);

    CHECK(tf_closure(&ctx,0,4,NULL,0,&closure)==TF_OK);
    CHECK(tf_call_closure(&ctx,&table,33,closure,&arg,1,&out)==TF_FAULT && out.tag==TF_VALUE_UNIT);
    const tf_fault *fault=tf_context_fault(&ctx);
    CHECK(fault->code==TF_R002 && fault->span==33 && fault->frame_count==1 &&
          fault->frames[0].function==4 && fault->frames[0].call_span==33 && tf_invocation_span(&ctx)==0);
    tf_release(&closure);
    CHECK(tf_context_live_bytes(&ctx)==0);

    CHECK(tf_context_init(&ctx,&program,NULL)==TF_OK);
    CHECK(tf_call_static(&ctx,&table,44,3,&arg,1,&out)==TF_FAULT);
    fault=tf_context_fault(&ctx);
    CHECK(fault->code==TF_R004 && fault->span==44 && fault->frame_count==0);

    CHECK(tf_context_init(&ctx,&program,NULL)==TF_OK);
    CHECK(tf_call_static(&ctx,&table,55,5,NULL,0,&out)==TF_FAULT && out.tag==TF_VALUE_UNIT);
    fault=tf_context_fault(&ctx);
    CHECK(fault->code==TF_R004 && fault->span==55 && fault->frame_count==1 && tf_context_live_bytes(&ctx)==0);
    printf("Native call dispatch: %zu checks passed\n",checks);
    return 0;
}
