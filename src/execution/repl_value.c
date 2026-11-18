#include "repl_value.h"
#include "scalars.h"
#include "values.h"
#include "../internal.h"

#include <string.h>

enum { MAX_DEPTH=6, MAX_NODES=128, MAX_ITEMS=12, MAX_TEXT=96, MAX_NAME=64 };
typedef struct {
    TfReplValue *out;
    const TfCoreProgramData *program;
    size_t nodes;
    bool full;
} Render;

static void append(Render *r, const char *bytes, size_t length) {
    static const char omitted[]="...";
    if (r->full) return;
    TfReplValue *out=r->out;
    /* Reserve the final ellipsis and terminator. Each append is atomic so a
     * byte cap never cuts a scalar escape in half. */
    if (length>sizeof out->bytes-sizeof omitted-out->length) {
        memcpy(out->bytes+out->length,omitted,sizeof omitted-1);
        out->length+=sizeof omitted-1;
        r->full=true;
    } else {
        memcpy(out->bytes+out->length,bytes,length);
        out->length+=length;
    }
}

static void token(Render *r, const char *s) { append(r,s,strlen(s)); }

static bool name_char(uint32_t scalar) {
    return (scalar>='A' && scalar<='Z') || (scalar>='a' && scalar<='z') ||
           (scalar>='0' && scalar<='9') || scalar=='_';
}

/* Display ASCII only, so source bytes cannot add lines, terminal controls,
 * or invisible directional formatting to a transcript. */
static void escaped(Render *r, uint32_t scalar, char quote) {
    if (quote!='\0' && (scalar==(uint32_t)(unsigned char)quote || scalar=='\\')) {
        const char pair[]={'\\',(char)scalar};
        append(r,pair,sizeof pair);
    } else if (quote!='\0' && scalar=='\n') token(r,"\\n");
    else if (quote!='\0' && scalar=='\r') token(r,"\\r");
    else if (quote!='\0' && scalar=='\t') token(r,"\\t");
    else if ((quote=='\0' && name_char(scalar)) ||
             (quote!='\0' && scalar>=0x20 && scalar<=0x7e)) {
        const char ch=(char)scalar;
        append(r,&ch,1);
    } else {
        static const char hex[]="0123456789abcdef";
        char buffer[12]={'\\','u','{'};
        size_t width=1;
        for (uint32_t rest=scalar>>4; rest!=0; rest>>=4) ++width;
        for (size_t i=width; i!=0; --i) {
            buffer[2+i]=hex[scalar&15u];
            scalar>>=4;
        }
        buffer[3+width]='}';
        append(r,buffer,4+width);
    }
}

static void string(Render *r, TfCoreText text, char quote, size_t budget) {
    size_t offset=0;
    while (offset<text.length && budget!=0 && !r->full) {
        uint32_t scalar;
        if (!tf_decode(text.bytes,text.length,offset,&scalar,&offset)) break;
        escaped(r,scalar,quote);
        --budget;
    }
    if (offset<text.length) token(r,"...");
}

static void name(Render *r, TfCoreText text, const char *fallback) {
    if (text.length==0) token(r,fallback);
    else string(r,text,'\0',MAX_NAME);
}

static void value(Render *r, const TfExecValue *v, size_t depth);

static void items(Render *r, TfExecValues values, const TfCoreField *fields, size_t depth) {
    size_t i=0;
    for (; i<values.count && i<MAX_ITEMS && r->nodes!=0 && !r->full; ++i) {
        if (i!=0) token(r,", ");
        if (fields!=NULL) {
            name(r,fields[i].name,"_");
            token(r,": ");
        }
        value(r,&values.items[i],depth);
    }
    if (i<values.count) token(r,i==0 ? "..." : ", ...");
}

static void variant(Render *r, const TfCoreType *type, const TfExecValue *v, size_t depth) {
    const size_t tag=v->as.aggregate.identity;
    const TfExecValues payload=v->as.aggregate.values;
    if (type->kind==TF_CORE_TYPE_ENUM) {
        name(r,type->name,"<enum>");
        token(r,"::");
        name(r,type->as.enumeration.variants[tag].name,"<variant>");
    } else if (type->kind==TF_CORE_TYPE_OPTION) {
        token(r,tag==TF_CORE_OPTION_SOME ? "Some" : "None");
    } else token(r,tag==TF_CORE_DECISION_ACCEPT ? "Accept" : "Reject");
    if (payload.count!=0) {
        token(r,"(");
        items(r,payload,NULL,depth+1);
        token(r,")");
    }
}

static void value(Render *r, const TfExecValue *v, size_t depth) {
    if (r->full) return;
    --r->nodes;
    if (depth==MAX_DEPTH) { token(r,"..."); return; }
    const TfCoreType *type=tf_core_type(r->program,v->type);
    switch (type->kind) {
    case TF_CORE_TYPE_INT: {
        char bytes[TF_EXEC_INT_TEXT_CAPACITY];
        const size_t length=tf_exec_int_text(v->as.integer,bytes);
        append(r,bytes,length);
        break;
    }
    case TF_CORE_TYPE_FLOAT: {
        char bytes[TF_EXEC_FLOAT_TEXT_CAPACITY];
        const size_t length=tf_exec_float_text(v->as.bits,bytes);
        append(r,bytes,length);
        break;
    }
    case TF_CORE_TYPE_BOOL: token(r,v->as.bits!=0 ? "true" : "false"); break;
    case TF_CORE_TYPE_TEXT:
        token(r,"\"");
        string(r,v->as.text,'"',MAX_TEXT);
        token(r,"\"");
        break;
    case TF_CORE_TYPE_CHAR:
        token(r,"'");
        escaped(r,(uint32_t)v->as.bits,'\'');
        token(r,"'");
        break;
    case TF_CORE_TYPE_UNIT: token(r,"()"); break;
    case TF_CORE_TYPE_FUNCTION: token(r,"<closure>"); break;
    case TF_CORE_TYPE_LIST:
        token(r,"[");
        items(r,v->as.aggregate.values,NULL,depth+1);
        token(r,"]");
        break;
    case TF_CORE_TYPE_RECORD: case TF_CORE_TYPE_MACHINE:
        name(r,type->name,type->kind==TF_CORE_TYPE_RECORD ? "<record>" : "<machine>");
        token(r," {");
        items(r,v->as.aggregate.values,type->kind==TF_CORE_TYPE_RECORD ?
              type->as.record.fields : type->as.machine.cells,depth+1);
        token(r,"}");
        break;
    case TF_CORE_TYPE_ENUM: case TF_CORE_TYPE_OPTION: case TF_CORE_TYPE_DECISION:
        variant(r,type,v,depth);
        break;
    case TF_CORE_TYPE_TAG: token(r,"<tag>"); break;
    }
}

void tf_repl_value_render(TfReplValue *out, const TfCoreProgramData *program,
                          const TfExecValue *result) {
    Render r={.out=out,.program=program,.nodes=MAX_NODES};
    out->length=0;
    value(&r,result,0);
    out->bytes[out->length]='\0';
}
