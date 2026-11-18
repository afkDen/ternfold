#include "check_internal.h"
#include "internal.h"
#include "type_operators.h"

#include <string.h>

typedef struct {
    const TfResolvedReference *reference;
    TfConcreteTypeId probed;
    TfStatus probed_status;
    bool probed_ready;
    TfTypedNode typed;
    bool has_literal;
    TfLiteralValue literal;
} NodeSlot;

typedef struct {
    const TfAst *ast;
    size_t count;
    NodeSlot *nodes;
} ModuleTable;

typedef struct {
    NodeSlot *node; /* non-NULL for a node write; symbol is otherwise non-NULL */
    TfConcreteTypeId *symbol;
    union {
        TfConcreteTypeId previous_type;
        struct {
            TfTypedNode typed;
            TfLiteralValue literal;
            bool has_literal;
        } previous_node;
    } previous;
} CheckUndo;

struct TfTypedProgram {
    const TfResolution *resolution;
    TfTypeStore *store;
    TfAllocator allocator;
    TfDiagnosticBag *diagnostics;
    TfDiagnosticPhaseResult phase;
    ModuleTable *modules;
    TfConcreteTypeId *symbols;
    size_t module_count, symbol_count;
    unsigned pending;
};

struct TfCheckContext {
    TfTypedProgram *program;
    TfTypedSignatures *signatures;
    const TfCheckAdapters *adapters;
    ModuleTable *module;
    size_t depth;
    CheckUndo *undo;
    size_t undo_count, undo_capacity, probes, probe_work;
    bool failed;
};

/* Recursive syntax is parser-bounded, but postfix/binary chains also grow
 * AST depth. Fail as a host resource error before exhausting the C stack. */
enum { EXPRESSION_DEPTH_LIMIT = 160, PROBE_WORK_LIMIT = 2*1024*1024 };

static void release(TfAllocator a, void *pointer) {
    if (pointer != NULL) a.deallocate(a.context,pointer);
}

static void *allocate(TfAllocator a, size_t count, size_t width) {
    if (count == 0 || count > SIZE_MAX / width) return NULL;
    return a.allocate(a.context,count * width);
}

static const TfAstNode *ast_node(TfNodeId id) { return tf_ast_get(id.owner,id); }

static TfSpan origin(TfNodeId id) {
    const TfAstNode *node = ast_node(id);
    return node != NULL ? node->span : (TfSpan){0};
}

static TfCheckExpectation expect(TfConcreteTypeId type, TfSpan span) {
    return (TfCheckExpectation){type,span,type.index == 0};
}

static TfDiagnosticText text(const char *value) {
    return (TfDiagnosticText){value,strlen(value)};
}

TfStatus tf_check_diagnostic(TfCheckContext *c, const char *code, const char *message,
                             TfSpan primary, TfSpan related) {
    TfDiagnosticLabel labels[] = {
        {primary,true,{0}}, {related,false,{0}}
    };
    TfDiagnosticRecord d = {0};
    if (c->failed) return TF_HOST_ERROR;
    labels[1].message = text("expected or declared here");
    d.code = code;
    d.severity = TF_DIAG_ERROR;
    d.message = text(message);
    d.labels = labels;
    d.label_count = related.source != NULL ? 2u : 1u;
    if (tf_diagnostic_bag_add(c->program->diagnostics,&d) != TF_OK) {
        c->failed = true;
        return TF_HOST_ERROR;
    }
    return TF_OK;
}

TfTypedSignatures *tf_check_signatures(TfCheckContext *c) { return c->signatures; }
const TfTypedProgram *tf_check_program(const TfCheckContext *c) { return c->program; }
const TfAllocator *tf_check_allocator(const TfCheckContext *c) { return &c->program->allocator; }

static NodeSlot *slot(const ModuleTable *m, TfNodeId id) {
    return m != NULL && id.owner == m->ast && id.index != 0 && id.index <= m->count
         ? &m->nodes[id.index - 1] : NULL;
}

static const ModuleTable *module_for(const TfTypedProgram *p, const TfAst *ast) {
    if (p != NULL && ast != NULL)
        for (size_t i = 0; i < p->module_count; ++i)
            if (p->modules[i].ast == ast) return &p->modules[i];
    return NULL;
}

static const TfResolvedReference *reference(TfCheckContext *c, TfNodeId id) {
    const NodeSlot *s = slot(c->module,id);
    if (s == NULL) { c->failed = true; return NULL; }
    return s->reference;
}

static TfSymbolId target(TfCheckContext *c, TfNodeId id) {
    const TfResolvedReference *r = reference(c,id);
    return r != NULL ? r->symbol : (TfSymbolId){0};
}

static const TfResolvedSymbol *resolved(TfCheckContext *c, TfNodeId id) {
    const TfResolvedSymbol *s = tf_resolution_symbol(c->program->resolution,target(c,id));
    if (s == NULL) c->failed = true;
    return s;
}

static bool save_undo(TfCheckContext *c, CheckUndo value) {
    void *buffer = c->undo;
    if (c->failed) return false;
    if (c->undo_count == SIZE_MAX ||
        !tf_grow(&buffer,&c->undo_capacity,c->undo_count+1,sizeof *c->undo,c->program->allocator)) {
        c->failed = true; return false;
    }
    c->undo = buffer;
    c->undo[c->undo_count++] = value;
    return true;
}

static bool save_node(TfCheckContext *c, NodeSlot *s) {
    return c->probes == 0 || save_undo(c,(CheckUndo){
        .node=s,.previous.previous_node={s->typed,s->literal,s->has_literal}});
}

static void restore_undo(TfCheckContext *c, size_t mark) {
    while (c->undo_count > mark) {
        const CheckUndo *undo = &c->undo[--c->undo_count];
        if (undo->node != NULL) {
            undo->node->typed = undo->previous.previous_node.typed;
            undo->node->literal = undo->previous.previous_node.literal;
            undo->node->has_literal = undo->previous.previous_node.has_literal;
        } else *undo->symbol = undo->previous.previous_type;
    }
}

static void record(TfCheckContext *c, TfNodeId id, TfConcreteTypeId type,
                   TfSymbolId symbol, size_t member) {
    NodeSlot *s;
    if (c->failed || type.index == 0) return;
    s = slot(c->module,id);
    if (s == NULL || tf_type_store_get(c->program->store,type) == NULL ||
        (symbol.index != 0 && tf_resolution_symbol(c->program->resolution,symbol) == NULL)) {
        c->failed = true;
        return;
    }
    if (symbol.index == 0 && s->reference != NULL) symbol = s->reference->symbol;
    if (save_node(c,s)) s->typed = (TfTypedNode){id,type,symbol,member};
}

static void bind(TfCheckContext *c, TfSymbolId id, TfConcreteTypeId type) {
    if (c->failed || type.index == 0) return;
    if (tf_resolution_symbol(c->program->resolution,id) == NULL ||
        tf_type_store_get(c->program->store,type) == NULL) {
        c->failed = true;
        return;
    }
    TfConcreteTypeId *value = &c->program->symbols[id.index - 1];
    if (c->probes == 0 ||
        save_undo(c,(CheckUndo){.symbol=value,.previous.previous_type=*value}))
        *value = type;
}

static TfConcreteTypeId primitive(TfCheckContext *c, TfConcreteTypeKind kind) {
    TfConcreteTypeId type = {0};
    if (!c->failed && tf_type_primitive(c->program->store,kind,&type) != TF_OK)
        c->failed = true;
    return type;
}

static TfConcreteTypeId list_type(TfCheckContext *c, TfConcreteTypeId element) {
    TfConcreteTypeId type = {0};
    if (!c->failed && element.index != 0 &&
        tf_type_list(c->program->store,element,&type) != TF_OK) c->failed = true;
    return type;
}

static TfConcreteTypeId list_element(const TfCheckContext *c, TfConcreteTypeId type) {
    const TfConcreteType *t = tf_type_store_get(c->program->store,type);
    return t != NULL && t->kind == TF_CONCRETE_LIST ? t->as.element : (TfConcreteTypeId){0};
}

static TfConcreteTypeId annotation(TfCheckContext *c, TfNodeId id) {
    TfConcreteTypeId type = {0};
    TfTypeIssue issue;
    if (c->failed) return type;
    switch (tf_type_from_syntax(c->program->store,id,&type,&issue)) {
    case TF_OK:
        record(c,id,type,(TfSymbolId){0},0);
        break;
    case TF_SOURCE_ERROR:
        (void)tf_check_diagnostic(c,issue.code,issue.message,
                                  issue.primary.source != NULL ? issue.primary : origin(id),
                                  issue.related);
        break;
    default: c->failed = true; break;
    }
    return type;
}

static bool conforms(TfCheckContext *c, TfConcreteTypeId type, TfCheckExpectation wanted,
                     TfSpan span) {
    if (type.index == 0) return false;
    if (wanted.type.index == 0 || tf_concrete_type_id_equal(type,wanted.type)) return true;
    (void)tf_check_diagnostic(c,"E302","expression does not have the expected type",span,wanted.origin);
    return false;
}

static TfConcreteTypeId expr(TfCheckContext *, TfNodeId, TfCheckExpectation, const TfAstNode *, bool);

TfConcreteTypeId tf_check_expression(TfCheckContext *c, TfNodeId id, TfCheckExpectation wanted) {
    return expr(c,id,wanted,NULL,false);
}

static TfConcreteTypeId argument(TfCheckContext *c, const TfAstNode *n, TfCheckExpectation wanted,
                                 TfSymbolId member, size_t index) {
    TfConcreteTypeId type = tf_check_expression(c,n->as.argument.value,wanted);
    record(c,n->id,type,member,index);
    return type;
}

TfConcreteTypeId tf_check_argument(TfCheckContext *c, TfNodeId id, TfCheckExpectation wanted) {
    const TfAstNode *n = ast_node(id);
    if (n == NULL || n->kind != TF_AST_ARGUMENT || slot(c->module,id) == NULL) {
        c->failed = true;
        return (TfConcreteTypeId){0};
    }
    return argument(c,n,wanted,(TfSymbolId){0},0);
}

TfStatus tf_check_probe_argument(TfCheckContext *c, TfNodeId id, TfConcreteTypeId *out) {
    const TfAstNode *argument = ast_node(id);
    TfDiagnosticBag *trial = NULL, *original;
    TfConcreteTypeId type;
    NodeSlot *slot_for_argument;
    size_t mark;
    unsigned pending;
    bool diagnosed;
    if (out == NULL) return TF_HOST_ERROR;
    *out = (TfConcreteTypeId){0};
    if (c == NULL || c->failed || argument == NULL || argument->kind != TF_AST_ARGUMENT ||
        (slot_for_argument = slot(c->module,id)) == NULL) return TF_HOST_ERROR;
    if (slot_for_argument->probed_ready) {
        *out = slot_for_argument->probed;
        return slot_for_argument->probed_status;
    }
    if (tf_diagnostic_bag_create(&trial,0,&c->program->allocator) != TF_OK) {
        c->failed = true; return TF_HOST_ERROR;
    }
    original = c->program->diagnostics;
    pending = c->program->pending;
    mark = c->undo_count;
    c->program->diagnostics = trial;
    c->program->pending = 0;
    ++c->probes;
    type = tf_check_expression(c,argument->as.argument.value,(TfCheckExpectation){.unavailable=true});
    diagnosed = tf_diagnostic_bag_has_errors(trial) || c->program->pending != 0;
    --c->probes;
    c->program->diagnostics = original;
    c->program->pending = pending;
    restore_undo(c,mark);
    tf_diagnostic_bag_destroy(trial);
    if (c->failed) return TF_HOST_ERROR;
    /* Independent synthesis has no expected type. Its success, deferred
     * context or independent error can all be reused by other trial walks.
     * Real checking still visits the expression and publishes diagnostics. */
    slot_for_argument->probed_ready = true;
    slot_for_argument->probed_status = diagnosed ? TF_SOURCE_ERROR : TF_OK;
    slot_for_argument->probed = diagnosed ? (TfConcreteTypeId){0} : type;
    *out = slot_for_argument->probed;
    return slot_for_argument->probed_status;
}

static TfConcreteTypeId literal(TfCheckContext *c, const TfAstNode *n, const TfAstNode *parent) {
    static const TfConcreteTypeKind kinds[] = {
        TF_CONCRETE_INT, TF_CONCRETE_FLOAT, TF_CONCRETE_BOOL,
        TF_CONCRETE_TEXT, TF_CONCRETE_CHAR, TF_CONCRETE_UNIT
    };
    NodeSlot *s = slot(c->module,n->id);
    TfLiteralValue value;
    TfStatus status = tf_literal_value(n,parent,&value);
    if (status == TF_OK) {
        if (save_node(c,s)) {
            s->literal = value;
            s->has_literal = true;
        }
        return primitive(c,kinds[value.kind]);
    }
    if (status == TF_SOURCE_ERROR) {
        (void)tf_check_diagnostic(c,"E301","literal is outside its finite type range",
                                  n->span,(TfSpan){0});
    } else c->failed = true;
    return (TfConcreteTypeId){0};
}

static TfConcreteTypeId list(TfCheckContext *c, const TfAstNode *n, TfCheckExpectation wanted) {
    const TfNodeList items = n->as.list.elements;
    TfCheckExpectation element = wanted;
    bool valid = true;
    element.type = list_element(c,wanted.type);
    if (wanted.type.index != 0 && element.type.index == 0) {
        (void)tf_check_diagnostic(c,"E302","list expression requires a List type",n->span,wanted.origin);
        element.unavailable = true;
        valid = false;
    }
    if (items.count == 0 && element.type.index == 0) {
        if (!element.unavailable)
            (void)tf_check_diagnostic(c,"E304","empty list needs an expected List element type",
                                      n->span,(TfSpan){0});
        return (TfConcreteTypeId){0};
    }
    for (size_t i = 0; i < items.count && !c->failed; ++i) {
        TfConcreteTypeId type = tf_check_expression(c,items.items[i],element);
        if (type.index == 0) valid = false;
        if (i == 0 && element.type.index == 0) element = expect(type,origin(items.items[i]));
    }
    return valid ? list_type(c,element.type) : (TfConcreteTypeId){0};
}

static TfConcreteTypeId conditional(TfCheckContext *c, const TfAstNode *n, TfCheckExpectation wanted) {
    const TfNodeId condition = n->as.conditional.condition, yes = n->as.conditional.then_block;
    TfConcreteTypeId test = tf_check_expression(c,condition,expect(primitive(c,TF_CONCRETE_BOOL),(TfSpan){0}));
    TfConcreteTypeId left = tf_check_expression(c,yes,wanted);
    TfCheckExpectation other = wanted.type.index != 0 ? wanted : expect(left,origin(yes));
    TfConcreteTypeId right = tf_check_expression(c,n->as.conditional.else_block,other);
    return test.index != 0 && right.index != 0 ? left : (TfConcreteTypeId){0};
}

static TfConcreteTypeId loop(TfCheckContext *c, const TfAstNode *n, TfCheckExpectation wanted) {
    const TfNodeId iterable = n->as.loop.iterable;
    TfConcreteTypeId collection = tf_check_expression(c,iterable,(TfCheckExpectation){0});
    TfConcreteTypeId element = list_element(c,collection);
    const bool expected_list = wanted.type.index == 0 || list_element(c,wanted.type).index != 0;
    TfConcreteTypeId body;
    if (collection.index != 0 && element.index == 0)
        (void)tf_check_diagnostic(c,"E302","for iterable must be a List",origin(iterable),(TfSpan){0});
    bind(c,target(c,n->id),element);
    wanted.type = list_element(c,wanted.type);
    if (!expected_list) {
        (void)tf_check_diagnostic(c,"E302","for expression requires a List result type",n->span,wanted.origin);
        wanted.unavailable = true;
    }
    body = tf_check_expression(c,n->as.loop.body,wanted);
    return expected_list && element.index != 0 ? list_type(c,body) : (TfConcreteTypeId){0};
}

static void statement(TfCheckContext *c, const TfAstNode *n) {
    TfConcreteTypeId declared = {0}, value;
    TfCheckExpectation wanted = {0};
    switch (n->kind) {
    case TF_AST_LET:
        if (n->as.binding.type.index != 0) {
            declared = annotation(c,n->as.binding.type);
            wanted = expect(declared,origin(n->as.binding.type));
        }
        value = tf_check_expression(c,n->as.binding.value,wanted);
        if (!n->as.binding.discard)
            bind(c,target(c,n->id),declared.index != 0 ? declared : value);
        if (!wanted.unavailable) record(c,n->id,value,(TfSymbolId){0},0);
        break;
    case TF_AST_INPUT: /* Root outer-block restriction is guaranteed by M05. */
        declared = annotation(c,n->as.input.type);
        value = primitive(c,TF_CONCRETE_TEXT);
        bind(c,target(c,n->id),value);
        if (conforms(c,declared,expect(value,(TfSpan){0}),origin(n->as.input.type)))
            record(c,n->id,value,(TfSymbolId){0},0);
        break;
    case TF_AST_OUTPUT:
        value = tf_check_expression(c,n->as.output.value,
                                     expect(primitive(c,TF_CONCRETE_TEXT),(TfSpan){0}));
        if (value.index != 0) record(c,n->id,primitive(c,TF_CONCRETE_UNIT),(TfSymbolId){0},0);
        break;
    default: c->failed = true; break;
    }
}

static TfConcreteTypeId block(TfCheckContext *c, const TfAstNode *n, TfCheckExpectation wanted) {
    const TfNodeList statements = n->as.block.statements;
    bool valid = true;
    for (size_t i = 0; i < statements.count && !c->failed; ++i) {
        const TfNodeId id = statements.items[i];
        statement(c,ast_node(id));
        if (slot(c->module,id)->typed.type.index == 0) valid = false;
    }
    TfConcreteTypeId result = tf_check_expression(c,n->as.block.final_expression,wanted);
    return valid ? result : (TfConcreteTypeId){0};
}

static TfConcreteTypeId closure(TfCheckContext *c, const TfAstNode *n) {
    const TfNodeList parameters = n->as.closure.parameters;
    TfConcreteTypeId *types = NULL, signature = {0};
    TfConcreteTypeId result = annotation(c,n->as.closure.result_type);
    bool valid = result.index != 0;
    if (parameters.count != 0 && !c->failed) {
        types = allocate(c->program->allocator,parameters.count,sizeof *types);
        if (types == NULL) c->failed = true;
    }
    for (size_t i = 0; i < parameters.count && !c->failed; ++i) {
        const TfNodeId param = parameters.items[i];
        types[i] = annotation(c,ast_node(param)->as.parameter.type);
        if (types[i].index == 0) valid = false;
        bind(c,target(c,param),types[i]);
        record(c,param,types[i],(TfSymbolId){0},0);
    }
    if (tf_check_expression(c,n->as.closure.body,expect(result,origin(n->as.closure.result_type))).index == 0)
        valid = false;
    if (valid && !c->failed && tf_type_function(c->program->store,types,parameters.count,
                                                result,&signature) != TF_OK) c->failed = true;
    release(c->program->allocator,types);
    return signature;
}

static TfConcreteTypeId operation(TfCheckContext *c, const TfAstNode *n, TfCheckExpectation wanted) {
    TfConcreteTypeId left, right = {0}, type = {0};
    TfTokenKind op;
    TfSpan span;
    const char *code;
    if (n->kind == TF_AST_UNARY) {
        op = n->as.unary.op;
        span = n->as.unary.operator_span;
        TfCheckExpectation operand = {0};
        const TfConcreteType *expected = tf_type_store_get(c->program->store,wanted.type);
        if (op == TF_BANG) operand = expect(primitive(c,TF_CONCRETE_BOOL),(TfSpan){0});
        else if (expected != NULL && (expected->kind == TF_CONCRETE_INT || expected->kind == TF_CONCRETE_FLOAT))
            operand = wanted;
        left = expr(c,n->as.unary.operand,operand,n,false);
    } else {
        TfCheckExpectation lhs = {0}, rhs = {0};
        const TfNodeId lhs_node = n->as.binary.left;
        const TfConcreteType *expected = tf_type_store_get(c->program->store,wanted.type);
        op = n->as.binary.op;
        span = n->as.binary.operator_span;
        switch (op) {
        case TF_AND_AND: case TF_OR_OR:
            lhs = expect(primitive(c,TF_CONCRETE_BOOL),(TfSpan){0});
            break;
        case TF_DIV: case TF_PERCENT:
            lhs = expect(primitive(c,TF_CONCRETE_INT),(TfSpan){0});
            break;
        case TF_PLUS: case TF_MINUS: case TF_STAR: case TF_CARET:
            if (expected != NULL && (expected->kind == TF_CONCRETE_INT || expected->kind == TF_CONCRETE_FLOAT))
                lhs = wanted;
            break;
        case TF_PLUS_PLUS:
            if (expected != NULL && (expected->kind == TF_CONCRETE_LIST || expected->kind == TF_CONCRETE_TEXT))
                lhs = wanted;
            else lhs.unavailable = wanted.unavailable;
            break;
        default: break;
        }
        left = tf_check_expression(c,lhs_node,lhs);
        if (op == TF_CARET) rhs = expect(primitive(c,TF_CONCRETE_INT),(TfSpan){0});
        else if (op != TF_SLASH) rhs = lhs.type.index != 0 ? lhs : expect(left,origin(lhs_node));
        right = tf_check_expression(c,n->as.binary.right,rhs);
        if (right.index == 0) return type;
    }
    if (left.index == 0 || c->failed) return type;
    switch (tf_type_operator(c->signatures,op,left,right,&type,&code)) {
    case TF_OK: break;
    case TF_SOURCE_ERROR:
        (void)tf_check_diagnostic(c,code,strcmp(code,"E305") == 0 ?
            "equality is unavailable for values containing functions" : "invalid operand types for operator",
            span,(TfSpan){0});
        break;
    default: c->failed = true; break;
    }
    return type;
}

/* Member nodes have their own declaration IDs. Walk the declaration order,
 * ignoring transition clauses when indexing a machine's stored cells. */
static const TfResolvedSymbol *member(TfCheckContext *c, TfConcreteTypeId receiver,
                                      TfAstText name, size_t *index) {
    const TfConcreteType *type = tf_type_store_get(c->program->store,receiver);
    const TfResolvedType *nominal;
    const TfAstNode *declaration;
    TfNodeList members;
    size_t ordinal = 0;
    if (type == NULL || (type->kind != TF_CONCRETE_RECORD && type->kind != TF_CONCRETE_MACHINE)) return NULL;
    nominal = tf_resolution_type(c->program->resolution,type->as.nominal);
    declaration = nominal != NULL ? ast_node(nominal->declaration) : NULL;
    if (declaration == NULL) { c->failed = true; return NULL; }
    members = type->kind == TF_CONCRETE_RECORD ? declaration->as.aggregate.members : declaration->as.machine.sections;
    for (size_t i = 0; i < members.count; ++i) {
        const TfNodeId id = members.items[i];
        const TfAstNode *m = ast_node(id);
        TfAstText candidate;
        if (m->kind == TF_AST_PARAMETER) candidate = m->as.parameter.name;
        else if (m->kind == TF_AST_STATE) candidate = m->as.state.name;
        else continue;
        if (candidate.length == name.length && memcmp(candidate.data,name.data,name.length) == 0) {
            const NodeSlot *s = slot(module_for(c->program,id.owner),id);
            const TfResolvedSymbol *symbol = s != NULL && s->reference != NULL ?
                tf_resolution_symbol(c->program->resolution,s->reference->symbol) : NULL;
            if (symbol == NULL) c->failed = true;
            *index = ordinal;
            return symbol;
        }
        ++ordinal;
    }
    return NULL;
}

static TfSpan symbol_annotation(const TfResolvedSymbol *symbol) {
    const TfAstNode *d = ast_node(symbol->declaration);
    if (d != NULL) {
        if (d->kind == TF_AST_PARAMETER) return origin(d->as.parameter.type);
        if (d->kind == TF_AST_STATE) return origin(d->as.state.type);
    }
    return symbol->name.span;
}

static TfConcreteTypeId project(TfCheckContext *c, const TfAstNode *n, TfSymbolId *symbol, size_t *index) {
    const TfConcreteTypeId base = tf_check_expression(c,n->as.project.base,(TfCheckExpectation){0});
    if (base.index != 0) {
        const TfResolvedSymbol *m = member(c,base,n->as.project.name,index);
        if (m != NULL) {
            *symbol = m->id;
            return tf_typed_program_symbol(c->program,*symbol);
        }
        (void)tf_check_diagnostic(c,"E302","receiver does not have this field",n->as.project.name.span,
                                  origin(n->as.project.base));
    }
    return (TfConcreteTypeId){0};
}

static bool builtin_symbol(const TfResolvedSymbol *s) {
    return s->kind == TF_SYMBOL_BUILTIN || (s->kind == TF_SYMBOL_VARIANT && s->declaration.index == 0);
}

static void recover_arguments(TfCheckContext *c, const TfAstNode *call) {
    const TfNodeList arguments = call->as.call.arguments;
    for (size_t i = 0; i < arguments.count && !c->failed; ++i)
        (void)tf_check_argument(c,arguments.items[i],(TfCheckExpectation){.unavailable = true});
}

static TfConcreteTypeId builtin(TfCheckContext *c, const TfAstNode *use,
                                const TfResolvedSymbol *symbol, TfCheckExpectation wanted) {
    TfConcreteTypeId type = {0}, callable = {0};
    const bool is_call = use->kind == TF_AST_CALL;
    TfStatus status;
    if (c->adapters == NULL || c->adapters->builtin == NULL) {
        c->program->pending |= TF_CHECK_PENDING_BUILTINS;
        if (is_call) recover_arguments(c,use);
        return type;
    }
    status = c->adapters->builtin(c,use,symbol,wanted,&type,&callable);
    if (status == TF_SOURCE_ERROR) {
        if (!tf_diagnostic_bag_has_errors(c->program->diagnostics)) c->failed = true;
        return (TfConcreteTypeId){0};
    }
    if (status != TF_OK || (type.index != 0 && tf_type_store_get(c->program->store,type) == NULL)) {
        c->failed = true;
        return (TfConcreteTypeId){0};
    }
    if (is_call && type.index != 0 && !c->failed) {
        const TfConcreteType *signature = tf_type_store_get(c->program->store,callable);
        const TfNodeList arguments = use->as.call.arguments;
        if (signature == NULL || signature->kind != TF_CONCRETE_FUNCTION ||
            signature->as.function.arity != arguments.count ||
            !tf_concrete_type_id_equal(signature->as.function.result,type)) c->failed = true;
        for (size_t i = 0; i < arguments.count && !c->failed; ++i) {
            const TfAstNode *a = ast_node(arguments.items[i]);
            const NodeSlot *s = slot(c->module,a->id);
            if (a->as.argument.named || !tf_concrete_type_id_equal(s->typed.type,signature->as.function.parameters[i]))
                c->failed = true;
        }
        record(c,use->as.call.callee,callable,symbol->id,0);
    }
    return type;
}

static TfConcreteTypeId path(TfCheckContext *c, const TfAstNode *n, TfCheckExpectation wanted,
                             bool direct) {
    const TfResolvedSymbol *s = resolved(c,n->id);
    TfConcreteTypeId type = {0};
    if (s == NULL) return type;
    if (builtin_symbol(s)) return builtin(c,n,s,wanted);
    type = tf_typed_program_symbol(c->program,s->id);
    if (!direct && (s->kind == TF_SYMBOL_CONSTRUCTOR ||
        (s->kind == TF_SYMBOL_VARIANT && ast_node(s->declaration)->as.variant.types.count != 0))) {
        (void)tf_check_diagnostic(c,"E303","constructor is only usable in direct construction syntax",
                                  n->span,s->name.span);
        return (TfConcreteTypeId){0};
    }
    return type;
}

static TfSpan parameter_origin(const TfResolvedSymbol *callee, size_t index) {
    const TfAstNode *d = callee != NULL ? ast_node(callee->declaration) : NULL;
    if (d == NULL) return (TfSpan){0};
    if (d->kind == TF_AST_FUNCTION) {
        const TfAstNode *p = ast_node(d->as.function.parameters.items[index]);
        return origin(p->as.parameter.type);
    }
    if (d->kind == TF_AST_VARIANT) return origin(d->as.variant.types.items[index]);
    return callee->name.span;
}

static TfConcreteTypeId call(TfCheckContext *c, const TfAstNode *n, TfCheckExpectation wanted,
                             TfSymbolId *target_symbol) {
    const TfAstNode *callee = ast_node(n->as.call.callee);
    const TfResolvedSymbol *symbol = callee->kind == TF_AST_PATH ? resolved(c,callee->id) : NULL;
    const TfNodeList arguments = n->as.call.arguments;
    TfConcreteTypeId callable;
    const TfConcreteType *signature;
    const TfAstNode *decl = symbol != NULL ? ast_node(symbol->declaration) : NULL;
    const TfSpan declared = symbol != NULL ? symbol->name.span : callee->span;
    const bool is_record = symbol != NULL && symbol->kind == TF_SYMBOL_CONSTRUCTOR &&
                           decl != NULL && decl->kind == TF_AST_RECORD;
    bool valid, *seen = NULL;
    if (symbol != NULL) {
        *target_symbol = symbol->id;
        if (builtin_symbol(symbol)) return builtin(c,n,symbol,wanted);
    }
    callable = expr(c,callee->id,(TfCheckExpectation){0},NULL,true);
    signature = tf_type_store_get(c->program->store,callable);
    if (signature == NULL || signature->kind != TF_CONCRETE_FUNCTION) {
        if (signature != NULL) (void)tf_check_diagnostic(c,"E303","callee is not callable",
                                                        callee->span,declared);
        recover_arguments(c,n);
        return (TfConcreteTypeId){0};
    }
    valid = signature->as.function.arity == arguments.count;
    if (!valid) (void)tf_check_diagnostic(c,"E303",is_record ? "wrong number of record fields" : "wrong call arity",
                                         n->span,declared);
    if (is_record && signature->as.function.arity != 0 && !c->failed) {
        seen = allocate(c->program->allocator,signature->as.function.arity,sizeof *seen);
        if (seen == NULL) c->failed = true;
        else for (size_t i = 0; i < signature->as.function.arity; ++i) seen[i] = false;
    }
    for (size_t i = 0; i < arguments.count && !c->failed; ++i) {
        const TfAstNode *a = ast_node(arguments.items[i]);
        const TfResolvedSymbol *field = NULL;
        size_t index = i;
        TfCheckExpectation parameter = {.unavailable = true};
        if (is_record) {
            if (!a->as.argument.named)
                (void)tf_check_diagnostic(c,"E303","record construction requires named fields",a->span,declared);
            else {
                field = member(c,signature->as.function.result,a->as.argument.name,&index);
                if (field == NULL)
                    (void)tf_check_diagnostic(c,"E303","unknown record field",a->as.argument.name.span,declared);
                else if (index >= signature->as.function.arity) c->failed = true;
                else {
                    parameter = expect(signature->as.function.parameters[index],symbol_annotation(field));
                    if (seen[index]) {
                        (void)tf_check_diagnostic(c,"E303","duplicate record field",a->as.argument.name.span,
                                                  field->name.span);
                        valid = false;
                    }
                    seen[index] = true;
                }
            }
            if (field == NULL) valid = false;
        } else if (a->as.argument.named) {
            (void)tf_check_diagnostic(c,"E303","call requires positional arguments",a->as.argument.name.span,declared);
            valid = false;
        } else if (i < signature->as.function.arity) {
            parameter = expect(signature->as.function.parameters[i],parameter_origin(symbol,i));
        }
        if (argument(c,a,parameter,field != NULL ? field->id : (TfSymbolId){0},
                     field != NULL ? index : 0).index == 0) valid = false;
    }
    release(c->program->allocator,seen);
    return valid && !c->failed ? signature->as.function.result : (TfConcreteTypeId){0};
}

static bool pattern_records(TfCheckContext *c, const TfTypedPatterns *patterns, TfNodeId id) {
    const TfAstNode *n = ast_node(id);
    TfTypedNode typed = {0};
    TfLiteralValue value = {0};
    bool has_literal = false, valid = true;
    if (c->failed) return false;
    if (c->adapters->pattern_node != NULL &&
        c->adapters->pattern_node(patterns,id,&typed,&value,&has_literal)) {
        if (typed.node.owner != id.owner || typed.node.index != id.index || typed.type.index == 0 ||
            has_literal != (n->kind == TF_AST_PATTERN_LITERAL || n->kind == TF_AST_PATTERN_UNIT)) {
            c->failed = true;
            return false;
        }
        record(c,id,typed.type,typed.target,typed.member_index);
        if (has_literal) {
            NodeSlot *s = slot(c->module,id);
            if (save_node(c,s)) {
                s->literal = value;
                s->has_literal = true;
            }
        }
    } else valid = false;
    if (n->kind == TF_AST_PATTERN_BINDER) {
        const TfSymbolId binder = target(c,id);
        TfConcreteTypeId type = c->adapters->pattern_binding(patterns,binder);
        bind(c,binder,type);
        if (typed.type.index != 0 && !tf_concrete_type_id_equal(typed.type,type)) c->failed = true;
        if (typed.type.index == 0) record(c,id,type,binder,0);
    } else if (n->kind == TF_AST_PATTERN_VARIANT) {
        const TfNodeList children = n->as.pattern_variant.arguments;
        for (size_t i = 0; i < children.count && !c->failed; ++i)
            if (!pattern_records(c,patterns,children.items[i])) valid = false;
    }
    return valid && !c->failed;
}

static TfConcreteTypeId match(TfCheckContext *c, const TfAstNode *n, TfCheckExpectation wanted) {
    TfTypedPatterns *patterns = NULL;
    const TfNodeList arms = n->as.matching.arms;
    const TfConcreteTypeId scrutinee = tf_check_expression(c,n->as.matching.value,(TfCheckExpectation){0});
    TfConcreteTypeId result = {0};
    bool valid = scrutinee.index != 0;
    if (c->adapters == NULL || c->adapters->patterns == NULL || c->adapters->pattern_node == NULL) {
        c->program->pending |= TF_CHECK_PENDING_PATTERNS;
        valid = false;
    }
    if (!c->failed && scrutinee.index != 0 && c->adapters != NULL && c->adapters->patterns != NULL &&
        (c->adapters->patterns(&patterns,c->program->resolution,c->signatures,n,scrutinee,
                              c->program->diagnostics,&c->program->allocator) != TF_OK || patterns == NULL))
        c->failed = true;
    for (size_t i = 0; i < arms.count && !c->failed; ++i) {
        const TfAstNode *arm = ast_node(arms.items[i]);
        if (patterns != NULL && !pattern_records(c,patterns,arm->as.arm.pattern)) valid = false;
        TfConcreteTypeId type = tf_check_expression(c,arm->as.arm.body,wanted);
        record(c,arm->id,type,(TfSymbolId){0},0);
        if (i == 0) {
            result = type;
            if (wanted.type.index == 0) wanted = expect(type,origin(arm->as.arm.body));
        }
        if (type.index == 0) valid = false;
    }
    if (patterns != NULL) c->adapters->patterns_destroy(patterns);
    return valid ? result : (TfConcreteTypeId){0};
}

static TfConcreteTypeId expr(TfCheckContext *c, TfNodeId id, TfCheckExpectation wanted,
                             const TfAstNode *parent, bool direct) {
    TfConcreteTypeId type = {0};
    TfSymbolId symbol = {0};
    size_t index = 0;
    const TfAstNode *n;
    if (c->failed) return type;
    if (c->probes != 0) {
        if (c->probe_work == 0) { c->failed = true; return type; }
        --c->probe_work;
    }
    if (c->depth == EXPRESSION_DEPTH_LIMIT || slot(c->module,id) == NULL ||
        (n = ast_node(id)) == NULL) {
        c->failed = true;
        return type;
    }
    ++c->depth;
    switch (n->kind) {
    case TF_AST_PATH: type = path(c,n,wanted,direct); break;
    case TF_AST_NEXT: type = tf_typed_program_symbol(c->program,target(c,id)); break;
    case TF_AST_UNIT: case TF_AST_LITERAL: type = literal(c,n,parent); break;
    case TF_AST_GROUP: type = tf_check_expression(c,n->as.group.expression,wanted); break;
    case TF_AST_LIST: type = list(c,n,wanted); break;
    case TF_AST_UNARY: case TF_AST_BINARY: type = operation(c,n,wanted); break;
    case TF_AST_CALL: type = call(c,n,wanted,&symbol); break;
    case TF_AST_PROJECT: type = project(c,n,&symbol,&index); break;
    case TF_AST_BLOCK: type = block(c,n,wanted); break;
    case TF_AST_IF: type = conditional(c,n,wanted); break;
    case TF_AST_MATCH: type = match(c,n,wanted); break;
    case TF_AST_CLOSURE: type = closure(c,n); break;
    case TF_AST_FOR: type = loop(c,n,wanted); break;
    default: c->failed = true; break;
    }
    --c->depth;
    if (c->failed || !conforms(c,type,wanted,n->span)) return (TfConcreteTypeId){0};
    record(c,id,type,symbol,index);
    return type;
}

static void record_parameter(TfCheckContext *c, TfNodeId id, TfConcreteTypeId type) {
    const TfAstNode *n = ast_node(id);
    if (type.index == 0 || n == NULL || n->kind != TF_AST_PARAMETER) { c->failed = true; return; }
    record(c,id,type,(TfSymbolId){0},0);
    record(c,n->as.parameter.type,type,(TfSymbolId){0},0);
}

static bool function(TfCheckContext *c, const TfAstNode *n) {
    const TfResolvedSymbol *s = resolved(c,n->id);
    TfConcreteTypeId type = s != NULL ? tf_typed_program_symbol(c->program,s->id) : (TfConcreteTypeId){0};
    const TfConcreteType *signature = tf_type_store_get(c->program->store,type);
    const TfNodeId result_type = n->as.function.result_type;
    bool valid = true;
    if (signature == NULL || signature->kind != TF_CONCRETE_FUNCTION ||
        signature->as.function.arity != n->as.function.parameters.count) {
        c->failed = true;
        return false;
    }
    for (size_t i = 0; i < signature->as.function.arity && !c->failed; ++i)
        record_parameter(c,n->as.function.parameters.items[i],signature->as.function.parameters[i]);
    record(c,result_type,signature->as.function.result,(TfSymbolId){0},0);
    if (s->entry_only) {
        TfConcreteTypeId entry_result = list_type(c,primitive(c,TF_CONCRETE_TEXT));
        valid = conforms(c,signature->as.function.result,expect(entry_result,(TfSpan){0}),origin(result_type));
    }
    if (tf_check_expression(c,n->as.function.body,expect(signature->as.function.result,origin(result_type))).index == 0)
        valid = false;
    record(c,n->id,type,s->id,0);
    return valid && !c->failed;
}

static bool state(TfCheckContext *c, const TfAstNode *section) {
    TfConcreteTypeId type = tf_typed_program_symbol(c->program,target(c,section->id));
    TfStatus status;
    if (type.index == 0) { c->failed = true; return false; }
    record(c,section->as.state.type,type,(TfSymbolId){0},0);
    record(c,section->id,type,(TfSymbolId){0},0);
    const TfConcreteTypeId initializer = tf_check_expression(c,section->as.state.value,
                                                   expect(type,origin(section->as.state.type)));
    if (c->adapters == NULL || c->adapters->constant == NULL) {
        c->program->pending |= TF_CHECK_PENDING_CONSTANTS;
        return false;
    }
    if (initializer.index == 0 || c->failed) return false;
    status = c->adapters->constant(c,section);
    if (status != TF_OK && (status != TF_SOURCE_ERROR ||
        !tf_diagnostic_bag_has_errors(c->program->diagnostics))) c->failed = true;
    return status == TF_OK && !c->failed;
}

static bool machine(TfCheckContext *c, const TfAstNode *n) {
    const TfResolvedSymbol *s = resolved(c,n->id);
    TfMachineSignature signature;
    const TfNodeList sections = n->as.machine.sections;
    bool valid = true;
    if (s == NULL || !tf_signatures_machine(c->signatures,s->type,&signature)) {
        c->failed = true;
        return false;
    }
    record_parameter(c,n->as.machine.parameter,signature.input);
    record(c,n->id,tf_typed_program_symbol(c->program,s->id),s->id,0);
    record(c,n->as.machine.yields,signature.output,(TfSymbolId){0},0);
    record(c,n->as.machine.rejects,signature.rejection,(TfSymbolId){0},0);
    for (size_t i = 0; i < sections.count && !c->failed; ++i) {
        const TfAstNode *section = ast_node(sections.items[i]);
        TfConcreteTypeId type = {0};
        switch (section->kind) {
        case TF_AST_STATE:
            if (!state(c,section)) valid = false;
            continue;
        case TF_AST_WRITER: {
            const TfResolvedSymbol *cell = resolved(c,section->id);
            if (cell == NULL) break;
            type = tf_check_expression(c,section->as.writer.value,
                       expect(tf_typed_program_symbol(c->program,cell->id),symbol_annotation(cell)));
            break;
        }
        case TF_AST_GUARD: {
            TfConcreteTypeId condition = tf_check_expression(c,section->as.guard.condition,
                                     expect(primitive(c,TF_CONCRETE_BOOL),(TfSpan){0}));
            TfConcreteTypeId rejection = tf_check_expression(c,section->as.guard.rejection,
                                     expect(signature.rejection,origin(n->as.machine.rejects)));
            if (condition.index != 0 && rejection.index != 0) type = primitive(c,TF_CONCRETE_UNIT);
            break;
        }
        case TF_AST_EMIT:
            if (tf_check_expression(c,section->as.emission.value,
                    expect(signature.output,origin(n->as.machine.yields))).index != 0)
                type = primitive(c,TF_CONCRETE_UNIT);
            break;
        default: c->failed = true; break;
        }
        if (type.index == 0) valid = false;
        record(c,section->id,type,(TfSymbolId){0},0);
    }
    return valid && !c->failed;
}

static void aggregate(TfCheckContext *c, const TfAstNode *n) {
    const TfResolvedReference *ref = reference(c,n->id);
    const TfNodeList members = n->as.aggregate.members;
    TfConcreteTypeId type = {0};
    if (ref == NULL || tf_type_nominal(c->program->store,ref->type,&type) != TF_OK) {
        c->failed = true;
        return;
    }
    record(c,n->id,type,ref->symbol,0);
    for (size_t i = 0; i < members.count && !c->failed; ++i) {
        const TfNodeId id = members.items[i];
        TfConcreteTypeId member_type = tf_typed_program_symbol(c->program,target(c,id));
        if (n->kind == TF_AST_RECORD) record_parameter(c,id,member_type);
        else {
            const TfAstNode *v = ast_node(id);
            const TfConcreteType *variant = tf_type_store_get(c->program->store,member_type);
            if (variant == NULL) { c->failed = true; break; }
            record(c,id,member_type,(TfSymbolId){0},0);
            if (v->as.variant.types.count != 0) {
                if (variant->kind != TF_CONCRETE_FUNCTION ||
                    variant->as.function.arity != v->as.variant.types.count) { c->failed = true; break; }
                for (size_t j = 0; j < variant->as.function.arity; ++j)
                    record(c,v->as.variant.types.items[j],variant->as.function.parameters[j],(TfSymbolId){0},0);
            }
        }
    }
}

static bool init_tables(TfTypedProgram *p, const TfTypedSignatures *signatures) {
    const TfResolution *r = p->resolution;
    size_t modules = tf_resolution_module_count(r), symbols = tf_resolution_symbol_count(r);
    if (modules == 0 || symbols == 0) return false;
    p->modules = allocate(p->allocator,modules,sizeof *p->modules);
    p->symbols = allocate(p->allocator,symbols,sizeof *p->symbols);
    if (p->modules == NULL || p->symbols == NULL) return false;
    for (size_t i = 0; i < modules; ++i) p->modules[i] = (ModuleTable){0};
    p->module_count = modules;
    p->symbol_count = symbols;
    for (size_t i = 0; i < modules; ++i) {
        const TfResolvedModule *m = tf_resolution_module_at(r,i);
        ModuleTable *table = &p->modules[i];
        table->ast = m->ast;
        table->count = tf_ast_count(m->ast);
        table->nodes = allocate(p->allocator,table->count,sizeof *table->nodes);
        if (table->nodes == NULL) return false;
        for (size_t j = 0; j < table->count; ++j) table->nodes[j] = (NodeSlot){0};
    }
    for (size_t i = 0; i < symbols; ++i) {
        const TfResolvedSymbol *s = tf_resolution_symbol_at(r,i);
        const TfConcreteTypeId type = tf_signatures_symbol(signatures,s->id);
        if (type.index == 0) {
            switch (s->kind) {
            case TF_SYMBOL_FUNCTION: case TF_SYMBOL_CONSTRUCTOR: case TF_SYMBOL_FIELD:
            case TF_SYMBOL_CELL: case TF_SYMBOL_MACHINE_INPUT:
                return false;
            case TF_SYMBOL_PARAMETER:
                if (s->closure.index == 0) return false;
                break;
            case TF_SYMBOL_VARIANT:
                if (s->declaration.index != 0) return false;
                break;
            default: break;
            }
        }
        p->symbols[i] = type;
    }
    for (size_t i = 0; i < tf_resolution_reference_count(r); ++i) {
        const TfResolvedReference *ref = tf_resolution_reference_at(r,i);
        NodeSlot *s = slot(module_for(p,ref->node.owner),ref->node);
        if (s == NULL) return false;
        s->reference = ref;
    }
    return true;
}

TfStatus tf_check_with_adapters(TfTypedProgram **out, const TfResolution *resolution,
                                TfTypedSignatures *signatures, size_t diagnostic_cap,
                                const TfAllocator *allocator, const TfCheckAdapters *adapters) {
    const TfAllocator a = tf_allocator(allocator);
    TfTypedProgram *program;
    TfCheckContext c = {0};
    bool complete = true;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (!tf_allocator_valid(a) || diagnostic_cap > 100 ||
        tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) != TF_DIAG_PHASE_VALID ||
        tf_signatures_resolution(signatures) != resolution || tf_signatures_store(signatures) == NULL ||
        tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(signatures)) ||
        (adapters != NULL && adapters->patterns != NULL &&
         (adapters->patterns_destroy == NULL || adapters->pattern_binding == NULL))) return TF_HOST_ERROR;
    program = a.allocate(a.context,sizeof *program);
    if (program == NULL) return TF_HOST_ERROR;
    *program = (TfTypedProgram){.resolution = resolution,.store = tf_signatures_store(signatures),.allocator = a};
    if (tf_diagnostic_bag_create(&program->diagnostics,diagnostic_cap,&a) != TF_OK ||
        !init_tables(program,signatures)) goto fail;
    c.program = program;
    c.signatures = signatures;
    c.adapters = adapters;
    c.probe_work = PROBE_WORK_LIMIT;
    for (size_t i = 0; i < program->module_count && !c.failed; ++i) {
        c.module = &program->modules[i];
        const TfAstNode *root = ast_node(tf_ast_root(c.module->ast));
        const TfNodeList declarations = root->as.program.declarations;
        for (size_t j = 0; j < declarations.count && !c.failed; ++j) {
            const TfAstNode *n = ast_node(declarations.items[j]);
            switch (n->kind) {
            case TF_AST_IMPORT: break;
            case TF_AST_FUNCTION: if (!function(&c,n)) complete = false; break;
            case TF_AST_MACHINE: if (!machine(&c,n)) complete = false; break;
            case TF_AST_RECORD: case TF_AST_ENUM: aggregate(&c,n); break;
            default: c.failed = true; break;
            }
        }
    }
    if (c.failed) goto fail;
    if (tf_diagnostic_bag_has_errors(program->diagnostics)) {
        if (tf_diagnostic_phase_error(&program->phase,program->diagnostics) != TF_OK) goto fail;
    } else if (program->pending == 0) {
        if (!complete || tf_diagnostic_phase_valid(&program->phase,program,program->diagnostics) != TF_OK) goto fail;
    }
    *out = program;
    release(a,c.undo);
    return TF_OK;
fail:
    release(a,c.undo);
    tf_typed_program_destroy(program);
    return TF_HOST_ERROR;
}

void tf_typed_program_destroy(TfTypedProgram *p) {
    if (p != NULL) {
        const TfAllocator a = p->allocator;
        for (size_t i = 0; i < p->module_count; ++i) release(a,p->modules[i].nodes);
        release(a,p->modules);
        release(a,p->symbols);
        tf_diagnostic_bag_destroy(p->diagnostics);
        release(a,p);
    }
}

const TfDiagnosticBag *tf_typed_program_diagnostics(const TfTypedProgram *p) {
    return p != NULL ? p->diagnostics : NULL;
}

const TfDiagnosticPhaseResult *tf_typed_program_phase(const TfTypedProgram *p) {
    return p != NULL ? &p->phase : NULL;
}

unsigned tf_typed_program_pending(const TfTypedProgram *p) { return p != NULL ? p->pending : 0; }

const TfResolution *tf_typed_program_resolution(const TfTypedProgram *p) { return p != NULL ? p->resolution : NULL; }

const TfTypeStore *tf_typed_program_store(const TfTypedProgram *p) { return p != NULL ? p->store : NULL; }

const TfTypedNode *tf_typed_program_node(const TfTypedProgram *p, TfNodeId id) {
    const NodeSlot *s = slot(module_for(p,id.owner),id);
    return s != NULL && s->typed.type.index != 0 ? &s->typed : NULL;
}

TfConcreteTypeId tf_typed_program_symbol(const TfTypedProgram *p, TfSymbolId id) {
    return p != NULL && id.owner == p->resolution && id.index != 0 && id.index <= p->symbol_count
         ? p->symbols[id.index - 1] : (TfConcreteTypeId){0};
}

const TfLiteralValue *tf_typed_program_literal(const TfTypedProgram *p, TfNodeId id) {
    const NodeSlot *s = slot(module_for(p,id.owner),id);
    return s != NULL && s->has_literal ? &s->literal : NULL;
}
