#include "ternfold/trace_value.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct { TfTracePreview *out; TfTraceValueAdapter adapter; size_t nodes; } Render;
static void append(Render *r, const char *text, size_t length) {
    if (r->out->incomplete) return;
    if (length>sizeof r->out->bytes-r->out->length) r->out->incomplete=true;
    else {
        memcpy(r->out->bytes+r->out->length,text,length);
        r->out->length+=length;
    }
}
static void literal(Render *r, const char *text) { append(r,text,strlen(text)); }
static void value(Render *r, const void *input, size_t depth) {
    if (r->out->incomplete) return;
    if (depth>4 || r->nodes==0) { r->out->incomplete=true; return; }
    --r->nodes;
    const TfTraceValueView v=r->adapter.view(r->adapter.context,input);
    char text[48]; int length=0;
    switch (v.kind) {
    case TF_TRACE_VALUE_UNIT: literal(r,"()"); return;
    case TF_TRACE_VALUE_BOOL: literal(r,v.bits ? "true" : "false"); return;
    case TF_TRACE_VALUE_INT: {
        int64_t integer;
        memcpy(&integer,&v.bits,sizeof integer);
        length=snprintf(text,sizeof text,"i:%" PRId64,integer); break;
    }
    case TF_TRACE_VALUE_FLOAT: length=snprintf(text,sizeof text,"f:%016" PRIx64,v.bits); break;
    case TF_TRACE_VALUE_CHAR: length=snprintf(text,sizeof text,"c:%" PRIx64,v.bits); break;
    case TF_TRACE_VALUE_TAG: length=snprintf(text,sizeof text,"tag:%zu",v.tag); break;
    case TF_TRACE_VALUE_FUNCTION: literal(r,"fn"); return;
    case TF_TRACE_VALUE_TEXT: {
        static const char hex[]="0123456789abcdef";
        literal(r,"x:");
        for (size_t i=0;i<v.count && !r->out->incomplete;++i) {
            const char pair[]={hex[v.bytes[i]>>4],hex[v.bytes[i]&15u]};
            append(r,pair,2);
        }
        return;
    }
    case TF_TRACE_VALUE_LIST: case TF_TRACE_VALUE_RECORD: case TF_TRACE_VALUE_MACHINE:
    case TF_TRACE_VALUE_VARIANT:
        if (v.kind==TF_TRACE_VALUE_VARIANT) {
            length=snprintf(text,sizeof text,"v:%zu",v.tag);
            if (length>0 && (size_t)length<sizeof text) append(r,text,(size_t)length);
        } else literal(r,v.kind==TF_TRACE_VALUE_LIST ? "l" : v.kind==TF_TRACE_VALUE_RECORD ? "r" : "m");
        literal(r,"[");
        for (size_t i=0;i<v.count && !r->out->incomplete;++i) {
            if (i) literal(r,",");
            value(r,r->adapter.child(r->adapter.context,input,i),depth+1);
        }
        literal(r,"]"); return;
    }
    if (length>0 && (size_t)length<sizeof text) append(r,text,(size_t)length);
    else r->out->incomplete=true;
}
void tf_trace_preview(TfTracePreview *out, TfTraceValueAdapter adapter, const void *input) {
    if (!out) return;
    *out=(TfTracePreview){0};
    if (!adapter.view || !adapter.child || !input) { out->incomplete=true; return; }
    Render r={out,adapter,32};
    value(&r,input,0);
}
void tf_trace_preview_pair(TfTracePreview *out, TfTraceValueAdapter adapter,
                           const void *old, const void *input) {
    if (!out) return;
    *out=(TfTracePreview){0};
    if (!adapter.view || !adapter.child || !old || !input) { out->incomplete=true; return; }
    Render r={out,adapter,32};
    literal(&r,"old="); value(&r,old,0); literal(&r,";input="); value(&r,input,0);
}
