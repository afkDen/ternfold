#include "typed_signatures.h"
#include "internal.h"

#include <string.h>

typedef struct {
    TfTypeId nominal;
    TfMachineSignature type;
} Machine;
typedef struct {
    size_t target, next;
    /* Machine header edges matter to dependency cycles, but do not make
     * an immutable snapshot contain the input/output/rejection values. */
    bool header_only;
} TypeEdge;
typedef struct {
    size_t first;
    unsigned color;
    bool function_data;
} TypeNode;
typedef struct { size_t id, next; } VisitType;
typedef struct {
    TypeNode *nodes;
    TypeEdge *edges;
    size_t count, used, capacity;
} TypeGraph;

struct TfTypedSignatures {
    const TfResolution *resolution;
    TfAllocator allocator;
    TfTypeStore *store;
    TfDiagnosticBag *diagnostics;
    TfConcreteTypeId *symbols;
    Machine *machines;
    size_t machine_count, machine_capacity;
    bool *function_data;
    size_t capability_count, capability_capacity;
    bool failed;
};

static void release(const TfTypedSignatures *s, void *p) {
    if (p != NULL) s->allocator.deallocate(s->allocator.context,p);
}

static void *array(TfTypedSignatures *s, size_t count, size_t width) {
    void *data;
    if (count == 0) return NULL;
    if (count > SIZE_MAX/width) { s->failed = true; return NULL; }
    data = s->allocator.allocate(s->allocator.context,count*width);
    if (data != NULL) memset(data,0,count*width);
    else s->failed = true;
    return data;
}

static const TfAstNode *node(TfNodeId id) { return tf_ast_get(id.owner,id); }

static TfConcreteTypeId annotation(TfTypedSignatures *s, TfNodeId syntax) {
    TfConcreteTypeId type = {0};
    TfTypeIssue issue = {0};
    TfStatus status;
    if (s->failed) return type;
    status = tf_type_from_syntax(s->store,syntax,&type,&issue);
    if (status == TF_SOURCE_ERROR) {
        TfDiagnosticLabel labels[2] = {0};
        TfDiagnosticRecord d = {0};
        const TfAstNode *n = node(syntax);
        TfSpan primary = issue.primary.source != NULL ? issue.primary : n->span;
        if (issue.code == NULL || issue.message == NULL) { s->failed = true; return type; }
        labels[0] = (TfDiagnosticLabel){primary,true,{"invalid type",12}};
        d.code = issue.code;
        d.severity = TF_DIAG_ERROR;
        d.message = (TfDiagnosticText){issue.message,strlen(issue.message)};
        d.labels = labels;
        d.label_count = 1;
        if (issue.related.source != NULL) {
            labels[1] = (TfDiagnosticLabel){issue.related,false,{"declared here",13}};
            d.label_count = 2;
        }
        status = tf_diagnostic_bag_add(s->diagnostics,&d);
    }
    if (status != TF_OK) s->failed = true;
    return type;
}

static TfConcreteTypeId nominal(TfTypedSignatures *s, TfTypeId id) {
    TfConcreteTypeId type = {0};
    if (!s->failed && tf_type_nominal(s->store,id,&type) != TF_OK) s->failed = true;
    return type;
}

static TfConcreteTypeId callable(TfTypedSignatures *s, TfNodeList members,
                                 bool parameters, TfConcreteTypeId result) {
    TfConcreteTypeId type = {0};
    TfConcreteTypeId *arguments;
    bool valid = result.index != 0;
    if (s->failed) return type;
    arguments = array(s,members.count,sizeof *arguments);
    for (size_t i=0; i<members.count && !s->failed; ++i) {
        const TfAstNode *param = parameters ? node(members.items[i]) : NULL;
        if (parameters && (param == NULL || param->kind != TF_AST_PARAMETER)) {
            s->failed = true;
            break;
        }
        /* Parameter/member annotations have their own symbols, processed by
         * the signature builder before their containing callable. */
        if (parameters) {
            const TfResolvedReference *ref = tf_resolution_reference(s->resolution,param->id);
            if (ref == NULL || ref->kind != TF_REFERENCE_DECLARATION || ref->symbol.index == 0) {
                s->failed = true;
                break;
            }
            arguments[i] = tf_signatures_symbol(s,ref->symbol);
        } else arguments[i] = annotation(s,members.items[i]);
        if (arguments[i].index == 0) valid = false;
    }
    if (valid && !s->failed &&
        tf_type_function(s->store,arguments,members.count,result,&type) != TF_OK) s->failed = true;
    release(s,arguments);
    return type;
}

static TfConcreteTypeId symbol_annotation(TfTypedSignatures *s, const TfResolvedSymbol *symbol) {
    const TfAstNode *decl = node(symbol->declaration);
    if (decl == NULL) return (TfConcreteTypeId){0};
    switch (symbol->kind) {
    case TF_SYMBOL_PARAMETER: case TF_SYMBOL_FIELD: case TF_SYMBOL_MACHINE_INPUT:
        return annotation(s,decl->as.parameter.type);
    case TF_SYMBOL_CELL:
        return annotation(s,decl->as.state.type);
    default:
        return (TfConcreteTypeId){0};
    }
}

static void machine_signature(TfTypedSignatures *s, const TfResolvedSymbol *ctor, const TfAstNode *decl) {
    const TfResolvedReference *input = tf_resolution_reference(s->resolution,decl->as.machine.parameter);
    TfMachineSignature types = {0};
    void *data = s->machines;
    if (input == NULL || input->symbol.index == 0) { s->failed = true; return; }
    types.input = tf_signatures_symbol(s,input->symbol);
    types.output = annotation(s,decl->as.machine.yields);
    types.rejection = annotation(s,decl->as.machine.rejects);
    if (s->failed || types.input.index == 0 || types.output.index == 0 || types.rejection.index == 0)
        return;
    if (s->machine_count == SIZE_MAX || !tf_grow(&data,&s->machine_capacity,s->machine_count+1,
                                                sizeof *s->machines,s->allocator)) {
        s->failed = true; return;
    }
    s->machines = data;
    s->machines[s->machine_count++] = (Machine){ctor->type,types};
}

static TfConcreteTypeId signature(TfTypedSignatures *s, const TfResolvedSymbol *symbol) {
    const TfAstNode *decl = node(symbol->declaration);
    TfConcreteTypeId result = {0};
    if (decl == NULL) return result;
    switch (symbol->kind) {
    case TF_SYMBOL_FUNCTION:
        result = annotation(s,decl->as.function.result_type);
        return callable(s,decl->as.function.parameters,true,result);
    case TF_SYMBOL_CONSTRUCTOR:
        result = nominal(s,symbol->type);
        if (decl->kind == TF_AST_MACHINE) {
            machine_signature(s,symbol,decl);
            return callable(s,(TfNodeList){0},true,result);
        }
        return callable(s,decl->as.aggregate.members,true,result);
    case TF_SYMBOL_VARIANT:
        result = nominal(s,symbol->type);
        if (decl->as.variant.types.count != 0)
            result = callable(s,decl->as.variant.types,false,result);
        return result;
    default:
        return result;
    }
}

static void add_edge(TfTypedSignatures *s, TypeGraph *g, TfConcreteTypeId from,
                     TfConcreteTypeId to, bool header_only) {
    void *data = g->edges;
    if (s->failed) return;
    if (from.index == 0 || from.index > g->count || to.index == 0 || to.index > g->count ||
        g->used == SIZE_MAX || !tf_grow(&data,&g->capacity,g->used+1,sizeof *g->edges,s->allocator)) {
        s->failed = true; return;
    }
    g->edges = data;
    g->edges[g->used++] = (TypeEdge){to.index,g->nodes[from.index-1].first,header_only};
    g->nodes[from.index-1].first = g->used;
}

static void type_error(TfTypedSignatures *s, const char *code, const char *message,
                       TfSpan origin, TfSpan related) {
    TfDiagnosticLabel labels[2] = {
        {origin,true,{"declared here",13}}, {related,false,{"recursive dependency",20}}
    };
    TfDiagnosticRecord diagnostic = {0};
    diagnostic.code = code;
    diagnostic.severity = TF_DIAG_ERROR;
    diagnostic.message = (TfDiagnosticText){message,strlen(message)};
    if (origin.source != NULL) {
        diagnostic.labels = labels;
        diagnostic.label_count = related.source != NULL ? 2 : 1;
    }
    if (tf_diagnostic_bag_add(s->diagnostics,&diagnostic) != TF_OK) s->failed = true;
}

static TfSpan type_origin(TfTypedSignatures *s, size_t id) {
    const TfConcreteType *type = tf_type_store_at(s->store,id-1);
    const TfResolvedType *nom;
    if (type->kind != TF_CONCRETE_RECORD && type->kind != TF_CONCRETE_ENUM &&
        type->kind != TF_CONCRETE_MACHINE) return (TfSpan){0};
    nom = tf_resolution_type(s->resolution,type->as.nominal);
    return nom != NULL ? nom->name.span : (TfSpan){0};
}

static void check_type_graph(TfTypedSignatures *s) {
    TypeGraph graph = {0};
    VisitType *stack = NULL;
    bool cycle = false;
    /* Intern the nominal records before indexing the store. From this point
     * graph queries use canonical IDs and cannot grow its node count. */
    for (size_t i=0; i<tf_resolution_type_count(s->resolution) && !s->failed; ++i) {
        const TfResolvedType *t = tf_resolution_type_at(s->resolution,i);
        if (t->kind != TF_TYPE_BUILTIN) (void)nominal(s,t->id);
    }
    graph.count = tf_type_store_count(s->store);
    graph.nodes = array(s,graph.count,sizeof *graph.nodes);
    stack = array(s,graph.count,sizeof *stack);
    for (size_t i=0; i<graph.count && !s->failed; ++i) {
        const TfConcreteType *t = tf_type_store_at(s->store,i);
        switch (t->kind) {
        case TF_CONCRETE_LIST: case TF_CONCRETE_OPTION:
            add_edge(s,&graph,t->id,t->as.element,false);
            break;
        case TF_CONCRETE_FUNCTION:
            graph.nodes[i].function_data = true;
            add_edge(s,&graph,t->id,t->as.function.result,false);
            for (size_t j=0; j<t->as.function.arity && !s->failed; ++j)
                add_edge(s,&graph,t->id,t->as.function.parameters[j],false);
            break;
        case TF_CONCRETE_DECISION:
            add_edge(s,&graph,t->id,t->as.decision.machine,false);
            add_edge(s,&graph,t->id,t->as.decision.output,false);
            add_edge(s,&graph,t->id,t->as.decision.rejection,false);
            break;
        default: break;
        }
    }
    for (size_t i=0; i<tf_resolution_symbol_count(s->resolution) && !s->failed; ++i) {
        const TfResolvedSymbol *symbol = tf_resolution_symbol_at(s->resolution,i);
        if (symbol->type.module_index == 0) continue;
        if (symbol->kind == TF_SYMBOL_FIELD || symbol->kind == TF_SYMBOL_CELL) {
            const TfConcreteTypeId owner = nominal(s,symbol->type);
            add_edge(s,&graph,owner,s->symbols[i],false);
        } else if (symbol->kind == TF_SYMBOL_VARIANT) {
            const TfConcreteTypeId owner = nominal(s,symbol->type);
            const TfConcreteType *payload = tf_type_store_get(s->store,s->symbols[i]);
            if (payload == NULL) { s->failed = true; break; }
            if (payload->kind == TF_CONCRETE_FUNCTION)
                for (size_t j=0; j<payload->as.function.arity && !s->failed; ++j)
                    add_edge(s,&graph,owner,payload->as.function.parameters[j],false);
        }
    }
    for (size_t i=0; i<s->machine_count && !s->failed; ++i) {
        const TfConcreteTypeId owner = nominal(s,s->machines[i].nominal);
        const TfMachineSignature signature_types = s->machines[i].type;
        add_edge(s,&graph,owner,signature_types.input,true);
        add_edge(s,&graph,owner,signature_types.output,true);
        add_edge(s,&graph,owner,signature_types.rejection,true);
    }
    for (size_t i=0; i<graph.count && !s->failed && !cycle; ++i) {
        size_t depth = 1;
        if (graph.nodes[i].color != 0) continue;
        graph.nodes[i].color = 1;
        stack[0] = (VisitType){i+1,graph.nodes[i].first};
        while (depth != 0 && !s->failed && !cycle) {
            VisitType *visit = &stack[depth-1];
            TypeNode *current = &graph.nodes[visit->id-1];
            const TypeEdge *edge;
            TypeNode *child;
            if (visit->next == 0) {
                for (size_t e=current->first; e!=0; e=graph.edges[e-1].next) {
                    edge = &graph.edges[e-1];
                    if (!edge->header_only && graph.nodes[edge->target-1].function_data)
                        current->function_data = true;
                }
                current->color = 2;
                --depth;
                continue;
            }
            edge = &graph.edges[visit->next-1];
            visit->next = edge->next;
            child = &graph.nodes[edge->target-1];
            if (child->color == 0) {
                child->color = 1;
                stack[depth++] = (VisitType){edge->target,child->first};
            } else if (child->color == 1) {
                TfSpan primary = {0}, related = {0};
                /* Structural nodes are acyclic by construction. At least one
                 * nominal declaration on the cycle has an original span. */
                for (size_t k=depth; k!=0; --k) {
                    const TfSpan span = type_origin(s,stack[k-1].id);
                    if (span.source != NULL) {
                        if (primary.source == NULL) primary = span;
                        else { related = span; break; }
                    }
                    if (stack[k-1].id == edge->target) break;
                }
                type_error(s,"E307","recursive nominal type or machine signature",primary,related);
                cycle = true;
            }
        }
    }
    if (!cycle)
        for (size_t i=0; i<tf_resolution_symbol_count(s->resolution) && !s->failed; ++i) {
            const TfResolvedSymbol *symbol = tf_resolution_symbol_at(s->resolution,i);
            if (symbol->kind == TF_SYMBOL_CELL && graph.nodes[s->symbols[i].index-1].function_data)
                type_error(s,"E407","state cell type contains a function",symbol->name.span,(TfSpan){0});
        }
    if (!s->failed && !tf_diagnostic_bag_has_errors(s->diagnostics)) {
        s->function_data = array(s,graph.count,sizeof *s->function_data);
        if (!s->failed) {
            for (size_t i=0; i<graph.count; ++i)
                s->function_data[i] = graph.nodes[i].function_data;
            s->capability_count = s->capability_capacity = graph.count;
        }
    }
    release(s,stack);
    release(s,graph.nodes);
    release(s,graph.edges);
}

TfStatus tf_signatures_build(TfTypedSignatures **out, const TfResolution *resolution,
                             size_t diagnostic_cap, const TfAllocator *allocator) {
    const TfAllocator a = tf_allocator(allocator);
    TfTypedSignatures *s;
    size_t count;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) != TF_DIAG_PHASE_VALID ||
        !tf_allocator_valid(a)) return TF_HOST_ERROR;
    s = a.allocate(a.context,sizeof *s);
    if (s == NULL) return TF_HOST_ERROR;
    *s = (TfTypedSignatures){.resolution = resolution, .allocator = a};
    if (tf_type_store_create(&s->store,resolution,&a) != TF_OK ||
        tf_diagnostic_bag_create(&s->diagnostics,diagnostic_cap,&a) != TF_OK) goto fail;
    count = tf_resolution_symbol_count(resolution);
    s->symbols = array(s,count,sizeof *s->symbols);
    for (size_t i=0; i<count && !s->failed; ++i) {
        const TfResolvedSymbol *symbol = tf_resolution_symbol_at(resolution,i);
        /* Closure parameters belong to the later expression pass. */
        s->symbols[i] = (TfConcreteTypeId){0};
        if (symbol->closure.index == 0) s->symbols[i] = symbol_annotation(s,symbol);
    }
    for (size_t i=0; i<count && !s->failed; ++i) {
        const TfConcreteTypeId type = signature(s,tf_resolution_symbol_at(resolution,i));
        if (type.index != 0) s->symbols[i] = type;
    }
    if (!s->failed && !tf_diagnostic_bag_has_errors(s->diagnostics)) check_type_graph(s);
    if (s->failed) goto fail;
    *out = s;
    return TF_OK;
fail:
    tf_signatures_destroy(s);
    return TF_HOST_ERROR;
}

void tf_signatures_destroy(TfTypedSignatures *s) {
    if (s == NULL) return;
    tf_diagnostic_bag_destroy(s->diagnostics);
    tf_type_store_destroy(s->store);
    release(s,s->machines);
    release(s,s->symbols);
    release(s,s->function_data);
    release(s,s);
}

const TfDiagnosticBag *tf_signatures_diagnostics(const TfTypedSignatures *s) {
    return s == NULL ? NULL : s->diagnostics;
}
TfTypeStore *tf_signatures_store(TfTypedSignatures *s) { return s == NULL ? NULL : s->store; }
TfConcreteTypeId tf_signatures_symbol(const TfTypedSignatures *s, TfSymbolId symbol) {
    if (s == NULL || tf_resolution_symbol(s->resolution,symbol) == NULL)
        return (TfConcreteTypeId){0};
    return s->symbols[symbol.index-1];
}
bool tf_signatures_machine(const TfTypedSignatures *s, TfTypeId id, TfMachineSignature *out) {
    if (s == NULL || out == NULL || id.owner != s->resolution) return false;
    for (size_t i=0; i<s->machine_count; ++i) {
        const Machine *machine = &s->machines[i];
        if (id.module_index == machine->nominal.module_index &&
            id.declaration_index == machine->nominal.declaration_index) {
            *out = machine->type;
            return true;
        }
    }
    return false;
}

TfStatus tf_signatures_function_data(TfTypedSignatures *s, TfConcreteTypeId type, bool *out) {
    void *data;
    if (s == NULL || out == NULL || tf_diagnostic_bag_has_errors(s->diagnostics) ||
        tf_type_store_get(s->store,type) == NULL) return TF_HOST_ERROR;
    if (type.index > s->capability_count) {
        data = s->function_data;
        if (!tf_grow(&data,&s->capability_capacity,type.index,sizeof *s->function_data,s->allocator))
            return TF_HOST_ERROR;
        s->function_data = data;
        for (; s->capability_count<type.index; ++s->capability_count) {
            size_t i = s->capability_count;
            const TfConcreteType *t = tf_type_store_at(s->store,i);
            bool contains = false;
            /* Children of newly interned structural types have smaller IDs.
             * All nominal declarations were interned in the graph pass. */
            switch (t->kind) {
            case TF_CONCRETE_LIST: case TF_CONCRETE_OPTION:
                contains = s->function_data[t->as.element.index-1]; break;
            case TF_CONCRETE_DECISION:
                contains = s->function_data[t->as.decision.machine.index-1] ||
                           s->function_data[t->as.decision.output.index-1] ||
                           s->function_data[t->as.decision.rejection.index-1];
                break;
            case TF_CONCRETE_FUNCTION: contains = true; break;
            case TF_CONCRETE_RECORD: case TF_CONCRETE_ENUM: case TF_CONCRETE_MACHINE:
                return TF_HOST_ERROR;
            default: break;
            }
            s->function_data[i] = contains;
        }
    }
    *out = s->function_data[type.index-1];
    return TF_OK;
}
