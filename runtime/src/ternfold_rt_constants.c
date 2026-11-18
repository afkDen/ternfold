#include "ternfold_rt_constants.h"

struct tf_constant_pool {
    tf_context *context;
    size_t count, bytes;
    tf_value values[];
};

static void *scratch(tf_context *ctx, tf_span_id span, size_t count, size_t width) {
    if (count==0) return NULL;
    if (count>SIZE_MAX/width || count*width>ctx->config.byte_budget-ctx->live_bytes) {
        (void)tf_context_fail(ctx,TF_R003,span); return NULL;
    }
    void *ptr=ctx->config.allocator.allocate(ctx->config.allocator.user,count*width);
    if (ptr==NULL) { (void)tf_context_fail(ctx,TF_R003,span); return NULL; }
    ctx->live_bytes+=count*width;
    return ptr;
}

static void release_scratch(tf_context *ctx, void *ptr, size_t bytes) {
    if (ptr==NULL) return;
    ctx->live_bytes-=bytes;
    ctx->config.allocator.deallocate(ctx->config.allocator.user,ptr);
}

void tf_constants_destroy(tf_constant_pool *pool) {
    if (pool==NULL) return;
    tf_context *ctx=pool->context;
    while (pool->count!=0) tf_release(&pool->values[--pool->count]);
    release_scratch(ctx,pool,pool->bytes);
}

tf_status tf_constants_create(tf_context *ctx, const tf_rt_constant *descriptors,
                              size_t count, tf_constant_pool **out) {
    *out=NULL;
    if (tf_context_fault(ctx)!=NULL) return TF_FAULT;
    if ((count!=0 && descriptors==NULL) || count>SIZE_MAX/sizeof *descriptors)
        return tf_context_fail(ctx,TF_R004,0);
    if (count>(SIZE_MAX-sizeof(tf_constant_pool))/sizeof(tf_value)) return tf_context_fail(ctx,TF_R003,0);
    const size_t bytes=sizeof(tf_constant_pool)+count*sizeof(tf_value);
    tf_constant_pool *pool=scratch(ctx,0,bytes,1);
    if (pool==NULL) return TF_FAULT;
    pool->context=ctx; pool->count=0; pool->bytes=bytes;
    for (size_t i=0; i<count; ++i) {
        const tf_rt_constant *desc=&descriptors[i];
        tf_value value=tf_unit();
        tf_status status=TF_OK;
        switch (desc->kind) {
        case TF_RT_CONST_UNIT: break;
        case TF_RT_CONST_INT: value=tf_int(desc->as.integer); break;
        case TF_RT_CONST_BOOL:
            if (desc->as.bits>1) status=tf_context_fail(ctx,TF_R004,desc->span);
            else value=tf_bool(desc->as.bits!=0);
            break;
        case TF_RT_CONST_CHAR:
            if (desc->as.bits>UINT32_MAX) status=tf_context_fail(ctx,TF_R004,desc->span);
            else status=tf_char(ctx,desc->span,(uint32_t)desc->as.bits,&value);
            break;
        case TF_RT_CONST_FLOAT: status=tf_float(ctx,desc->span,desc->as.bits,&value); break;
        case TF_RT_CONST_TEXT: status=tf_text(ctx,desc->span,desc->type,desc->as.text.bytes,desc->as.text.length,&value); break;
        case TF_RT_CONST_LIST: case TF_RT_CONST_RECORD: case TF_RT_CONST_VARIANT: {
            const size_t n=desc->as.children.count;
            if (n>SIZE_MAX/sizeof(size_t) || (n!=0 && desc->as.children.ids==NULL)) {
                status=tf_context_fail(ctx,TF_R004,desc->span); break;
            }
            for (size_t j=0; j<n; ++j)
                if (desc->as.children.ids[j]==0 || desc->as.children.ids[j]>i) {
                    status=tf_context_fail(ctx,TF_R004,desc->span); break;
                }
            if (status!=TF_OK) break;
            tf_value *children=scratch(ctx,desc->span,n,sizeof *children);
            if (n!=0 && children==NULL) { status=TF_FAULT; break; }
            for (size_t j=0; j<n; ++j) children[j]=pool->values[desc->as.children.ids[j]-1];
            if (desc->kind==TF_RT_CONST_LIST) status=tf_list(ctx,desc->span,desc->type,children,n,&value);
            else if (desc->kind==TF_RT_CONST_RECORD) status=tf_record(ctx,desc->span,desc->type,children,n,&value);
            else status=tf_variant(ctx,desc->span,desc->type,desc->as.children.tag,children,n,&value);
            release_scratch(ctx,children,n*sizeof *children);
            break;
        }
        default: status=tf_context_fail(ctx,TF_R004,desc->span); break;
        }
        if (status!=TF_OK || tf_value_type(ctx,value)!=desc->type || desc->type==0) {
            tf_release(&value);
            (void)tf_context_fail(ctx,TF_R004,desc->span);
            tf_constants_destroy(pool);
            return TF_FAULT;
        }
        pool->values[pool->count++]=value;
    }
    *out=pool;
    return TF_OK;
}

tf_status tf_constant_get(tf_constant_pool *pool, tf_span_id span, size_t id, tf_value *out) {
    *out=tf_unit();
    if (tf_context_fault(pool->context)!=NULL) return TF_FAULT;
    if (id==0 || id>pool->count) return tf_context_fail(pool->context,TF_R004,span);
    return tf_retain(pool->context,span,pool->values[id-1],out);
}
