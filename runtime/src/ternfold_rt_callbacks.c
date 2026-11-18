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
