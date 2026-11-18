#include "ternfold_rt_callbacks.h"

typedef struct { size_t count, done, bytes; tf_value values[]; } Mapped;

static void release_mapped(tf_stack *s,tf_stack_frame *f) {
    Mapped *mapped=f->auxiliary;
    if (mapped==NULL) return;
    while (mapped->done!=0) tf_release(&mapped->values[--mapped->done]);
    s->ctx->live_bytes-=mapped->bytes;
    s->ctx->config.allocator.deallocate(s->ctx->config.allocator.user,mapped);
    f->auxiliary=NULL; f->cleanup=NULL;
}

static tf_status prepare_map(tf_stack *s,tf_stack_frame *f,size_t count) {
    if (count>(SIZE_MAX-sizeof(Mapped))/sizeof(tf_value)) return tf_context_fail(s->ctx,TF_R003,f->span);
    const size_t bytes=sizeof(Mapped)+count*sizeof(tf_value);
    tf_context *ctx=s->ctx;
    if (bytes>ctx->config.byte_budget-ctx->live_bytes) return tf_context_fail(ctx,TF_R003,f->span);
    Mapped *mapped=ctx->config.allocator.allocate(ctx->config.allocator.user,bytes);
    if (mapped==NULL) return tf_context_fail(ctx,TF_R003,f->span);
    *mapped=(Mapped){count,0,bytes};
    ctx->live_bytes+=bytes;
    f->auxiliary=mapped; f->cleanup=release_mapped;
    return TF_OK;
}

static tf_status callback(tf_stack *s,tf_stack_frame *f,bool fold) {
    tf_context *ctx=s->ctx;
    if (f->slot_count<4 || f->argc!=(fold ? 3u : 2u) || f->capture_count!=0)
        return tf_context_fail(ctx,TF_R004,f->span);
    size_t count;
    if (tf_list_length(ctx,f->span,f->args[0],&count)!=TF_OK) return TF_FAULT;
    const tf_type_id result=s->program->bodies[f->function-1].result;
    if (f->pc==0) {
        if (fold) {
            if (tf_retain(ctx,f->span,f->args[1],&f->slots[0])!=TF_OK) return TF_FAULT;
        } else if (count!=0 && prepare_map(s,f,count)!=TF_OK) return TF_FAULT;
        f->slots[2]=tf_int(0);
    } else if (f->pc==1) {
        if (fold) {
            tf_release(&f->slots[0]);
            f->slots[0]=f->slots[1]; f->slots[1]=tf_unit();
        } else {
            Mapped *mapped=f->auxiliary;
            if (mapped==NULL || mapped->done==mapped->count) return tf_context_fail(ctx,TF_R004,f->span);
            mapped->values[mapped->done++]=f->slots[1]; f->slots[1]=tf_unit();
        }
        tf_release(&f->slots[3]);
        if (f->slots[2].as.integer==INT64_MAX) return tf_context_fail(ctx,TF_R003,f->span);
        ++f->slots[2].as.integer;
    } else return tf_context_fail(ctx,TF_R004,f->span);

    const uint64_t index=(uint64_t)f->slots[2].as.integer;
    if (index==count) {
        if (!fold) {
            const Mapped *mapped=f->auxiliary;
            if (tf_list(ctx,f->span,result,mapped!=NULL ? mapped->values : NULL,count,&f->slots[0])!=TF_OK)
                return TF_FAULT;
        }
        return tf_stack_return(s,f,0);
    }
    if (index>count) return tf_context_fail(ctx,TF_R004,f->span);
    if (tf_list_at(ctx,f->span,f->args[0],(size_t)index,&f->slots[3])!=TF_OK) return TF_FAULT;
    const tf_value args[]={f->slots[0],f->slots[3]};
    return tf_stack_call_closure(s,f,f->span,f->args[fold ? 2 : 1],fold ? args : &f->slots[3],
                                 fold ? 2u : 1u,1,1);
}

tf_status tf_stack_map_resume(tf_stack *s,tf_stack_frame *f) { return callback(s,f,false); }
tf_status tf_stack_fold_resume(tf_stack *s,tf_stack_frame *f) { return callback(s,f,true); }

typedef struct Loop Loop;
struct Loop { Loop *parent; size_t count,used,bytes; tf_value items[]; };

static void drop_loop(tf_stack *s,tf_stack_frame *f) {
    Loop *loop=f->auxiliary;
    f->auxiliary=loop->parent;
    while (loop->used!=0) tf_release(&loop->items[--loop->used]);
    s->ctx->live_bytes-=loop->bytes;
    s->ctx->config.allocator.deallocate(s->ctx->config.allocator.user,loop);
}

static void drop_loops(tf_stack *s,tf_stack_frame *f) {
    while (f->auxiliary!=NULL) drop_loop(s,f);
    f->cleanup=NULL;
}

tf_status tf_loop_begin(tf_stack *s,tf_stack_frame *f,tf_span_id span,tf_value list) {
    tf_context *ctx=s->ctx;
    size_t count;
    if (f->cleanup!=NULL && f->cleanup!=drop_loops) return tf_context_fail(ctx,TF_R004,span);
    if (tf_list_length(ctx,span,list,&count)!=TF_OK) return TF_FAULT;
    if (count>(SIZE_MAX-sizeof(Loop))/sizeof(tf_value)) return tf_context_fail(ctx,TF_R003,span);
    const size_t bytes=sizeof(Loop)+count*sizeof(tf_value);
    if (bytes>ctx->config.byte_budget-ctx->live_bytes) return tf_context_fail(ctx,TF_R003,span);
    Loop *loop=ctx->config.allocator.allocate(ctx->config.allocator.user,bytes);
    if (loop==NULL) return tf_context_fail(ctx,TF_R003,span);
    *loop=(Loop){f->auxiliary,count,0,bytes};
    ctx->live_bytes+=bytes;
    f->auxiliary=loop; f->cleanup=drop_loops;
    return TF_OK;
}

tf_status tf_loop_element(tf_stack *s,tf_stack_frame *f,tf_span_id span,tf_value list,
                           tf_value *out,bool *done) {
    *out=tf_unit(); *done=false;
    if (tf_context_fault(s->ctx)!=NULL) return TF_FAULT;
    const Loop *loop=f->auxiliary;
    if (loop==NULL || f->cleanup!=drop_loops) return tf_context_fail(s->ctx,TF_R004,span);
    *done=loop->used==loop->count;
    return *done ? TF_OK : tf_list_at(s->ctx,span,list,loop->used,out);
}

tf_status tf_loop_keep(tf_stack *s,tf_stack_frame *f,tf_span_id span,tf_value value) {
    Loop *loop=f->auxiliary;
    if (loop==NULL || f->cleanup!=drop_loops || loop->used==loop->count)
        return tf_context_fail(s->ctx,TF_R004,span);
    if (tf_retain(s->ctx,span,value,&loop->items[loop->used])!=TF_OK) return TF_FAULT;
    ++loop->used;
    return TF_OK;
}

tf_status tf_loop_finish(tf_stack *s,tf_stack_frame *f,tf_span_id span,tf_type_id result,tf_value *out) {
    *out=tf_unit();
    const Loop *loop=f->auxiliary;
    if (loop==NULL || f->cleanup!=drop_loops || loop->used!=loop->count)
        return tf_context_fail(s->ctx,TF_R004,span);
    if (tf_list(s->ctx,span,result,loop->items,loop->used,out)!=TF_OK) return TF_FAULT;
    drop_loop(s,f);
    if (f->auxiliary==NULL) f->cleanup=NULL;
    return TF_OK;
}
