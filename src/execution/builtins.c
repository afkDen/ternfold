#include "builtins.h"
#include "scalars.h"
#include "../internal.h"
#include "ternfold/checked_numeric.h"

#include <string.h>

static TfExecStatus bad(TfRuntimeCode *out, TfRuntimeCode code) { *out=code; return TF_EXEC_FAULT; }

static bool charge(uint64_t *steps, uint64_t cost) {
    if (cost>*steps) return false;
    *steps-=cost; return true;
}

static bool option(TfExecArena *arena, TfCoreTypeId type, const TfExecValue *item, TfExecValue *out) {
    TfExecValues payload={0};
    if (item!=NULL && !tf_exec_copy_values(arena,(TfExecValues){item,1},&payload)) return false;
    *out=(TfExecValue){.type=type,.as.aggregate={item!=NULL ? TF_CORE_OPTION_SOME : TF_CORE_OPTION_NONE,payload}};
    return true;
}

typedef enum { TEXT_FOUND, TEXT_MISSING, TEXT_BUDGET, TEXT_INVALID } TextStatus;

static TextStatus text_range(TfCoreText text, uint64_t low, uint64_t high, TfCoreText *out,
                             uint64_t *steps) {
    size_t start=0,end=0,offset=0;
    for (uint64_t i=0;;++i) {
        if (i==low) start=offset;
        if (i==high) { end=offset; break; }
        if (offset==text.length) return TEXT_MISSING;
        if (!charge(steps,1)) return TEXT_BUDGET;
        uint32_t scalar;
        if (!tf_decode(text.bytes,text.length,offset,&scalar,&offset)) return TEXT_INVALID;
    }
    *out=(TfCoreText){end==start ? NULL : text.bytes+start,end-start};
    return TEXT_FOUND;
}

TfExecStatus tf_exec_builtin(TfExecArena *arena, const TfCoreProgramData *d,
                             TfCoreBuiltin builtin, TfCoreTypeId result, const TfExecValue *args,
                             TfExecValue *out, TfRuntimeCode *fault, uint64_t *steps) {
    const TfExecValue a=args[0];
    TfExecValue value={.type=result};
    switch (builtin) {
    case TF_CORE_BUILTIN_LEN:
        value.as.integer=(int64_t)a.as.aggregate.values.count;
        break;
    case TF_CORE_BUILTIN_AT: {
        const int64_t index=args[1].as.integer;
        const TfExecValues list=a.as.aggregate.values;
        const TfExecValue *item=index>=0 && (uint64_t)index<list.count ? &list.items[(size_t)index] : NULL;
        if (!option(arena,result,item,&value)) return bad(fault,TF_RUNTIME_RESOURCE);
        break;
    }
    case TF_CORE_BUILTIN_APPEND: {
        const TfExecValues list=a.as.aggregate.values;
        if (list.count==SIZE_MAX || list.count>=INT64_MAX || !charge(steps,(uint64_t)list.count+1))
            return bad(fault,TF_RUNTIME_RESOURCE);
        TfExecValue *items=tf_exec_allocate(arena,list.count+1,sizeof *items);
        if (items==NULL) return bad(fault,TF_RUNTIME_RESOURCE);
        if (list.count!=0) memcpy(items,list.items,list.count*sizeof *items);
        items[list.count]=args[1];
        value.as.aggregate.identity=0;
        value.as.aggregate.values=(TfExecValues){items,list.count+1};
        break;
    }
    case TF_CORE_BUILTIN_RANGE: {
        const int64_t start=a.as.integer,end=args[1].as.integer;
        const uint64_t length=start<end ? (uint64_t)end-(uint64_t)start : 0;
        if (length>INT64_MAX || length>SIZE_MAX || !charge(steps,length)) return bad(fault,TF_RUNTIME_RESOURCE);
        TfExecValue *items=tf_exec_allocate(arena,(size_t)length,sizeof *items);
        if (length!=0 && items==NULL) return bad(fault,TF_RUNTIME_RESOURCE);
        const TfCoreTypeId element=tf_core_type(d,result)->as.element;
        for (size_t i=0; i<(size_t)length; ++i) items[i]=(TfExecValue){.type=element,.as.integer=start+(int64_t)i};
        value.as.aggregate.identity=0;
        value.as.aggregate.values=(TfExecValues){items,(size_t)length};
        break;
    }
    case TF_CORE_BUILTIN_INT_FLOAT: value.as.bits=tf_checked_int_to_float_bits(a.as.integer); break;
    case TF_CORE_BUILTIN_BOOL_TEXT:
        value.as.text=a.as.bits!=0 ? (TfCoreText){(const unsigned char *)"true",4} :
                                     (TfCoreText){(const unsigned char *)"false",5};
        break;
    case TF_CORE_BUILTIN_INT_TEXT: case TF_CORE_BUILTIN_FLOAT_TEXT: {
        char bytes[TF_EXEC_FLOAT_TEXT_CAPACITY];
        const size_t length=builtin==TF_CORE_BUILTIN_INT_TEXT ? tf_exec_int_text(a.as.integer,bytes) :
                                                              tf_exec_float_text(a.as.bits,bytes);
        if (length==0) return bad(fault,TF_RUNTIME_INVARIANT);
        if (!tf_exec_copy_text(arena,(TfCoreText){(const unsigned char *)bytes,length},&value.as.text))
            return bad(fault,TF_RUNTIME_RESOURCE);
        break;
    }
    case TF_CORE_BUILTIN_FLOAT_INT: case TF_CORE_BUILTIN_PARSE_INT: {
        int64_t converted=0;
        if (builtin==TF_CORE_BUILTIN_PARSE_INT && !charge(steps,(uint64_t)a.as.text.length))
            return bad(fault,TF_RUNTIME_RESOURCE);
        const bool have=builtin==TF_CORE_BUILTIN_FLOAT_INT ? tf_exec_float_int(a.as.bits,&converted) :
            tf_exec_parse_int(a.as.text.bytes,a.as.text.length,&converted);
        const TfExecValue item={.type=tf_core_type(d,result)->as.element,.as.integer=converted};
        if (!option(arena,result,have ? &item : NULL,&value)) return bad(fault,TF_RUNTIME_RESOURCE);
        break;
    }
    case TF_CORE_BUILTIN_TEXT_LEN: {
        int64_t length=0;
        for (size_t i=0,next; i<a.as.text.length; i=next) {
            uint32_t scalar;
            if (length==INT64_MAX || !charge(steps,1)) return bad(fault,TF_RUNTIME_RESOURCE);
            if (!tf_decode(a.as.text.bytes,a.as.text.length,i,&scalar,&next)) return bad(fault,TF_RUNTIME_INVARIANT);
            ++length;
        }
        value.as.integer=length;
        break;
    }
    case TF_CORE_BUILTIN_TEXT_AT: case TF_CORE_BUILTIN_TEXT_SLICE: {
        const int64_t first=args[1].as.integer;
        TfCoreText text={0};
        TextStatus status=TEXT_MISSING;
        if (first>=0) {
            if (builtin==TF_CORE_BUILTIN_TEXT_AT)
                status=text_range(a.as.text,(uint64_t)first,(uint64_t)first+1,&text,steps);
            else if (args[2].as.integer>=first)
                status=text_range(a.as.text,(uint64_t)first,(uint64_t)args[2].as.integer,&text,steps);
        }
        if (status==TEXT_BUDGET) return bad(fault,TF_RUNTIME_RESOURCE);
        if (status==TEXT_INVALID) return bad(fault,TF_RUNTIME_INVARIANT);
        const TfExecValue item={.type=tf_core_type(d,result)->as.element,.as.text=text};
        if (!option(arena,result,status==TEXT_FOUND ? &item : NULL,&value)) return bad(fault,TF_RUNTIME_RESOURCE);
        break;
    }
    default: return bad(fault,TF_RUNTIME_INVARIANT);
    }
    *out=value;
    return TF_EXEC_OK;
}
