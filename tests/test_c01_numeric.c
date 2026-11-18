#include "ternfold/machine_plan.h"
#include "../src/check_internal.h"

#include <stdlib.h>
#include <string.h>

#define FIXTURE(name) "tests/fixtures/types/c01-" name ".tfd"
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static const char *fixture_path;
static size_t checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "test_c01_numeric:%d (%s): %s\n", __LINE__, fixture_path, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

typedef struct {
    TfSourceManager *manager;
    TfResolution *resolution;
    TfTypedSignatures *signatures;
    TfTypedProgram *typed;
    const TfSource *source;
} Fixture;

typedef struct {
    const char *code, *message, *lexeme;
    size_t line, column, related_column;
    const char *related_lexeme;
} ExpectedDiagnostic;

static void assert_phase(const TfDiagnosticPhaseResult *phase, const TfDiagnosticBag *bag,
                         const void *payload, size_t errors) {
    CHECK(bag != NULL);
    if (tf_diagnostic_bag_count(bag) != errors)
        (void)tf_diagnostic_bag_write(stderr, bag, TF_DIAG_TEXT, TF_DIAG_COLOR_NEVER);
    CHECK(tf_diagnostic_bag_count(bag) == errors);
    CHECK(tf_diagnostic_bag_suppressed(bag) == 0);
    CHECK(tf_diagnostic_bag_has_errors(bag) == (errors != 0));
    CHECK(tf_diagnostic_phase_kind(phase) ==
          (errors == 0 ? TF_DIAG_PHASE_VALID : TF_DIAG_PHASE_ERROR));
    CHECK(tf_diagnostic_phase_payload(phase) == payload);
    CHECK(tf_diagnostic_phase_bag(phase) == bag);
}

static Fixture resolve(const char *path, size_t errors) {
    Fixture f = {0};
    const TfResolvedModule *module;
    fixture_path = path;
    CHECK(tf_source_manager_create(&f.manager, NULL) == TF_OK);
    CHECK(tf_resolve(&f.resolution, f.manager, path, NULL, NULL) == TF_OK);
    assert_phase(tf_resolution_phase(f.resolution), tf_resolution_diagnostics(f.resolution),
                 errors == 0 ? f.resolution : NULL, errors);
    CHECK(tf_resolution_module_count(f.resolution) == 1);
    module = tf_resolution_module(f.resolution, tf_resolution_root(f.resolution));
    CHECK(module != NULL && module->is_root && module->source != NULL && module->ast != NULL);
    f.source = module->source->source;
    CHECK(f.source != NULL && tf_ast_source(module->ast) == f.source);
    return f;
}

static void type_check(Fixture *f, size_t errors) {
    const TfDiagnosticBag *signatures;
    CHECK(tf_signatures_build(&f->signatures, f->resolution, 100, NULL) == TF_OK);
    signatures = tf_signatures_diagnostics(f->signatures);
    CHECK(signatures != NULL && !tf_diagnostic_bag_has_errors(signatures));
    CHECK(tf_diagnostic_bag_count(signatures) == 0 && tf_diagnostic_bag_suppressed(signatures) == 0);
    CHECK(tf_check(&f->typed, f->resolution, f->signatures, 100, NULL) == TF_OK);
    assert_phase(tf_typed_program_phase(f->typed), tf_typed_program_diagnostics(f->typed),
                 errors == 0 ? f->typed : NULL, errors);
    CHECK(tf_typed_program_pending(f->typed) == 0);
}

static void destroy(Fixture *f) {
    tf_typed_program_destroy(f->typed);
    tf_signatures_destroy(f->signatures);
    tf_resolution_destroy(f->resolution);
    tf_source_manager_destroy(f->manager);
}

static void assert_span(TfSpan span, const TfSource *source, size_t line, size_t column,
                        const char *lexeme) {
    size_t start, end;
    const size_t length = strlen(lexeme);
    CHECK(tf_source_byte(source, line, column, &start));
    CHECK(tf_source_byte(source, line, column + length, &end));
    CHECK(span.source == source && span.start == start && span.end == end);
    CHECK(end - start == length && memcmp(source->bytes + start, lexeme, length) == 0);
}

static void assert_text(TfDiagnosticText actual, const char *expected) {
    CHECK(actual.length == strlen(expected));
    if (actual.length != 0)
        CHECK(actual.data != NULL && memcmp(actual.data, expected, actual.length) == 0);
}

static void diagnostic(const TfDiagnosticBag *bag, size_t index, const TfSource *source,
                       ExpectedDiagnostic expected) {
    const TfDiagnosticRecord *record = tf_diagnostic_bag_get(bag, index);
    CHECK(record != NULL && strcmp(record->code, expected.code) == 0);
    CHECK(record->severity == TF_DIAG_ERROR);
    assert_text(record->message, expected.message);
    CHECK(record->note_count == 0 && record->help_count == 0 && record->edit_count == 0);
    CHECK(record->label_count == (expected.related_column == 0 ? 1u : 2u));
    CHECK(record->labels[0].primary);
    assert_text(record->labels[0].message, "");
    assert_span(record->labels[0].span, source, expected.line, expected.column, expected.lexeme);
    if (expected.related_column != 0) {
        CHECK(!record->labels[1].primary);
        assert_text(record->labels[1].message, "expected or declared here");
        assert_span(record->labels[1].span, source, expected.line, expected.related_column,
                    expected.related_lexeme);
    }
}

static void numeric_token(const TfSource *source, size_t line, const char *lexeme, bool malformed) {
    TfLexResult lex = {0};
    const size_t column = 27;
    const size_t plus_column = column + strlen(lexeme) + 1;
    size_t start, index = 0;
    CHECK(tf_lex(&lex, source, 100, NULL) == (malformed ? TF_LEX_ERROR : TF_OK));
    CHECK(lex.has_errors == malformed && lex.diagnostic_count == (malformed ? 1u : 0u));
    CHECK(lex.suppressed == 0);
    if (malformed) {
        CHECK(lex.diagnostics[0].code == TF_E102);
        assert_span(lex.diagnostics[0].span, source, line, column, lexeme);
    }
    CHECK(tf_source_byte(source, line, column, &start));
    while (index < lex.token_count && lex.tokens[index].span.start < start) ++index;
    CHECK(lex.token_count - index >= 3);
    CHECK(lex.tokens[index].kind == (malformed ? TF_INVALID : TF_FLOAT));
    assert_span(lex.tokens[index].span, source, line, column, lexeme);
    CHECK(lex.tokens[index + 1].kind == TF_PLUS && lex.tokens[index + 2].kind == TF_FLOAT);
    assert_span(lex.tokens[index + 1].span, source, line, plus_column, "+");
    assert_span(lex.tokens[index + 2].span, source, line, plus_column + 2, "2.0");
    tf_lex_destroy(&lex);
}

static void malformed_literal(void) {
    Fixture f = resolve(FIXTURE("malformed"), 1);
    numeric_token(f.source, 1, "1e999foo", true);
    diagnostic(tf_resolution_diagnostics(f.resolution), 0, f.source,
               (ExpectedDiagnostic){"E102", "malformed decimal numeric literal",
                                    "1e999foo", 1, 27, 0, NULL});
    destroy(&f);
}

#define MISMATCH "expression does not have the expected type"
#define OVERFLOW "numeric overflow in state initializer"
#define ZERO "division by zero in state initializer"

static void typed_diagnostics(void) {
    const ExpectedDiagnostic types[] = {
        {"E302", MISMATCH, "1", 1, 26, 18, "Float"},
        {"E302", MISMATCH, "1", 2, 27, 19, "Float"},
        {"E302", MISMATCH, "2.0", 3, 32, 28, "1"},
        {"E302", MISMATCH, "1.0", 4, 33, 28, "1"},
        {"E302", MISMATCH, "3.0", 5, 32, 0, NULL},
        {"E302", MISMATCH, "3.0", 6, 37, 0, NULL},
        {"E302", MISMATCH, "1.0", 7, 26, 0, NULL},
        {"E302", MISMATCH, "2.0", 8, 34, 0, NULL},
        {"E302", MISMATCH, "1.0", 9, 41, 0, NULL},
        {"E301", "literal is outside its finite type range", "1e999", 10, 27, 0, NULL},
    };
    const ExpectedDiagnostic faults[] = {
        {"E306", OVERFLOW, "div", 2, 51, 0, NULL},
        {"E306", OVERFLOW, "%", 3, 51, 0, NULL},
        {"E306", ZERO, "^", 4, 41, 0, NULL},
        {"E306", ZERO, "^", 5, 41, 0, NULL},
        {"E306", OVERFLOW, "*", 6, 42, 0, NULL},
    };
    Fixture f = resolve(FIXTURE("types"), 0);
    numeric_token(f.source, 10, "1e999", false);
    type_check(&f, COUNT(types));
    for (size_t i = 0; i < COUNT(types); ++i)
        diagnostic(tf_typed_program_diagnostics(f.typed), i, f.source, types[i]);
    destroy(&f);

    f = resolve(FIXTURE("faults"), 0);
    type_check(&f, COUNT(faults));
    for (size_t i = 0; i < COUNT(faults); ++i)
        diagnostic(tf_typed_program_diagnostics(f.typed), i, f.source, faults[i]);
    destroy(&f);
}

static void assert_type(const TfTypedProgram *program, TfNodeId node, TfConcreteTypeKind kind) {
    const TfTypedNode *typed = tf_typed_program_node(program, node);
    const TfConcreteType *type;
    CHECK(typed != NULL);
    type = tf_type_store_get(tf_typed_program_store(program), typed->type);
    CHECK(type != NULL && type->kind == kind);
}

static void valid_program(void) {
    const char *const names[] = {"powers", "ratios", "lazy", "value"};
    const TfMachineCellId schedule[] = {0, 1, 3, 2};
    const TfMachineCellId writer_cells[] = {2, 3, 1, 0};
    Fixture f = resolve(FIXTURE("valid"), 0);
    TfMachineProgram *program = NULL;
    const TfMachinePlan *plan;
    const TfAstNode *division;
    const TfMachineRead *dependency_read;

    /* Initializer comparisons make a wrong numerical result reach div by zero.
     * Their success is evidence from the closed constant checker alone. */
    type_check(&f, 0);
    CHECK(tf_machine_program_build(&program, f.typed, 100, NULL) == TF_OK);
    assert_phase(tf_machine_program_phase(program), tf_machine_program_diagnostics(program), program, 0);
    CHECK(tf_machine_program_count(program) == 1);
    plan = tf_machine_program_at(program, 0);
    CHECK(plan != NULL && plan->name.length == 7 && memcmp(plan->name.data, "Numeric", 7) == 0);
    CHECK(plan->cell_count == COUNT(names) && plan->schedule_count == COUNT(schedule));
    CHECK(plan->writer_count == COUNT(writer_cells) && plan->emission_count == 1);
    for (size_t i = 0; i < COUNT(names); ++i) {
        const TfMachineCell *cell = &plan->cells[i];
        CHECK(cell->id == i && cell->name.length == strlen(names[i]));
        CHECK(memcmp(cell->name.data, names[i], cell->name.length) == 0);
        CHECK(cell->writer_count == 1 && cell->writers[0] < plan->writer_count);
        CHECK(plan->writers[cell->writers[0]].cell == i);
        CHECK(plan->writers[i].cell == writer_cells[i] && plan->schedule[i] == schedule[i]);
        assert_type(f.typed, cell->declaration.expression,
                    i == 3 ? TF_CONCRETE_FLOAT : TF_CONCRETE_BOOL);
    }
    CHECK(plan->dependency_count == 1);
    CHECK(plan->dependencies[0].prerequisite == 3 && plan->dependencies[0].dependent == 2);
    CHECK(plan->dependencies[0].occurrence_count == 1);
    CHECK(plan->dependencies[0].occurrences[0] < plan->read_count);
    dependency_read = &plan->reads[plan->dependencies[0].occurrences[0]];
    CHECK(dependency_read->kind == TF_REFERENCE_CANDIDATE && dependency_read->cell == 3);
    CHECK(dependency_read->context == TF_MACHINE_WRITER && dependency_read->clause_index == 0);
    assert_span(dependency_read->span, f.source, 11, 26, "next.value");
    {
        const TfNodeId expression = plan->writers[1].clause.expression;
        division = tf_ast_get(expression.owner, expression);
        CHECK(division != NULL && division->kind == TF_AST_BINARY && division->as.binary.op == TF_SLASH);
        assert_span(division->as.binary.operator_span, f.source, 12, 30, "/");
        assert_type(f.typed, expression, TF_CONCRETE_FLOAT);
        assert_type(f.typed, division->as.binary.left, TF_CONCRETE_INT);
        assert_type(f.typed, division->as.binary.right, TF_CONCRETE_FLOAT);
    }
    assert_type(f.typed, plan->emissions[0].expression, TF_CONCRETE_FLOAT);
    tf_machine_program_destroy(program);
    destroy(&f);
}

int main(void) {
    valid_program();
    malformed_literal();
    typed_diagnostics();
    printf("C01 source numerical conformance: %zu checks passed\n", checks);
    return EXIT_SUCCESS;
}
