#include "ternfold/machine_plan.h"
#include "../src/typed_signatures.h"
#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr,"machine_plan:%d: %s\n",__LINE__,#c); exit(1); } } while (0)
typedef struct {
    TfSourceManager *manager;
    TfResolution *resolution;
    TfTypedSignatures *signatures;
    TfTypedProgram *typed;
    TfMachineProgram *program;
} Fixture;
typedef struct { size_t calls, fail_at, live; } Counter;
static void *count_allocate(void *context, size_t bytes) {
    Counter *c = context;
    void *p;
    if (++c->calls == c->fail_at) return NULL;
    p = malloc(bytes); if (p != NULL) ++c->live;
    return p;
}
static void count_free(void *context, void *p) {
    Counter *c = context;
    CHECK(p != NULL && c->live != 0); --c->live; free(p);
}
static bool named(TfAstText name, const char *s) {
    return name.length == strlen(s) && memcmp(name.data,s,name.length) == 0;
}
static void phase(const TfDiagnosticPhaseResult *result, TfDiagnosticPhaseKind kind) {
    if (tf_diagnostic_phase_kind(result) != kind)
        (void)tf_diagnostic_bag_write(stderr,tf_diagnostic_phase_bag(result),TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
    CHECK(tf_diagnostic_phase_kind(result) == kind);
}
static Fixture load(const char *path) {
    Fixture f = {0};
    CHECK(tf_source_manager_create(&f.manager,NULL) == TF_OK);
    CHECK(tf_resolve(&f.resolution,f.manager,path,NULL,NULL) == TF_OK);
    return f;
}
static void type(Fixture *f) {
    phase(tf_resolution_phase(f->resolution),TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&f->signatures,f->resolution,100,NULL) == TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(f->signatures)));
    CHECK(tf_check(&f->typed,f->resolution,f->signatures,100,NULL) == TF_OK);
    phase(tf_typed_program_phase(f->typed),TF_DIAG_PHASE_VALID);
}
static void build(Fixture *f, size_t cap) {
    type(f);
    CHECK(tf_machine_program_build(&f->program,f->typed,cap,NULL) == TF_OK);
    CHECK(tf_machine_program_typed(f->program) == f->typed);
}
static void destroy(Fixture f) {
    tf_machine_program_destroy(f.program); tf_typed_program_destroy(f.typed);
    tf_signatures_destroy(f.signatures); tf_resolution_destroy(f.resolution);
    tf_source_manager_destroy(f.manager);
}
static Fixture source(const char *s) {
    const char *path = "build/m07-machine-case.tfd";
    FILE *file = fopen(path,"wb");
    CHECK(file != NULL); CHECK(fwrite(s,1,strlen(s),file) == strlen(s)); CHECK(fclose(file) == 0);
    return load(path);
}
static const TfMachinePlan *machine(const Fixture *f, size_t i) {
    const TfMachinePlan *m = tf_machine_program_at(f->program,i);
    CHECK(m != NULL); CHECK(tf_machine_program_get(f->program,m->type) == m);
    CHECK(tf_machine_program_get(f->program,(TfTypeId){0}) == NULL);
    return m;
}
static void schedule(const TfMachinePlan *m, const char *const *names, size_t count) {
    CHECK(m->schedule_count == count);
    for (size_t i = 0; i < count; ++i) {
        CHECK(m->schedule[i] < m->cell_count);
        CHECK(named(m->cells[m->schedule[i]].name,names[i]));
    }
}
static void span_text(TfSpan span, const char *s) {
    CHECK(span.source != NULL && span.end - span.start == strlen(s));
    CHECK(memcmp(span.source->bytes + span.start,s,strlen(s)) == 0);
}
static void analysis(void) {
    const char *const order[] = {"a","B","b","z"};
    const char *const swap[] = {"left","right"};
    Fixture f = load("tests/fixtures/machines/analysis.tfd");
    const TfMachinePlan *m;
    size_t old = 0, input = 0, candidate = 0, duplicate_edge = 0;
    build(&f,100); phase(tf_machine_program_phase(f.program),TF_DIAG_PHASE_VALID);
    CHECK(tf_diagnostic_phase_payload(tf_machine_program_phase(f.program)) == f.program);
    CHECK(tf_machine_program_count(f.program) == 3);
    CHECK(tf_machine_program_at(f.program,3) == NULL);
    m = machine(&f,0); CHECK(named(m->name,"Ordered")); schedule(m,order,4);
    CHECK(m->cell_count == 4 && m->writer_count == 4 && m->guard_count == 2 && m->emission_count == 2);
    CHECK(m->dependency_count == 3);
    for (size_t i = 0; i < m->cell_count; ++i) {
        const TfMachineCell *cell = &m->cells[i];
        CHECK(cell->id == i && cell->writer_count == 1 && cell->symbol.index != 0);
        CHECK(m->writers[cell->writers[0]].cell == i);
        CHECK(tf_type_store_get(tf_typed_program_store(f.typed),cell->type)->kind == TF_CONCRETE_INT);
    }
    for (size_t i = 0; i < m->read_count; ++i) {
        const TfMachineRead *r = &m->reads[i];
        CHECK(tf_typed_program_node(f.typed,r->node) != NULL);
        if (i != 0) CHECK(m->reads[i-1].span.end <= r->span.start);
        if (r->kind == TF_REFERENCE_OLD) ++old;
        if (r->kind == TF_REFERENCE_INPUT) { ++input; CHECK(r->cell == SIZE_MAX); }
        if (r->kind == TF_REFERENCE_CANDIDATE) ++candidate;
        if (r->context == TF_MACHINE_REJECTION) { CHECK(r->clause_index == 0); span_text(r->span,"next.a"); }
    }
    CHECK(old == 4 && input == 2 && candidate == 7 && m->read_count == 13);
    for (size_t i = 0; i < m->dependency_count; ++i) {
        const TfMachineDependency *edge = &m->dependencies[i];
        CHECK(edge->occurrence_count >= 1);
        for (size_t j = 0; j < edge->occurrence_count; ++j) {
            const TfMachineRead *r = &m->reads[edge->occurrences[j]];
            CHECK(r->kind == TF_REFERENCE_CANDIDATE && r->context == TF_MACHINE_WRITER);
            CHECK(r->cell == edge->prerequisite);
            CHECK(m->writers[r->clause_index].cell == edge->dependent);
        }
        if (edge->occurrence_count == 2) { ++duplicate_edge; CHECK(named(m->cells[edge->prerequisite].name,"B")); }
    }
    CHECK(duplicate_edge == 1);
    CHECK(m->guards[0].clause.span.start < m->guards[1].clause.span.start);
    span_text(tf_ast_get(m->guards[0].rejection.owner,m->guards[0].rejection)->span,"Failure::Bad(next.a)");
    CHECK(m->emissions[0].span.start < m->emissions[1].span.start);
    m = machine(&f,1); schedule(m,swap,2); CHECK(m->dependency_count == 0);
    CHECK(m->read_count == 3 && m->reads[0].kind == TF_REFERENCE_OLD && m->reads[1].kind == TF_REFERENCE_OLD);
    m = machine(&f,2); CHECK(m->dependency_count == 1);
    CHECK(named(m->cells[m->schedule[0]].name,"right"));
    CHECK(m->read_count == 6 && m->reads[0].kind == TF_REFERENCE_INPUT);
    CHECK(m->reads[2].kind == TF_REFERENCE_OLD && m->reads[2].context == TF_MACHINE_WRITER);
    CHECK(m->reads[3].kind == TF_REFERENCE_INPUT && m->reads[3].context == TF_MACHINE_WRITER);
    destroy(f);
}
static void modules(void) {
    Fixture f = load("tests/fixtures/machines/modules/root.tfd");
    const TfMachinePlan *a, *b;
    build(&f,100); phase(tf_machine_program_phase(f.program),TF_DIAG_PHASE_VALID);
    CHECK(tf_machine_program_count(f.program) == 2);
    a = machine(&f,0); b = machine(&f,1);
    CHECK(named(a->name,"Same") && named(b->name,"Same"));
    CHECK(a->type.module_index != b->type.module_index && a->span.source != b->span.source);
    CHECK(a->cells[0].symbol.index != b->cells[0].symbol.index);
    for (size_t i = 0; i < 2; ++i) {
        const TfMachinePlan *m = machine(&f,i);
        CHECK(m->schedule_count == 1 && m->dependency_count == 0 && m->read_count == 3);
        CHECK(m->reads[0].symbol.index == m->cells[0].symbol.index);
        CHECK(m->reads[1].symbol.index == m->input.index);
        for (size_t j = 0; j < m->read_count; ++j) CHECK(m->reads[j].span.source == m->span.source);
    }
    destroy(f);
}

static void diagnostic(const Fixture *f, size_t i, const char *code) {
    const TfDiagnosticRecord *d = tf_diagnostic_bag_get(tf_machine_program_diagnostics(f->program),i);
    CHECK(d != NULL && strcmp(d->code,code) == 0 && d->label_count != 0 && d->labels[0].primary);
}
static void cycles(void) {
    Fixture f = source("machine Cycle(x: Int) yields Unit rejects Unit {"
        "state a: Int = 0; state b: Int = 0; state c: Int = 0; state d: Int = 0; state tail: Int = 0;"
        "next a = if false { next.b } else { 0 }; next b = next.a + next.a;"
        "next c = next.c; next d = next.d; next tail = next.a; }");
    build(&f,100); phase(tf_machine_program_phase(f.program),TF_DIAG_PHASE_ERROR);
    CHECK(tf_diagnostic_phase_payload(tf_machine_program_phase(f.program)) == NULL);
    CHECK(tf_diagnostic_bag_count(tf_machine_program_diagnostics(f.program)) == 3);
    CHECK(machine(&f,0)->schedule_count == 0 && machine(&f,0)->schedule == NULL);
    {
        size_t spans = 0, pairs = 0;
        for (size_t i = 0; i < 3; ++i) {
            const TfDiagnosticRecord *d = tf_diagnostic_bag_get(tf_machine_program_diagnostics(f.program),i);
            diagnostic(&f,i,"E404"); CHECK(d->help_count == 1);
            CHECK(strstr(d->help[0].data,"old snapshot") != NULL);
            if (d->label_count == 3) ++pairs;
            for (size_t j = 0; j < d->label_count; ++j) {
                TfSpan s = d->labels[j].span;
                CHECK(s.end-s.start == 6 && memcmp(s.source->bytes+s.start,"next.",5) == 0);
                CHECK(d->labels[j].primary == (j == 0)); ++spans;
            }
        }
        CHECK(spans == 5 && pairs == 1);
    }
    for (size_t cap = 0; cap < 3; ++cap) {
        TfMachineProgram *p = NULL;
        CHECK(tf_machine_program_build(&p,f.typed,cap,NULL) == TF_OK);
        phase(tf_machine_program_phase(p),TF_DIAG_PHASE_ERROR);
        CHECK(tf_diagnostic_bag_count(tf_machine_program_diagnostics(p)) == cap);
        CHECK(tf_diagnostic_bag_suppressed(tf_machine_program_diagnostics(p)) == 3-cap);
        tf_machine_program_destroy(p);
    }
    destroy(f);
}
static void coverage(void) {
    Fixture f = source("machine Missing(x: Int) yields Unit rejects Unit {"
        "state a: Int = 0; state b: Int = 0; next a = next.a; }"
        "machine Duplicate(x: Int) yields Unit rejects Unit {"
        "state a: Int = 0; next a = next.a; next a = a; next a = 0; }"
        "machine Independent(x: Int) yields Unit rejects Unit {state a: Int = 0; next a = next.a; }");
    build(&f,100); phase(tf_machine_program_phase(f.program),TF_DIAG_PHASE_ERROR);
    CHECK(tf_diagnostic_bag_count(tf_machine_program_diagnostics(f.program)) == 4);
    diagnostic(&f,0,"E401"); diagnostic(&f,1,"E402"); diagnostic(&f,2,"E402"); diagnostic(&f,3,"E404");
    span_text(tf_diagnostic_bag_get(tf_machine_program_diagnostics(f.program),0)->labels[0].span,"b");
    for (size_t i = 1; i <= 2; ++i) {
        const TfDiagnosticRecord *d = tf_diagnostic_bag_get(tf_machine_program_diagnostics(f.program),i);
        CHECK(d->label_count == 2); span_text(d->labels[0].span,"a"); span_text(d->labels[1].span,"a");
    }
    for (size_t i = 0; i < 2; ++i) {
        const TfMachinePlan *m = machine(&f,i);
        CHECK(m->schedule == NULL && m->schedule_count == 0 && m->dependency_count == 0);
    }
    CHECK(machine(&f,0)->cells[1].writer_count == 0);
    CHECK(machine(&f,1)->cells[0].writer_count == 3);
    destroy(f);
}
static void earlier_rejections(void) {
    const char *const bodies[] = {
        "machine M(x: Int) yields Unit rejects Unit {state a: Int = 0; next absent = 0;}",
        "machine M(x: Int) yields Unit rejects Unit {state a: Int = 0; next a = next.absent;}",
        "fn f() -> Int { next.a }",
        "machine M(x: Int) yields Unit rejects Unit {state a: Int = 0; next a = {let f = fn () -> Int {next.a}; f()};}"
    };
    const char *const codes[] = {"E403","E403","E405","E406"};
    const char *const spellings[] = {"absent","absent","next.a","next.a"};
    for (size_t i = 0; i < 4; ++i) {
        Fixture f = source(bodies[i]);
        const TfDiagnosticBag *bag = tf_resolution_diagnostics(f.resolution);
        phase(tf_resolution_phase(f.resolution),TF_DIAG_PHASE_ERROR);
        CHECK(tf_diagnostic_bag_count(bag) == 1);
        CHECK(strcmp(tf_diagnostic_bag_get(bag,0)->code,codes[i]) == 0);
        span_text(tf_diagnostic_bag_get(bag,0)->labels[0].span,spellings[i]);
        destroy(f);
    }
}
static void allocation_failures(const char *s) {
    Fixture f = source(s);
    Counter c = {0};
    TfAllocator a = {&c,count_allocate,count_free};
    TfMachineProgram *p = NULL;
    size_t calls;
    type(&f);
    CHECK(tf_machine_program_build(&p,f.typed,100,&a) == TF_OK);
    calls = c.calls; CHECK(calls != 0); tf_machine_program_destroy(p); CHECK(c.live == 0);
    for (size_t i = 1; i <= calls; ++i) {
        c = (Counter){.fail_at=i};
        CHECK(tf_machine_program_build(&p,f.typed,100,&a) == TF_HOST_ERROR);
        CHECK(p == NULL && c.live == 0);
        phase(tf_typed_program_phase(f.typed),TF_DIAG_PHASE_VALID);
    }
    CHECK(tf_machine_program_build(&p,f.typed,100,NULL) == TF_OK);
    tf_machine_program_destroy(p); destroy(f);
}
static void permutations(void) {
    static const char *const writers[] = {"next z = next.b + next.B;","next b = 9 div x;","next B = next.a;","next a = 7 div x;"};
    static const char *const expected[] = {"a","B","b","z"};
    size_t count = 0;
    for (size_t a = 0; a < 4; ++a) for (size_t b = 0; b < 4; ++b)
    for (size_t c = 0; c < 4; ++c) for (size_t d = 0; d < 4; ++d) {
        char buffer[1024];
        int length;
        Fixture f;
        if (a == b || a == c || a == d || b == c || b == d || c == d) continue;
        length = snprintf(buffer,sizeof buffer,"machine M(x: Int) yields Unit rejects Unit {"
            "state z: Int = 0; state b: Int = 0; state B: Int = 0; state a: Int = 0; %s %s %s %s }",
            writers[a],writers[b],writers[c],writers[d]);
        CHECK(length > 0 && (size_t)length < sizeof buffer);
        f = source(buffer); build(&f,100); phase(tf_machine_program_phase(f.program),TF_DIAG_PHASE_VALID);
        {
            const TfMachinePlan *m = machine(&f,0);
            const TfNodeId first = m->writers[m->cells[m->schedule[0]].writers[0]].clause.expression;
            schedule(m,expected,4);
            /* The first potential divide fault is from a, whatever writer order. */
            span_text(tf_ast_get(first.owner,first)->span,"7 div x");
        }
        destroy(f); ++count;
    }
    CHECK(count == 24);
}
static void boundaries(void) {
    Fixture f = source("");
    TfMachineProgram *p = NULL;
    build(&f,0); phase(tf_machine_program_phase(f.program),TF_DIAG_PHASE_VALID);
    CHECK(tf_machine_program_count(f.program) == 0);
    CHECK(tf_machine_program_build(&p,f.typed,101,NULL) == TF_HOST_ERROR && p == NULL);
    CHECK(tf_machine_program_build(NULL,f.typed,100,NULL) == TF_HOST_ERROR);
    CHECK(tf_machine_program_build(&p,NULL,100,NULL) == TF_HOST_ERROR && p == NULL);
    { TfAllocator invalid = {0}; CHECK(tf_machine_program_build(&p,f.typed,100,&invalid) == TF_HOST_ERROR); }
    CHECK(tf_machine_program_typed(NULL) == NULL && tf_machine_program_phase(NULL) == NULL);
    CHECK(tf_machine_program_diagnostics(NULL) == NULL && tf_machine_program_at(NULL,0) == NULL);
    CHECK(tf_machine_program_count(NULL) == 0 && tf_machine_program_get(NULL,(TfTypeId){0}) == NULL);
    tf_machine_program_destroy(NULL); destroy(f);
    f = source("fn bad() -> Int {true}");
    phase(tf_resolution_phase(f.resolution),TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&f.signatures,f.resolution,100,NULL) == TF_OK);
    CHECK(tf_check(&f.typed,f.resolution,f.signatures,100,NULL) == TF_OK);
    phase(tf_typed_program_phase(f.typed),TF_DIAG_PHASE_ERROR);
    CHECK(tf_machine_program_build(&p,f.typed,100,NULL) == TF_HOST_ERROR && p == NULL);
    destroy(f);
}
static void long_graph(bool cyclic) {
    const size_t cells = 160;
    const char *path = "build/m07-machine-case.tfd";
    FILE *file = fopen(path,"wb");
    Fixture f;
    const TfMachinePlan *m;
    CHECK(file != NULL);
    CHECK(fputs("machine M(x: Int) yields Unit rejects Unit {",file) >= 0);
    for (size_t i = 0; i < cells; ++i) CHECK(fprintf(file,"state c%03zu: Int = 0;",i) > 0);
    for (size_t i = 0; i < cells; ++i) {
        if (i + 1 == cells && !cyclic) CHECK(fprintf(file,"next c%03zu = 0;",i) > 0);
        else CHECK(fprintf(file,"next c%03zu = next.c%03zu;",i,(i+1) % cells) > 0);
    }
    CHECK(fputs("}",file) >= 0 && fclose(file) == 0);
    f = load(path); build(&f,100); m = machine(&f,0);
    if (cyclic) {
        phase(tf_machine_program_phase(f.program),TF_DIAG_PHASE_ERROR); diagnostic(&f,0,"E404");
        CHECK(tf_diagnostic_bag_get(tf_machine_program_diagnostics(f.program),0)->label_count == cells);
        CHECK(m->schedule_count == 0);
    } else {
        phase(tf_machine_program_phase(f.program),TF_DIAG_PHASE_VALID);
        CHECK(m->schedule_count == cells);
        for (size_t i = 0; i < cells; ++i) CHECK(m->schedule[i] == cells-1-i);
    }
    destroy(f);
}
int main(void) {
    analysis(); modules(); cycles(); coverage(); earlier_rejections(); permutations(); boundaries();
    long_graph(false); long_graph(true);
    allocation_failures("machine M(x: Int) yields Int rejects Unit {state a: Int = 0; state b: Int = 0;"
        "next a = a + x; next b = next.a + next.a; guard next.b > 0 else (); emit next.a;}");
    allocation_failures("machine M(x: Int) yields Unit rejects Unit {state a: Int = 0; next a = next.a;}");
    allocation_failures("machine M(x: Int) yields Unit rejects Unit {state a: Int = 0; state b: Int = 0; next a = a; next a = a;}");
    CHECK(remove("build/m07-machine-case.tfd") == 0);
    printf("machine plan: %zu checks passed\n",checks); return 0;
}
