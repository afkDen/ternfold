#include "typed_patterns.h"
#include "pattern_coverage.h"
#include "literal_value.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct Chunk Chunk;
struct Chunk { Chunk *next; max_align_t alignment; };
typedef struct Domain Domain;
typedef struct Constructor Constructor;
struct Constructor {
    TfCoverageConstructor coverage;
    TfSymbolId variant;
    TfLiteralValue literal;
    uint64_t hash;
    bool covered;
    const TfCoveragePattern **rows;
    size_t row_count, row_capacity;
};
struct Domain {
    TfCoverageDomain coverage;
    TfConcreteTypeId type;
    Constructor **literals;
    size_t literal_count, literal_capacity;
    Domain *next;
    bool built;
};
struct TfTypedPatterns {
    const TfResolution *resolution;
    TfTypeStore *store;
    TfAllocator allocator;
    TfDiagnosticBag *diagnostics;
    Chunk *chunks;
    Domain *domains;
    TfTypedPatternNode *nodes;
    const TfAst *ast;
    size_t node_count, node_capacity, work;
    bool failed, bad_patterns;
};

static void *allocate(TfTypedPatterns *p, size_t count, size_t width) {
    Chunk *c;
    if (p->failed || count > (SIZE_MAX-sizeof *c)/width) { p->failed = true; return NULL; }
    c = p->allocator.allocate(p->allocator.context,sizeof *c+count*width);
    if (c == NULL) { p->failed = true; return NULL; }
    c->next = p->chunks; p->chunks = c;
    return c+1;
}

static bool name_is(TfAstText text, const char *name) {
    size_t length = strlen(name);
    return text.length == length && memcmp(text.data,name,length) == 0;
}

static bool same_nominal(TfTypeId a, TfTypeId b) {
    return a.owner == b.owner && a.module_index == b.module_index &&
           a.declaration_index == b.declaration_index;
}

static Domain *domain_for(TfTypedPatterns *p, TfConcreteTypeId type) {
    Domain *d;
    for (d=p->domains; d!=NULL; d=d->next)
        if (tf_concrete_type_id_equal(type,d->type)) return d;
    d = allocate(p,1,sizeof *d);
    if (d != NULL) {
        *d = (Domain){.type=type,.next=p->domains};
        p->domains = d;
    }
    return d;
}

static Constructor *constructor(TfTypedPatterns *p, Domain *d, TfSymbolId variant,
                                TfLiteralValue literal, const TfConcreteTypeId *types,
                                size_t arity) {
    Constructor *c = allocate(p,1,sizeof *c);
    const TfCoverageDomain **args = arity != 0 ? allocate(p,arity,sizeof *args) : NULL;
    if (c == NULL || p->failed) return NULL;
    for (size_t i=0; i<arity; ++i) {
        Domain *sub = domain_for(p,types[i]);
        if (sub == NULL) return NULL;
        args[i] = &sub->coverage;
    }
    *c = (Constructor){.coverage={&d->coverage,args,arity},.variant=variant,.literal=literal};
    return c;
}

static bool enum_member(TfTypedPatterns *p, const TfConcreteType *type,
                        const TfResolvedSymbol *s) {
    const TfResolvedType *owner;
    if (s->kind != TF_SYMBOL_VARIANT) return false;
    if (type->kind == TF_CONCRETE_ENUM) return same_nominal(type->as.nominal,s->type);
    if (s->type.module_index != 0) return false;
    owner = tf_resolution_type(p->resolution,s->type);
    if (owner == NULL) return false;
    return name_is(owner->name,type->kind == TF_CONCRETE_OPTION ? "Option" : "Decision");
}

static Constructor *variant_constructor(TfTypedPatterns *p, TfTypedSignatures *s,
                                        Domain *d, const TfConcreteType *t,
                                        const TfResolvedSymbol *variant) {
    TfConcreteTypeId args[2];
    size_t arity;
    if (t->kind == TF_CONCRETE_ENUM) {
        const TfConcreteType *sig = tf_type_store_get(p->store,tf_signatures_symbol(s,variant->id));
        if (sig == NULL) { p->failed = true; return NULL; }
        if (sig->kind == TF_CONCRETE_FUNCTION)
            return constructor(p,d,variant->id,(TfLiteralValue){0},
                               sig->as.function.parameters,sig->as.function.arity);
        return constructor(p,d,variant->id,(TfLiteralValue){0},NULL,0);
    }
    if (t->kind == TF_CONCRETE_OPTION) {
        arity = name_is(variant->name,"Some") ? 1 : 0;
        args[0] = t->as.element;
    } else {
        arity = 2;
        args[0] = t->as.decision.machine;
        args[1] = t->as.decision.rejection;
        if (name_is(variant->name,"Accept") &&
            tf_type_list(p->store,t->as.decision.output,&args[1]) != TF_OK) {
            p->failed = true; return NULL;
        }
    }
    return constructor(p,d,variant->id,(TfLiteralValue){0},args,arity);
}

static void build_domain(TfTypedPatterns *p, TfTypedSignatures *s, Domain *d) {
    const TfConcreteType *t;
    const TfCoverageConstructor **items;
    size_t count = 0, at = 0;
    bool variants;
    if (p->failed || d->built) return;
    t = tf_type_store_get(p->store,d->type);
    if (t == NULL) { p->failed = true; return; }
    variants = t->kind == TF_CONCRETE_ENUM || t->kind == TF_CONCRETE_OPTION ||
               t->kind == TF_CONCRETE_DECISION;
    d->built = true;
    if (t->kind == TF_CONCRETE_BOOL) count = 2;
    else if (t->kind == TF_CONCRETE_UNIT) count = 1;
    else if (variants)
        for (size_t i=0; i<tf_resolution_symbol_count(p->resolution); ++i)
            if (enum_member(p,t,tf_resolution_symbol_at(p->resolution,i))) ++count;
    if (count == 0) return;
    items = allocate(p,count,sizeof *items);
    if (items == NULL) return;
    if (variants) {
        for (size_t i=0; i<tf_resolution_symbol_count(p->resolution) && !p->failed; ++i) {
            const TfResolvedSymbol *v = tf_resolution_symbol_at(p->resolution,i);
            if (enum_member(p,t,v)) {
                Constructor *c = variant_constructor(p,s,d,t,v);
                if (c == NULL) return;
                items[at++] = &c->coverage;
            }
        }
    } else for (size_t i=0; i<count; ++i) {
        TfLiteralValue lit = {.kind=t->kind == TF_CONCRETE_BOOL ? TF_LITERAL_BOOL : TF_LITERAL_UNIT};
        if (lit.kind == TF_LITERAL_BOOL) lit.as.boolean = i != 0;
        Constructor *c = constructor(p,d,(TfSymbolId){0},lit,NULL,0);
        if (c == NULL) return;
        items[at++] = &c->coverage;
    }
    d->coverage = (TfCoverageDomain){items,at};
}

static void diagnose(TfTypedPatterns *p, const char *code, TfSpan span,
                     const char *message, TfDiagnosticText note) {
    TfDiagnosticLabel label = {span,true,{NULL,0}};
    TfDiagnosticRecord d = {0};
    if (p->failed) return;
    d.code = code; d.severity = TF_DIAG_ERROR;
    d.message = (TfDiagnosticText){message,strlen(message)};
    d.labels = &label; d.label_count = 1;
    if (note.length != 0) { d.notes = &note; d.note_count = 1; }
    if (tf_diagnostic_bag_add(p->diagnostics,&d) != TF_OK) p->failed = true;
}

static bool literal_equal(TfLiteralValue a, TfLiteralValue b) {
    switch (a.kind) {
    case TF_LITERAL_INT: return a.as.signed_pattern == b.as.signed_pattern;
    case TF_LITERAL_FLOAT: return a.as.float_bits == b.as.float_bits;
    case TF_LITERAL_TEXT: return a.as.text.length == b.as.text.length &&
        (a.as.text.length == 0 || memcmp(a.as.text.data,b.as.text.data,a.as.text.length) == 0);
    case TF_LITERAL_CHAR: return a.as.scalar == b.as.scalar;
    case TF_LITERAL_BOOL: return a.as.boolean == b.as.boolean;
    case TF_LITERAL_UNIT: return true;
    }
    return false;
}

static uint64_t literal_hash(TfLiteralValue value) {
    uint64_t bits = 0;
    switch (value.kind) {
    case TF_LITERAL_INT: bits = (uint64_t)value.as.signed_pattern; break;
    case TF_LITERAL_FLOAT: bits = value.as.float_bits; break;
    case TF_LITERAL_CHAR: bits = value.as.scalar; break;
    case TF_LITERAL_TEXT:
        bits = UINT64_C(14695981039346656037);
        for (size_t i=0; i<value.as.text.length; ++i)
            bits = (bits ^ value.as.text.data[i])*UINT64_C(1099511628211);
        break;
    case TF_LITERAL_BOOL: case TF_LITERAL_UNIT: break;
    }
    bits = (bits ^ (bits>>30))*UINT64_C(0xbf58476d1ce4e5b9);
    bits = (bits ^ (bits>>27))*UINT64_C(0x94d049bb133111eb);
    return bits ^ (bits>>31);
}

static bool consume_probe(TfTypedPatterns *p) {
    if (p->work == 0) { p->failed = true; return false; }
    --p->work;
    return true;
}

static bool grow_literals(TfTypedPatterns *p, Domain *d) {
    Constructor **table;
    size_t capacity;
    if (d->literal_capacity > SIZE_MAX/2) { p->failed = true; return false; }
    capacity = d->literal_capacity != 0 ? 2*d->literal_capacity : 16;
    table = allocate(p,capacity,sizeof *table);
    if (table == NULL) return false;
    for (size_t i=0; i<capacity; ++i) table[i] = NULL;
    for (size_t i=0; i<d->literal_capacity; ++i) {
        Constructor *old = d->literals[i];
        if (old != NULL) {
            size_t slot = (size_t)old->hash & (capacity-1);
            while (table[slot] != NULL) {
                if (!consume_probe(p)) return false;
                slot = (slot+1) & (capacity-1);
            }
            table[slot] = old;
        }
    }
    d->literals = table;
    d->literal_capacity = capacity;
    return true;
}

static const TfCoverageConstructor *literal_constructor(TfTypedPatterns *p, Domain *d,
                                                         TfLiteralValue value) {
    const TfConcreteTypeKind kinds[] = {
        TF_CONCRETE_INT,TF_CONCRETE_FLOAT,TF_CONCRETE_BOOL,
        TF_CONCRETE_TEXT,TF_CONCRETE_CHAR,TF_CONCRETE_UNIT
    };
    if (tf_type_store_get(p->store,d->type)->kind != kinds[(size_t)value.kind]) return NULL;
    if (value.kind == TF_LITERAL_BOOL || value.kind == TF_LITERAL_UNIT) {
        const size_t index = value.kind == TF_LITERAL_BOOL && value.as.boolean ? 1u : 0u;
        if (d->coverage.constructors == NULL || index >= d->coverage.count) {
            p->failed = true; return NULL;
        }
        return d->coverage.constructors[index];
    }
    const uint64_t hash = literal_hash(value);
    if (d->literal_count == d->literal_capacity/2 && !grow_literals(p,d)) return NULL;
    const size_t mask = d->literal_capacity-1;
    size_t slot = (size_t)hash & mask;
    Constructor *c;
    while ((c = d->literals[slot]) != NULL) {
        if (!consume_probe(p)) return NULL;
        if (c->hash == hash && literal_equal(value,c->literal)) return &c->coverage;
        slot = (slot+1) & mask;
    }
    c = constructor(p,d,(TfSymbolId){0},value,NULL,0);
    if (c == NULL) return NULL;
    c->hash = hash;
    d->literals[slot] = c;
    ++d->literal_count;
    return &c->coverage;
}

static void remember(TfTypedPatterns *p, TfTypedPatternNode value) {
    void *nodes = p->nodes;
    if (p->node_count == SIZE_MAX ||
        !tf_grow(&nodes,&p->node_capacity,p->node_count+1,sizeof *p->nodes,p->allocator)) {
        p->failed = true; return;
    }
    p->nodes = nodes;
    p->nodes[p->node_count++] = value;
}

static int compare_nodes(const void *a, const void *b) {
    const size_t left = ((const TfTypedPatternNode *)a)->node.index;
    const size_t right = ((const TfTypedPatternNode *)b)->node.index;
    return (left > right) - (left < right);
}

static const TfCoveragePattern *type_pattern(TfTypedPatterns *p, TfTypedSignatures *s,
                                             const TfAstNode *n, Domain *d) {
    const TfCoverageConstructor *ctor = NULL;
    TfCoveragePattern *result;
    const TfCoveragePattern **args = NULL;
    TfTypedPatternNode typed = {0};
    if (p->failed) return NULL;
    if (n == NULL || d == NULL) { p->failed = true; return NULL; }
    typed.node = n->id;
    typed.type = d->type;
    if (n->kind == TF_AST_PATTERN_WILDCARD) { remember(p,typed); return NULL; }
    if (n->kind == TF_AST_PATTERN_BINDER) {
        const TfResolvedReference *ref = tf_resolution_reference(p->resolution,n->id);
        if (ref == NULL || ref->symbol.index == 0) p->failed = true;
        else {
            typed.target = ref->symbol;
            remember(p,typed);
        }
        return NULL;
    }
    build_domain(p,s,d);
    if (p->failed) return NULL;
    if (n->kind == TF_AST_PATTERN_LITERAL || n->kind == TF_AST_PATTERN_UNIT) {
        TfLiteralValue value;
        TfStatus status = tf_literal_value(n,NULL,&value);
        if (status == TF_HOST_ERROR) p->failed = true;
        if (status == TF_SOURCE_ERROR) {
            diagnose(p,"E301",n->span,"pattern literal is outside its numeric range",(TfDiagnosticText){0});
            p->bad_patterns = true; return NULL;
        }
        if (status == TF_OK) {
            ctor = literal_constructor(p,d,value);
            typed.has_literal = true;
            typed.literal = value;
        }
    } else if (n->kind == TF_AST_PATTERN_VARIANT) {
        const TfResolvedReference *ref = tf_resolution_reference(p->resolution,n->id);
        if (ref == NULL || ref->symbol.index == 0) { p->failed = true; return NULL; }
        typed.target = ref->symbol;
        /* Domain constructors were collected in stable symbol ID order. */
        size_t lo = 0, hi = d->coverage.count;
        while (lo<hi) {
            size_t mid = lo+(hi-lo)/2;
            const Constructor *c = (const Constructor *)d->coverage.constructors[mid];
            if (c->variant.index == ref->symbol.index && c->variant.owner == ref->symbol.owner) {
                ctor = &c->coverage; break;
            }
            if (c->variant.index < ref->symbol.index) lo = mid+1;
            else hi = mid;
        }
        if (ctor != NULL && (ctor->arity != n->as.pattern_variant.arguments.count ||
                             n->as.pattern_variant.parentheses != (ctor->arity != 0))) ctor = NULL;
        if (ctor != NULL && ctor->arity != 0) {
            args = allocate(p,ctor->arity,sizeof *args);
            if (args == NULL) return NULL;
            for (size_t i=0; i<ctor->arity && !p->failed; ++i)
                args[i] = type_pattern(p,s,tf_ast_get(n->id.owner,n->as.pattern_variant.arguments.items[i]),
                                       (Domain *)ctor->payload[i]);
        }
    } else p->failed = true;
    if (p->failed) return NULL;
    if (ctor == NULL) {
        diagnose(p,"E312",n->span,"pattern does not match the scrutinee type or variant arity",(TfDiagnosticText){0});
        p->bad_patterns = true; return NULL;
    }
    remember(p,typed);
    result = allocate(p,1,sizeof *result);
    if (result != NULL) *result = (TfCoveragePattern){ctor,args};
    return result;
}

typedef struct { char *text; size_t count, capacity; } Render;
static void append(TfTypedPatterns *p, Render *r, const void *bytes, size_t n) {
    void *data = r->text;
    if (p->failed) return;
    if (n > SIZE_MAX-r->count ||
        !tf_grow(&data,&r->capacity,r->count+n,1,p->allocator)) {
        p->failed = true; return;
    }
    r->text = data;
    memcpy(r->text+r->count,bytes,n);
    r->count += n;
}

static void witness(TfTypedPatterns *p, Render *r, const TfCoveragePattern *pattern,
                    const TfResolvedModule *module) {
    const Constructor *c;
    const TfResolvedSymbol *v;
    const TfResolvedType *t;
    if (p->failed) return;
    if (pattern == NULL) { append(p,r,"_",1); return; }
    c = (const Constructor *)pattern->constructor;
    v = tf_resolution_symbol(p->resolution,c->variant);
    if (v == NULL) {
        if (c->literal.kind == TF_LITERAL_BOOL)
            append(p,r,c->literal.as.boolean ? "true" : "false",c->literal.as.boolean ? 4u : 5u);
        else if (c->literal.kind == TF_LITERAL_UNIT) append(p,r,"()",2);
        else append(p,r,"_",1); /* Missing witnesses never require an open literal. */
        return;
    }
    t = tf_resolution_type(p->resolution,v->type);
    if (t == NULL) { p->failed = true; return; }
    if (t->id.module_index != 0 && t->id.module_index != module->id.index) {
        const TfResolvedSymbol *alias = NULL;
        for (size_t i=0; i<tf_resolution_symbol_count(p->resolution); ++i) {
            const TfResolvedSymbol *a = tf_resolution_symbol_at(p->resolution,i);
            if (a->kind == TF_SYMBOL_ALIAS && a->module.index == module->id.index &&
                a->target_module.index == t->id.module_index) { alias = a; break; }
        }
        if (alias == NULL) { append(p,r,"_",1); return; }
        append(p,r,alias->name.data,alias->name.length); append(p,r,"::",2);
    }
    append(p,r,t->name.data,t->name.length); append(p,r,"::",2);
    append(p,r,v->name.data,v->name.length);
    if (c->coverage.arity != 0) {
        append(p,r,"(",1);
        for (size_t i=0; i<c->coverage.arity && !p->failed; ++i) {
            if (i != 0) append(p,r,", ",2);
            witness(p,r,pattern->arguments[i],module);
        }
        append(p,r,")",1);
    }
}

static bool insert_useful(TfTypedPatterns *p, Constructor *c, const TfCoveragePattern *pattern) {
    TfCoverageWitness *w = NULL;
    bool useful = true;
    if (c->covered) return false;
    if (c->row_count != 0) {
        TfStatus status = tf_coverage_witness_budget(&w,c->coverage.domain,c->rows,c->row_count,
                                                     pattern,&p->allocator,&p->work);
        useful = w != NULL;
        tf_coverage_destroy(w);
        if (status != TF_OK) { p->failed = true; return false; }
    }
    if (!useful) return false;
    for (size_t i=0; i<c->coverage.arity; ++i) {
        if (pattern->arguments[i] == NULL) continue;
        if (c->row_count == c->row_capacity) {
            const size_t capacity = c->row_capacity != 0 ? c->row_capacity*2 : 4;
            const TfCoveragePattern **rows;
            if (capacity <= c->row_capacity) { p->failed = true; return false; }
            rows = allocate(p,capacity,sizeof *rows);
            if (rows == NULL) return false;
            if (c->row_count != 0) memcpy(rows,c->rows,c->row_count*sizeof *rows);
            c->rows = rows; c->row_capacity = capacity;
        }
        c->rows[c->row_count++] = pattern;
        return true;
    }
    /* A nullary constructor or a constructor with only wildcards covers
     * its whole branch. No future query needs to inspect earlier rows. */
    c->covered = true;
    return true;
}

static bool uncovered(TfTypedPatterns *p, const Domain *d, TfCoveragePattern *query,
                      const TfCoveragePattern **residual, TfCoverageWitness **owned) {
    *residual = NULL; *owned = NULL;
    if (d->coverage.count == 0) return true; /* Open-domain wildcard. */
    for (size_t i=0; i<d->coverage.count && !p->failed; ++i) {
        Constructor *c = (Constructor *)d->coverage.constructors[i];
        const TfCoveragePattern **args = NULL;
        if (c->covered) continue;
        if (c->coverage.arity != 0) {
            args = allocate(p,c->coverage.arity,sizeof *args);
            if (args == NULL) return false;
            for (size_t j=0; j<c->coverage.arity; ++j) args[j] = NULL;
        }
        *query = (TfCoveragePattern){&c->coverage,args};
        if (c->row_count == 0) { *residual = query; return true; }
        if (tf_coverage_witness_budget(owned,&d->coverage,c->rows,c->row_count,query,
                                       &p->allocator,&p->work) != TF_OK) {
            p->failed = true; return false;
        }
        if (*owned != NULL) { *residual = tf_coverage_root(*owned); return true; }
        c->covered = true;
    }
    return false;
}

TfStatus tf_typed_patterns(TfTypedPatterns **out, const TfResolution *resolution,
                           TfTypedSignatures *s, const TfAstNode *match,
                           TfConcreteTypeId scrutinee, TfDiagnosticBag *destination,
                           const TfAllocator *allocator) {
    TfAllocator a = tf_allocator(allocator);
    TfTypeStore *store = tf_signatures_store(s);
    TfTypedPatterns *p;
    Domain *root;
    const TfCoveragePattern **patterns;
    const TfResolvedModule *module = NULL;
    size_t count;
    bool total = false;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (match == NULL || match->kind != TF_AST_MATCH || destination == NULL || !tf_allocator_valid(a) ||
        tf_type_store_get(store,scrutinee) == NULL ||
        tf_signatures_resolution(s) != resolution ||
        tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) != TF_DIAG_PHASE_VALID ||
        tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(s))) return TF_HOST_ERROR;
    for (size_t i=0; i<tf_resolution_module_count(resolution); ++i) {
        const TfResolvedModule *m = tf_resolution_module_at(resolution,i);
        if (m->ast == match->id.owner) { module = m; break; }
    }
    if (module == NULL || tf_ast_get(module->ast,match->id) != match) return TF_HOST_ERROR;
    p = a.allocate(a.context,sizeof *p);
    if (p == NULL) return TF_HOST_ERROR;
    *p = (TfTypedPatterns){.resolution=resolution,.store=store,.allocator=a,.diagnostics=destination,
                          .ast=module->ast,.work=TF_COVERAGE_WORK};
    root = domain_for(p,scrutinee);
    count = match->as.matching.arms.count;
    patterns = allocate(p,count,sizeof *patterns);
    for (size_t i=0; i<count && !p->failed; ++i) {
        const TfAstNode *arm = tf_ast_get(module->ast,match->as.matching.arms.items[i]);
        patterns[i] = type_pattern(p,s,tf_ast_get(module->ast,arm->as.arm.pattern),root);
    }
    for (size_t i=0; i<=count && !p->failed && !p->bad_patterns; ++i) {
        TfCoverageWitness *w = NULL;
        TfCoveragePattern missing = {0};
        const TfCoveragePattern *residual = NULL;
        const TfCoveragePattern *current = i<count ? patterns[i] : NULL;
        bool useful = false;
        if (total && i==count) break;
        if (!total) {
            if (current != NULL) useful = insert_useful(p,(Constructor *)current->constructor,current);
            else {
                useful = uncovered(p,root,&missing,&residual,&w);
                if (i<count) total = true; /* A catchall covers the entire residual. */
            }
        }
        if (i<count && !useful) {
            const TfAstNode *arm = tf_ast_get(module->ast,match->as.matching.arms.items[i]);
            const TfAstNode *pat = tf_ast_get(module->ast,arm->as.arm.pattern);
            diagnose(p,"E311",pat->span,"match arm is already covered",(TfDiagnosticText){0});
        } else if (i==count && useful) {
            Render render = {0};
            append(p,&render,"missing pattern: ",17);
            witness(p,&render,residual,module);
            diagnose(p,"E310",match->span,"match is not exhaustive",
                     (TfDiagnosticText){render.text,render.count});
            if (render.text != NULL) a.deallocate(a.context,render.text);
        }
        tf_coverage_destroy(w);
        if (i==count) break;
    }
    if (p->failed) { tf_typed_patterns_destroy(p); return TF_HOST_ERROR; }
    if (p->node_count > 1) qsort(p->nodes,p->node_count,sizeof *p->nodes,compare_nodes);
    *out = p;
    return TF_OK;
}

TfConcreteTypeId tf_typed_pattern_binding(const TfTypedPatterns *p, TfSymbolId symbol) {
    const TfResolvedSymbol *s = p == NULL ? NULL : tf_resolution_symbol(p->resolution,symbol);
    const TfTypedPatternNode *n = s != NULL && s->kind == TF_SYMBOL_PATTERN ?
                                 tf_typed_pattern_node(p,s->declaration) : NULL;
    return n == NULL ? (TfConcreteTypeId){0} : n->type;
}

const TfTypedPatternNode *tf_typed_pattern_node(const TfTypedPatterns *p, TfNodeId node) {
    if (p != NULL && node.owner == p->ast && node.index != 0) {
        size_t lo = 0, hi = p->node_count;
        while (lo<hi) {
            size_t mid = lo+(hi-lo)/2;
            size_t index = p->nodes[mid].node.index;
            if (index == node.index) return &p->nodes[mid];
            if (index < node.index) lo = mid+1;
            else hi = mid;
        }
    }
    return NULL;
}

void tf_typed_patterns_destroy(TfTypedPatterns *p) {
    if (p != NULL) {
        const TfAllocator a = p->allocator;
        Chunk *c = p->chunks;
        while (c != NULL) {
            Chunk *next = c->next;
            a.deallocate(a.context,c);
            c = next;
        }
        if (p->nodes != NULL) a.deallocate(a.context,p->nodes);
        a.deallocate(a.context,p);
    }
}
