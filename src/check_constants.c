#include "check_constants.h"
#include "ternfold/checked_numeric.h"
#include "internal.h"

#include <string.h>

typedef struct Arena Arena;
struct Arena { Arena *next; max_align_t alignment; };
typedef TfConstantValue Constant;
typedef struct {
    TfCheckContext *checker;
    const TfTypedProgram *program;
    const TfResolution *resolution;
    const TfTypeStore *store;
    TfAllocator allocator;
    Arena *arena;
    TfStatus status;
} Evaluation;
enum { CONSTANT_DEPTH = 512 };

static void *allocate(Evaluation *e, size_t count, size_t width) {
    Arena *p;
    if (e->status != TF_OK) return NULL;
    if (count > (SIZE_MAX-sizeof *p)/width) { e->status = TF_HOST_ERROR; return NULL; }
    p = e->allocator.allocate(e->allocator.context,sizeof *p+count*width);
    if (p == NULL) { e->status = TF_HOST_ERROR; return NULL; }
    p->next = e->arena; e->arena = p;
    return p+1;
}

static const TfAstNode *node(TfNodeId id) { return tf_ast_get(id.owner,id); }
static void error(Evaluation *e, TfSpan span, const char *message) {
    if (e->status == TF_OK) {
        if (e->checker == NULL) { e->status = TF_HOST_ERROR; return; }
        TfStatus status = tf_check_diagnostic(e->checker,"E306",message,span,(TfSpan){0});
        e->status = status == TF_OK ? TF_SOURCE_ERROR : TF_HOST_ERROR;
    }
}

static const TfResolvedSymbol *target(Evaluation *e, TfNodeId id) {
    const TfTypedNode *typed = tf_typed_program_node(e->program,id);
    if (typed == NULL) { e->status = TF_HOST_ERROR; return NULL; }
    return tf_resolution_symbol(e->resolution,typed->target);
}

static const TfResolvedSymbol *direct_symbol(const Evaluation *e, TfNodeId id) {
    const TfAstNode *syntax = node(id);
    const TfResolvedReference *reference;
    if (syntax == NULL || syntax->kind != TF_AST_PATH) return NULL;
    reference = tf_resolution_reference(e->resolution,id);
    return reference == NULL ? NULL : tf_resolution_symbol(e->resolution,reference->symbol);
}

/* Validate closure of the syntax separately from evaluation, including
 * unreachable right operands of short-circuit operators. */
static void closed(Evaluation *e, const TfAstNode *n, size_t depth) {
    if (e->status != TF_OK) return;
    if (n == NULL || depth == CONSTANT_DEPTH) { e->status = TF_HOST_ERROR; return; }
    switch (n->kind) {
    case TF_AST_LITERAL: case TF_AST_UNIT: return;
    case TF_AST_GROUP: closed(e,node(n->as.group.expression),depth+1); return;
    case TF_AST_UNARY: closed(e,node(n->as.unary.operand),depth+1); return;
    case TF_AST_BINARY:
        closed(e,node(n->as.binary.left),depth+1);
        closed(e,node(n->as.binary.right),depth+1);
        return;
    case TF_AST_LIST:
        for (size_t i=0; i<n->as.list.elements.count && e->status == TF_OK; ++i)
            closed(e,node(n->as.list.elements.items[i]),depth+1);
        return;
    case TF_AST_PATH: {
        const TfResolvedSymbol *s = direct_symbol(e,n->id);
        if (s != NULL && s->kind == TF_SYMBOL_VARIANT) return;
        break;
    }
    case TF_AST_CALL: {
        const TfResolvedSymbol *s = direct_symbol(e,n->as.call.callee);
        const TfResolvedType *t = s == NULL ? NULL : tf_resolution_type(e->resolution,s->type);
        if (s != NULL && (s->kind == TF_SYMBOL_VARIANT ||
            (s->kind == TF_SYMBOL_CONSTRUCTOR && t != NULL && t->kind == TF_TYPE_RECORD))) {
            for (size_t i=0; i<n->as.call.arguments.count && e->status == TF_OK; ++i) {
                const TfAstNode *arg = node(n->as.call.arguments.items[i]);
                closed(e,node(arg->as.argument.value),depth+1);
            }
            return;
        }
        break;
    }
    default: break;
    }
    error(e,n->span,"state initializer requires a closed constant expression");
}

static Constant *new_value(Evaluation *e, const TfConcreteType *type) {
    Constant *v = allocate(e,1,sizeof *v);
    if (v != NULL) *v = (Constant){.type=type->id,.kind=type->kind};
    return v;
}

static void checked(Evaluation *e, TfCheckedNumericStatus status, TfSpan span) {
    switch (status) {
    case TF_CHECKED_NUMERIC_OK: return;
    case TF_CHECKED_NUMERIC_OVERFLOW: error(e,span,"numeric overflow in state initializer"); return;
    case TF_CHECKED_NUMERIC_ZERO_DIVISOR: error(e,span,"division by zero in state initializer"); return;
    case TF_CHECKED_NUMERIC_NEGATIVE_EXPONENT: error(e,span,"negative Int power in state initializer"); return;
    default: e->status = TF_HOST_ERROR; return;
    }
}

static bool equal(Evaluation *e, const Constant *a, const Constant *b, size_t depth) {
    if (depth == CONSTANT_DEPTH) { e->status = TF_HOST_ERROR; return false; }
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case TF_CONCRETE_INT: return a->as.integer == b->as.integer;
    case TF_CONCRETE_FLOAT: return a->as.bits == b->as.bits;
    case TF_CONCRETE_BOOL: return a->as.boolean == b->as.boolean;
    case TF_CONCRETE_CHAR: return a->as.scalar == b->as.scalar;
    case TF_CONCRETE_UNIT: return true;
    case TF_CONCRETE_TEXT: return a->as.text.length == b->as.text.length &&
        (a->as.text.length == 0 || memcmp(a->as.text.bytes,b->as.text.bytes,a->as.text.length) == 0);
    default:
        if (a->variant.index != b->variant.index || a->as.aggregate.count != b->as.aggregate.count)
            return false;
        for (size_t i=0; i<a->as.aggregate.count; ++i)
            if (!equal(e,a->as.aggregate.items[i],b->as.aggregate.items[i],depth+1)) return false;
        return true;
    }
}

static uint64_t float_order(uint64_t bits) {
    const uint64_t sign = UINT64_C(1)<<63;
    return (bits & sign) != 0 ? ~bits : bits ^ sign;
}

static const Constant *evaluate(Evaluation *e, const TfAstNode *n, size_t depth);

static const Constant *aggregate(Evaluation *e, Constant *out, TfNodeList list,
                                 bool arguments, bool named, size_t depth) {
    const Constant **values = list.count != 0 ? allocate(e,list.count,sizeof *values) : NULL;
    out->as.aggregate.items = values;
    out->as.aggregate.count = list.count;
    for (size_t i=0; i<list.count && e->status == TF_OK; ++i) {
        size_t destination = i;
        const TfAstNode *item = node(list.items[i]);
        if (named) {
            const TfTypedNode *typed = tf_typed_program_node(e->program,item->id);
            if (typed == NULL || typed->member_index >= list.count) {
                e->status = TF_HOST_ERROR; return NULL;
            }
            destination = typed->member_index;
        }
        values[destination] = evaluate(e,arguments ? node(item->as.argument.value) : item,depth+1);
    }
    return e->status == TF_OK ? out : NULL;
}

static void binary(Evaluation *e, Constant *out, TfTokenKind op, const Constant *a,
                   const Constant *b, TfSpan span) {
    TfCheckedNumericStatus status = TF_CHECKED_NUMERIC_OK;
    int compare = 0;
    if (op == TF_EQUAL_EQUAL || op == TF_BANG_EQUAL) {
        out->as.boolean = equal(e,a,b,0) == (op == TF_EQUAL_EQUAL);
        return;
    }
    if (op == TF_PLUS_PLUS) {
        if (out->kind == TF_CONCRETE_TEXT) {
            const size_t m = a->as.text.length, n = b->as.text.length;
            unsigned char *bytes;
            if (n > SIZE_MAX-m) { e->status = TF_HOST_ERROR; return; }
            bytes = allocate(e,m+n,1);
            if (bytes == NULL) return;
            if (m != 0) memcpy(bytes,a->as.text.bytes,m);
            if (n != 0) memcpy(bytes+m,b->as.text.bytes,n);
            out->as.text.bytes = bytes;
            out->as.text.length = m+n;
        } else {
            const size_t m = a->as.aggregate.count, n = b->as.aggregate.count;
            const Constant **items;
            if (n > SIZE_MAX-m) { e->status = TF_HOST_ERROR; return; }
            items = allocate(e,m+n,sizeof *items);
            if (items == NULL) return;
            if (m != 0) memcpy(items,a->as.aggregate.items,m*sizeof *items);
            if (n != 0) memcpy(items+m,b->as.aggregate.items,n*sizeof *items);
            out->as.aggregate.items = items;
            out->as.aggregate.count = m+n;
        }
        return;
    }
    if (op == TF_AND_AND || op == TF_OR_OR) {
        out->as.boolean = b->as.boolean; return;
    }
    if (op == TF_LESS || op == TF_GREATER || op == TF_LESS_EQUAL || op == TF_GREATER_EQUAL) {
        if (a->kind == TF_CONCRETE_INT)
            compare = (a->as.integer>b->as.integer) - (a->as.integer<b->as.integer);
        else {
            const uint64_t x = float_order(a->as.bits), y = float_order(b->as.bits);
            compare = (x>y) - (x<y);
        }
        out->as.boolean = op == TF_LESS ? compare<0 : op == TF_GREATER ? compare>0 :
                          op == TF_LESS_EQUAL ? compare<=0 : compare>=0;
        return;
    }
    if (out->kind == TF_CONCRETE_INT) {
        const int64_t x = a->as.integer, y = b->as.integer;
        switch (op) {
        case TF_PLUS: status = tf_checked_int_add(x,y,&out->as.integer); break;
        case TF_MINUS: status = tf_checked_int_sub(x,y,&out->as.integer); break;
        case TF_STAR: status = tf_checked_int_mul(x,y,&out->as.integer); break;
        case TF_DIV: status = tf_checked_int_div(x,y,&out->as.integer); break;
        case TF_PERCENT: status = tf_checked_int_rem(x,y,&out->as.integer); break;
        case TF_CARET: status = tf_checked_int_pow(x,y,&out->as.integer); break;
        default: e->status = TF_HOST_ERROR; return;
        }
    } else {
        const uint64_t x = a->kind == TF_CONCRETE_INT ? tf_checked_int_to_float_bits(a->as.integer) : a->as.bits;
        const uint64_t y = b->kind == TF_CONCRETE_INT ? tf_checked_int_to_float_bits(b->as.integer) : b->as.bits;
        switch (op) {
        case TF_PLUS: status = tf_checked_float_add(x,y,&out->as.bits); break;
        case TF_MINUS: status = tf_checked_float_sub(x,y,&out->as.bits); break;
        case TF_STAR: status = tf_checked_float_mul(x,y,&out->as.bits); break;
        case TF_SLASH: status = tf_checked_float_div(x,y,&out->as.bits); break;
        case TF_CARET: status = tf_checked_float_pow(x,b->as.integer,&out->as.bits); break;
        default: e->status = TF_HOST_ERROR; return;
        }
    }
    checked(e,status,span);
}

static const Constant *evaluate(Evaluation *e, const TfAstNode *n, size_t depth) {
    const TfTypedNode *typed;
    const TfConcreteType *type;
    Constant *out;
    if (e->status != TF_OK) return NULL;
    if (n == NULL || depth == CONSTANT_DEPTH) { e->status = TF_HOST_ERROR; return NULL; }
    typed = tf_typed_program_node(e->program,n->id);
    type = typed != NULL ? tf_type_store_get(e->store,typed->type) : NULL;
    if (type == NULL) { e->status = TF_HOST_ERROR; return NULL; }
    if (n->kind == TF_AST_GROUP) return evaluate(e,node(n->as.group.expression),depth+1);
    out = new_value(e,type);
    if (out == NULL) return NULL;
    switch (n->kind) {
    case TF_AST_UNIT: break;
    case TF_AST_LITERAL: {
        const TfLiteralValue *lit = tf_typed_program_literal(e->program,n->id);
        if (lit == NULL || lit->minimum_magnitude) { e->status = TF_HOST_ERROR; return NULL; }
        switch (lit->kind) {
        case TF_LITERAL_INT: out->as.integer = (int64_t)lit->as.int_magnitude; break;
        case TF_LITERAL_FLOAT: out->as.bits = lit->as.float_bits; break;
        case TF_LITERAL_CHAR: out->as.scalar = lit->as.scalar; break;
        case TF_LITERAL_BOOL: out->as.boolean = lit->as.boolean; break;
        case TF_LITERAL_TEXT:
            out->as.text.bytes = lit->as.text.data; out->as.text.length = lit->as.text.length; break;
        default: e->status = TF_HOST_ERROR; return NULL;
        }
        break;
    }
    case TF_AST_UNARY: {
        const TfLiteralValue *literal = tf_typed_program_literal(e->program,n->as.unary.operand);
        const Constant *a;
        if (n->as.unary.op == TF_MINUS && literal != NULL && literal->minimum_magnitude) {
            out->as.integer = INT64_MIN; break;
        }
        a = evaluate(e,node(n->as.unary.operand),depth+1);
        if (a == NULL) return NULL;
        if (n->as.unary.op == TF_BANG) out->as.boolean = !a->as.boolean;
        else if (a->kind == TF_CONCRETE_INT)
            checked(e,tf_checked_int_neg(a->as.integer,&out->as.integer),n->as.unary.operator_span);
        else checked(e,tf_checked_float_neg(a->as.bits,&out->as.bits),n->as.unary.operator_span);
        break;
    }
    case TF_AST_BINARY: {
        const TfTokenKind op = n->as.binary.op;
        const Constant *a = evaluate(e,node(n->as.binary.left),depth+1), *b;
        if (a == NULL) return NULL;
        if ((op == TF_AND_AND && !a->as.boolean) || (op == TF_OR_OR && a->as.boolean)) return a;
        b = evaluate(e,node(n->as.binary.right),depth+1);
        if (b == NULL) return NULL;
        binary(e,out,op,a,b,n->as.binary.operator_span);
        break;
    }
    case TF_AST_LIST: return aggregate(e,out,n->as.list.elements,false,false,depth);
    case TF_AST_PATH:
        out->variant = typed->target;
        out->as.aggregate.items = NULL;
        out->as.aggregate.count = 0;
        break;
    case TF_AST_CALL: {
        const TfResolvedSymbol *s = target(e,n->as.call.callee);
        if (s == NULL) { e->status = TF_HOST_ERROR; return NULL; }
        if (s->kind == TF_SYMBOL_VARIANT) out->variant = s->id;
        return aggregate(e,out,n->as.call.arguments,true,s->kind == TF_SYMBOL_CONSTRUCTOR,depth);
    }
    default: e->status = TF_HOST_ERROR; return NULL;
    }
    return e->status == TF_OK ? out : NULL;
}

static TfStatus initializer(TfCheckContext *checker, const TfTypedProgram *program,
                            const TfAstNode *state, TfAllocator allocator,
                            TfConstantVisitor visit, void *context) {
    const TfAstNode *expression = node(state->as.state.value);
    Evaluation e = {.checker=checker,.program=program,
        .resolution=tf_typed_program_resolution(program),.store=tf_typed_program_store(program),
        .allocator=allocator,.status=TF_OK};
    closed(&e,expression,0);
    if (e.status == TF_OK) {
        const Constant *value = evaluate(&e,expression,0);
        if (e.status == TF_OK && (value == NULL ||
            (visit != NULL && visit(context,value) != TF_OK))) e.status = TF_HOST_ERROR;
    }
    while (e.arena != NULL) {
        Arena *next = e.arena->next;
        e.allocator.deallocate(e.allocator.context,e.arena);
        e.arena = next;
    }
    return e.status;
}

TfStatus tf_check_constant(TfCheckContext *checker, const TfAstNode *state) {
    if (checker == NULL || state == NULL || state->kind != TF_AST_STATE) return TF_HOST_ERROR;
    return initializer(checker,tf_check_program(checker),state,*tf_check_allocator(checker),NULL,NULL);
}

TfStatus tf_visit_checked_initializer(const TfTypedProgram *program, TfNodeId state,
                                      const TfAllocator *allocator, TfConstantVisitor visit,
                                      void *context) {
    const TfAllocator a = tf_allocator(allocator);
    const TfAstNode *n;
    if (visit == NULL || !tf_allocator_valid(a) ||
        tf_diagnostic_phase_kind(tf_typed_program_phase(program)) != TF_DIAG_PHASE_VALID ||
        tf_typed_program_node(program,state) == NULL ||
        (n = node(state)) == NULL || n->kind != TF_AST_STATE) return TF_HOST_ERROR;
    return initializer(NULL,program,n,a,visit,context);
}
