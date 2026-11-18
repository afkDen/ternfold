#include "ternfold/machine_plan.h"
#include "../../src/repl_resolve.h"
#include "../../src/typed_signatures.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

enum { CELLS = 7, FIXED_CASES = 8, CASES = 48, SOURCE_CAP = 4096 };

/* Index is ASCII rank, deliberately unrelated to declaration, writer or DAG
 * construction order. Includes prefixes, case differences and underscore. */
static const char *const names[CELLS] = {"A", "A0", "B", "_x", "a", "a0", "z"};

typedef struct {
    size_t count;
    unsigned occurrences[CELLS][CELLS]; /* prerequisite -> dependent, 0..2 */
    bool dead[CELLS][CELLS];
} Graph;

typedef struct {
    uint32_t seed;
    size_t number, layout, length;
    char source[SOURCE_CAP];
    size_t declarations[CELLS], writers[CELLS];
    size_t offsets[CELLS][CELLS][2]; /* byte starts of rendered projections */
} Run;

static void fail(const Run *run, int line, const char *condition) {
    fprintf(stderr, "plan_properties:%d: %s\nReplay: test-plan-properties %" PRIu32
            " %zu (layout %zu)\n%s", line, condition, run->seed, run->number,
            run->layout, run->source);
    exit(EXIT_FAILURE);
}

#define CHECK(run, condition) do { if (!(condition)) fail(run, __LINE__, #condition); } while (0)

static uint32_t draw(uint32_t *state) {
    *state = (uint32_t)((uint64_t)*state * UINT32_C(1664525) + UINT32_C(1013904223));
    return *state >> 16;
}

static void shuffle(size_t *order, size_t count, uint32_t *state) {
    for (size_t i = 0; i < count; ++i) order[i] = i;
    for (size_t i = count; i > 1; --i) {
        size_t j = draw(state) % i, saved = order[i - 1];
        order[i - 1] = order[j];
        order[j] = saved;
    }
}

static Graph generate(size_t number, uint32_t *state) {
    Graph g = {.count = CELLS};
    switch (number) {
        case 0: g.count = 1; return g; /* an old-state self read is allowed */
        case 1: g.count = 1; g.occurrences[0][0] = 1; return g;
        case 2: g.count = 1; g.occurrences[0][0] = 2; g.dead[0][0] = true; return g;
        case 3: /* B and then A become ready while z is already waiting. */
            g.occurrences[4][2] = 1; g.dead[4][2] = true;
            g.occurrences[2][0] = 2;
            return g;
        case 4: return g; /* widest ready set; old reads alone form a ring */
        case 5: /* Maximum density for a DAG; reverse the natural ASCII order. */
            for (size_t a = 0; a < g.count; ++a)
                for (size_t b = 0; b < a; ++b) g.occurrences[a][b] = 2;
            return g;
        case 6: /* Maximum ring length; the only back edge is dead. */
            for (size_t a = 1; a < g.count; ++a) g.occurrences[a - 1][a] = 1;
            g.occurrences[g.count - 1][0] = 1; g.dead[g.count - 1][0] = true;
            return g;
        case 7: /* Independent cycles plus an incoming and outgoing edge. */
            g.occurrences[0][1] = g.occurrences[1][0] = g.occurrences[3][3] = 1;
            g.occurrences[2][0] = g.occurrences[1][4] = 1;
            return g;
        default: break;
    }

    size_t order[CELLS];
    g.count = 2 + draw(state) % (CELLS - 1);
    shuffle(order, g.count, state);
    for (size_t i = 0; i < g.count; ++i) {
        for (size_t j = i + 1; j < g.count; ++j) {
            if (draw(state) % 3u == 0) continue;
            g.occurrences[order[i]][order[j]] = 1u + draw(state) % 2u;
            g.dead[order[i]][order[j]] = draw(state) % 2u != 0;
        }
    }
    if (number % 4 == 2) {
        size_t a = order[draw(state) % g.count];
        g.occurrences[a][a] = 1;
        g.dead[a][a] = true;
    } else if (number % 4 == 3) {
        size_t start = draw(state) % (g.count - 1);
        size_t end = start + 1 + draw(state) % (g.count - start - 1);
        for (size_t i = start; i < end; ++i)
            if (g.occurrences[order[i]][order[i + 1]] == 0)
                g.occurrences[order[i]][order[i + 1]] = 1;
        g.occurrences[order[end]][order[start]] = 1;
        g.dead[order[end]][order[start]] = true;
    }
    return g;
}

static void append(Run *run, const char *format, ...) {
    size_t remaining = sizeof run->source - run->length;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(run->source + run->length, remaining, format, args);
    va_end(args);
    CHECK(run, written >= 0 && (size_t)written < remaining);
    run->length += (size_t)written;
}

static void render(Run *run, const Graph *g) {
    append(run, "machine Graph(inputValue: Int) yields Unit rejects Unit {\n");
    for (size_t i = 0; i < g->count; ++i)
        append(run, "  state %s: Int = 0;\n", names[run->declarations[i]]);
    for (size_t i = 0; i < g->count; ++i) {
        size_t b = run->writers[i];
        append(run, "  next %s = %s", names[b], names[(b + 1) % g->count]);
        for (size_t a = 0; a < g->count; ++a) {
            for (unsigned j = 0; j < g->occurrences[a][b]; ++j) {
                append(run, "%s", g->dead[a][b] ? " + (if false { " : " + ");
                run->offsets[a][b][j] = run->length;
                append(run, "next.%s", names[a]);
                if (g->dead[a][b]) append(run, " } else { 0 })");
            }
        }
        append(run, ";\n");
    }
    append(run, "}\n");
}

/* Exhaustively enumerate complete permutations in ASCII order, testing the
 * edge inequalities only at each leaf. At most 7! leaves. No ready queue,
 * indegrees, plan data or production name comparator enter the oracle. */
static bool first_order(const Graph *g, size_t *order, size_t depth, unsigned used) {
    if (depth == g->count) {
        size_t position[CELLS];
        for (size_t i = 0; i < g->count; ++i) position[order[i]] = i;
        for (size_t a = 0; a < g->count; ++a)
            for (size_t b = 0; b < g->count; ++b)
                if (g->occurrences[a][b] && position[a] >= position[b]) return false;
        return true;
    }
    for (size_t a = 0; a < g->count; ++a) {
        unsigned bit = 1u << a;
        if (used & bit) continue;
        order[depth] = a;
        if (first_order(g, order, depth + 1, used | bit)) return true;
    }
    return false;
}

static bool projection(const Run *run, const TfSource *source, TfSpan span,
                       size_t a, size_t b, unsigned j) {
    size_t start = run->offsets[a][b][j];
    return span.source == source && span.start == start && span.end == start + 5 + strlen(names[a]);
}

static void check_edges(const Run *run, const Graph *g, const TfSource *source,
                        const TfMachinePlan *plan) {
    bool seen[CELLS][CELLS] = {{false}};
    bool read_seen[CELLS * CELLS * 2 + CELLS] = {false};
    size_t edges = 0, reads = g->count; /* one bare old-state read per writer */
    for (size_t a = 0; a < g->count; ++a)
        for (size_t b = 0; b < g->count; ++b) {
            if (g->occurrences[a][b]) ++edges;
            reads += g->occurrences[a][b];
        }
    CHECK(run, plan->dependency_count == edges && plan->read_count == reads);
    CHECK(run, (!edges || plan->dependencies) && plan->reads);
    for (size_t i = 0; i < edges; ++i) {
        const TfMachineDependency *edge = &plan->dependencies[i];
        CHECK(run, edge->prerequisite < g->count && edge->dependent < g->count);
        size_t a = run->declarations[edge->prerequisite], b = run->declarations[edge->dependent];
        CHECK(run, !seen[a][b] && g->occurrences[a][b] != 0);
        seen[a][b] = true;
        CHECK(run, edge->occurrence_count == g->occurrences[a][b] && edge->occurrences);
        unsigned matched = 0;
        for (size_t j = 0; j < edge->occurrence_count; ++j) {
            size_t index = edge->occurrences[j];
            CHECK(run, index < reads && !read_seen[index]);
            read_seen[index] = true;
            const TfMachineRead *read = &plan->reads[index];
            CHECK(run, read->kind == TF_REFERENCE_CANDIDATE && read->context == TF_MACHINE_WRITER);
            CHECK(run, read->cell == edge->prerequisite && read->clause_index < g->count);
            CHECK(run, run->writers[read->clause_index] == b);
            for (unsigned k = 0; k < g->occurrences[a][b]; ++k)
                if (projection(run, source, read->span, a, b, k)) matched |= 1u << k;
        }
        CHECK(run, matched == (1u << g->occurrences[a][b]) - 1u);
    }
    for (size_t i = 0; i < reads; ++i)
        if (!read_seen[i]) CHECK(run, plan->reads[i].kind == TF_REFERENCE_OLD);
}

static void check_cycles(const Run *run, const Graph *g, const TfSource *source,
                         const TfDiagnosticBag *bag) {
    bool reach[CELLS][CELLS], reported[CELLS] = {false};
    size_t representative[CELLS], components = 0;
    for (size_t a = 0; a < g->count; ++a)
        for (size_t b = 0; b < g->count; ++b) reach[a][b] = g->occurrences[a][b] != 0;
    for (size_t k = 0; k < g->count; ++k)
        for (size_t a = 0; a < g->count; ++a)
            for (size_t b = 0; b < g->count; ++b)
                reach[a][b] = reach[a][b] || (reach[a][k] && reach[k][b]);
    for (size_t a = 0; a < g->count; ++a) {
        representative[a] = a;
        for (size_t b = 0; b < a; ++b)
            if (reach[a][b] && reach[b][a]) { representative[a] = b; break; }
        if (representative[a] == a && reach[a][a]) ++components;
    }
    CHECK(run, components != 0 && tf_diagnostic_bag_count(bag) == components);
    for (size_t i = 0; i < components; ++i) {
        const TfDiagnosticRecord *record = tf_diagnostic_bag_get(bag, i);
        CHECK(run, record && strcmp(record->code, "E404") == 0 && record->severity == TF_DIAG_ERROR);
        CHECK(run, record->label_count != 0 && record->labels[0].primary);
        size_t witness = CELLS;
        for (size_t a = 0; a < g->count; ++a)
            for (size_t b = 0; b < g->count; ++b)
                if (reach[b][a])
                    for (unsigned j = 0; j < g->occurrences[a][b]; ++j)
                        if (projection(run, source, record->labels[0].span, a, b, j))
                            witness = representative[a];
        CHECK(run, witness < g->count && !reported[witness]);
        reported[witness] = true;
    }
}

static void phase(const Run *run, const TfDiagnosticPhaseResult *result,
                  TfDiagnosticPhaseKind expected) {
    if (tf_diagnostic_phase_kind(result) != expected)
        (void)tf_diagnostic_bag_write(stderr, tf_diagnostic_phase_bag(result), TF_DIAG_TEXT, TF_DIAG_COLOR_NEVER);
    CHECK(run, tf_diagnostic_phase_kind(result) == expected);
}

static void check_plan(const Run *run, const Graph *g, const size_t *expected, bool acyclic) {
    TfSource source;
    TfSourceManager *manager = NULL;
    TfResolution *resolution = NULL;
    TfTypedSignatures *signatures = NULL;
    TfTypedProgram *typed = NULL;
    TfMachineProgram *program = NULL;
    CHECK(run, tf_source_init(&source, "<plan-property>", run->source, run->length, NULL) == TF_OK);
    CHECK(run, tf_source_manager_create(&manager, NULL) == TF_OK);
    CHECK(run, tf_resolve_virtual(&resolution, manager, &source, NULL, NULL, NULL) == TF_OK);
    phase(run, tf_resolution_phase(resolution), TF_DIAG_PHASE_VALID);
    CHECK(run, tf_signatures_build(&signatures, resolution, 100, NULL) == TF_OK);
    CHECK(run, !tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(signatures)));
    CHECK(run, tf_check(&typed, resolution, signatures, 100, NULL) == TF_OK);
    phase(run, tf_typed_program_phase(typed), TF_DIAG_PHASE_VALID);
    CHECK(run, tf_machine_program_build(&program, typed, 100, NULL) == TF_OK);
    const TfDiagnosticPhaseResult *result = tf_machine_program_phase(program);
    phase(run, result, acyclic ? TF_DIAG_PHASE_VALID : TF_DIAG_PHASE_ERROR);
    CHECK(run, tf_diagnostic_phase_payload(result) == (acyclic ? program : NULL));
    const TfDiagnosticBag *bag = tf_machine_program_diagnostics(program);
    CHECK(run, tf_diagnostic_bag_suppressed(bag) == 0 && tf_machine_program_count(program) == 1);
    const TfMachinePlan *plan = tf_machine_program_at(program, 0);
    CHECK(run, plan && plan->cell_count == g->count && plan->writer_count == g->count);
    CHECK(run, plan->cells && plan->writers);
    for (size_t i = 0; i < g->count; ++i) {
        TfAstText name = plan->cells[i].name;
        const char *declared = names[run->declarations[i]];
        CHECK(run, name.data && name.length == strlen(declared) && memcmp(name.data, declared, name.length) == 0);
        CHECK(run, plan->writers[i].cell < g->count && run->declarations[plan->writers[i].cell] == run->writers[i]);
    }
    check_edges(run, g, &source, plan);
    if (acyclic) {
        CHECK(run, tf_diagnostic_bag_count(bag) == 0 && plan->schedule_count == g->count && plan->schedule);
        for (size_t i = 0; i < g->count; ++i) {
            CHECK(run, plan->schedule[i] < g->count);
            CHECK(run, run->declarations[plan->schedule[i]] == expected[i]);
        }
    } else {
        CHECK(run, plan->schedule == NULL && plan->schedule_count == 0);
        check_cycles(run, g, &source, bag);
    }
    tf_machine_program_destroy(program);
    tf_typed_program_destroy(typed);
    tf_signatures_destroy(signatures);
    tf_resolution_destroy(resolution);
    tf_source_manager_destroy(manager);
    tf_source_destroy(&source);
}

static void run_case(uint32_t seed, size_t number) {
    /* Independent per-case streams: replay needs neither preceding cases nor
     * global random state. Layout 1 reverses both independent shuffles. */
    uint32_t state = seed ^ (uint32_t)((uint64_t)(number + 1) * UINT32_C(2654435761));
    Graph g = generate(number, &state);
    size_t expected[CELLS], declarations[CELLS], writers[CELLS];
    bool acyclic = first_order(&g, expected, 0, 0);
    shuffle(declarations, g.count, &state);
    shuffle(writers, g.count, &state);
    for (size_t layout = 0; layout < 2; ++layout) {
        Run run = {.seed = seed, .number = number, .layout = layout};
        for (size_t i = 0; i < g.count; ++i) {
            size_t j = layout ? g.count - 1 - i : i;
            run.declarations[i] = declarations[j];
            run.writers[i] = writers[j];
        }
        render(&run, &g);
        bool designed_dag = number == 0 || (number >= 3 && number <= 5) ||
                            (number >= FIXED_CASES && number % 4 < 2);
        CHECK(&run, acyclic == designed_dag);
        check_plan(&run, &g, expected, acyclic);
    }
}

static bool decimal(const char *text, uintmax_t max, uintmax_t *value) {
    char *end;
    if (*text < '0' || *text > '9') return false;
    errno = 0;
    *value = strtoumax(text, &end, 10);
    return !errno && *end == '\0' && *value <= max;
}

int main(int argc, char **argv) {
    const uint32_t defaults[] = {UINT32_C(0x5445524e), UINT32_C(0xc0ffee17)};
    uintmax_t seed = 0, number = 0;
    size_t seed_count = argc == 1 ? sizeof defaults / sizeof *defaults : 1;
    if (argc > 3 || (argc > 1 && !decimal(argv[1], UINT32_MAX, &seed)) ||
        (argc == 3 && !decimal(argv[2], CASES - 1, &number))) {
        fprintf(stderr, "usage: %s [decimal-uint32-seed [case 0..%d]]\n", argv[0], CASES - 1);
        return 2;
    }
    size_t start = (size_t)number, end = argc == 3 ? start + 1 : CASES;
    for (size_t i = 0; i < seed_count; ++i) {
        uint32_t selected = argc == 1 ? defaults[i] : (uint32_t)seed;
        for (size_t j = start; j < end; ++j) run_case(selected, j);
        printf("plan_properties: seed=%" PRIu32 " cases=%zu..%zu passed (%zu layouts)\n",
               selected, start, end - 1, (end - start) * 2);
    }
    return EXIT_SUCCESS;
}
