#include "../src/check_internal.h"

#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr,"test_check.c:%d: %s\n",__LINE__,#condition); exit(1); \
} } while (0)

typedef struct {
    TfSourceManager *manager;
    TfResolution *resolution;
    TfTypedSignatures *signatures;
    TfTypedProgram *program;
} Fixture;

static const char *const root_path = "build/m06-check-case.tfd";
static const char *const import_path = "build/m06-check-dependency.tfd";

static void write_source(const char *path, const char *source) {
    FILE *f = fopen(path,"wb");
    size_t length = strlen(source);
    CHECK(f != NULL);
    CHECK(fwrite(source,1,length,f) == length);
    CHECK(fclose(f) == 0);
}

static Fixture resolve_source(const char *source) {
    Fixture f = {0};
    write_source(root_path,source);
    CHECK(tf_source_manager_create(&f.manager,NULL) == TF_OK);
    CHECK(tf_resolve(&f.resolution,f.manager,root_path,NULL,NULL) == TF_OK);
    return f;
}

static void require_resolved(const Fixture *f) {
    if (tf_diagnostic_phase_kind(tf_resolution_phase(f->resolution)) != TF_DIAG_PHASE_VALID)
        (void)tf_diagnostic_bag_write(stderr,tf_resolution_diagnostics(f->resolution),TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(f->resolution)) == TF_DIAG_PHASE_VALID);
}

static Fixture check_source(const char *source, size_t cap, const TfCheckAdapters *adapters) {
    Fixture f = resolve_source(source);
    require_resolved(&f);
    CHECK(tf_signatures_build(&f.signatures,f.resolution,100,NULL) == TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(f.signatures)));
    CHECK(tf_check_with_adapters(&f.program,f.resolution,f.signatures,cap,NULL,adapters) == TF_OK);
    CHECK(f.program != NULL);
    return f;
}

static void destroy(Fixture f) {
    tf_typed_program_destroy(f.program);
    tf_signatures_destroy(f.signatures);
    tf_resolution_destroy(f.resolution);
    tf_source_manager_destroy(f.manager);
}

static void require_phase(const Fixture *f, TfDiagnosticPhaseKind phase, unsigned pending) {
    const TfDiagnosticPhaseResult *result = tf_typed_program_phase(f->program);
    if (tf_diagnostic_phase_kind(result) != phase)
        (void)tf_diagnostic_bag_write(stderr,tf_typed_program_diagnostics(f->program),TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
    CHECK(tf_diagnostic_phase_kind(result) == phase);
    CHECK(tf_diagnostic_phase_payload(result) == (phase == TF_DIAG_PHASE_VALID ? f->program : NULL));
    CHECK(tf_typed_program_pending(f->program) == pending);
}

static const TfAst *root_ast(const Fixture *f) {
    return tf_resolution_module(f->resolution,tf_resolution_root(f->resolution))->ast;
}

static bool named(TfAstText name, const char *wanted) {
    return name.length == strlen(wanted) && memcmp(name.data,wanted,name.length) == 0;
}

static const TfResolvedSymbol *find_symbol(const Fixture *f, const char *name, TfSymbolKind kind) {
    for (size_t i = 0; i < tf_resolution_symbol_count(f->resolution); ++i) {
        const TfResolvedSymbol *s = tf_resolution_symbol_at(f->resolution,i);
        if (s->kind == kind && named(s->name,name)) return s;
    }
    CHECK(false);
    return NULL;
}

static const TfConcreteType *require_type(const Fixture *f, TfConcreteTypeId type, TfConcreteTypeKind kind) {
    const TfConcreteType *t = tf_type_store_get(tf_typed_program_store(f->program),type);
    CHECK(t != NULL && t->kind == kind);
    return t;
}

static const TfTypedNode *require_node(const Fixture *f, TfNodeId node, TfConcreteTypeKind kind) {
    const TfTypedNode *record = tf_typed_program_node(f->program,node);
    CHECK(record != NULL && record->node.index == node.index && record->node.owner == node.owner);
    (void)require_type(f,record->type,kind);
    return record;
}

static void diagnostics(const Fixture *f, const char *source, size_t index,
                        const char *code, const char *spelling, bool related) {
    const TfDiagnosticRecord *d = tf_diagnostic_bag_get(tf_typed_program_diagnostics(f->program),index);
    const char *at = strstr(source,spelling);
    if (d == NULL || strcmp(d->code,code) != 0 || (at != NULL && d->label_count != 0 &&
        d->labels[0].span.start != (size_t)(at - source)))
        (void)tf_diagnostic_bag_write(stderr,tf_typed_program_diagnostics(f->program),TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
    CHECK(d != NULL && strcmp(d->code,code) == 0);
    CHECK(at != NULL && d->label_count >= (related ? 2u : 1u) && d->labels[0].primary);
    CHECK(d->labels[0].span.start == (size_t)(at - source));
    CHECK(d->labels[0].span.end == d->labels[0].span.start + strlen(spelling));
    CHECK(d->labels[0].span.source == tf_ast_source(root_ast(f)));
    if (related) CHECK(!d->labels[1].primary && d->labels[1].span.source != NULL);
}

static void typed_functions(void) {
    const char *source =
        "record Point { x: Int, label: Text }\n"
        "record Empty {}\n"
        "enum Direction { Left, Move(Int) }\n"
        "fn apply(f: fn(Int) -> Int, n: Int) -> Int { f(n) }\n"
        "fn pure() -> List[List[Int]] {\n"
        "  let point = Point(label: \"here\", x: 2);\n"
        "  let _: Empty = Empty();\n"
        "  let _: Direction = Direction::Left;\n"
        "  let _: Direction = Direction::Move(point.x);\n"
        "  let empty: List[Int] = [];\n"
        "  let close: fn(Int) -> Int = fn(arg: Int) -> Int { arg + point.x };\n"
        "  let base = apply(close, 1);\n"
        "  for row in [base] { if row == 3 { [] } else { { [row] } } }\n"
        "}\n"
        "fn arithmetic() -> Float { let _: Int = -9223372036854775808; 3 / 2.0 ^ 2 }\n"
        "fn marker() -> Char { 'a' }\n"
        "fn before() -> Int { later(4) }\n"
        "fn later(n: Int) -> Int { if n == 0 { 0 } else { before() } }\n"
        "pub fn main() -> List[Text] { input visitor: Text from stdin; output visitor to stdout; [visitor] }\n";
    Fixture f = check_source(source,100,NULL);
    const TfAst *ast = root_ast(&f);
    const TfResolvedSymbol *arg = find_symbol(&f,"arg",TF_SYMBOL_PARAMETER);
    const TfResolvedSymbol *row = find_symbol(&f,"row",TF_SYMBOL_LOOP);
    const TfResolvedSymbol *visitor = find_symbol(&f,"visitor",TF_SYMBOL_ENTRY_INPUT);
    const TfResolvedSymbol *x = find_symbol(&f,"x",TF_SYMBOL_FIELD);
    const TfResolvedSymbol *label = find_symbol(&f,"label",TF_SYMBOL_FIELD);
    size_t fields = 0, projections = 0, lists = 0, numeric = 0;
    require_phase(&f,TF_DIAG_PHASE_VALID,0);
    CHECK(tf_typed_program_resolution(f.program) == f.resolution);
    (void)require_type(&f,tf_typed_program_symbol(f.program,arg->id),TF_CONCRETE_INT);
    (void)require_type(&f,tf_typed_program_symbol(f.program,row->id),TF_CONCRETE_INT);
    (void)require_type(&f,tf_typed_program_symbol(f.program,visitor->id),TF_CONCRETE_TEXT);
    for (size_t i = 0; i < tf_ast_count(ast); ++i) {
        const TfAstNode *n = tf_ast_at(ast,i);
        if (n->kind == TF_AST_ARGUMENT && n->as.argument.named) {
            bool first = named(n->as.argument.name,"x");
            const TfTypedNode *t = require_node(&f,n->id,first ? TF_CONCRETE_INT : TF_CONCRETE_TEXT);
            CHECK(t->target.owner == f.resolution && t->target.index == (first ? x->id.index : label->id.index));
            CHECK(t->member_index == (first ? 0u : 1u));
            ++fields;
        } else if (n->kind == TF_AST_PROJECT) {
            const TfTypedNode *t = require_node(&f,n->id,TF_CONCRETE_INT);
            CHECK(t->target.index == x->id.index && t->member_index == 0);
            ++projections;
        } else if (n->kind == TF_AST_LIST && n->as.list.elements.count == 0) {
            const TfTypedNode *t = require_node(&f,n->id,TF_CONCRETE_LIST);
            (void)require_type(&f,tf_type_store_get(tf_typed_program_store(f.program),t->type)->as.element,TF_CONCRETE_INT);
            ++lists;
        } else if (n->kind == TF_AST_LITERAL && n->as.literal.token == TF_FLOAT) {
            const TfLiteralValue *value = tf_typed_program_literal(f.program,n->id);
            CHECK(value != NULL && value->kind == TF_LITERAL_FLOAT && value->as.float_bits == UINT64_C(0x4000000000000000));
            ++numeric;
        } else if (n->kind == TF_AST_LITERAL && n->as.literal.token == TF_INT && n->as.literal.value.length == 19) {
            const TfLiteralValue *value = tf_typed_program_literal(f.program,n->id);
            CHECK(value != NULL && value->kind == TF_LITERAL_INT && value->minimum_magnitude);
            CHECK(value->as.int_magnitude == UINT64_C(9223372036854775808));
            ++numeric;
        }
    }
    CHECK(fields == 2 && projections == 2 && lists == 2 && numeric == 2);
    CHECK(tf_typed_program_node(f.program,(TfNodeId){NULL,1}) == NULL);
    CHECK(tf_typed_program_node(f.program,(TfNodeId){ast,SIZE_MAX}) == NULL);
    CHECK(tf_typed_program_symbol(f.program,(TfSymbolId){NULL,arg->id.index}).index == 0);
    destroy(f);
}

static void bad_expressions(void) {
    static const struct { const char *source, *code, *span; bool related; } cases[] = {
        {"fn f() -> Int { true }", "E302", "true", true},
        {"fn f() -> Float { 7 }", "E302", "7", true},
        {"fn f() -> List[Int] { let nope = []; [1] }", "E304", "[]", false},
        {"fn f() -> List[Int] { let nope = if true { [] } else { [1] }; [1] }", "E304", "[]", false},
        {"fn f() -> List[Int] { for v in 17 { v } }", "E302", "17", false},
        {"fn f() -> List[Int] { for v in [] { v } }", "E304", "[]", false},
        {"fn f() -> Int { [[]] }", "E302", "[[]]", true},
        {"fn f() -> Int { if 4 { 1 } else { 2 } }", "E302", "4", false},
        {"fn f() -> Int { let a = if true { 1 } else { false }; 0 }", "E302", "false", true},
        {"fn f() -> List[Int] { [1, 2.5] }", "E302", "2.5", true},
        {"fn f() -> Int { - 9223372036854775808 }", "E301", "9223372036854775808", false},
        {"fn f() -> Int { -(9223372036854775808) }", "E301", "9223372036854775808", false},
        {"fn f() -> Float { 1e999 }", "E301", "1e999", false},
        {"fn f() -> Int { 1 div 2.0 }", "E302", "2.0", false},
        {"fn f() -> Bool { let q = fn() -> Int { 1 }; [q] == [q] }", "E305", "==", false},
        {"fn f(a: Int) -> Int { a } fn g() -> Int { f(true) }", "E302", "true", true},
        {"fn f(a: Int) -> Int { a } fn g() -> Int { f() }", "E303", "f()", true},
        {"fn f(a: Int) -> Int { a } fn g() -> Int { f(k: 1) }", "E303", "k", true},
        {"enum Dir { Left } fn g() -> Dir { Dir::Left() }", "E303", "Dir::Left", true},
        {"enum Dir { Go(Int) } fn g() -> fn(Int) -> Dir { Dir::Go }", "E303", "Dir::Go", true},
        {"record R { x: Int } fn g() -> R { R(1) }", "E303", "1", true},
        {"record R { x: Int } fn g() -> R { R(y: 1) }", "E303", "y", true},
        {"record R { x: Int, y: Int } fn g() -> R { R(x: 1, x: 2) }", "E303", NULL, true},
        {"record R { x: Int } fn g() -> Int { R(x: 1).bad }", "E302", "bad", true},
        {"fn g() -> Int { let z: List[Int, Int] = []; 2 }", "E303", "List[Int, Int]", false},
        {"fn g() -> Int { let z = fn(p: List[Int, Int]) -> Int { 1 }; 2 }", "E303", "List[Int, Int]", false},
        {"pub fn main() -> List[Text] { input value: Char; [] }", "E302", "Char", false},
        {"pub fn main() -> List[Text] { output false; [] }", "E302", "false", false}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        Fixture f = check_source(cases[i].source,100,NULL);
        require_phase(&f,TF_DIAG_PHASE_ERROR,0);
        CHECK(tf_diagnostic_bag_count(tf_typed_program_diagnostics(f.program)) == 1);
        if (cases[i].span != NULL) diagnostics(&f,cases[i].source,0,cases[i].code,cases[i].span,cases[i].related);
        else {
            const TfDiagnosticRecord *d = tf_diagnostic_bag_get(tf_typed_program_diagnostics(f.program),0);
            CHECK(strcmp(d->code,cases[i].code) == 0 && d->label_count == 2);
            CHECK(d->labels[0].span.start == (size_t)(strstr(cases[i].source,"x: 2") - cases[i].source));
            CHECK(d->labels[0].span.end == d->labels[0].span.start + 1);
        }
        destroy(f);
    }
}

static void modules_and_machine(void) {
    static const char *const source =
        "import \"m06-check-dependency.tfd\" as lib;\n"
        "fn sample() -> Int { lib::Device().amount + lib::Pair(y: 4, x: 2).x }\n";
    Fixture f;
    const TfAst *ast;
    const TfResolvedSymbol *cell, *input;
    size_t next = 0, old = 0, inputs = 0, fields = 0;
    write_source(import_path,
        "pub record Pair { x: Int, y: Int }\n"
        "pub enum Problem { Low }\n"
        "pub machine Device(delta: Int) yields Int rejects Problem {\n"
        "  state base: Int = 1; state amount: Int = 0;\n"
        "  next amount = amount + next.base; next base = base + delta;\n"
        "  guard next.amount >= 0 else Problem::Low; emit next.amount;\n"
        "}\n");
    f = check_source(source,100,NULL);
    require_phase(&f,TF_DIAG_PHASE_INVALID,TF_CHECK_PENDING_CONSTANTS);
    CHECK(!tf_diagnostic_bag_has_errors(tf_typed_program_diagnostics(f.program)));
    CHECK(tf_resolution_module_count(f.resolution) == 2);
    cell = find_symbol(&f,"amount",TF_SYMBOL_CELL);
    input = find_symbol(&f,"delta",TF_SYMBOL_MACHINE_INPUT);
    ast = root_ast(&f);
    for (size_t i = 0; i < tf_ast_count(ast); ++i) {
        const TfAstNode *n = tf_ast_at(ast,i);
        if (n->kind == TF_AST_PROJECT) {
            const TfTypedNode *t = require_node(&f,n->id,TF_CONCRETE_INT);
            if (named(n->as.project.name,"amount")) CHECK(t->target.index == cell->id.index && t->member_index == 1);
            else CHECK(t->target.index == find_symbol(&f,"x",TF_SYMBOL_FIELD)->id.index && t->member_index == 0);
            ++fields;
        }
    }
    for (size_t i = 0; i < tf_resolution_reference_count(f.resolution); ++i) {
        const TfResolvedReference *r = tf_resolution_reference_at(f.resolution,i);
        const TfAstNode *n = tf_ast_get(r->node.owner,r->node);
        if ((r->kind == TF_REFERENCE_OLD || r->kind == TF_REFERENCE_INPUT) && n->as.path.segments.count == 0)
            continue; /* qualifier leaf, not a value expression */
        if (r->kind == TF_REFERENCE_CANDIDATE || r->kind == TF_REFERENCE_OLD || r->kind == TF_REFERENCE_INPUT) {
            const TfTypedNode *t = require_node(&f,r->node,TF_CONCRETE_INT);
            CHECK(t->target.owner == f.resolution && t->target.index == r->symbol.index);
            if (r->kind == TF_REFERENCE_CANDIDATE) ++next;
            else if (r->kind == TF_REFERENCE_OLD) ++old;
            else { CHECK(t->target.index == input->id.index); ++inputs; }
        }
    }
    CHECK(next == 3 && old == 2 && inputs == 1 && fields == 2);
    destroy(f);

    const char *bad = "machine M(i: Int) yields Text rejects Bool { state a: Int = true; "
                      "next a = 9.0; guard 3 else (); emit 'q'; }";
    f = check_source(bad,100,NULL);
    require_phase(&f,TF_DIAG_PHASE_ERROR,TF_CHECK_PENDING_CONSTANTS);
    CHECK(tf_diagnostic_bag_count(tf_typed_program_diagnostics(f.program)) == 5);
    diagnostics(&f,bad,0,"E302","true",true);
    diagnostics(&f,bad,1,"E302","9.0",true);
    diagnostics(&f,bad,2,"E302","3",false);
    diagnostics(&f,bad,3,"E302","()",true);
    diagnostics(&f,bad,4,"E302","'q'",true);
    destroy(f);
    f = check_source(bad,0,NULL);
    require_phase(&f,TF_DIAG_PHASE_ERROR,TF_CHECK_PENDING_CONSTANTS);
    CHECK(tf_diagnostic_bag_count(tf_typed_program_diagnostics(f.program)) == 0);
    CHECK(tf_diagnostic_bag_suppressed(tf_typed_program_diagnostics(f.program)) == 5);
    destroy(f);
}

static void pending_and_prerequisites(void) {
    Fixture f = check_source("fn f() -> Option[Int] { Option::None } "
                             "fn g() -> Int { len([]) } "
                             "fn h(v: Int) -> Int { match v { a => { a } } }",100,NULL);
    require_phase(&f,TF_DIAG_PHASE_INVALID,TF_CHECK_PENDING_BUILTINS | TF_CHECK_PENDING_PATTERNS);
    CHECK(!tf_diagnostic_bag_has_errors(tf_typed_program_diagnostics(f.program)));
    destroy(f);

    f = check_source("pub fn main() -> List[Text] { [] } fn use() -> List[Text] { main() }",100,NULL);
    require_phase(&f,TF_DIAG_PHASE_VALID,0);
    destroy(f);
    f = resolve_source("pub fn main() -> List[Text] { output \"hi\"; [] } fn use() -> List[Text] { main() }");
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(f.resolution)) == TF_DIAG_PHASE_ERROR);
    CHECK(strcmp(tf_diagnostic_bag_get(tf_resolution_diagnostics(f.resolution),0)->code,"E208") == 0);
    CHECK(tf_check_with_adapters(&f.program,f.resolution,NULL,100,NULL,NULL) == TF_HOST_ERROR && f.program == NULL);
    destroy(f);

    f = resolve_source("fn f() -> List[Int, Int] { [] }");
    require_resolved(&f);
    CHECK(tf_signatures_build(&f.signatures,f.resolution,100,NULL) == TF_OK);
    CHECK(tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(f.signatures)));
    CHECK(tf_check_with_adapters(&f.program,f.resolution,f.signatures,100,NULL,NULL) == TF_HOST_ERROR && f.program == NULL);
    destroy(f);

    f = check_source("",100,NULL);
    require_phase(&f,TF_DIAG_PHASE_VALID,0);
    Fixture other = resolve_source("");
    TfTypedProgram *foreign = NULL;
    require_resolved(&other);
    CHECK(tf_check_with_adapters(&foreign,other.resolution,f.signatures,100,NULL,NULL) == TF_HOST_ERROR);
    CHECK(foreign == NULL);
    destroy(other);
    destroy(f);
}

typedef struct { size_t calls, fail_at, live; } Counter;

static void *counted_alloc(void *context, size_t size) {
    Counter *c = context;
    void *p;
    if (++c->calls == c->fail_at) return NULL;
    p = malloc(size);
    if (p != NULL) ++c->live;
    return p;
}

static void counted_free(void *context, void *p) {
    Counter *c = context;
    CHECK(c->live != 0 && p != NULL);
    --c->live;
    free(p);
}

static size_t allocation_failures(void) {
    static const char *const source = "record Pair { count: Int, key: Char } "
        "fn f() -> Int { let x = Pair(key: 'x', count: 5); "
        "let q = fn(p: Int) -> List[Int] { for z in [p] { z + x.count } }; "
        "let bad: Char = q(3); 0 }";
    Fixture f = resolve_source(source);
    Counter counter = {0};
    const TfAllocator allocator = {&counter,counted_alloc,counted_free};
    size_t first, count;
    require_resolved(&f);
    CHECK(tf_signatures_build(&f.signatures,f.resolution,100,&allocator) == TF_OK);
    first = counter.calls;
    CHECK(tf_check_with_adapters(&f.program,f.resolution,f.signatures,100,&allocator,NULL) == TF_OK);
    require_phase(&f,TF_DIAG_PHASE_ERROR,0);
    count = counter.calls - first;
    tf_typed_program_destroy(f.program);
    f.program = NULL;
    tf_signatures_destroy(f.signatures);
    CHECK(count != 0 && counter.live == 0);
    for (size_t i = 1; i <= count; ++i) {
        counter = (Counter){0};
        CHECK(tf_signatures_build(&f.signatures,f.resolution,100,&allocator) == TF_OK);
        CHECK(counter.calls == first);
        counter.fail_at = first + i;
        CHECK(tf_check_with_adapters(&f.program,f.resolution,f.signatures,100,&allocator,NULL) == TF_HOST_ERROR);
        CHECK(f.program == NULL);
        tf_signatures_destroy(f.signatures);
        f.signatures = NULL;
        CHECK(counter.live == 0);
    }
    destroy(f);
    return count;
}

static void depth_budget(void) {
    char source[2048] = "fn f() -> Int { 0";
    size_t n = strlen(source);
    for (size_t i = 0; i < 128; ++i) { memcpy(source + n,"+1",2); n += 2; }
    memcpy(source + n," }",3);
    Fixture f = check_source(source,100,NULL);
    require_phase(&f,TF_DIAG_PHASE_VALID,0);
    destroy(f);
    for (size_t i = 128; i < 600; ++i) { memcpy(source + n,"+1",2); n += 2; }
    memcpy(source + n," }",3);
    f = resolve_source(source);
    require_resolved(&f);
    CHECK(tf_signatures_build(&f.signatures,f.resolution,100,NULL) == TF_OK);
    CHECK(tf_check_with_adapters(&f.program,f.resolution,f.signatures,100,NULL,NULL) == TF_HOST_ERROR);
    CHECK(f.program == NULL);
    destroy(f);
}

int main(void) {
    size_t injected;
    typed_functions();
    bad_expressions();
    modules_and_machine();
    pending_and_prerequisites();
    depth_budget();
    injected = allocation_failures();
    CHECK(remove(root_path) == 0);
    CHECK(remove(import_path) == 0);
    printf("expression checker: %zu checks passed, %zu injected allocation failures\n",checks,injected);
    return 0;
}
