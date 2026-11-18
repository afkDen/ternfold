#include "ternfold_rt_io.h"

#include <string.h>

typedef struct { const unsigned char *bytes; size_t length; } Line;

static void *scratch(tf_context *ctx,size_t bytes) {
    if (bytes>ctx->config.byte_budget-ctx->live_bytes) return NULL;
    void *p=ctx->config.allocator.allocate(ctx->config.allocator.user,bytes);
    if (p!=NULL) ctx->live_bytes+=bytes;
    return p;
}
static void unreserve(tf_context *ctx,void *p,size_t bytes) {
    if (p!=NULL) {
        ctx->live_bytes-=bytes;
        ctx->config.allocator.deallocate(ctx->config.allocator.user,p);
    }
}

static bool valid_utf8(const unsigned char *bytes,size_t length) {
    for (size_t i=0;i<length;) {
        unsigned char lead=bytes[i++];
        if (lead<0x80) continue;
        size_t tail;
        uint32_t scalar,minimum;
        if (lead>=0xc2 && lead<=0xdf) { tail=1; scalar=lead&0x1fu; minimum=0x80; }
        else if (lead>=0xe0 && lead<=0xef) { tail=2; scalar=lead&0x0fu; minimum=0x800; }
        else if (lead>=0xf0 && lead<=0xf4) { tail=3; scalar=lead&0x07u; minimum=0x10000; }
        else return false;
        if (tail>length-i) return false;
        while (tail--!=0) {
            const unsigned char next=bytes[i++];
            if ((next&0xc0u)!=0x80u) return false;
            scalar=(scalar<<6)|(next&0x3fu);
        }
        if (scalar<minimum || scalar>0x10ffffu || (scalar>=0xd800u && scalar<=0xdfffu)) return false;
    }
    return true;
}

tf_io_status tf_input_capture(tf_context *ctx,tf_type_id text_list,FILE *stream,tf_value *out) {
    *out=tf_unit();
    if (tf_context_fault(ctx)!=NULL) return TF_IO_FAULT;
    if (stream==NULL) return TF_IO_HOST_ERROR;
    if (text_list==0 || text_list>ctx->program.type_count ||
        ctx->program.types[text_list-1].kind!=TF_RT_TYPE_LIST) {
        (void)tf_context_fail(ctx,TF_R004,0); return TF_IO_FAULT;
    }
    const tf_type_id text=ctx->program.types[text_list-1].as.element;
    if (text==0 || text>ctx->program.type_count || ctx->program.types[text-1].kind!=TF_RT_TYPE_TEXT) {
        (void)tf_context_fail(ctx,TF_R004,0); return TF_IO_FAULT;
    }
    unsigned char *bytes=NULL;
    tf_value *values=NULL;
    size_t length=0,capacity=0,count=0,made=0;
    tf_io_status status=TF_IO_HOST_ERROR;
    unsigned char chunk[4096];
    for (;;) {
        const size_t received=fread(chunk,1,sizeof chunk,stream);
        if (received>SIZE_MAX-length || length+received>INT64_MAX) goto resource;
        const size_t needed=length+received;
        if (needed>capacity) {
            size_t next=needed;
            if (capacity<=SIZE_MAX/2 && capacity*2>needed) next=capacity*2;
            if (next>ctx->config.byte_budget-ctx->live_bytes) next=needed;
            unsigned char *replacement=scratch(ctx,next);
            if (replacement==NULL) goto resource;
            if (length!=0) memcpy(replacement,bytes,length);
            unreserve(ctx,bytes,capacity);
            bytes=replacement; capacity=next;
        }
        if (received!=0) memcpy(bytes+length,chunk,received);
        length=needed;
        if (received<sizeof chunk) {
            if (ferror(stream) || !feof(stream)) goto done;
            break;
        }
    }
    if (!valid_utf8(bytes,length)) { status=TF_IO_INVALID_UTF8; goto done; }
    for (size_t i=0;i<length;++i) if (bytes[i]=='\n') ++count;
    if (length!=0 && bytes[length-1]!='\n') ++count;
    if (count>SIZE_MAX/sizeof *values) goto resource;
    if (count!=0) {
        values=scratch(ctx,count*sizeof *values);
        if (values==NULL) goto resource;
    }
    size_t start=0;
    for (size_t i=0;i<length;++i) {
        if (bytes[i]!='\n') continue;
        const size_t end=i>start && bytes[i-1]=='\r' ? i-1 : i;
        if (tf_text(ctx,0,text,bytes+start,end-start,&values[made])!=TF_OK) { status=TF_IO_FAULT; goto done; }
        ++made; start=i+1;
    }
    if (start<length) {
        if (tf_text(ctx,0,text,bytes+start,length-start,&values[made])!=TF_OK) { status=TF_IO_FAULT; goto done; }
        ++made;
    }
    status=tf_list(ctx,0,text_list,values,count,out)==TF_OK ? TF_IO_OK : TF_IO_FAULT;
    goto done;
resource:
    (void)tf_context_fail(ctx,TF_R003,0); status=TF_IO_FAULT;
done:
    while (made!=0) tf_release(&values[--made]);
    unreserve(ctx,values,count*sizeof *values);
    unreserve(ctx,bytes,capacity);
    return status;
}

tf_io_status tf_output_publish(tf_context *ctx,FILE *stream,tf_value lines) {
    if (tf_context_fault(ctx)!=NULL) return TF_IO_FAULT;
    if (stream==NULL) return TF_IO_HOST_ERROR;
    size_t count;
    if (tf_list_length(ctx,0,lines,&count)!=TF_OK) return TF_IO_FAULT;
    if (count>SIZE_MAX/sizeof(Line)) { (void)tf_context_fail(ctx,TF_R003,0); return TF_IO_FAULT; }
    Line *views=count!=0 ? scratch(ctx,count*sizeof *views) : NULL;
    if (count!=0 && views==NULL) { (void)tf_context_fail(ctx,TF_R003,0); return TF_IO_FAULT; }
    tf_io_status status=TF_IO_FAULT;
    for (size_t i=0;i<count;++i) {
        tf_value line=tf_unit();
        if (tf_list_at(ctx,0,lines,i,&line)!=TF_OK) goto done;
        const tf_status view=tf_text_view(ctx,0,line,&views[i].bytes,&views[i].length);
        tf_release(&line);
        if (view!=TF_OK) goto done;
    }
    status=TF_IO_HOST_ERROR;
    for (size_t i=0;i<count;++i)
        if ((views[i].length!=0 && fwrite(views[i].bytes,1,views[i].length,stream)!=views[i].length) ||
            fputc('\n',stream)==EOF) goto done;
    status=ferror(stream) ? TF_IO_HOST_ERROR : TF_IO_OK;
done:
    unreserve(ctx,views,count*sizeof *views);
    return status;
}

tf_status tf_entry_input(tf_context *ctx,tf_span_id span,tf_value snapshot,size_t index,tf_value *out) {
    *out=tf_unit();
    if (tf_context_fault(ctx)!=NULL) return TF_FAULT;
    const tf_type_id type=tf_value_type(ctx,snapshot);
    if (type==0 || ctx->program.types[type-1].kind!=TF_RT_TYPE_LIST)
        return tf_context_fail(ctx,TF_R004,span);
    const tf_type_id element=ctx->program.types[type-1].as.element;
    if (element==0 || element>ctx->program.type_count || ctx->program.types[element-1].kind!=TF_RT_TYPE_TEXT)
        return tf_context_fail(ctx,TF_R004,span);
    size_t count;
    if (tf_list_length(ctx,span,snapshot,&count)!=TF_OK) return TF_FAULT;
    if (index>=count) return tf_context_fail(ctx,TF_R006,span);
    return tf_list_at(ctx,span,snapshot,index,out);
}
