#include "ternfold/core_lower.h"
#include "lower_constants.h"
#include "../check_internal.h"
#include "../internal.h"

#include <stdlib.h>
#include <string.h>

typedef union Scratch Scratch;
union Scratch { Scratch *next; max_align_t alignment; };
typedef struct { TfCoreBlock block; size_t capacity; } WorkBlock;
typedef struct Wrapper Wrapper;
struct Wrapper { Wrapper *next; TfCoreBuiltin builtin; TfCoreFunctionId target; };
typedef struct { TfCoreValueId value; TfCoreFunctionId function; } Binding;
typedef struct { size_t index; Binding previous; } BindingUndo;
typedef struct CaptureLink CaptureLink;
struct CaptureLink { const TfResolvedCapture *capture; CaptureLink *next; };
typedef struct {
    TfAllocator allocator, scratch_allocator;
    Scratch *scratch;
    const TfTypedProgram *typed;
    const TfResolution *resolution;
    const TfTypeStore *store;
    TfCoreLowerTypes *types;
    TfCoreLowerConstants *constants;
    TfCoreFunction *functions;
    TfCoreFunctionId *targets;
    Binding *bindings;
    BindingUndo *undo;
    size_t undo_count, undo_capacity, lift_depth;
    CaptureLink ***module_captures; /* indexed by module ID then AST node ID */
    const TfResolvedModule *module;
    Wrapper **wrappers; /* chains keyed by original concrete function type ID */
    size_t symbol_count, function_count, function_capacity;
    WorkBlock *blocks;
    size_t block_count, block_capacity, value_count, depth;
    TfCoreFunctionId function;
    TfCoreFunctionId entry;
    TfCoreLowerIssue issue;
    bool failed;
} Lower;
enum { LOWER_DEPTH = 160 };

static void *scratch_allocate(void *context, size_t size) {
    Lower *l = context;
    Scratch *chunk;
    if (size > SIZE_MAX-sizeof *chunk) return NULL;
    chunk = l->allocator.allocate(l->allocator.context,sizeof *chunk+size);
    if (chunk == NULL) return NULL;
    chunk->next=l->scratch;
    l->scratch=chunk;
    return chunk+1;
}
static void scratch_release(void *context, void *ptr) { (void)context; (void)ptr; }
static void *array(Lower *l, size_t n, size_t width) {
    void *p;
    if (n == 0 || l->failed) return NULL;
    if (n > SIZE_MAX/width || (p=scratch_allocate(l,n*width)) == NULL) {
        l->failed = true; return NULL;
    }
    return p;
}
static bool grow(Lower *l, void **p, size_t *cap, size_t count, size_t width) {
    if (l->failed || count == SIZE_MAX || !tf_grow(p,cap,count+1,width,l->scratch_allocator)) {
        l->failed = true; return false;
    }
    return true;
}
static TfCoreValueId fail_at(Lower *l, TfCoreLowerCode code, const TfAstNode *n) {
    if (!l->failed) l->issue=(TfCoreLowerIssue){code,n != NULL ? n->span : (TfSpan){0}};
    l->failed=true; return 0;
}
static const TfAstNode *node(TfNodeId id) { return tf_ast_get(id.owner,id); }
static TfCoreText text(TfAstText s) { return (TfCoreText){s.data,s.length}; }
static bool spelling(TfAstText s, const char *expected) {
    const size_t n = strlen(expected);
    return s.length == n && memcmp(s.data,expected,n) == 0;
}
static TfCoreSpanId origin(Lower *l, TfSpan span) {
    TfCoreSpanId id=0;
    if (!l->failed && tf_core_lower_span(l->types,span,&id) != TF_OK) l->failed=true;
    return id;
}
static size_t symbol_index(const Lower *l, TfSymbolId id) {
    return id.owner==l->resolution && id.index<=l->symbol_count ? id.index : 0;
}
static TfCoreValueId read_binding(const Lower *l, size_t index) {
    if (index==0 || l->bindings[index].function!=l->function) return 0;
    return l->bindings[index].value;
}
static bool set_binding(Lower *l, size_t index, TfCoreValueId value) {
    if (l->failed) return false;
    if (index==0 || value==0) { l->failed=true; return false; }
    if (l->lift_depth!=0) {
        void *undo=l->undo;
        if (!grow(l,&undo,&l->undo_capacity,l->undo_count,sizeof *l->undo)) return false;
        l->undo=undo;
        l->undo[l->undo_count++]=(BindingUndo){index,l->bindings[index]};
    }
    l->bindings[index]=(Binding){value,l->function};
    return true;
}

static bool index_captures(Lower *l) {
    const size_t count=tf_resolution_capture_count(l->resolution);
    const size_t modules=tf_resolution_module_count(l->resolution);
    if (count==0) return true;
    if (modules==SIZE_MAX) return false;
    l->module_captures=array(l,modules+1,sizeof *l->module_captures);
    CaptureLink *links=array(l,count,sizeof *links);
    if (l->failed) return false;
    for (size_t i=0; i<=modules; ++i) l->module_captures[i]=NULL;
    for (size_t i=0; i<count; ++i) {
        const TfResolvedCapture *cap=tf_resolution_capture_at(l->resolution,i);
        const TfResolvedSymbol *symbol=tf_resolution_symbol(l->resolution,cap->symbol);
        const TfResolvedModule *module=symbol!=NULL ? tf_resolution_module(l->resolution,symbol->module) : NULL;
        const size_t nodes=module!=NULL ? tf_ast_count(module->ast) : 0;
        if (module==NULL || module->id.index==0 || module->id.index>modules ||
            cap->closure.owner!=module->ast || cap->closure.index==0 || cap->closure.index>nodes || nodes==SIZE_MAX)
            return false;
        CaptureLink **heads=l->module_captures[module->id.index];
        if (heads==NULL) {
            heads=array(l,nodes+1,sizeof *heads);
            if (l->failed) return false;
            for (size_t j=0; j<=nodes; ++j) heads[j]=NULL;
            l->module_captures[module->id.index]=heads;
        }
        CaptureLink **head=&heads[cap->closure.index];
        links[i]=(CaptureLink){cap,*head};
        *head=&links[i];
    }
    return true;
}
static const TfTypedNode *typed(const Lower *l, TfNodeId id) { return tf_typed_program_node(l->typed,id); }
static TfCoreTypeId type_of(Lower *l, const TfAstNode *n) {
    const TfTypedNode *t = typed(l,n->id);
    const TfCoreTypeId id = t != NULL ? tf_core_lower_type_id(l->types,t->type) : 0;
    if (id == 0) (void)fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    return id;
}
static const TfResolvedSymbol *target(const Lower *l, TfNodeId id) {
    const TfTypedNode *t = typed(l,id);
    return t != NULL ? tf_resolution_symbol(l->resolution,t->target) : NULL;
}
static TfCoreValueId fresh(Lower *l) {
    if (l->failed || l->value_count == SIZE_MAX) { l->failed=true; return 0; }
    return ++l->value_count;
}
static TfCoreBlockId new_block(Lower *l, TfSpan span) {
    void *blocks = l->blocks;
    if (!grow(l,&blocks,&l->block_capacity,l->block_count,sizeof *l->blocks)) return 0;
    l->blocks=blocks;
    l->blocks[l->block_count++] = (WorkBlock){.block.span=origin(l,span)};
    return l->failed ? 0 : l->block_count;
}
static TfCoreValueIds operands(Lower *l, const TfCoreValueId *values, size_t n) {
    TfCoreValueId *copy = array(l,n,sizeof *copy);
    if (copy != NULL) memcpy(copy,values,n*sizeof *copy);
    return (TfCoreValueIds){copy,n};
}
static TfCoreValueId emit(Lower *l, TfCoreBlockId block, TfCoreTypeId type,
                          TfCoreOperation operation, TfSpan span) {
    TfCoreLet *lets;
    void *buffer;
    if (l->failed) return 0;
    if (block==0 || block>l->block_count || type==0) return fail_at(l,TF_CORE_LOWER_INVARIANT,NULL);
    WorkBlock *b = &l->blocks[block-1];
    buffer = (void *)b->block.lets;
    if (!grow(l,&buffer,&b->capacity,b->block.let_count,sizeof *lets)) return 0;
    b->block.lets=lets=buffer;
    TfCoreValueId value = fresh(l);
    lets[b->block.let_count++] = (TfCoreLet){value,type,operation,origin(l,span)};
    return l->failed ? 0 : value;
}
static TfCoreValueId emit_const(Lower *l, TfCoreBlockId block, TfCoreConstant k, TfSpan span) {
    TfCoreConstantId id;
    if (l->failed) return 0;
    k.span=origin(l,span);
    if (l->failed || tf_core_lower_constant_append(l->constants,k,&id)!=TF_OK) {
        l->failed=true; return 0;
    }
    return emit(l,block,k.type,(TfCoreOperation){.kind=TF_CORE_OP_CONST,.as.constant=id},span);
}
static void finish(Lower *l, TfCoreBlockId block, TfCoreValueId value, TfSpan span) {
    if (!l->failed && block!=0 && block<=l->block_count && value!=0)
        l->blocks[block-1].block.terminator=(TfCoreTerminator){TF_CORE_YIELD,value,origin(l,span)};
    else l->failed=true;
}

static TfCoreValueId expression(Lower *l, TfCoreBlockId block, TfNodeId id);
static TfCoreFunctionId builtin_wrapper(Lower *l, const TfAstNode *n,
                                        const TfResolvedSymbol *s, TfCoreTypeId type);

static TfCoreValueId literal(Lower *l, TfCoreBlockId b, const TfAstNode *n, TfCoreTypeId type) {
    const TfLiteralValue *value = tf_typed_program_literal(l->typed,n->id);
    TfCoreConstant k = {.type=type};
    if (value == NULL || value->minimum_magnitude) return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    switch (value->kind) {
    case TF_LITERAL_INT: k.kind=TF_CORE_CONSTANT_INT; k.as.integer=(int64_t)value->as.int_magnitude; break;
    case TF_LITERAL_FLOAT: k.kind=TF_CORE_CONSTANT_FLOAT; k.as.float_bits=value->as.float_bits; break;
    case TF_LITERAL_BOOL: k.kind=TF_CORE_CONSTANT_BOOL; k.as.boolean=value->as.boolean ? 1u : 0u; break;
    case TF_LITERAL_TEXT:
        k.kind=TF_CORE_CONSTANT_TEXT;
        k.as.text=(TfCoreText){value->as.text.data,value->as.text.length}; break;
    case TF_LITERAL_CHAR: k.kind=TF_CORE_CONSTANT_CHAR; k.as.character=value->as.scalar; break;
    case TF_LITERAL_UNIT: k.kind=TF_CORE_CONSTANT_UNIT; break;
    }
    return emit_const(l,b,k,n->span);
}

static TfCoreBinary binary(TfTokenKind token) {
    switch (token) {
    case TF_PLUS: return TF_CORE_BINARY_ADD;
    case TF_MINUS: return TF_CORE_BINARY_SUBTRACT;
    case TF_STAR: return TF_CORE_BINARY_MULTIPLY;
    case TF_SLASH: return TF_CORE_BINARY_REAL_DIVIDE;
    case TF_DIV: return TF_CORE_BINARY_FLOOR_DIVIDE;
    case TF_PERCENT: return TF_CORE_BINARY_REMAINDER;
    case TF_CARET: return TF_CORE_BINARY_POWER;
    case TF_LESS: return TF_CORE_BINARY_LESS;
    case TF_LESS_EQUAL: return TF_CORE_BINARY_LESS_EQUAL;
    case TF_GREATER: return TF_CORE_BINARY_GREATER;
    case TF_GREATER_EQUAL: return TF_CORE_BINARY_GREATER_EQUAL;
    case TF_EQUAL_EQUAL: return TF_CORE_BINARY_EQUAL;
    case TF_BANG_EQUAL: return TF_CORE_BINARY_NOT_EQUAL;
    case TF_PLUS_PLUS: return TF_CORE_BINARY_CONCAT;
    default: return 0;
    }
}

static TfCoreValueId if_op(Lower *l, TfCoreBlockId b, TfCoreTypeId type, TfCoreValueId test,
                           TfCoreBlockId yes, TfCoreBlockId no, TfSpan span) {
    return emit(l,b,type,(TfCoreOperation){.kind=TF_CORE_OP_IF,
        .operands=operands(l,&test,1),.as.branch={yes,no}},span);
}

static TfCoreValueId operation(Lower *l, TfCoreBlockId b, const TfAstNode *n, TfCoreTypeId type) {
    TfCoreValueId args[2];
    if (n->kind == TF_AST_UNARY) {
        const TfLiteralValue *inner = tf_typed_program_literal(l->typed,n->as.unary.operand);
        if (n->as.unary.op==TF_MINUS && inner!=NULL && inner->minimum_magnitude)
            return emit_const(l,b,(TfCoreConstant){.kind=TF_CORE_CONSTANT_INT,.type=type,
                               .as.integer=INT64_MIN},n->as.unary.operator_span);
        args[0]=expression(l,b,n->as.unary.operand);
        return emit(l,b,type,(TfCoreOperation){.kind=TF_CORE_OP_UNARY,.operands=operands(l,args,1),
            .as.unary=n->as.unary.op==TF_MINUS ? TF_CORE_UNARY_NEGATE : TF_CORE_UNARY_NOT},
            n->as.unary.operator_span);
    }
    const TfTokenKind op=n->as.binary.op;
    const TfSpan span=n->as.binary.operator_span;
    args[0]=expression(l,b,n->as.binary.left);
    if (op==TF_AND_AND || op==TF_OR_OR) {
        const TfCoreBlockId evaluated=new_block(l,node(n->as.binary.right)->span);
        const TfCoreBlockId skipped=new_block(l,span);
        args[1]=expression(l,evaluated,n->as.binary.right);
        finish(l,evaluated,args[1],node(n->as.binary.right)->span);
        args[1]=emit_const(l,skipped,(TfCoreConstant){.kind=TF_CORE_CONSTANT_BOOL,.type=type,
                            .as.boolean=op==TF_OR_OR ? 1u : 0u},span);
        finish(l,skipped,args[1],span);
        return if_op(l,b,type,args[0],op==TF_AND_AND ? evaluated : skipped,
                     op==TF_AND_AND ? skipped : evaluated,span);
    }
    args[1]=expression(l,b,n->as.binary.right);
    const TfCoreBinary k=binary(op);
    if (k==0) return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    return emit(l,b,type,(TfCoreOperation){.kind=TF_CORE_OP_BINARY,.operands=operands(l,args,2),
                .as.binary=k},span);
}

static bool variant_tag(const Lower *l, const TfResolvedSymbol *s, size_t *out) {
    const TfResolvedType *t=tf_resolution_type(l->resolution,s->type);
    if (t==NULL || s->kind!=TF_SYMBOL_VARIANT) return false;
    if (t->kind==TF_TYPE_BUILTIN) {
        if (spelling(s->name,"Some")) *out=TF_CORE_OPTION_SOME;
        else if (spelling(s->name,"None")) *out=TF_CORE_OPTION_NONE;
        else if (spelling(s->name,"Accept")) *out=TF_CORE_DECISION_ACCEPT;
        else if (spelling(s->name,"Reject")) *out=TF_CORE_DECISION_REJECT;
        else return false;
        return true;
    }
    const TfAstNode *declaration=node(t->declaration);
    if (declaration==NULL || declaration->kind!=TF_AST_ENUM) return false;
    const TfNodeList members=declaration->as.aggregate.members;
    for (size_t i=0; i<members.count; ++i)
        if (members.items[i].owner==s->declaration.owner && members.items[i].index==s->declaration.index) {
            *out=i; return true;
        }
    return false;
}

static TfCoreValueId path(Lower *l, TfCoreBlockId b, const TfAstNode *n, TfCoreTypeId type) {
    const TfResolvedSymbol *s=target(l,n->id);
    if (s==NULL) return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    const size_t index=symbol_index(l,s->id);
    if (s->kind==TF_SYMBOL_VARIANT) {
        size_t tag;
        if (!variant_tag(l,s,&tag)) return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
        return emit(l,b,type,(TfCoreOperation){.kind=TF_CORE_OP_VARIANT,.as.variant={type,tag}},n->span);
    }
    if (s->kind==TF_SYMBOL_FUNCTION && index!=0 && l->targets[index]!=0 && !s->entry_only)
        return emit(l,b,type,(TfCoreOperation){.kind=TF_CORE_OP_CLOSURE,.as.function=l->targets[index]},n->span);
    if (s->kind==TF_SYMBOL_BUILTIN) {
        const TfCoreFunctionId wrapper=builtin_wrapper(l,n,s,type);
        return emit(l,b,type,(TfCoreOperation){.kind=TF_CORE_OP_CLOSURE,.as.function=wrapper},n->span);
    }
    const TfCoreValueId value=read_binding(l,index);
    return value!=0 ? value : fail_at(l,TF_CORE_LOWER_INVARIANT,n);
}

static TfCoreBuiltin builtin(TfAstText n) {
    static const struct { const char *name; TfCoreBuiltin kind; } builtins[] = {
        {"len",TF_CORE_BUILTIN_LEN},{"at",TF_CORE_BUILTIN_AT},{"append",TF_CORE_BUILTIN_APPEND},
        {"map",TF_CORE_BUILTIN_MAP},{"fold",TF_CORE_BUILTIN_FOLD},{"range",TF_CORE_BUILTIN_RANGE},
        {"int_text",TF_CORE_BUILTIN_INT_TEXT},{"int_float",TF_CORE_BUILTIN_INT_FLOAT},
        {"float_int",TF_CORE_BUILTIN_FLOAT_INT},{"float_text",TF_CORE_BUILTIN_FLOAT_TEXT},
        {"bool_text",TF_CORE_BUILTIN_BOOL_TEXT},{"parse_int",TF_CORE_BUILTIN_PARSE_INT},
        {"text_len",TF_CORE_BUILTIN_TEXT_LEN},{"text_at",TF_CORE_BUILTIN_TEXT_AT},
        {"text_slice",TF_CORE_BUILTIN_TEXT_SLICE}
    };
    for (size_t i=0; i<sizeof builtins/sizeof *builtins; ++i)
        if (spelling(n,builtins[i].name)) return builtins[i].kind;
    return 0;
}

static bool builtin_operation(Lower *l, const TfAstNode *callee, const TfResolvedSymbol *s,
                               TfCoreOperation *op) {
    const TfTypedNode *t=typed(l,callee->id);
    const TfConcreteType *fn=t!=NULL ? tf_type_store_get(l->store,t->type) : NULL;
    const TfCoreBuiltin kind=builtin(s->name);
    if (kind==0) { (void)fail_at(l,TF_CORE_LOWER_UNSUPPORTED_FORM,callee); return false; }
    if (fn==NULL || fn->kind!=TF_CONCRETE_FUNCTION) return false;
    size_t n=0;
    TfCoreTypeId *args=NULL;
    if (kind==TF_CORE_BUILTIN_LEN || kind==TF_CORE_BUILTIN_AT || kind==TF_CORE_BUILTIN_APPEND ||
        kind==TF_CORE_BUILTIN_MAP || kind==TF_CORE_BUILTIN_FOLD) {
        n=kind==TF_CORE_BUILTIN_MAP || kind==TF_CORE_BUILTIN_FOLD ? 2u : 1u;
        args=array(l,n,sizeof *args);
        if (args==NULL || fn->as.function.arity==0) return false;
        const TfConcreteType *first=tf_type_store_get(l->store,fn->as.function.parameters[0]);
        if (first==NULL || first->kind!=TF_CONCRETE_LIST ||
            (args[0]=tf_core_lower_type_id(l->types,first->as.element))==0) return false;
        if (kind==TF_CORE_BUILTIN_MAP) {
            const TfConcreteType *result=tf_type_store_get(l->store,fn->as.function.result);
            if (result==NULL || result->kind!=TF_CONCRETE_LIST ||
                (args[1]=tf_core_lower_type_id(l->types,result->as.element))==0) return false;
        } else if (kind==TF_CORE_BUILTIN_FOLD &&
                   (args[1]=tf_core_lower_type_id(l->types,fn->as.function.result))==0) return false;
    }
    op->kind=TF_CORE_OP_BUILTIN;
    op->as.builtin.id=kind;
    op->as.builtin.type_arguments=(TfCoreTypeIds){args,n};
    return true;
}

static TfCoreFunctionId append_function(Lower *l) {
    void *functions=l->functions;
    if (!grow(l,&functions,&l->function_capacity,l->function_count,sizeof *l->functions)) return 0;
    l->functions=functions;
    l->functions[l->function_count]=(TfCoreFunction){0};
    return ++l->function_count;
}

static TfCoreFunctionId builtin_wrapper(Lower *l, const TfAstNode *n,
                                        const TfResolvedSymbol *s, TfCoreTypeId type) {
    const size_t count=tf_type_store_count(l->store);
    const TfCoreBuiltin kind=builtin(s->name);
    if (kind==0) return fail_at(l,TF_CORE_LOWER_UNSUPPORTED_FORM,n);
    if (type==0 || type>count) return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    if (l->wrappers==NULL) {
        l->wrappers=array(l,count,sizeof *l->wrappers);
        if (l->failed) return 0;
        for (size_t i=0; i<count; ++i) l->wrappers[i]=NULL;
    }
    Wrapper **chain=&l->wrappers[type-1];
    for (const Wrapper *entry=*chain; entry!=NULL; entry=entry->next)
        if (entry->builtin==kind) return entry->target;
    TfCoreOperation op={0};
    if (!builtin_operation(l,n,s,&op)) return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    TfCoreProgramData d={0};
    tf_core_lower_type_view(l->types,&d);
    const TfCoreType *signature=tf_core_type(&d,type);
    if (signature==NULL || signature->kind!=TF_CORE_TYPE_FUNCTION)
        return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    const TfCoreTypeIds types=signature->as.function.parameters;
    const TfCoreTypeId result=signature->as.function.result;
    if (types.count==SIZE_MAX) return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    TfCoreParameter *params=array(l,types.count,sizeof *params);
    TfCoreValueId *args=array(l,types.count,sizeof *args);
    TfCoreLet *body=array(l,1,sizeof *body);
    TfCoreBlock *block=array(l,1,sizeof *block);
    Wrapper *entry=array(l,1,sizeof *entry);
    const TfCoreSpanId span=origin(l,n->span);
    const TfCoreFunctionId id=append_function(l);
    if (l->failed) return 0;
    for (size_t i=0; i<types.count; ++i) {
        args[i]=i+1;
        params[i]=(TfCoreParameter){.value=args[i],.type=types.items[i],.span=span};
    }
    op.operands=(TfCoreValueIds){args,types.count};
    *body=(TfCoreLet){types.count+1,result,op,span};
    *block=(TfCoreBlock){.span=span,.lets=body,.let_count=1,
                         .terminator={TF_CORE_YIELD,body->value,span}};
    l->functions[id-1]=(TfCoreFunction){.kind=TF_CORE_FUNCTION_ORDINARY,
        .name={(const unsigned char *)"$prelude",sizeof "$prelude"-1},
        .span=span,.parameters=params,.parameter_count=types.count,.result=result,
        .blocks=block,.block_count=1,.body=1,.value_count=body->value};
    *entry=(Wrapper){*chain,kind,id};
    *chain=entry;
    return id;
}

static TfCoreValueId call(Lower *l, TfCoreBlockId b, const TfAstNode *n, TfCoreTypeId type) {
    const TfAstNode *callee=node(n->as.call.callee);
    const TfResolvedSymbol *s=callee!=NULL && callee->kind==TF_AST_PATH ? target(l,callee->id) : NULL;
    const TfNodeList arguments=n->as.call.arguments;
    TfCoreOperation op={0};
    size_t base=0;
    bool named=false;
    if (s!=NULL && s->kind==TF_SYMBOL_FUNCTION) {
        const size_t index=symbol_index(l,s->id);
        if (index==0 || l->targets[index]==0 || s->entry_only) return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
        op.kind=TF_CORE_OP_CALL; op.as.function=l->targets[index];
    } else if (s!=NULL && s->kind==TF_SYMBOL_VARIANT) {
        op.kind=TF_CORE_OP_VARIANT;
        op.as.variant.type=type;
        if (!variant_tag(l,s,&op.as.variant.tag)) return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    } else if (s!=NULL && s->kind==TF_SYMBOL_CONSTRUCTOR) {
        const TfResolvedType *owner=tf_resolution_type(l->resolution,s->type);
        if (owner==NULL || owner->kind!=TF_TYPE_RECORD) return fail_at(l,TF_CORE_LOWER_UNSUPPORTED_FORM,n);
        op.kind=TF_CORE_OP_RECORD; op.as.constructed_type=type; named=true;
    } else if (s!=NULL && s->kind==TF_SYMBOL_BUILTIN) {
        if (!builtin_operation(l,callee,s,&op)) return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    } else { op.kind=TF_CORE_OP_CALL_CLOSURE; base=1; }
    if (arguments.count>SIZE_MAX-base) return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    const size_t count=arguments.count+base;
    TfCoreValueId *values=array(l,count,sizeof *values);
    if (count!=0 && values==NULL) return 0;
    if (base!=0) values[0]=expression(l,b,n->as.call.callee);
    for (size_t i=0; i<arguments.count && !l->failed; ++i) {
        const TfAstNode *arg=node(arguments.items[i]);
        size_t slot=i+base;
        if (named) {
            const TfTypedNode *record=typed(l,arg->id);
            if (record==NULL || record->member_index>=count) return fail_at(l,TF_CORE_LOWER_INVARIANT,arg);
            slot=record->member_index;
        }
        values[slot]=expression(l,b,arg->as.argument.value);
    }
    op.operands=(TfCoreValueIds){values,count};
    return emit(l,b,type,op,n->span);
}

static bool binding(Lower *l, const TfAstNode *statement, TfCoreValueId value) {
    const TfTypedNode *t=typed(l,statement->id);
    const size_t index=t!=NULL ? symbol_index(l,t->target) : 0;
    if (index==0) { (void)fail_at(l,TF_CORE_LOWER_INVARIANT,statement); return false; }
    return set_binding(l,index,value);
}

static TfCoreValueId lower_block(Lower *l, TfCoreBlockId b, const TfAstNode *n) {
    const TfNodeList statements=n->as.block.statements;
    for (size_t i=0; i<statements.count && !l->failed; ++i) {
        const TfAstNode *statement=node(statements.items[i]);
        if (statement==NULL || statement->kind!=TF_AST_LET)
            return fail_at(l,TF_CORE_LOWER_UNSUPPORTED_FORM,statement);
        const TfCoreValueId value=expression(l,b,statement->as.binding.value);
        if (!statement->as.binding.discard && !binding(l,statement,value)) return 0;
    }
    return expression(l,b,n->as.block.final_expression);
}

static TfCoreValueId comprehension(Lower *l, TfCoreBlockId b, const TfAstNode *n, TfCoreTypeId type) {
    const TfCoreValueId collection=expression(l,b,n->as.loop.iterable);
    const TfTypedNode *source=typed(l,n->as.loop.iterable);
    const TfConcreteType *list=source!=NULL ? tf_type_store_get(l->store,source->type) : NULL;
    const TfTypedNode *t=typed(l,n->id);
    const size_t binder=t!=NULL ? symbol_index(l,t->target) : 0;
    if (binder==0 || list==NULL || list->kind!=TF_CONCRETE_LIST)
        return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    const TfCoreBlockId body=new_block(l,node(n->as.loop.body)->span);
    const TfCoreParameter element={fresh(l),tf_core_lower_type_id(l->types,list->as.element),
                                    text(n->as.loop.name),origin(l,n->as.loop.name.span)};
    if (!set_binding(l,binder,element.value)) return 0;
    const TfCoreValueId result=expression(l,body,n->as.loop.body);
    finish(l,body,result,node(n->as.loop.body)->span);
    return emit(l,b,type,(TfCoreOperation){.kind=TF_CORE_OP_LIST_MAP,
        .operands=operands(l,&collection,1),.as.list_map={element,body}},n->span);
}

static bool parameters(Lower *l, TfCoreParameter *out, TfNodeList input) {
    for (size_t i=0; i<input.count && !l->failed; ++i) {
        const TfAstNode *p=node(input.items[i]);
        const TfTypedNode *t=typed(l,p->id);
        const size_t index=t!=NULL ? symbol_index(l,t->target) : 0;
        if (index==0) { (void)fail_at(l,TF_CORE_LOWER_INVARIANT,p); return false; }
        out[i]=(TfCoreParameter){fresh(l),tf_core_lower_type_id(l->types,t->type),
                                  text(p->as.parameter.name),origin(l,p->span)};
        if (!set_binding(l,index,out[i].value)) return false;
    }
    return !l->failed;
}

static bool save_function(Lower *l, TfCoreFunction out) {
    TfCoreBlock *blocks=array(l,l->block_count,sizeof *blocks);
    if (l->failed) return false;
    for (size_t i=0; i<l->block_count; ++i) blocks[i]=l->blocks[i].block;
    out.blocks=blocks; out.block_count=l->block_count; out.value_count=l->value_count;
    l->functions[l->function-1]=out;
    return true;
}

static int capture_order(const void *a, const void *b) {
    const TfResolvedCapture *left=*(const TfResolvedCapture *const *)a;
    const TfResolvedCapture *right=*(const TfResolvedCapture *const *)b;
    if (left->symbol.index!=right->symbol.index) return left->symbol.index<right->symbol.index ? -1 : 1;
    return (left->kind>right->kind)-(left->kind<right->kind);
}

static TfCoreValueId closure(Lower *l, TfCoreBlockId block, const TfAstNode *n, TfCoreTypeId type) {
    const TfTypedNode *t=typed(l,n->id);
    const TfConcreteType *fn=t!=NULL ? tf_type_store_get(l->store,t->type) : NULL;
    const TfNodeList params=n->as.closure.parameters;
    if (fn==NULL || fn->kind!=TF_CONCRETE_FUNCTION || fn->as.function.arity!=params.count)
        return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    CaptureLink **module=l->module_captures!=NULL ? l->module_captures[l->module->id.index] : NULL;
    CaptureLink *captures=module!=NULL ? module[n->id.index] : NULL;
    size_t count=0;
    for (const CaptureLink *i=captures; i!=NULL; i=i->next) ++count;
    const TfResolvedCapture **ordered=array(l,count,sizeof *ordered);
    TfCoreValueId *args=array(l,count,sizeof *args);
    TfCoreParameter *captured=array(l,count,sizeof *captured);
    TfCoreParameter *parameters_out=array(l,params.count,sizeof *parameters_out);
    const TfCoreFunctionId id=append_function(l);
    if (l->failed) return 0;
    for (size_t i=0; i<count; ++i, captures=captures->next) ordered[i]=captures->capture;
    if (count>1) qsort(ordered,count,sizeof *ordered,capture_order);
    for (size_t i=0; i<count; ++i) {
        const size_t index=symbol_index(l,ordered[i]->symbol);
        args[i]=read_binding(l,index);
        if (args[i]==0 || ordered[i]->kind==TF_REFERENCE_CANDIDATE)
            return fail_at(l,TF_CORE_LOWER_INVARIANT,n);
    }
    const TfCoreFunctionId outer=l->function;
    WorkBlock *outer_blocks=l->blocks;
    const size_t outer_count=l->block_count, outer_capacity=l->block_capacity, outer_values=l->value_count;
    const size_t undo_base=l->undo_count;
    l->function=id; l->blocks=NULL; l->block_count=0; l->block_capacity=0; l->value_count=0; ++l->lift_depth;
    TfCoreFunction out={.kind=TF_CORE_FUNCTION_ORDINARY,
        .name={(const unsigned char *)"$lambda",sizeof "$lambda"-1},.module=tf_core_lower_module_name(l->module),
        .span=origin(l,n->span),.parameters=parameters_out,.parameter_count=params.count,
        .captures=captured,.capture_count=count,.result=tf_core_lower_type_id(l->types,fn->as.function.result)};
    if (parameters(l,parameters_out,params)) {
        for (size_t i=0; i<count && !l->failed; ++i) {
            const TfSymbolId symbol=ordered[i]->symbol;
            const TfResolvedSymbol *info=tf_resolution_symbol(l->resolution,symbol);
            captured[i]=(TfCoreParameter){fresh(l),tf_core_lower_type_id(l->types,tf_typed_program_symbol(l->typed,symbol)),
                                         text(info->name),origin(l,info->name.span)};
            (void)set_binding(l,symbol.index,captured[i].value);
        }
        const TfAstNode *body=node(n->as.closure.body);
        out.body=new_block(l,body->span);
        const TfCoreValueId value=expression(l,out.body,body->id);
        finish(l,out.body,value,body->span);
        (void)save_function(l,out);
    }
    while (l->undo_count>undo_base) {
        const BindingUndo undo=l->undo[--l->undo_count];
        l->bindings[undo.index]=undo.previous;
    }
    --l->lift_depth;
    l->function=outer; l->blocks=outer_blocks;
    l->block_count=outer_count; l->block_capacity=outer_capacity; l->value_count=outer_values;
    return emit(l,block,type,(TfCoreOperation){.kind=TF_CORE_OP_CLOSURE,
        .operands={args,count},.as.function=id},n->span);
}

static TfCoreValueId expression(Lower *l, TfCoreBlockId b, TfNodeId id) {
    const TfAstNode *n=node(id);
    TfCoreValueId v=0;
    if (l->failed) return 0;
    if (l->depth==LOWER_DEPTH || n==NULL) { l->failed=true; return 0; }
    const TfCoreTypeId type=type_of(l,n);
    if (l->failed) return 0;
    ++l->depth;
    switch (n->kind) {
    case TF_AST_LITERAL: case TF_AST_UNIT: v=literal(l,b,n,type); break;
    case TF_AST_PATH: v=path(l,b,n,type); break;
    case TF_AST_GROUP: v=expression(l,b,n->as.group.expression); break;
    case TF_AST_BLOCK: v=lower_block(l,b,n); break;
    case TF_AST_UNARY: case TF_AST_BINARY: v=operation(l,b,n,type); break;
    case TF_AST_CALL: v=call(l,b,n,type); break;
    case TF_AST_FOR: v=comprehension(l,b,n,type); break;
    case TF_AST_CLOSURE: v=closure(l,b,n,type); break;
    case TF_AST_PROJECT: {
        const TfCoreValueId receiver=expression(l,b,n->as.project.base);
        const TfTypedNode *member=typed(l,n->id);
        v=emit(l,b,type,(TfCoreOperation){.kind=TF_CORE_OP_PROJECT,.operands=operands(l,&receiver,1),
                .as.field=member->member_index},n->span);
        break;
    }
    case TF_AST_IF: {
        const TfCoreValueId test=expression(l,b,n->as.conditional.condition);
        const TfAstNode *then=node(n->as.conditional.then_block), *otherwise=node(n->as.conditional.else_block);
        const TfCoreBlockId yes=new_block(l,then->span), no=new_block(l,otherwise->span);
        TfCoreValueId branch=expression(l,yes,then->id); finish(l,yes,branch,then->span);
        branch=expression(l,no,otherwise->id); finish(l,no,branch,otherwise->span);
        v=if_op(l,b,type,test,yes,no,n->span);
        break;
    }
    case TF_AST_LIST: {
        const TfNodeList elements=n->as.list.elements;
        TfCoreValueId *values=array(l,elements.count,sizeof *values);
        const TfConcreteType *list=tf_type_store_get(l->store,typed(l,n->id)->type);
        if (list==NULL || list->kind!=TF_CONCRETE_LIST) { v=fail_at(l,TF_CORE_LOWER_INVARIANT,n); break; }
        for (size_t i=0; i<elements.count && !l->failed; ++i) values[i]=expression(l,b,elements.items[i]);
        v=emit(l,b,type,(TfCoreOperation){.kind=TF_CORE_OP_LIST,.operands={values,elements.count},
               .as.element_type=tf_core_lower_type_id(l->types,list->as.element)},n->span);
        break;
    }
    default: v=fail_at(l,TF_CORE_LOWER_UNSUPPORTED_FORM,n); break;
    }
    --l->depth;
    return v;
}

static bool entry_signature(Lower *l, const TfAstNode *n, TfCoreTypeId result) {
    TfCoreProgramData d={0};
    tf_core_lower_type_view(l->types,&d);
    const TfCoreType *list=tf_core_type(&d,result);
    const TfCoreType *element=list!=NULL && list->kind==TF_CORE_TYPE_LIST ?
                             tf_core_type(&d,list->as.element) : NULL;
    return n->as.function.public_decl && spelling(n->as.function.name,"main") &&
           n->as.function.parameters.count==0 && element!=NULL && element->kind==TF_CORE_TYPE_TEXT;
}

static TfCoreValueId entry_body(Lower *l, TfCoreBlockId block, const TfAstNode *body,
                                TfCoreParameter snapshot) {
    const TfNodeList statements=body->as.block.statements;
    size_t inputs=0, outputs=0;
    for (size_t i=0; i<statements.count; ++i)
        if (node(statements.items[i])->kind==TF_AST_OUTPUT) ++outputs;
    TfCoreValueId *staged=array(l,outputs,sizeof *staged);
    TfCoreProgramData data={0};
    tf_core_lower_type_view(l->types,&data);
    const TfCoreTypeId text_type=tf_core_type(&data,snapshot.type)->as.element;
    size_t staged_count=0;
    for (size_t i=0; i<statements.count && !l->failed; ++i) {
        const TfAstNode *statement=node(statements.items[i]);
        if (statement->kind==TF_AST_INPUT) {
            const TfCoreValueId value=emit(l,block,text_type,(TfCoreOperation){.kind=TF_CORE_OP_INPUT_AT,
                .operands=operands(l,&snapshot.value,1),.as.input_index=inputs++},statement->span);
            if (!binding(l,statement,value)) return 0;
        } else if (statement->kind==TF_AST_OUTPUT) {
            staged[staged_count++]=expression(l,block,statement->as.output.value);
        } else if (statement->kind==TF_AST_LET) {
            const TfCoreValueId value=expression(l,block,statement->as.binding.value);
            if (!statement->as.binding.discard && !binding(l,statement,value)) return 0;
        } else return fail_at(l,TF_CORE_LOWER_INVARIANT,statement);
    }
    const TfCoreValueId returned=expression(l,block,body->as.block.final_expression);
    if (outputs==0 || l->failed) return returned;
    const TfCoreValueId prefix=emit(l,block,snapshot.type,(TfCoreOperation){.kind=TF_CORE_OP_LIST,
        .operands={staged,staged_count},.as.element_type=text_type},body->span);
    const TfCoreValueId values[]={prefix,returned};
    return emit(l,block,snapshot.type,(TfCoreOperation){.kind=TF_CORE_OP_BINARY,
        .operands=operands(l,values,2),.as.binary=TF_CORE_BINARY_CONCAT},body->span);
}

static bool function(Lower *l, const TfResolvedSymbol *s) {
    const TfAstNode *n=node(s->declaration);
    const TfConcreteType *signature=tf_type_store_get(l->store,tf_typed_program_symbol(l->typed,s->id));
    const size_t symbol=symbol_index(l,s->id);
    if (n==NULL || symbol==0 || signature==NULL || signature->kind!=TF_CONCRETE_FUNCTION ||
        signature->as.function.arity!=n->as.function.parameters.count)
        return fail_at(l,TF_CORE_LOWER_INVARIANT,n)!=0;
    const TfResolvedModule *module=tf_resolution_module(l->resolution,s->module);
    if (module==NULL) return fail_at(l,TF_CORE_LOWER_INVARIANT,n)!=0;
    l->blocks=NULL; l->block_count=0; l->block_capacity=0; l->value_count=0;
    l->function=l->targets[symbol]; l->module=module;
    const TfNodeList source_params=n->as.function.parameters;
    TfCoreFunction out={.kind=TF_CORE_FUNCTION_ORDINARY,.name=text(n->as.function.name),
        .module=tf_core_lower_module_name(module),.span=origin(l,n->span),.parameter_count=source_params.count,
        .result=tf_core_lower_type_id(l->types,signature->as.function.result)};
    const bool is_entry=module->is_root && entry_signature(l,n,out.result);
    if (s->entry_only) {
        if (!is_entry) return fail_at(l,TF_CORE_LOWER_INVARIANT,n)!=0;
        out.kind=TF_CORE_FUNCTION_ENTRY_ONLY;
        out.name=(TfCoreText){(const unsigned char *)"$entry",sizeof "$entry"-1};
        out.parameter_count=1;
    }
    TfCoreParameter *params=array(l,out.parameter_count,sizeof *params);
    out.parameters=params;
    if (s->entry_only && !l->failed)
        params[0]=(TfCoreParameter){fresh(l),out.result,
                                   {(const unsigned char *)"$stdin",sizeof "$stdin"-1},origin(l,n->span)};
    if (!parameters(l,params,source_params)) return false;
    const TfAstNode *body=node(n->as.function.body);
    out.body=new_block(l,body->span);
    if (l->failed) return false;
    const TfCoreValueId value=s->entry_only ? entry_body(l,out.body,body,params[0]) :
                                            expression(l,out.body,body->id);
    finish(l,out.body,value,body->span);
    if (!save_function(l,out)) return false;
    if (is_entry) l->entry=l->targets[symbol];
    return true;
}

TfStatus tf_core_lower(TfCoreProgram **out, const TfMachineProgram *p,
                       const TfAllocator *allocator, TfCoreLowerIssue *issue) {
    Lower l={.allocator=tf_allocator(allocator)};
    TfStatus status=TF_HOST_ERROR;
    if (issue!=NULL) *issue=(TfCoreLowerIssue){0};
    if (out==NULL) return TF_HOST_ERROR;
    *out=NULL;
    if (!tf_allocator_valid(l.allocator) || tf_diagnostic_phase_kind(tf_machine_program_phase(p))!=TF_DIAG_PHASE_VALID ||
        tf_diagnostic_phase_payload(tf_machine_program_phase(p))!=p) return TF_HOST_ERROR;
    if (tf_machine_program_count(p)!=0) {
        (void)fail_at(&l,TF_CORE_LOWER_UNSUPPORTED_FORM,node(tf_machine_program_at(p,0)->declaration));
        goto done;
    }
    l.scratch_allocator=(TfAllocator){&l,scratch_allocate,scratch_release};
    l.typed=tf_machine_program_typed(p);
    l.resolution=tf_typed_program_resolution(l.typed);
    l.store=tf_typed_program_store(l.typed);
    l.symbol_count=tf_resolution_symbol_count(l.resolution);
    if (l.symbol_count==SIZE_MAX) goto done;
    l.targets=array(&l,l.symbol_count+1,sizeof *l.targets);
    l.bindings=array(&l,l.symbol_count+1,sizeof *l.bindings);
    if (l.failed) goto done;
    memset(l.targets,0,(l.symbol_count+1)*sizeof *l.targets);
    for (size_t i=0; i<=l.symbol_count; ++i) l.bindings[i]=(Binding){0};
    if (!index_captures(&l)) goto done;
    for (size_t i=0; i<l.symbol_count; ++i) {
        const TfResolvedSymbol *s=tf_resolution_symbol_at(l.resolution,i);
        if (s->kind==TF_SYMBOL_FUNCTION) l.targets[s->id.index]=++l.function_count;
    }
    l.functions=array(&l,l.function_count,sizeof *l.functions);
    l.function_capacity=l.function_count;
    if (l.failed || tf_core_lower_types_create(&l.types,p,&l.allocator)!=TF_OK ||
        tf_core_lower_constants_create(&l.constants,l.types,&l.allocator)!=TF_OK) goto done;
    for (size_t i=0; i<l.symbol_count; ++i) {
        const TfResolvedSymbol *s=tf_resolution_symbol_at(l.resolution,i);
        if (s->kind==TF_SYMBOL_FUNCTION && !function(&l,s)) goto done;
    }
    TfCoreProgramData d={.functions=l.functions,.function_count=l.function_count,.entry=l.entry};
    tf_core_lower_type_view(l.types,&d);
    tf_core_lower_constant_view(l.constants,&d);
    status=tf_core_program_copy(out,&d,&l.allocator);
done:
    tf_core_lower_constants_destroy(l.constants);
    tf_core_lower_types_destroy(l.types);
    while (l.scratch!=NULL) {
        Scratch *next=l.scratch->next;
        l.allocator.deallocate(l.allocator.context,l.scratch);
        l.scratch=next;
    }
    if (issue!=NULL && status!=TF_OK) *issue=l.issue;
    return status;
}
