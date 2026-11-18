#include "ternfold/machine_plan.h"
#include <stdlib.h>
#include <string.h>

/* Each allocation is an aligned phase-arena segment. Destruction is iterative,
 * allocation never moves published records, and workspace has a shorter arena. */
typedef union Segment Segment;
union Segment { struct { Segment *next; } link; max_align_t alignment; };
typedef struct { TfAllocator allocator; Segment *head; bool failed; } Arena;
struct TfMachineProgram {
    Arena arena;
    const TfTypedProgram *typed;
    TfDiagnosticBag *diagnostics;
    TfDiagnosticPhaseResult phase;
    TfMachinePlan *plans;
    size_t count;
};
typedef struct { size_t machine, cell; } SymbolLocation;
typedef struct { const TfAst *ast; size_t *symbols; } BindingIndex;
typedef struct { size_t from, to, read; } Occurrence;
typedef struct { size_t vertex, cursor; } Frame;

static void *system_allocate(void *context, size_t size) { (void)context; return malloc(size); }
static void system_deallocate(void *context, void *ptr) { (void)context; free(ptr); }
static void *allocate(Arena *arena, size_t count, size_t width) {
    Segment *segment;
    size_t bytes;
    if (count == 0) return NULL;
    if (width == 0 || count > (SIZE_MAX - sizeof *segment) / width) {
        arena->failed = true; return NULL;
    }
    bytes = count * width;
    segment = arena->allocator.allocate(arena->allocator.context,sizeof *segment + bytes);
    if (segment == NULL) { arena->failed = true; return NULL; }
    segment->link.next = arena->head; arena->head = segment;
    memset(segment + 1,0,bytes);
    return segment + 1;
}
static void clear(Arena *arena) {
    while (arena->head != NULL) {
        Segment *next = arena->head->link.next;
        arena->allocator.deallocate(arena->allocator.context,arena->head);
        arena->head = next;
    }
}
static const TfAstNode *node(TfNodeId id) { return tf_ast_get(id.owner,id); }
static TfMachineClause clause(const TfAstNode *n, TfNodeId expression) {
    return (TfMachineClause){n->id,expression,n->span};
}
static TfDiagnosticText text(const char *s) { return (TfDiagnosticText){s,strlen(s)}; }
static bool lexical_read(const TfResolvedReference *ref) {
    const TfAstNode *n;
    if (ref->kind != TF_REFERENCE_OLD && ref->kind != TF_REFERENCE_INPUT &&
        ref->kind != TF_REFERENCE_CANDIDATE) return false;
    /* A resolved path also records its spelling segments. Only the whole
     * path or next projection is an expression occurrence. */
    n = node(ref->node);
    return n != NULL && (n->kind == TF_AST_NEXT ||
                        (n->kind == TF_AST_PATH && n->as.path.segments.count != 0));
}
static bool index_bindings(const TfResolution *r, BindingIndex *bindings, size_t module_count) {
    for (size_t i = 0; i < tf_resolution_reference_count(r); ++i) {
        const TfResolvedReference *ref = tf_resolution_reference_at(r,i);
        const TfResolvedSymbol *symbol;
        BindingIndex *index;
        if (ref->kind != TF_REFERENCE_DECLARATION && ref->kind != TF_REFERENCE_WRITER) continue;
        symbol = tf_resolution_symbol(r,ref->symbol);
        if (symbol == NULL || (symbol->kind != TF_SYMBOL_CELL && symbol->kind != TF_SYMBOL_MACHINE_INPUT)) continue;
        if (symbol->module.index == 0 || symbol->module.index > module_count) return false;
        index = &bindings[symbol->module.index - 1];
        if (index->symbols == NULL || index->ast != ref->node.owner || ref->node.index == 0 ||
            ref->node.index > tf_ast_count(index->ast)) return false;
        index->symbols[ref->node.index - 1] = ref->symbol.index;
    }
    return true;
}
static TfSymbolId binding(const TfResolution *r, const BindingIndex *index, TfNodeId id) {
    return index->symbols != NULL && id.owner == index->ast &&
           id.index != 0 && id.index <= tf_ast_count(index->ast)
        ? (TfSymbolId){r,index->symbols[id.index - 1]} : (TfSymbolId){0};
}
static bool diagnose(TfMachineProgram *p, const char *code, const char *message,
                     TfSpan primary, const TfSpan *related) {
    TfDiagnosticLabel labels[2] = {{primary,true,{0}},{0}};
    TfDiagnosticRecord record = {0};
    labels[0].message = text(message);
    if (related != NULL) labels[1] = (TfDiagnosticLabel){*related,false,text("state cell declared here")};
    record.code = code; record.severity = TF_DIAG_ERROR; record.message = text(message);
    record.labels = labels; record.label_count = related != NULL ? 2u : 1u;
    return tf_diagnostic_bag_add(p->diagnostics,&record) == TF_OK;
}
static int read_compare(const void *a, const void *b) {
    const TfMachineRead *x = a, *y = b;
    return x->span.start < y->span.start ? -1 : x->span.start > y->span.start ? 1 :
           x->node.index < y->node.index ? -1 : x->node.index > y->node.index ? 1 : 0;
}
static int occurrence_compare(const void *a, const void *b) {
    const Occurrence *x = a, *y = b;
    if (x->from != y->from) return x->from < y->from ? -1 : 1;
    if (x->to != y->to) return x->to < y->to ? -1 : 1;
    return x->read < y->read ? -1 : x->read > y->read ? 1 : 0;
}
static int name_compare(const TfMachinePlan *m, size_t a, size_t b) {
    TfAstText x = m->cells[a].name, y = m->cells[b].name;
    size_t length = x.length < y.length ? x.length : y.length;
    int result = memcmp(x.data,y.data,length);
    return result != 0 ? result : x.length < y.length ? -1 : x.length > y.length ? 1 : 0;
}
static void heap_push(const TfMachinePlan *m, size_t *heap, size_t *count, size_t value) {
    size_t i = (*count)++;
    while (i != 0) {
        size_t parent = (i - 1) / 2;
        if (name_compare(m,heap[parent],value) <= 0) break;
        heap[i] = heap[parent]; i = parent;
    }
    heap[i] = value;
}
static size_t heap_pop(const TfMachinePlan *m, size_t *heap, size_t *count) {
    size_t result = heap[0], value = heap[--*count], i = 0;
    while (i < *count / 2) {
        size_t child = i * 2 + 1;
        if (child + 1 < *count && name_compare(m,heap[child + 1],heap[child]) < 0) ++child;
        if (name_compare(m,value,heap[child]) <= 0) break;
        heap[i] = heap[child]; i = child;
    }
    if (*count != 0) heap[i] = value;
    return result;
}

static bool skeleton(TfMachineProgram *p, TfMachinePlan *m, const BindingIndex *index,
                     SymbolLocation *locations, size_t machine, Arena *work) {
    const TfResolution *r = tf_typed_program_resolution(p->typed);
    const TfAstNode *decl = node(m->declaration);
    TfMachineCell *cells;
    TfMachineWriter *writers;
    TfMachineGuard *guards;
    TfMachineClause *emissions;
    size_t c = 0, w = 0, g = 0, e = 0;
    size_t *counts, *offsets, *indexes;
    m->input = binding(r,index,decl->as.machine.parameter);
    if (m->input.index == 0) return false;
    locations[m->input.index] = (SymbolLocation){machine,SIZE_MAX};
    for (size_t i = 0; i < decl->as.machine.sections.count; ++i) {
        switch (node(decl->as.machine.sections.items[i])->kind) {
        case TF_AST_STATE: ++m->cell_count; break;
        case TF_AST_WRITER: ++m->writer_count; break;
        case TF_AST_GUARD: ++m->guard_count; break;
        case TF_AST_EMIT: ++m->emission_count; break;
        default: return false;
        }
    }
    cells = allocate(&p->arena,m->cell_count,sizeof *cells);
    writers = allocate(&p->arena,m->writer_count,sizeof *writers);
    guards = allocate(&p->arena,m->guard_count,sizeof *guards);
    emissions = allocate(&p->arena,m->emission_count,sizeof *emissions);
    counts = allocate(work,m->cell_count,sizeof *counts);
    offsets = allocate(work,m->cell_count,sizeof *offsets);
    indexes = allocate(&p->arena,m->writer_count,sizeof *indexes);
    if (p->arena.failed || work->failed) return false;
    m->cells = cells; m->writers = writers; m->guards = guards; m->emissions = emissions;
    /* State sections precede writers; populate the direct symbol map first. */
    for (size_t i = 0; i < decl->as.machine.sections.count; ++i) {
        const TfAstNode *n = node(decl->as.machine.sections.items[i]);
        TfSymbolId symbol;
        if (n->kind != TF_AST_STATE) continue;
        symbol = binding(r,index,n->id);
        if (symbol.index == 0) return false;
        cells[c] = (TfMachineCell){.id=c,.symbol=symbol,.name=n->as.state.name,
            .type=tf_typed_program_symbol(p->typed,symbol),.declaration=clause(n,n->as.state.value)};
        locations[symbol.index] = (SymbolLocation){machine,c}; ++c;
    }
    for (size_t i = 0; i < decl->as.machine.sections.count; ++i) {
        const TfAstNode *n = node(decl->as.machine.sections.items[i]);
        if (n->kind == TF_AST_WRITER) {
            TfSymbolId symbol = binding(r,index,n->id);
            size_t cell;
            if (symbol.index == 0 || locations[symbol.index].machine != machine) return false;
            cell = locations[symbol.index].cell;
            if (cell >= m->cell_count) return false;
            writers[w++] = (TfMachineWriter){clause(n,n->as.writer.value),cell};
            ++counts[cell];
        } else if (n->kind == TF_AST_GUARD)
            guards[g++] = (TfMachineGuard){clause(n,n->as.guard.condition),n->as.guard.rejection};
        else if (n->kind == TF_AST_EMIT) emissions[e++] = clause(n,n->as.emission.value);
    }
    w = 0;
    for (size_t i = 0; i < c; ++i) {
        cells[i].writer_count = counts[i];
        cells[i].writers = counts[i] != 0 ? indexes + w : NULL;
        offsets[i] = w; w += counts[i];
        if (counts[i] == 0 && !diagnose(p,"E401","state cell has no next writer",cells[i].name.span,NULL)) return false;
    }
    for (size_t i = 0; i < m->writer_count; ++i) {
        size_t cell = writers[i].cell, at = offsets[cell]++;
        indexes[at] = i;
        if (at != (size_t)(cells[cell].writers - indexes) &&
            !diagnose(p,"E402","state cell has more than one next writer",
                      node(writers[i].clause.node)->as.writer.name.span,&cells[cell].name.span)) return false;
    }
    return true;
}

/* Classify with ordered section spans, without traversing/evaluating branches.
 * Resolution already includes all lexical references, even inside closures. */
static bool classify(TfMachinePlan *m, TfMachineRead *read) {
    const TfAstNode *decl = node(m->declaration), *section;
    size_t lo = 0, hi = decl->as.machine.sections.count, i;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (node(decl->as.machine.sections.items[mid])->span.start <= read->span.start) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return false;
    section = node(decl->as.machine.sections.items[lo - 1]);
    if (read->span.end > section->span.end) return false;
    i = lo - 1;
    if (section->kind == TF_AST_STATE) { read->context = TF_MACHINE_INITIALIZER; read->clause_index = i; }
    else if (section->kind == TF_AST_WRITER) { read->context = TF_MACHINE_WRITER; read->clause_index = i - m->cell_count; }
    else if (section->kind == TF_AST_GUARD) {
        read->clause_index = i - m->cell_count - m->writer_count;
        read->context = read->span.start >= node(section->as.guard.rejection)->span.start
            ? TF_MACHINE_REJECTION : TF_MACHINE_GUARD;
    } else if (section->kind == TF_AST_EMIT) {
        read->context = TF_MACHINE_EMISSION;
        read->clause_index = i - m->cell_count - m->writer_count - m->guard_count;
    } else return false;
    return true;
}

static bool graph(TfMachineProgram *p, TfMachinePlan *m, Arena *work) {
    size_t n = m->cell_count, occurrence_count = 0, edge_count = 0;
    Occurrence *occurrences;
    TfMachineDependency *edges;
    size_t *read_indexes;
    size_t *out, *in, *reverse, *cursor, *order, *component, *sizes, *stack, *indegree, *heap;
    bool *seen, *cyclic;
    Frame *frames;
    size_t order_count = 0, components = 0;
    bool has_cycle = false;
    if (work->failed || n == SIZE_MAX) return false;
    /* Coverage is a prerequisite: never build a graph from ambiguous writers. */
    for (size_t i = 0; i < n; ++i) if (m->cells[i].writer_count != 1) return true;
    occurrences = allocate(work,m->read_count,sizeof *occurrences);
    if (work->failed) return false;
    for (size_t i = 0; i < m->read_count; ++i) {
        const TfMachineRead *read = &m->reads[i];
        if (read->kind == TF_REFERENCE_CANDIDATE && read->context == TF_MACHINE_WRITER)
            occurrences[occurrence_count++] = (Occurrence){read->cell,m->writers[read->clause_index].cell,i};
    }
    if (occurrence_count > 1) qsort(occurrences,occurrence_count,sizeof *occurrences,occurrence_compare);
    for (size_t i = 0; i < occurrence_count; ++i)
        if (i == 0 || occurrences[i].from != occurrences[i-1].from || occurrences[i].to != occurrences[i-1].to) ++edge_count;
    edges = allocate(&p->arena,edge_count,sizeof *edges);
    read_indexes = allocate(&p->arena,occurrence_count,sizeof *read_indexes);
    if (p->arena.failed) return false;
    m->dependencies = edges; m->dependency_count = edge_count;
    edge_count = 0;
    for (size_t i = 0; i < occurrence_count; ++i) {
        if (i == 0 || occurrences[i].from != occurrences[i-1].from || occurrences[i].to != occurrences[i-1].to) {
            edges[edge_count++] = (TfMachineDependency){occurrences[i].from,occurrences[i].to,read_indexes + i,0};
        }
        read_indexes[i] = occurrences[i].read; ++edges[edge_count-1].occurrence_count;
    }
    out = allocate(work,n + 1,sizeof *out); in = allocate(work,n + 1,sizeof *in);
    reverse = allocate(work,edge_count,sizeof *reverse); cursor = allocate(work,n,sizeof *cursor);
    order = allocate(work,n,sizeof *order); component = allocate(work,n,sizeof *component);
    sizes = allocate(work,n,sizeof *sizes); stack = allocate(work,n,sizeof *stack);
    indegree = allocate(work,n,sizeof *indegree); heap = allocate(work,n,sizeof *heap);
    seen = allocate(work,n,sizeof *seen); cyclic = allocate(work,n,sizeof *cyclic);
    frames = allocate(work,n,sizeof *frames);
    if (work->failed) return false;
    for (size_t i = 0; i < edge_count; ++i) { ++out[edges[i].prerequisite + 1]; ++in[edges[i].dependent + 1]; }
    for (size_t i = 1; i <= n; ++i) { out[i] += out[i-1]; in[i] += in[i-1]; }
    for (size_t i = 0; i < n; ++i) { cursor[i] = in[i]; component[i] = SIZE_MAX; }
    for (size_t i = 0; i < edge_count; ++i) reverse[cursor[edges[i].dependent]++] = edges[i].prerequisite;
    /* Iterative Kosaraju; edges are already grouped by prerequisite. */
    for (size_t root = 0; root < n; ++root) {
        size_t depth = 0;
        if (seen[root]) continue;
        seen[root] = true; frames[depth++] = (Frame){root,out[root]};
        while (depth != 0) {
            Frame *f = &frames[depth-1];
            if (f->cursor < out[f->vertex+1]) {
                size_t next = edges[f->cursor++].dependent;
                if (!seen[next]) { seen[next] = true; frames[depth++] = (Frame){next,out[next]}; }
            } else { order[order_count++] = f->vertex; --depth; }
        }
    }
    while (order_count != 0) {
        size_t root = order[--order_count], depth = 0;
        if (component[root] != SIZE_MAX) continue;
        component[root] = components; stack[depth++] = root;
        while (depth != 0) {
            size_t v = stack[--depth]; ++sizes[components];
            for (size_t i = in[v]; i < in[v+1]; ++i) {
                size_t next = reverse[i];
                if (component[next] == SIZE_MAX) { component[next] = components; stack[depth++] = next; }
            }
        }
        ++components;
    }
    for (size_t i = 0; i < components; ++i) cyclic[i] = sizes[i] > 1;
    for (size_t i = 0; i < edge_count; ++i)
        if (edges[i].prerequisite == edges[i].dependent) cyclic[component[edges[i].prerequisite]] = true;
    /* One diagnostic per cyclic SCC, every internal projection occurrence is
     * a witness. Build all label groups in one pass, including cap suppression. */
    memset(sizes,0,n * sizeof *sizes);
    for (size_t i = 0; i < edge_count; ++i) {
        size_t c = component[edges[i].prerequisite];
        if (cyclic[c] && c == component[edges[i].dependent]) sizes[c] += edges[i].occurrence_count;
    }
    {
        TfDiagnosticLabel *labels = allocate(work,occurrence_count,sizeof *labels);
        size_t total = 0;
        if (work->failed) return false;
        for (size_t c = 0; c < components; ++c) { cursor[c] = total; order[c] = total; total += sizes[c]; }
        for (size_t i = 0; i < edge_count; ++i) {
            size_t c = component[edges[i].prerequisite];
            if (!cyclic[c] || c != component[edges[i].dependent]) continue;
            for (size_t j = 0; j < edges[i].occurrence_count; ++j)
                labels[cursor[c]++] = (TfDiagnosticLabel){m->reads[edges[i].occurrences[j]].span,false,text("candidate dependency participates in this cycle")};
        }
        for (size_t c = 0; c < components; ++c) {
            TfDiagnosticText help = text("Use the bare cell name to read the old snapshot; next.cell reads the candidate and creates a dependency.");
            TfDiagnosticRecord record = {0};
            if (!cyclic[c]) continue;
            has_cycle = true; labels[order[c]].primary = true;
            record.code = "E404"; record.severity = TF_DIAG_ERROR;
            record.message = text("cyclic candidate-state dependencies");
            record.labels = labels + order[c]; record.label_count = sizes[c];
            record.help = &help; record.help_count = 1;
            if (tf_diagnostic_bag_add(p->diagnostics,&record) != TF_OK) return false;
        }
    }
    if (!has_cycle) {
        size_t ready = 0, count = 0;
        size_t *schedule = allocate(&p->arena,n,sizeof *schedule);
        if (p->arena.failed) return false;
        for (size_t i = 0; i < n; ++i) {
            indegree[i] = in[i+1] - in[i];
            if (indegree[i] == 0) heap_push(m,heap,&ready,i);
        }
        while (ready != 0) {
            size_t v = heap_pop(m,heap,&ready); schedule[count++] = v;
            for (size_t i = out[v]; i < out[v+1]; ++i)
                if (--indegree[edges[i].dependent] == 0) heap_push(m,heap,&ready,edges[i].dependent);
        }
        if (count != n) return false;
        m->schedule = schedule; m->schedule_count = count;
    }
    return true;
}

TfStatus tf_machine_program_build(TfMachineProgram **out, const TfTypedProgram *typed,
                                  size_t diagnostic_cap, const TfAllocator *allocator) {
    TfAllocator a = {NULL,system_allocate,system_deallocate};
    TfMachineProgram *p;
    Arena work = {0}, local = {0};
    const TfResolution *r;
    SymbolLocation *locations;
    size_t *counts, symbol_count, module_count, m = 0;
    BindingIndex *bindings;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (typed == NULL || diagnostic_cap > 100 ||
        tf_diagnostic_phase_kind(tf_typed_program_phase(typed)) != TF_DIAG_PHASE_VALID) return TF_HOST_ERROR;
    if (allocator != NULL) a = *allocator;
    if (a.allocate == NULL || a.deallocate == NULL) return TF_HOST_ERROR;
    p = a.allocate(a.context,sizeof *p);
    if (p == NULL) return TF_HOST_ERROR;
    *p = (TfMachineProgram){.arena={.allocator=a},.typed=typed}; work.allocator = a; local.allocator = a;
    if (tf_diagnostic_bag_create(&p->diagnostics,diagnostic_cap,&a) != TF_OK) goto fail;
    r = tf_typed_program_resolution(typed);
    for (size_t i = 0; i < tf_resolution_type_count(r); ++i)
        if (tf_resolution_type_at(r,i)->kind == TF_TYPE_MACHINE) ++p->count;
    p->plans = allocate(&p->arena,p->count,sizeof *p->plans);
    if (p->arena.failed) goto fail;
    if (p->count == 0) goto freeze;
    symbol_count = tf_resolution_symbol_count(r); module_count = tf_resolution_module_count(r);
    if (symbol_count == SIZE_MAX) goto fail;
    locations = allocate(&work,symbol_count + 1,sizeof *locations);
    counts = allocate(&work,p->count,sizeof *counts);
    bindings = allocate(&work,module_count,sizeof *bindings);
    if (work.failed) goto fail;
    for (size_t i = 0; i <= symbol_count; ++i) locations[i].machine = SIZE_MAX;
    for (size_t i = 0; i < module_count; ++i) bindings[i] = (BindingIndex){0};
    for (size_t i = 0; i < tf_resolution_type_count(r); ++i) {
        const TfResolvedType *t = tf_resolution_type_at(r,i);
        BindingIndex *index;
        if (t->kind != TF_TYPE_MACHINE) continue;
        if (t->id.module_index == 0 || t->id.module_index > module_count) goto fail;
        index = &bindings[t->id.module_index - 1];
        if (index->ast == NULL) {
            index->ast = t->declaration.owner;
            index->symbols = allocate(&work,tf_ast_count(index->ast),sizeof *index->symbols);
            if (work.failed) goto fail;
        }
        p->plans[m++] = (TfMachinePlan){.type=t->id,.declaration=t->declaration,
                                      .span=node(t->declaration)->span,.name=t->name};
    }
    if (!index_bindings(r,bindings,module_count)) goto fail;
    for (m = 0; m < p->count; ++m) {
        TfMachinePlan *plan = &p->plans[m];
        if (!skeleton(p,plan,&bindings[plan->type.module_index - 1],locations,m,&local)) goto fail;
        clear(&local);
    }
    for (size_t i = 0; i < tf_resolution_reference_count(r); ++i) {
        const TfResolvedReference *ref = tf_resolution_reference_at(r,i);
        if (lexical_read(ref)) {
            if (ref->symbol.owner != r || ref->symbol.index == 0 || ref->symbol.index > symbol_count) goto fail;
            m = locations[ref->symbol.index].machine;
            if (m == SIZE_MAX) goto fail;
            ++counts[m];
        }
    }
    for (m = 0; m < p->count; ++m) {
        p->plans[m].reads = allocate(&p->arena,counts[m],sizeof *p->plans[m].reads);
        p->plans[m].read_count = counts[m]; counts[m] = 0;
    }
    if (p->arena.failed) goto fail;
    for (size_t i = 0; i < tf_resolution_reference_count(r); ++i) {
        const TfResolvedReference *ref = tf_resolution_reference_at(r,i);
        if (lexical_read(ref)) {
            SymbolLocation loc = locations[ref->symbol.index];
            TfMachinePlan *plan = &p->plans[loc.machine];
            TfMachineRead *read = &((TfMachineRead *)plan->reads)[counts[loc.machine]++];
            *read = (TfMachineRead){.kind=ref->kind,.symbol=ref->symbol,.node=ref->node,
                                   .span=node(ref->node)->span,.cell=loc.cell};
            if (!classify(plan,read)) goto fail;
        }
    }
    clear(&work);
    for (m = 0; m < p->count; ++m) {
        if (p->plans[m].read_count > 1)
            qsort((TfMachineRead *)p->plans[m].reads,p->plans[m].read_count,sizeof *p->plans[m].reads,read_compare);
        if (!graph(p,&p->plans[m],&local)) goto fail;
        clear(&local);
    }
freeze:
    if ((tf_diagnostic_bag_has_errors(p->diagnostics)
         ? tf_diagnostic_phase_error(&p->phase,p->diagnostics)
         : tf_diagnostic_phase_valid(&p->phase,p,p->diagnostics)) != TF_OK) goto fail;
    *out = p; return TF_OK;
fail:
    clear(&local); clear(&work); tf_machine_program_destroy(p); return TF_HOST_ERROR;
}
void tf_machine_program_destroy(TfMachineProgram *p) {
    TfAllocator a;
    if (p == NULL) return;
    a = p->arena.allocator; tf_diagnostic_bag_destroy(p->diagnostics); clear(&p->arena);
    a.deallocate(a.context,p);
}
const TfTypedProgram *tf_machine_program_typed(const TfMachineProgram *p) { return p != NULL ? p->typed : NULL; }
const TfDiagnosticBag *tf_machine_program_diagnostics(const TfMachineProgram *p) { return p != NULL ? p->diagnostics : NULL; }
const TfDiagnosticPhaseResult *tf_machine_program_phase(const TfMachineProgram *p) { return p != NULL ? &p->phase : NULL; }
size_t tf_machine_program_count(const TfMachineProgram *p) { return p != NULL ? p->count : 0; }
const TfMachinePlan *tf_machine_program_at(const TfMachineProgram *p, size_t i) { return p != NULL && i < p->count ? &p->plans[i] : NULL; }
const TfMachinePlan *tf_machine_program_get(const TfMachineProgram *p, TfTypeId type) {
    if (p == NULL || type.owner != tf_typed_program_resolution(p->typed) || type.declaration_index == 0) return NULL;
    for (size_t i = 0; i < p->count; ++i)
        if (p->plans[i].type.module_index == type.module_index && p->plans[i].type.declaration_index == type.declaration_index) return &p->plans[i];
    return NULL;
}
