#include "ternfold_rt_calls.h"

typedef enum { STATIC_CALL, CLOSURE_CALL, HOST_CALL } DispatchKind;

static bool signature(tf_context *ctx, tf_rt_shape expected, const tf_value *values, size_t count) {
    if (expected.count!=count || count>SIZE_MAX/sizeof *values ||
        count>SIZE_MAX/sizeof *expected.types || (count!=0 && (expected.types==NULL || values==NULL))) return false;
    for (size_t i=0; i<count; ++i)
        if (expected.types[i]==0 || tf_value_type(ctx,values[i])!=expected.types[i]) return false;
    return true;
}

static tf_status invoke(tf_context *ctx, const tf_rt_dispatch *table, tf_span_id span,
                        DispatchKind kind, tf_function_id target, const tf_value *captures, size_t captures_count,
                        const tf_value *args, size_t count, tf_value *out) {
    *out=tf_unit();
    if (tf_context_fault(ctx)!=NULL) return TF_FAULT;
    if (table==NULL || table->bodies==NULL || table->count!=ctx->program.function_count ||
        table->count>SIZE_MAX/sizeof *table->bodies || target==0 || target>table->count)
        return tf_context_fail(ctx,TF_R004,span);
    const tf_rt_body *body=&table->bodies[target-1];
    const tf_rt_function *function=&ctx->program.functions[target-1];
    if (body->code==NULL || body->kind>TF_RT_PRELUDE_WRAPPER ||
        (kind==STATIC_CALL && body->kind!=TF_RT_ORDINARY) ||
        (kind==CLOSURE_CALL && body->kind==TF_RT_ENTRY_ONLY) ||
        (kind==HOST_CALL && body->kind==TF_RT_PRELUDE_WRAPPER) ||
        body->result==0 || !signature(ctx,body->parameters,args,count) ||
        !signature(ctx,function->captures,captures,captures_count))
        return tf_context_fail(ctx,TF_R004,span);
    if (tf_context_enter(ctx,target,span)!=TF_OK) return TF_FAULT;
    const tf_status status=body->code(ctx,captures,captures_count,args,count,out);
    bool success=status==TF_OK && tf_context_fault(ctx)==NULL && tf_value_type(ctx,*out)==body->result;
    if (!success) {
        tf_release(out);
        (void)tf_context_fail(ctx,TF_R004,span);
    }
    tf_context_leave(ctx);
    return success ? TF_OK : TF_FAULT;
}

tf_status tf_call_static(tf_context *ctx, const tf_rt_dispatch *table, tf_span_id span,
                        tf_function_id target, const tf_value *args, size_t count, tf_value *out) {
    return invoke(ctx,table,span,STATIC_CALL,target,NULL,0,args,count,out);
}

tf_status tf_call_closure(tf_context *ctx, const tf_rt_dispatch *table, tf_span_id span,
                         tf_value closure, const tf_value *args, size_t count, tf_value *out) {
    tf_function_id target;
    const tf_value *captures;
    size_t captures_count;
    *out=tf_unit();
    if (tf_closure_view(ctx,span,closure,&target,&captures,&captures_count)!=TF_OK) return TF_FAULT;
    return invoke(ctx,table,span,CLOSURE_CALL,target,captures,captures_count,args,count,out);
}

tf_status tf_call_entry(tf_context *ctx, const tf_rt_dispatch *table,
                       const tf_value *args, size_t count, tf_value *out) {
    return invoke(ctx,table,0,HOST_CALL,table!=NULL ? table->entry : 0,NULL,0,args,count,out);
}

tf_span_id tf_invocation_span(const tf_context *ctx) {
    return ctx->depth!=0 ? ctx->frames[ctx->depth-1].call_span : 0;
}
