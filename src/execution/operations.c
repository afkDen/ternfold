#include "operations.h"
#include "ternfold/checked_numeric.h"

#include <string.h>

static TfExecStatus raise(TfRuntimeCode *out, TfRuntimeCode code) {
    *out=code; return TF_EXEC_FAULT;
}

static TfExecStatus numeric(TfCheckedNumericStatus status, TfRuntimeCode *fault) {
    switch (status) {
    case TF_CHECKED_NUMERIC_OK: return TF_EXEC_OK;
    case TF_CHECKED_NUMERIC_OVERFLOW: return raise(fault,TF_RUNTIME_OVERFLOW);
    case TF_CHECKED_NUMERIC_ZERO_DIVISOR: return raise(fault,TF_RUNTIME_ZERO_DIVISOR);
    case TF_CHECKED_NUMERIC_NEGATIVE_EXPONENT: return raise(fault,TF_RUNTIME_NEGATIVE_EXPONENT);
    case TF_CHECKED_NUMERIC_UNSUPPORTED: return TF_EXEC_HOST;
    }
    return raise(fault,TF_RUNTIME_INVARIANT);
}

TfExecStatus tf_exec_unary(const TfCoreProgramData *d, TfCoreUnary op, TfExecValue value,
                           TfCoreTypeId result, TfExecValue *out, TfRuntimeCode *fault) {
    TfExecValue v={.type=result};
    if (op==TF_CORE_UNARY_NOT) v.as.bits=value.as.bits==0 ? 1u : 0u;
    else if (op==TF_CORE_UNARY_NEGATE) {
        const TfCheckedNumericStatus status=tf_core_type(d,value.type)->kind==TF_CORE_TYPE_INT ?
            tf_checked_int_neg(value.as.integer,&v.as.integer) : tf_checked_float_neg(value.as.bits,&v.as.bits);
        if (status!=TF_CHECKED_NUMERIC_OK) return numeric(status,fault);
    } else return raise(fault,TF_RUNTIME_INVARIANT);
    *out=v;
    return TF_EXEC_OK;
}

/* All floats are finite with canonical zero. For negative encodings, larger
 * magnitudes sort earlier. Comparisons never depend on the host FPU. */
static uint64_t float_key(uint64_t bits) {
    const uint64_t sign=UINT64_C(1)<<63;
    return (bits & sign)!=0 ? ~bits : bits|sign;
}

static int compare(TfCoreTypeKind kind, TfExecValue a, TfExecValue b) {
    if (kind==TF_CORE_TYPE_INT) return (a.as.integer>b.as.integer)-(a.as.integer<b.as.integer);
    const uint64_t left=float_key(a.as.bits), right=float_key(b.as.bits);
    return (left>right)-(left<right);
}

static bool charge(uint64_t *steps, uint64_t amount) {
    if (amount>*steps) return false;
    *steps-=amount; return true;
}

/* Work stack keeps a single cursor for each nontrivial aggregate depth. It
 * grows only when descending into children, never with aggregate width. */
typedef struct Pair Pair;
struct Pair { TfExecValues a,b; size_t next; Pair *parent; };

static TfExecStatus equal(TfExecArena *arena, const TfCoreProgramData *d,
                          TfExecValue a, TfExecValue b, bool *out, TfRuntimeCode *fault,
                          uint64_t *steps) {
    Pair *top=NULL, *spare=NULL;
    *out=false;
    for (;;) {
        if (!charge(steps,1)) return raise(fault,TF_RUNTIME_RESOURCE);
        if (a.type!=b.type) return raise(fault,TF_RUNTIME_INVARIANT);
        switch (tf_core_type(d,a.type)->kind) {
        case TF_CORE_TYPE_INT: if (a.as.integer!=b.as.integer) return TF_EXEC_OK; break;
        case TF_CORE_TYPE_BOOL: case TF_CORE_TYPE_CHAR: case TF_CORE_TYPE_FLOAT:
            if (a.as.bits!=b.as.bits) return TF_EXEC_OK;
            break;
        case TF_CORE_TYPE_UNIT: break;
        case TF_CORE_TYPE_TEXT: {
            const TfCoreText x=a.as.text,y=b.as.text;
            if (x.length!=y.length) return TF_EXEC_OK;
            if (!charge(steps,(uint64_t)x.length)) return raise(fault,TF_RUNTIME_RESOURCE);
            if (x.length!=0 && x.bytes!=y.bytes && memcmp(x.bytes,y.bytes,x.length)!=0) return TF_EXEC_OK;
            break;
        }
        case TF_CORE_TYPE_LIST: case TF_CORE_TYPE_OPTION: case TF_CORE_TYPE_DECISION:
        case TF_CORE_TYPE_RECORD: case TF_CORE_TYPE_ENUM: case TF_CORE_TYPE_MACHINE: {
            const TfExecValues x=a.as.aggregate.values,y=b.as.aggregate.values;
            if (a.as.aggregate.identity!=b.as.aggregate.identity || x.count!=y.count) return TF_EXEC_OK;
            if (x.count==0 || x.items==y.items) break;
            Pair *p=spare;
            if (p!=NULL) spare=p->parent;
            else p=tf_exec_allocate(arena,1,sizeof *p);
            if (p==NULL) return raise(fault,TF_RUNTIME_RESOURCE);
            *p=(Pair){x,y,1,top}; top=p;
            a=x.items[0]; b=y.items[0];
            continue;
        }
        default: return raise(fault,TF_RUNTIME_INVARIANT);
        }
        while (top!=NULL && top->next==top->a.count) {
            Pair *done=top; top=top->parent;
            done->parent=spare; spare=done;
        }
        if (top==NULL) { *out=true; return TF_EXEC_OK; }
        const size_t i=top->next++;
        a=top->a.items[i]; b=top->b.items[i];
    }
}

static TfExecStatus concatenate(TfExecArena *arena, TfCoreTypeKind kind, TfExecValue a, TfExecValue b,
                                 TfExecValue *out, TfRuntimeCode *fault, uint64_t *steps) {
    if (kind==TF_CORE_TYPE_TEXT) {
        const TfCoreText left=a.as.text,right=b.as.text;
        if (right.length>SIZE_MAX-left.length || right.length>INT64_MAX ||
            left.length>INT64_MAX-right.length) return raise(fault,TF_RUNTIME_RESOURCE);
        const size_t size=left.length+right.length;
        if (!charge(steps,(uint64_t)size)) return raise(fault,TF_RUNTIME_RESOURCE);
        if (left.length==0) { *out=b; return TF_EXEC_OK; }
        if (right.length==0) { *out=a; return TF_EXEC_OK; }
        unsigned char *bytes=tf_exec_allocate(arena,size,1);
        if (bytes==NULL) return raise(fault,TF_RUNTIME_RESOURCE);
        memcpy(bytes,left.bytes,left.length); memcpy(bytes+left.length,right.bytes,right.length);
        *out=(TfExecValue){.type=a.type,.as.text={bytes,size}};
    } else {
        const TfExecValues left=a.as.aggregate.values,right=b.as.aggregate.values;
        if (right.count>SIZE_MAX-left.count || right.count>INT64_MAX ||
            left.count>INT64_MAX-right.count) return raise(fault,TF_RUNTIME_RESOURCE);
        const size_t count=left.count+right.count;
        if (!charge(steps,(uint64_t)count)) return raise(fault,TF_RUNTIME_RESOURCE);
        if (left.count==0) { *out=b; return TF_EXEC_OK; }
        if (right.count==0) { *out=a; return TF_EXEC_OK; }
        TfExecValue *items=tf_exec_allocate(arena,count,sizeof *items);
        if (items==NULL) return raise(fault,TF_RUNTIME_RESOURCE);
        memcpy(items,left.items,left.count*sizeof *items);
        memcpy(items+left.count,right.items,right.count*sizeof *items);
        *out=(TfExecValue){.type=a.type,.as.aggregate={0,{items,count}}};
    }
    return TF_EXEC_OK;
}

TfExecStatus tf_exec_binary(TfExecArena *arena, const TfCoreProgramData *d,
                            TfCoreBinary op, TfExecValue a, TfExecValue b, TfCoreTypeId result,
                            TfExecValue *out, TfRuntimeCode *fault, uint64_t *steps) {
    const TfCoreTypeKind kind=tf_core_type(d,a.type)->kind;
    TfExecValue v={.type=result};
    TfCheckedNumericStatus status=TF_CHECKED_NUMERIC_OK;
    if (op==TF_CORE_BINARY_REAL_DIVIDE) {
        const uint64_t left=kind==TF_CORE_TYPE_INT ? tf_checked_int_to_float_bits(a.as.integer) : a.as.bits;
        const uint64_t right=tf_core_type(d,b.type)->kind==TF_CORE_TYPE_INT ?
            tf_checked_int_to_float_bits(b.as.integer) : b.as.bits;
        status=tf_checked_float_div(left,right,&v.as.bits);
    } else if (op==TF_CORE_BINARY_EQUAL || op==TF_CORE_BINARY_NOT_EQUAL) {
        bool matches;
        const TfExecStatus e=equal(arena,d,a,b,&matches,fault,steps);
        if (e!=TF_EXEC_OK) return e;
        v.as.bits=(matches==(op==TF_CORE_BINARY_EQUAL)) ? 1u : 0u;
    } else if (op==TF_CORE_BINARY_CONCAT) {
        return concatenate(arena,kind,a,b,out,fault,steps);
    } else if (op>=TF_CORE_BINARY_LESS && op<=TF_CORE_BINARY_GREATER_EQUAL) {
        const int cmp=compare(kind,a,b);
        const bool yes=op==TF_CORE_BINARY_LESS ? cmp<0 : op==TF_CORE_BINARY_LESS_EQUAL ? cmp<=0 :
                       op==TF_CORE_BINARY_GREATER ? cmp>0 : cmp>=0;
        v.as.bits=yes ? 1u : 0u;
    } else if (kind==TF_CORE_TYPE_INT) {
        switch (op) {
        case TF_CORE_BINARY_ADD: status=tf_checked_int_add(a.as.integer,b.as.integer,&v.as.integer); break;
        case TF_CORE_BINARY_SUBTRACT: status=tf_checked_int_sub(a.as.integer,b.as.integer,&v.as.integer); break;
        case TF_CORE_BINARY_MULTIPLY: status=tf_checked_int_mul(a.as.integer,b.as.integer,&v.as.integer); break;
        case TF_CORE_BINARY_FLOOR_DIVIDE: status=tf_checked_int_div(a.as.integer,b.as.integer,&v.as.integer); break;
        case TF_CORE_BINARY_REMAINDER: status=tf_checked_int_rem(a.as.integer,b.as.integer,&v.as.integer); break;
        case TF_CORE_BINARY_POWER: status=tf_checked_int_pow(a.as.integer,b.as.integer,&v.as.integer); break;
        default: return raise(fault,TF_RUNTIME_INVARIANT);
        }
    } else {
        switch (op) {
        case TF_CORE_BINARY_ADD: status=tf_checked_float_add(a.as.bits,b.as.bits,&v.as.bits); break;
        case TF_CORE_BINARY_SUBTRACT: status=tf_checked_float_sub(a.as.bits,b.as.bits,&v.as.bits); break;
        case TF_CORE_BINARY_MULTIPLY: status=tf_checked_float_mul(a.as.bits,b.as.bits,&v.as.bits); break;
        case TF_CORE_BINARY_POWER: status=tf_checked_float_pow(a.as.bits,b.as.integer,&v.as.bits); break;
        default: return raise(fault,TF_RUNTIME_INVARIANT);
        }
    }
    if (status!=TF_CHECKED_NUMERIC_OK) return numeric(status,fault);
    *out=v;
    return TF_EXEC_OK;
}
