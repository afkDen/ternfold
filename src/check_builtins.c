#include "check_builtins.h"

#include <string.h>

/* This is the finite prelude vocabulary, not a type representation for the
 * rest of the compiler. Only concrete types cross the checker boundary. */
typedef enum {
    A, B, M, I, O, E, SLOT_COUNT,
    INT = SLOT_COUNT, FLOAT, BOOL, TEXT, LIST_A, LIST_B, LIST_O, LIST_INT,
    OPTION_A, OPTION_INT, OPTION_TEXT, DECISION, MAP_FN, FOLD_FN
} Form;

enum { MAX_PARAMETERS = 3 };

typedef enum { VALUE_FUNCTION, PAYLOAD_VARIANT, NULLARY_VARIANT } UseKind;

typedef struct {
    const char *name;
    const char *family;
    UseKind kind;
    size_t arity;
    Form parameters[MAX_PARAMETERS];
    Form result;
} Scheme;

static const Scheme schemes[] = {
    {"step",NULL,VALUE_FUNCTION,2,{M,I},DECISION},
    {"len",NULL,VALUE_FUNCTION,1,{LIST_A},INT},
    {"at",NULL,VALUE_FUNCTION,2,{LIST_A,INT},OPTION_A},
    {"append",NULL,VALUE_FUNCTION,2,{LIST_A,A},LIST_A},
    {"map",NULL,VALUE_FUNCTION,2,{LIST_A,MAP_FN},LIST_B},
    {"fold",NULL,VALUE_FUNCTION,3,{LIST_A,B,FOLD_FN},B},
    {"range",NULL,VALUE_FUNCTION,2,{INT,INT},LIST_INT},
    {"int_text",NULL,VALUE_FUNCTION,1,{INT},TEXT},
    {"int_float",NULL,VALUE_FUNCTION,1,{INT},FLOAT},
    {"float_int",NULL,VALUE_FUNCTION,1,{FLOAT},OPTION_INT},
    {"float_text",NULL,VALUE_FUNCTION,1,{FLOAT},TEXT},
    {"bool_text",NULL,VALUE_FUNCTION,1,{BOOL},TEXT},
    {"parse_int",NULL,VALUE_FUNCTION,1,{TEXT},OPTION_INT},
    {"text_len",NULL,VALUE_FUNCTION,1,{TEXT},INT},
    {"text_at",NULL,VALUE_FUNCTION,2,{TEXT,INT},OPTION_TEXT},
    {"text_slice",NULL,VALUE_FUNCTION,3,{TEXT,INT,INT},OPTION_TEXT},
    {"Some","Option",PAYLOAD_VARIANT,1,{A},OPTION_A},
    {"None","Option",NULLARY_VARIANT,0,{0},OPTION_A},
    {"Accept","Decision",PAYLOAD_VARIANT,2,{M,LIST_O},DECISION},
    {"Reject","Decision",PAYLOAD_VARIANT,2,{M,E},DECISION}
};

typedef struct {
    TfCheckContext *check;
    TfTypeStore *store;
    const Scheme *scheme;
    const TfAstNode *use;
    TfConcreteTypeId types[SLOT_COUNT];
    TfSpan origins[SLOT_COUNT];
    size_t bound;
} Instance;

typedef struct {
    const char *code;
    const char *message;
    TfSpan primary, related;
} Issue;

static const TfAstNode *node(TfNodeId id) { return tf_ast_get(id.owner,id); }

static bool spells(TfAstText name, const char *text) {
    const size_t length = strlen(text);
    return name.length == length && memcmp(name.data,text,length) == 0;
}

static const Scheme *lookup(const TfResolution *resolution, const TfResolvedSymbol *symbol) {
    const TfResolvedType *family;
    if (symbol == NULL || symbol->module.index != 0 || symbol->declaration.index != 0 ||
        tf_resolution_symbol(resolution,symbol->id) != symbol) return NULL;
    family = tf_resolution_type(resolution,symbol->type);
    for (size_t i = 0; i < sizeof schemes / sizeof *schemes; ++i) {
        const Scheme *s = &schemes[i];
        if (!spells(symbol->name,s->name)) continue;
        if (s->family == NULL && symbol->kind == TF_SYMBOL_BUILTIN) return s;
        if (s->family != NULL && symbol->kind == TF_SYMBOL_VARIANT && family != NULL &&
            family->kind == TF_TYPE_BUILTIN && family->id.module_index == 0 &&
            spells(family->name,s->family)) return s;
    }
    return NULL;
}

static bool primitive(Form form, TfConcreteTypeKind *out) {
    switch (form) {
    case INT: *out = TF_CONCRETE_INT; return true;
    case FLOAT: *out = TF_CONCRETE_FLOAT; return true;
    case BOOL: *out = TF_CONCRETE_BOOL; return true;
    case TEXT: *out = TF_CONCRETE_TEXT; return true;
    default: return false;
    }
}

static bool collection(Form form, TfConcreteTypeKind *kind, Form *element) {
    switch (form) {
    case LIST_A: *kind = TF_CONCRETE_LIST; *element = A; return true;
    case LIST_B: *kind = TF_CONCRETE_LIST; *element = B; return true;
    case LIST_O: *kind = TF_CONCRETE_LIST; *element = O; return true;
    case LIST_INT: *kind = TF_CONCRETE_LIST; *element = INT; return true;
    case OPTION_A: *kind = TF_CONCRETE_OPTION; *element = A; return true;
    case OPTION_INT: *kind = TF_CONCRETE_OPTION; *element = INT; return true;
    case OPTION_TEXT: *kind = TF_CONCRETE_OPTION; *element = TEXT; return true;
    default: return false;
    }
}

static size_t callback(Form form, Form parameters[2]) {
    if (form == MAP_FN) {
        parameters[0] = A;
        return 1;
    }
    if (form == FOLD_FN) {
        parameters[0] = B;
        parameters[1] = A;
        return 2;
    }
    return 0;
}

static bool form_kind(Form form, TfConcreteTypeKind *out) {
    Form parts[2];
    if (primitive(form,out) || collection(form,out,&parts[0])) return true;
    if (form == M) { *out = TF_CONCRETE_MACHINE; return true; }
    if (form == DECISION) { *out = TF_CONCRETE_DECISION; return true; }
    if (callback(form,parts) == 0) return false;
    *out = TF_CONCRETE_FUNCTION;
    return true;
}

static TfStatus issue_at(Issue *issue, const char *code, const char *message,
                         TfSpan primary, TfSpan related) {
    *issue = (Issue){code,message,primary,related};
    return TF_SOURCE_ERROR;
}

static TfStatus mismatch(Issue *issue, TfSpan primary, TfSpan related) {
    return issue_at(issue,"E302","type does not match the builtin signature",primary,related);
}

static TfStatus slot_bind(Instance *s, Form form, TfConcreteTypeId type,
                          TfSpan span, TfSpan related, Issue *issue) {
    if (s->types[form].index == 0) {
        s->types[form] = type;
        s->origins[form] = span;
        ++s->bound;
    } else if (!tf_concrete_type_id_equal(s->types[form],type)) {
        return mismatch(issue,span,s->origins[form].source != NULL ? s->origins[form] : related);
    }
    return TF_OK;
}

static TfStatus match(Instance *s, Form form, TfConcreteTypeId type,
                      TfSpan span, TfSpan related, Issue *issue) {
    const TfConcreteType *t = tf_type_store_get(s->store,type);
    TfConcreteTypeKind kind;
    Form part, params[2];
    size_t arity;
    TfStatus status;
    if (t == NULL) return TF_HOST_ERROR;
    if (form == M) {
        TfMachineSignature machine;
        if (t->kind != TF_CONCRETE_MACHINE) {
            if (s->scheme->kind == PAYLOAD_VARIANT)
                return issue_at(issue,"E307","Decision requires a machine type",span,related);
            return mismatch(issue,span,related);
        }
        if (!tf_signatures_machine(tf_check_signatures(s->check),t->as.nominal,&machine))
            return TF_HOST_ERROR;
        status = slot_bind(s,M,type,span,related,issue);
        if (status == TF_OK) status = slot_bind(s,I,machine.input,span,related,issue);
        if (status == TF_OK) status = slot_bind(s,O,machine.output,span,related,issue);
        if (status == TF_OK) status = slot_bind(s,E,machine.rejection,span,related,issue);
        return status;
    }
    if (form < SLOT_COUNT) return slot_bind(s,form,type,span,related,issue);
    if (primitive(form,&kind)) return t->kind == kind ? TF_OK : mismatch(issue,span,related);
    if (collection(form,&kind,&part))
        return t->kind == kind ? match(s,part,t->as.element,span,related,issue)
                               : mismatch(issue,span,related);
    if (form == DECISION) {
        if (t->kind != TF_CONCRETE_DECISION) return mismatch(issue,span,related);
        /* The type store only admits Decisions consistent with the machine. */
        return match(s,M,t->as.decision.machine,span,related,issue);
    }
    arity = callback(form,params);
    if (arity == 0) return TF_HOST_ERROR;
    if (t->kind != TF_CONCRETE_FUNCTION || t->as.function.arity != arity)
        return mismatch(issue,span,related);
    for (size_t i = 0; i < arity; ++i) {
        status = match(s,params[i],t->as.function.parameters[i],span,related,issue);
        if (status != TF_OK) return status;
    }
    return match(s,B,t->as.function.result,span,related,issue);
}

static TfStatus materialize(Instance *s, Form form, TfConcreteTypeId *out, Issue *issue) {
    TfConcreteTypeKind kind;
    Form part, params[2];
    TfConcreteTypeId element, result, arguments[2];
    size_t arity;
    TfStatus status;
    *out = (TfConcreteTypeId){0};
    if (form < SLOT_COUNT) { *out = s->types[form]; return TF_OK; }
    if (primitive(form,&kind)) return tf_type_primitive(s->store,kind,out);
    if (collection(form,&kind,&part)) {
        status = materialize(s,part,&element,issue);
        if (status != TF_OK || element.index == 0) return status;
        return kind == TF_CONCRETE_LIST ? tf_type_list(s->store,element,out)
                                        : tf_type_option(s->store,element,out);
    }
    if (form == DECISION) {
        TfTypeIssue type_issue;
        if (s->types[M].index == 0) return TF_OK;
        status = tf_type_decision(s->store,s->types[M],s->types[O],s->types[E],out,&type_issue);
        if (status == TF_SOURCE_ERROR)
            return issue_at(issue,type_issue.code,type_issue.message,
                            type_issue.primary.source != NULL ? type_issue.primary : s->use->span,
                            type_issue.related);
        return status;
    }
    arity = callback(form,params);
    if (arity == 0) return TF_HOST_ERROR;
    for (size_t i = 0; i < arity; ++i) {
        status = materialize(s,params[i],&arguments[i],issue);
        if (status != TF_OK || arguments[i].index == 0) return status;
    }
    status = materialize(s,B,&result,issue);
    if (status != TF_OK || result.index == 0) return status;
    return tf_type_function(s->store,arguments,arity,result,out);
}

static void initialize(Instance *s, TfCheckContext *context, const TfAstNode *use,
                       const Scheme *scheme) {
    *s = (Instance){.check = context, .store = tf_signatures_store(tf_check_signatures(context)),
                    .scheme = scheme, .use = use};
}

static TfSpan form_origin(const Instance *s, Form form, TfSpan fallback) {
    TfConcreteTypeKind kind;
    Form element;
    if (form < SLOT_COUNT)
        return s->origins[form].source != NULL ? s->origins[form] : fallback;
    if (collection(form,&kind,&element)) return form_origin(s,element,fallback);
    if (form == MAP_FN || form == FOLD_FN)
        return form_origin(s,A,form_origin(s,B,fallback));
    return fallback;
}

static TfStatus report(TfCheckContext *context, TfStatus status, const Issue *issue) {
    if (status == TF_SOURCE_ERROR &&
        tf_check_diagnostic(context,issue->code,issue->message,issue->primary,issue->related) != TF_OK)
        return TF_HOST_ERROR;
    return status;
}

static TfStatus bare(Instance *s, TfCheckExpectation wanted, TfConcreteTypeId *result,
                     Issue *issue) {
    TfConcreteTypeId parameters[MAX_PARAMETERS], value;
    TfStatus status;
    const Scheme *scheme = s->scheme;
    const TfSpan span = s->use->span;
    if (scheme->kind == PAYLOAD_VARIANT)
        return issue_at(issue,"E303","constructor is only usable in direct construction syntax",
                        span,(TfSpan){0});
    if (wanted.type.index != 0) {
        if (scheme->kind == NULLARY_VARIANT) {
            status = match(s,scheme->result,wanted.type,span,wanted.origin,issue);
        } else {
            const TfConcreteType *t = tf_type_store_get(s->store,wanted.type);
            if (t == NULL) return TF_HOST_ERROR;
            if (t->kind != TF_CONCRETE_FUNCTION || t->as.function.arity != scheme->arity)
                return mismatch(issue,span,wanted.origin);
            status = match(s,scheme->result,t->as.function.result,span,wanted.origin,issue);
            for (size_t i = 0; i < scheme->arity && status == TF_OK; ++i)
                status = match(s,scheme->parameters[i],t->as.function.parameters[i],span,
                               wanted.origin,issue);
        }
        if (status == TF_OK) *result = wanted.type;
        return status;
    }
    status = materialize(s,scheme->result,&value,issue);
    if (status != TF_OK) return status;
    for (size_t i = 0; i < scheme->arity; ++i) {
        status = materialize(s,scheme->parameters[i],&parameters[i],issue);
        if (status != TF_OK) return status;
        if (parameters[i].index == 0) value = (TfConcreteTypeId){0};
    }
    if (value.index == 0) {
        if (wanted.unavailable) return TF_OK;
        return issue_at(issue,"E304","builtin value needs a concrete expected type",span,(TfSpan){0});
    }
    if (scheme->kind == NULLARY_VARIANT) { *result = value; return TF_OK; }
    return tf_type_function(s->store,parameters,scheme->arity,value,result);
}
/* A bare polymorphic callback still receives a concrete expected function
 * type in the final pass. Before that, its compiler-known signature can
 * constrain the enclosing map/fold, e.g. map([[1]], len) fixes B to Int.
 * Each exchange only adds bindings, so the two finite instances converge. */
static TfStatus constrain_bare_callback(Instance *outer, Form form, TfNodeId expression,
                                         TfSpan related, bool *handled, Issue *issue) {
    const TfAstNode *path = node(expression);
    const TfResolution *resolution = tf_typed_program_resolution(tf_check_program(outer->check));
    const TfResolvedReference *reference;
    const Scheme *scheme;
    Instance inner;
    Form parameters[2];
    size_t arity = callback(form,parameters);
    TfStatus status;
    *handled = false;
    if (arity == 0) return TF_OK;
    while (path != NULL && path->kind == TF_AST_GROUP) path = node(path->as.group.expression);
    if (path == NULL || path->kind != TF_AST_PATH) return TF_OK;
    reference = tf_resolution_reference(resolution,path->id);
    if (reference == NULL) return TF_HOST_ERROR;
    scheme = lookup(resolution,tf_resolution_symbol(resolution,reference->symbol));
    if (scheme == NULL || scheme->kind != VALUE_FUNCTION) return TF_OK;
    *handled = true;
    if (scheme->arity != arity) return mismatch(issue,path->span,related);
    if (form == FOLD_FN) {
        /* Both positions are B even when its element type is unknown. */
        TfConcreteTypeKind parameter_kind, result_kind;
        if (form_kind(scheme->parameters[0],&parameter_kind) &&
            form_kind(scheme->result,&result_kind) && parameter_kind != result_kind)
            return mismatch(issue,path->span,related);
    }
    initialize(&inner,outer->check,path,scheme);
    for (;;) {
        size_t previous = outer->bound + inner.bound;
        for (size_t i = 0; i <= arity; ++i) {
            const Form left = i == arity ? B : parameters[i];
            const Form right = i == arity ? scheme->result : scheme->parameters[i];
            TfConcreteTypeId type;
            status = materialize(outer,left,&type,issue);
            if (status != TF_OK) return status;
            if (type.index != 0) {
                status = match(&inner,right,type,path->span,related,issue);
                if (status != TF_OK) return status;
            }
            status = materialize(&inner,right,&type,issue);
            if (status != TF_OK) return status;
            if (type.index != 0) {
                status = match(outer,left,type,path->span,related,issue);
                if (status != TF_OK) return status;
            }
        }
        if (previous == outer->bound + inner.bound) return TF_OK;
    }
}

/* Inspect only syntax with an unconditional outer type constructor. This does
 * not choose an element type or infer through a branch/local binding. */
static bool expression_kind(Instance *s, TfNodeId id, TfConcreteTypeKind *kind) {
    const TfAstNode *n = node(id);
    const TfResolution *resolution = tf_typed_program_resolution(tf_check_program(s->check));
    const TfResolvedReference *reference;
    const Scheme *scheme;
    bool call = false;
    while (n != NULL && n->kind == TF_AST_GROUP) n = node(n->as.group.expression);
    if (n == NULL) return false;
    if (n->kind == TF_AST_LIST) { *kind = TF_CONCRETE_LIST; return true; }
    if (n->kind == TF_AST_CLOSURE) { *kind = TF_CONCRETE_FUNCTION; return true; }
    if (n->kind == TF_AST_CALL) { call = true; n = node(n->as.call.callee); }
    if (n == NULL || n->kind != TF_AST_PATH) return false;
    reference = tf_resolution_reference(resolution,n->id);
    if (reference == NULL) return false;
    scheme = lookup(resolution,tf_resolution_symbol(resolution,reference->symbol));
    if (scheme == NULL) return false;
    if (call && scheme->kind != NULLARY_VARIANT) return form_kind(scheme->result,kind);
    if (!call && scheme->kind == NULLARY_VARIANT) return form_kind(scheme->result,kind);
    if (!call && scheme->kind == VALUE_FUNCTION) { *kind = TF_CONCRETE_FUNCTION; return true; }
    return false;
}

static TfStatus shape_check(Instance *s, Form form, const TfAstNode *argument,
                             TfSpan related, Issue *issue) {
    TfConcreteTypeKind expected, actual;
    if (!form_kind(form,&expected) ||
        !expression_kind(s,argument->as.argument.value,&actual) || expected == actual)
        return TF_OK;
    const TfAstNode *expression = node(argument->as.argument.value);
    if (expression == NULL) return TF_HOST_ERROR;
    if (form == M && s->scheme->kind == PAYLOAD_VARIANT)
        return issue_at(issue,"E307","Decision requires a machine type",expression->span,related);
    return mismatch(issue,expression->span,related);
}

/* A failed call still owns the written-order walk of every child. This also
 * handles surplus arguments without indexing the fixed scheme arrays. */
static TfStatus recover_call(Instance *s, const Issue *call_issue) {
    const TfNodeList args = s->use->as.call.arguments;
    for (size_t i = 0; i < args.count; ++i)
        (void)tf_check_argument(s->check,args.items[i],(TfCheckExpectation){.unavailable = true});
    return report(s->check,TF_SOURCE_ERROR,call_issue);
}

static TfStatus check_call(Instance *s, TfCheckExpectation wanted, TfConcreteTypeId *result,
                           TfConcreteTypeId *callable, Issue *issue) {
    const Scheme *scheme = s->scheme;
    const TfNodeList args = s->use->as.call.arguments;
    const TfAstNode *callee = node(s->use->as.call.callee);
    const TfAstNode *arguments[MAX_PARAMETERS];
    TfConcreteTypeId parameters[MAX_PARAMETERS], value, signature;
    Issue argument_issues[MAX_PARAMETERS] = {0}, result_issue = {0};
    bool invalid_child = false, missing = false, handled;
    bool invalid_trial[MAX_PARAMETERS] = {0};
    TfStatus status;
    if (callee == NULL) return TF_HOST_ERROR;
    if (scheme->kind == NULLARY_VARIANT) {
        (void)issue_at(issue,"E303","nullary variant is not callable",callee->span,(TfSpan){0});
        status = recover_call(s,issue);
        *issue = (Issue){0};
        return status == TF_HOST_ERROR ? status : TF_OK;
    }
    if (args.count != scheme->arity) {
        (void)issue_at(issue,"E303","wrong builtin call arity",s->use->span,(TfSpan){0});
        status = recover_call(s,issue);
        *issue = (Issue){0};
        return status == TF_HOST_ERROR ? status : TF_OK;
    }
    for (size_t i = 0; i < args.count; ++i) {
        arguments[i] = node(args.items[i]);
        if (arguments[i] == NULL || arguments[i]->kind != TF_AST_ARGUMENT) return TF_HOST_ERROR;
        if (arguments[i]->as.argument.named)
            (void)issue_at(&argument_issues[i],"E303","builtin call arguments must be positional",
                           arguments[i]->span,callee->span);
    }
    if (wanted.type.index != 0) {
        Instance trial = *s;
        status = match(&trial,scheme->result,wanted.type,s->use->span,wanted.origin,&result_issue);
        if (status == TF_HOST_ERROR) return status;
        if (status == TF_OK) *s = trial;
    }

    /* Keep a failed constraint local to its argument. Failed matching must not
     * partially bind a scheme, nor bypass publication of an earlier child. */
    for (size_t i = 0; i < args.count; ++i) {
        const TfAstNode *argument = arguments[i];
        const Form form = scheme->parameters[i];
        TfConcreteTypeId inferred;
        Issue *arg_issue = &argument_issues[i];
        if (arg_issue->code != NULL) continue;
        status = shape_check(s,form,argument,callee->span,arg_issue);
        if (status == TF_HOST_ERROR) return status;
        if (status == TF_SOURCE_ERROR) continue;
        status = materialize(s,form,&inferred,issue);
        if (status != TF_OK) return status;
        if (inferred.index != 0) continue;
        Instance trial = *s;
        status = constrain_bare_callback(&trial,form,argument->as.argument.value,
                                         callee->span,&handled,arg_issue);
        if (status == TF_HOST_ERROR) return status;
        if (status == TF_SOURCE_ERROR) continue;
        *s = trial;
        if (handled) continue;
        status = tf_check_probe_argument(s->check,args.items[i],&inferred);
        if (status == TF_SOURCE_ERROR) { invalid_trial[i] = true; continue; }
        if (status != TF_OK) return status;
        if (inferred.index == 0) continue;
        const TfAstNode *expression = node(argument->as.argument.value);
        if (expression == NULL) return TF_HOST_ERROR;
        trial = *s;
        status = match(&trial,form,inferred,expression->span,callee->span,arg_issue);
        if (status == TF_HOST_ERROR) return status;
        if (status == TF_OK) *s = trial;
    }
    status = materialize(s,scheme->result,&value,issue);
    if (status != TF_OK) return status;
    missing = value.index == 0;
    for (size_t i = 0; i < args.count; ++i) {
        status = materialize(s,scheme->parameters[i],&parameters[i],issue);
        if (status != TF_OK) return status;
        if (parameters[i].index == 0) missing = true;
    }

    /* A retained argument issue is published after that child's independent
     * diagnostics. Its recovery check has no conflicting expected type, so
     * the same mismatch cannot be emitted twice. */
    for (size_t i = 0; i < args.count; ++i) {
        const Form form = scheme->parameters[i];
        const TfSpan origin = form_origin(s,form,callee->span);
        const bool diagnosed = argument_issues[i].code != NULL;
        TfConcreteTypeId checked = tf_check_argument(s->check,args.items[i],
            diagnosed ? (TfCheckExpectation){.unavailable = true}
                      : (TfCheckExpectation){parameters[i],origin,parameters[i].index == 0});
        if (diagnosed) {
            if (report(s->check,TF_SOURCE_ERROR,&argument_issues[i]) == TF_HOST_ERROR)
                return TF_HOST_ERROR;
            invalid_child = true;
        }
        if (checked.index == 0 && (parameters[i].index != 0 || invalid_trial[i])) invalid_child = true;
    }
    if (result_issue.code != NULL) {
        *issue = result_issue;
        return TF_SOURCE_ERROR;
    }
    if (invalid_child) return TF_OK;
    if (missing) {
        if (wanted.unavailable) return TF_OK;
        return issue_at(issue,"E304","builtin type arguments are not determined",
                        s->use->span,wanted.origin);
    }
    status = tf_type_function(s->store,parameters,args.count,value,&signature);
    if (status != TF_OK) return status;
    *result = value;
    *callable = signature;
    return TF_OK;
}

TfStatus tf_check_builtin(TfCheckContext *context, const TfAstNode *use,
                          const TfResolvedSymbol *symbol, TfCheckExpectation wanted,
                          TfConcreteTypeId *result, TfConcreteTypeId *callable) {
    const Scheme *scheme;
    Instance instance;
    Issue issue = {0};
    TfStatus status;
    if (result == NULL || callable == NULL || result == callable) return TF_HOST_ERROR;
    *result = *callable = (TfConcreteTypeId){0};
    if (context == NULL || use == NULL ||
        (use->kind != TF_AST_PATH && use->kind != TF_AST_CALL)) return TF_HOST_ERROR;
    scheme = lookup(tf_typed_program_resolution(tf_check_program(context)),symbol);
    if (scheme == NULL) return TF_HOST_ERROR;
    initialize(&instance,context,use,scheme);
    status = use->kind == TF_AST_CALL ? check_call(&instance,wanted,result,callable,&issue)
                                      : bare(&instance,wanted,result,&issue);
    return report(context,status,&issue);
}
