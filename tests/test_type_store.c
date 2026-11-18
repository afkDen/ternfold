#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "ternfold/types.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

static size_t checks;
#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "test_type_store:%d: %s\n", __LINE__, #expr); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

enum { PATH_CAP = 4096 };

static void fixture_directory(char path[PATH_CAP]) {
#ifdef _WIN32
    char name[128];
    int len = snprintf(name, sizeof name, "build/tf-types-%lu-%llu",
                       (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    DWORD actual;
    CHECK(len > 0 && (size_t)len < sizeof name);
    actual = GetFullPathNameA(name, PATH_CAP, path, NULL);
    CHECK(actual > 0 && actual < PATH_CAP);
    CHECK(CreateDirectoryA(path, NULL) != 0);
#else
    static const char suffix[] = "/build/tf-types-XXXXXX";
    CHECK(getcwd(path, PATH_CAP) != NULL);
    CHECK(strlen(path) < PATH_CAP - sizeof suffix);
    strcat(path, suffix);
    CHECK(mkdtemp(path) != NULL);
#endif
}

static FILE *open_fixture(char path[PATH_CAP], const char *directory, const char *name) {
    int len = snprintf(path, PATH_CAP, "%s/%s", directory, name);
    FILE *f;
    CHECK(len > 0 && len < PATH_CAP);
    f = fopen(path, "wbx");
    CHECK(f != NULL);
    return f;
}

static void write_fixture(char path[PATH_CAP], const char *dir, const char *name, const char *text) {
    FILE *f = open_fixture(path, dir, name);
    size_t length = strlen(text);
    CHECK(fwrite(text, 1, length, f) == length);
    CHECK(fclose(f) == 0);
}

static TfResolution *resolve(TfSourceManager *manager, const char *path, bool valid) {
    TfResolution *r = NULL;
    TfStatus status = tf_resolve(&r, manager, path, NULL, NULL);
    if (status != TF_OK) fprintf(stderr, "tf_resolve(%s): status %d\n", path, (int)status);
    CHECK(status == TF_OK && r != NULL);
    if (valid && tf_diagnostic_phase_kind(tf_resolution_phase(r)) != TF_DIAG_PHASE_VALID)
        (void)tf_diagnostic_bag_write(stderr, tf_resolution_diagnostics(r), TF_DIAG_TEXT, TF_DIAG_COLOR_NEVER);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(r)) ==
          (valid ? TF_DIAG_PHASE_VALID : TF_DIAG_PHASE_ERROR));
    return r;
}

static bool spelling(TfAstText text, const char *name) {
    return text.length == strlen(name) && memcmp(text.data, name, text.length) == 0;
}

static const TfAstNode *root_function(const TfResolution *r, const char *name) {
    const TfResolvedModule *m = tf_resolution_module(r, tf_resolution_root(r));
    CHECK(m != NULL && m->ast != NULL);
    for (size_t i = 0; i < tf_ast_count(m->ast); ++i) {
        const TfAstNode *n = tf_ast_at(m->ast, i);
        if (n->kind == TF_AST_FUNCTION && spelling(n->as.function.name, name)) return n;
    }
    CHECK(false);
    return NULL;
}

static TfNodeId annotation(const TfResolution *r, const char *name) {
    return root_function(r, name)->as.function.result_type;
}

static const TfResolvedType *nominal(const TfResolution *r, TfModuleId module, const char *name) {
    for (size_t i = 0; i < tf_resolution_type_count(r); ++i) {
        const TfResolvedType *t = tf_resolution_type_at(r, i);
        if (t->id.module_index == module.index && spelling(t->name, name)) return t;
    }
    CHECK(false);
    return NULL;
}

static bool absent(TfConcreteTypeId id) { return id.owner == NULL && id.index == 0; }

static TfConcreteTypeId from_syntax(TfTypeStore *s, TfNodeId id) {
    TfConcreteTypeId out = {0};
    TfTypeIssue issue = {.code = "stale"};
    CHECK(tf_type_from_syntax(s, id, &out, &issue) == TF_OK);
    CHECK(issue.code == NULL && issue.primary.source == NULL && issue.related.source == NULL);
    CHECK(tf_type_store_get(s, out) != NULL);
    return out;
}

static void expect_span(TfSpan span, const char *bytes) {
    CHECK(span.source != NULL && span.start <= span.end && span.end <= span.source->length);
    CHECK(span.end - span.start == strlen(bytes));
    CHECK(memcmp(span.source->bytes + span.start, bytes, span.end - span.start) == 0);
}

static void issue_for(TfTypeStore *s, TfNodeId node, const char *code,
                      const char *primary, const char *related) {
    TfTypeIssue issue;
    TfConcreteTypeId out = {s, 1};
    CHECK(tf_type_from_syntax(s, node, &out, &issue) == TF_SOURCE_ERROR);
    CHECK(absent(out) && issue.code != NULL && issue.message != NULL);
    CHECK(strcmp(issue.code, code) == 0 && tf_diagnostic_code_registered(issue.code));
    expect_span(issue.primary, primary);
    if (related == NULL) CHECK(issue.related.source == NULL);
    else expect_span(issue.related, related);
}

static void ordinary(const TfResolution *r, TfTypeStore *s) {
    static const TfConcreteTypeKind kinds[] = {
        TF_CONCRETE_INT, TF_CONCRETE_FLOAT, TF_CONCRETE_BOOL,
        TF_CONCRETE_TEXT, TF_CONCRETE_CHAR, TF_CONCRETE_UNIT
    };
    TfConcreteTypeId primitives[sizeof kinds / sizeof *kinds];
    TfConcreteTypeId id, list, option, local, imported, another, variant, machine, fn, zero;
    TfConcreteTypeId args[4], reversed[4], different;
    const TfConcreteType *t;
    const TfResolvedType *decl;
    const TfAstNode *signature = tf_ast_get(annotation(r, "all_primitives").owner,
                                            annotation(r, "all_primitives"));
    CHECK(signature != NULL && signature->kind == TF_AST_TYPE_FUNCTION);
    CHECK(signature->as.type_function.parameters.count == sizeof kinds / sizeof *kinds);
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; ++i) {
        CHECK(tf_type_primitive(s, kinds[i], &primitives[i]) == TF_OK);
        CHECK(tf_type_store_get(s, primitives[i])->kind == kinds[i]);
        id = from_syntax(s, signature->as.type_function.parameters.items[i]);
        CHECK(tf_concrete_type_id_equal(id, primitives[i]));
        for (size_t j = 0; j < i; ++j) CHECK(!tf_concrete_type_id_equal(id, primitives[j]));
    }
    CHECK(tf_type_list(s, primitives[0], &list) == TF_OK);
    CHECK(tf_type_option(s, list, &option) == TF_OK);
    t = tf_type_store_get(s, option);
    CHECK(t != NULL && t->kind == TF_CONCRETE_OPTION && tf_concrete_type_id_equal(t->as.element, list));
    CHECK(tf_type_store_get(s, list)->kind == TF_CONCRETE_LIST);
    CHECK(tf_concrete_type_id_equal(tf_type_store_get(s, list)->as.element, primitives[0]));
    CHECK(tf_concrete_type_id_equal(option, from_syntax(s, annotation(r, "nested"))));

    local = from_syntax(s, annotation(r, "local_twin"));
    imported = from_syntax(s, annotation(r, "import_twin"));
    another = from_syntax(s, annotation(r, "alias_twin"));
    CHECK(!tf_concrete_type_id_equal(local, imported) && tf_concrete_type_id_equal(imported, another));
    CHECK(tf_type_store_get(s, local)->kind == TF_CONCRETE_RECORD);
    CHECK(tf_type_store_get(s, imported)->kind == TF_CONCRETE_RECORD);
    decl = nominal(r, tf_resolution_root(r), "Twin");
    CHECK(tf_type_nominal(s, decl->id, &id) == TF_OK && tf_concrete_type_id_equal(id, local));
    CHECK(tf_type_store_get(s, imported)->as.nominal.module_index != decl->id.module_index);
    CHECK(tf_type_store_get(s, local)->as.nominal.owner == r);
    CHECK(tf_type_store_get(s, local)->as.nominal.declaration_index == decl->id.declaration_index);
    variant = from_syntax(s, annotation(r, "fault"));
    machine = from_syntax(s, annotation(r, "snapshot"));
    CHECK(tf_type_store_get(s, variant)->kind == TF_CONCRETE_ENUM);
    CHECK(tf_type_store_get(s, machine)->kind == TF_CONCRETE_MACHINE);

    id = from_syntax(s, annotation(r, "one_decision"));
    t = tf_type_store_get(s, id);
    CHECK(t != NULL && t->kind == TF_CONCRETE_DECISION);
    CHECK(tf_concrete_type_id_equal(t->as.decision.machine, machine));
    CHECK(tf_concrete_type_id_equal(t->as.decision.output, list));
    CHECK(tf_concrete_type_id_equal(t->as.decision.rejection, variant));
    CHECK(tf_type_decision(s, machine, list, variant, &different, NULL) == TF_OK);
    CHECK(tf_concrete_type_id_equal(id, different));
    CHECK(!tf_concrete_type_id_equal(id, from_syntax(s, annotation(r, "outer_decision"))));
    different = from_syntax(s, annotation(r, "imported_decision"));
    t = tf_type_store_get(s, different);
    CHECK(t != NULL && t->kind == TF_CONCRETE_DECISION && !tf_concrete_type_id_equal(id, different));
    CHECK(tf_type_store_get(s, t->as.decision.machine)->as.nominal.module_index !=
          tf_resolution_root(r).index);
    CHECK(tf_type_store_get(s, t->as.decision.output)->kind == TF_CONCRETE_CHAR);
    CHECK(tf_type_store_get(s, t->as.decision.rejection)->as.nominal.module_index !=
          tf_resolution_root(r).index);

    CHECK(tf_type_function(s, NULL, 0, primitives[3], &zero) == TF_OK);
    t = tf_type_store_get(s, zero);
    CHECK(t->kind == TF_CONCRETE_FUNCTION && t->as.function.arity == 0 && t->as.function.parameters == NULL);
    CHECK(tf_concrete_type_id_equal(zero, from_syntax(s, annotation(r, "zero_arity"))));
    args[0] = local; args[1] = option; args[2] = zero; args[3] = id;
    CHECK(tf_type_function(s, args, 4, primitives[4], &fn) == TF_OK);
    t = tf_type_store_get(s, fn);
    CHECK(t->kind == TF_CONCRETE_FUNCTION && t->as.function.arity == 4);
    CHECK(tf_concrete_type_id_equal(t->as.function.result, primitives[4]));
    for (size_t i = 0; i < 4; ++i) {
        CHECK(tf_concrete_type_id_equal(t->as.function.parameters[i], args[i]));
        reversed[i] = args[3 - i];
    }
    CHECK(tf_type_function(s, reversed, 4, primitives[4], &different) == TF_OK);
    CHECK(!tf_concrete_type_id_equal(fn, different));
    CHECK(tf_type_function(s, args, 4, primitives[3], &different) == TF_OK);
    CHECK(!tf_concrete_type_id_equal(fn, different));
    CHECK(tf_type_function(s, args, 3, primitives[4], &different) == TF_OK);
    CHECK(!tf_concrete_type_id_equal(fn, different));
    args[0] = (TfConcreteTypeId){0};
    CHECK(tf_concrete_type_id_equal(t->as.function.parameters[0], local));
    CHECK(tf_concrete_type_id_equal(fn, from_syntax(s, annotation(r, "callback"))));
}

static void invalid(const TfResolution *r, TfTypeStore *s, const TfResolution *other) {
    TfTypeStore *foreign = NULL;
    TfConcreteTypeId out, foreign_id, unit, fake = {s, SIZE_MAX};
    TfConcreteTypeId machine = from_syntax(s, annotation(r, "snapshot"));
    TfTypeIssue issue;
    const TfResolvedType *elsewhere = nominal(other, tf_resolution_root(other), "Twin");
    const TfResolvedType *builtin = nominal(r, (TfModuleId){r, 0}, "Float");
    CHECK(tf_type_primitive(s, TF_CONCRETE_UNIT, &unit) == TF_OK);
    CHECK(tf_type_store_create(&foreign, r, NULL) == TF_OK);
    CHECK(tf_type_primitive(foreign, TF_CONCRETE_UNIT, &foreign_id) == TF_OK);
    CHECK(!tf_concrete_type_id_equal(unit, foreign_id));
    CHECK(!tf_concrete_type_id_equal((TfConcreteTypeId){0}, (TfConcreteTypeId){0}));
    CHECK(tf_type_store_get(s, foreign_id) == NULL);
    CHECK(tf_type_store_get(s, fake) == NULL);
    CHECK(tf_type_store_get(NULL, unit) == NULL && tf_type_store_count(NULL) == 0);
    CHECK(tf_type_store_at(s, tf_type_store_count(s)) == NULL);
    CHECK(tf_type_store_at(s, unit.index - 1) == tf_type_store_get(s, unit));
    CHECK(tf_type_primitive(s, TF_CONCRETE_OPTION, &out) == TF_HOST_ERROR && absent(out));
    CHECK(tf_type_nominal(s, elsewhere->id, &out) == TF_HOST_ERROR && absent(out));
    CHECK(tf_type_nominal(s, builtin->id, &out) == TF_HOST_ERROR && absent(out));
    CHECK(tf_type_nominal(s, (TfTypeId){r, SIZE_MAX, SIZE_MAX}, &out) == TF_HOST_ERROR && absent(out));
    CHECK(tf_type_list(s, foreign_id, &out) == TF_HOST_ERROR && absent(out));
    CHECK(tf_type_option(s, (TfConcreteTypeId){0}, &out) == TF_HOST_ERROR && absent(out));
    CHECK(tf_type_function(s, NULL, 1, unit, &out) == TF_HOST_ERROR && absent(out));
    CHECK(tf_type_function(s, &unit, SIZE_MAX, unit, &out) == TF_HOST_ERROR && absent(out));
    CHECK(tf_type_function(s, &foreign_id, 1, unit, &out) == TF_HOST_ERROR && absent(out));
    CHECK(tf_type_function(s, NULL, 0, foreign_id, &out) == TF_HOST_ERROR && absent(out));
    CHECK(tf_type_decision(s, machine, foreign_id, unit, &out, &issue) == TF_HOST_ERROR &&
          absent(out) && issue.code == NULL);
    CHECK(tf_type_decision(s, unit, unit, unit, &out, &issue) == TF_SOURCE_ERROR &&
          absent(out) && issue.code != NULL && strcmp(issue.code, "E307") == 0);
    CHECK(tf_type_from_syntax(s, annotation(other, "callback"), &out, &issue) == TF_HOST_ERROR &&
          absent(out) && issue.code == NULL);
    CHECK(tf_type_from_syntax(s, (TfNodeId){annotation(r, "callback").owner, SIZE_MAX}, &out, &issue)
          == TF_HOST_ERROR && absent(out) && issue.code == NULL);
    CHECK(tf_type_from_syntax(s, root_function(r, "callback")->id, &out, &issue) == TF_HOST_ERROR &&
          absent(out) && issue.code == NULL);
    issue_for(s, annotation(r, "bad_list"), "E303", "List", NULL);
    issue_for(s, annotation(r, "bad_primitive"), "E303", "Int[Bool]", NULL);
    issue_for(s, annotation(r, "bad_nominal"), "E303", "Twin[Char]", "Twin");
    issue_for(s, annotation(r, "bad_arity"), "E303", "Decision[M, Unit]", NULL);
    issue_for(s, annotation(r, "bad_machine"), "E307", "Twin", NULL);
    issue_for(s, annotation(r, "bad_output"), "E307", "List[Float]", "List[Int]");
    issue_for(s, annotation(r, "bad_rejection"), "E307", "Unit", "Fault");
    tf_type_store_destroy(foreign);
}

static void deep_and_wide(TfTypeStore *s) {
    enum { DEPTH = 12000, ARITY = 4096 };
    TfConcreteTypeId unit, current, repeated, wide, *params;
    const TfConcreteType *pinned;
    CHECK(tf_type_primitive(s, TF_CONCRETE_UNIT, &unit) == TF_OK);
    pinned = tf_type_store_get(s, unit);
    current = unit;
    for (size_t i = 0; i < DEPTH; ++i) {
        TfConcreteTypeId parent;
        TfStatus status = i % 2 == 0 ? tf_type_list(s, current, &parent) : tf_type_option(s, current, &parent);
        CHECK(status == TF_OK);
        current = parent;
    }
    repeated = unit;
    for (size_t i = 0; i < DEPTH; ++i) {
        TfConcreteTypeId parent;
        TfStatus status = i % 2 == 0 ? tf_type_list(s, repeated, &parent) : tf_type_option(s, repeated, &parent);
        CHECK(status == TF_OK);
        repeated = parent;
    }
    CHECK(tf_concrete_type_id_equal(current, repeated));
    CHECK(tf_type_store_get(s, unit) == pinned);
    for (size_t i = DEPTH; i != 0; --i) {
        const TfConcreteType *t = tf_type_store_get(s, current);
        CHECK(t != NULL && t->kind == (i % 2 == 0 ? TF_CONCRETE_OPTION : TF_CONCRETE_LIST));
        current = t->as.element;
    }
    CHECK(tf_concrete_type_id_equal(current, unit));

    params = malloc(ARITY * sizeof *params);
    CHECK(params != NULL);
    for (size_t i = 0; i < ARITY; ++i) params[i] = i % 2 == 0 ? unit : repeated;
    CHECK(tf_type_function(s, params, ARITY, repeated, &wide) == TF_OK);
    free(params);
    pinned = tf_type_store_get(s, wide);
    CHECK(pinned != NULL && pinned->as.function.arity == ARITY);
    for (size_t i = 0; i < ARITY; ++i)
        CHECK(tf_concrete_type_id_equal(pinned->as.function.parameters[i], i % 2 == 0 ? unit : repeated));
}

typedef struct { size_t calls, fail_at, live, failures; } Faults;
static void *fault_alloc(void *context, size_t size) {
    Faults *f = context;
    void *p;
    if (++f->calls == f->fail_at) { ++f->failures; return NULL; }
    p = malloc(size);
    CHECK(p != NULL);
    ++f->live;
    return p;
}

static void fault_free(void *context, void *ptr) {
    Faults *f = context;
    CHECK(ptr != NULL && f->live != 0);
    --f->live;
    free(ptr);
}

static size_t allocation_failures(const TfResolution *r, TfNodeId node) {
    size_t attempts = 0, failed = 0;
    for (size_t index = 0; index <= attempts; ++index) {
        Faults faults = {0};
        TfAllocator a = {&faults, fault_alloc, fault_free};
        TfTypeStore *s = NULL;
        TfConcreteTypeId anchor, out, again;
        const TfConcreteType *pinned;
        TfTypeIssue issue;
        TfStatus status;
        CHECK(tf_type_store_create(&s, r, &a) == TF_OK);
        CHECK(tf_type_primitive(s, TF_CONCRETE_BOOL, &anchor) == TF_OK);
        pinned = tf_type_store_get(s, anchor);
        if (index != 0) faults.fail_at = faults.calls + index;
        {
            size_t baseline = faults.calls;
            status = tf_type_from_syntax(s, node, &out, &issue);
            if (index == 0) attempts = faults.calls - baseline;
        }
        if (index != 0) {
            CHECK(status == TF_HOST_ERROR && absent(out) && issue.code == NULL);
            CHECK(faults.failures == 1);
            ++failed;
            faults.fail_at = 0;
            out = from_syntax(s, node); /* verifies cycle marks/temporaries did not survive */
        } else {
            CHECK(status == TF_OK && attempts > 12);
        }
        again = from_syntax(s, node);
        CHECK(tf_concrete_type_id_equal(out, again) && tf_type_store_get(s, anchor) == pinned);
        tf_type_store_destroy(s);
        CHECK(faults.live == 0);
    }
    /* Allocation of the owner and the first record, including both tables. */
    for (size_t index = 1; index <= 4; ++index) {
        Faults faults = {.fail_at = index};
        TfAllocator a = {&faults, fault_alloc, fault_free};
        TfTypeStore *s = NULL;
        TfConcreteTypeId out;
        if (index == 1) CHECK(tf_type_store_create(&s, r, &a) == TF_HOST_ERROR && s == NULL);
        else {
            CHECK(tf_type_store_create(&s, r, &a) == TF_OK);
            CHECK(tf_type_primitive(s, TF_CONCRETE_INT, &out) == TF_HOST_ERROR && absent(out));
            CHECK(tf_type_store_count(s) == 0 && tf_type_store_at(s, 0) == NULL);
            CHECK(tf_type_primitive(s, TF_CONCRETE_INT, &out) == TF_OK && out.index == 1);
        }
        tf_type_store_destroy(s);
        CHECK(faults.live == 0 && faults.failures == 1);
        ++failed;
    }
    return failed;
}

static void decision_cycle(TfSourceManager *manager, const char *directory) {
    enum { MACHINES = 192 };
    char path[PATH_CAP];
    TfResolution *r;
    TfTypeStore *s = NULL;
    TfTypeIssue issue;
    TfConcreteTypeId out;
    FILE *f = open_fixture(path, directory, "cycle.tfd");
    for (unsigned i = 0; i < MACHINES; ++i)
        CHECK(fprintf(f, "machine C%u(i: Unit) yields Decision[C%u, Unit, Unit] rejects Unit "
                         "{ state cell: Unit = (); next cell = (); }\n", i, (i + 1) % MACHINES) > 0);
    CHECK(fputs("fn probe() -> Decision[C0, Unit, Unit] { () }\n", f) >= 0);
    CHECK(fclose(f) == 0);
    r = resolve(manager, path, true);
    CHECK(tf_type_store_create(&s, r, NULL) == TF_OK);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(tf_type_from_syntax(s, annotation(r, "probe"), &out, &issue) == TF_SOURCE_ERROR);
        CHECK(absent(out) && issue.code != NULL && strcmp(issue.code, "E307") == 0);
        expect_span(issue.primary, "Decision[C0, Unit, Unit]");
        CHECK(issue.related.source == issue.primary.source && issue.related.start == 0);
    }
    CHECK(tf_type_store_count(s) == MACHINES + 1); /* only machines and Unit; no Decision */
    tf_type_store_destroy(s);
    tf_resolution_destroy(r);
    CHECK(remove(path) == 0);
}

int main(void) {
    char directory[PATH_CAP], path[PATH_CAP], library[PATH_CAP], bad[PATH_CAP];
    TfSourceManager *manager = NULL;
    TfResolution *r, *other, *invalid_resolution;
    TfTypeStore *s = NULL;
    size_t injected;
    fixture_directory(directory);
    write_fixture(library, directory, "lib.tfd",
        "pub record Twin { n: Int } pub enum Fault { Foreign }\n"
        "pub machine M(i: Char) yields Char rejects Fault { state n: Char = 'a'; next n = i; }\n");
    write_fixture(bad, directory, "bad.tfd", "fn nope() -> Missing { () }\n");
    write_fixture(path, directory, "root.tfd",
        "import \"lib.tfd\" as lib; import \"lib.tfd\" as alias;\n"
        "record Twin { n: Int } enum Fault { Bad }\n"
        "machine M(i: Int) yields List[Int] rejects Fault { state n: Int = 0; next n = i; }\n"
        "machine Outer(i: fn(Int) -> Unit) yields Option[Decision[M, List[Int], Fault]] rejects Unit "
        "{ state n: Unit = (); next n = n; }\n"
        "fn all_primitives() -> fn(Int, Float, Bool, Text, Char, Unit) -> Unit { () }\n"
        "fn nested() -> Option[List[Int]] { () }\n"
        "fn local_twin() -> Twin { () } fn import_twin() -> lib::Twin { () }\n"
        "fn alias_twin() -> alias::Twin { () } fn fault() -> Fault { () } fn snapshot() -> M { () }\n"
        "fn zero_arity() -> fn() -> Text { () }\n"
        "fn one_decision() -> Decision[M, List[Int], Fault] { () }\n"
        "fn outer_decision() -> Decision[Outer, Option[Decision[M, List[Int], Fault]], Unit] { () }\n"
        "fn imported_decision() -> Decision[lib::M, Char, alias::Fault] { () }\n"
        "fn callback() -> fn(Twin, Option[List[Int]], fn() -> Text, Decision[M, List[Int], Fault]) -> Char { () }\n"
        "fn bad_list() -> List { () } fn bad_primitive() -> Int[Bool] { () }\n"
        "fn bad_nominal() -> Twin[Char] { () } fn bad_arity() -> Decision[M, Unit] { () }\n"
        "fn bad_machine() -> Decision[Twin, Unit, Unit] { () }\n"
        "fn bad_output() -> Decision[M, List[Float], Fault] { () }\n"
        "fn bad_rejection() -> Decision[M, List[Int], Unit] { () }\n"
        "fn memory() -> fn(Int, Float, Bool, Text, Char, Twin, Fault, M, Unit, Option[Char], "
        "Decision[Outer, Option[Decision[M, List[Int], Fault]], Unit]) -> "
        "List[Option[List[Option[List[Option[List[Option[List[Option[Unit]]]]]]]]]] { () }\n");
    CHECK(tf_source_manager_create(&manager, NULL) == TF_OK);
    r = resolve(manager, path, true);
    other = resolve(manager, path, true);
    invalid_resolution = resolve(manager, bad, false);
    CHECK(tf_type_store_create(&s, invalid_resolution, NULL) == TF_HOST_ERROR && s == NULL);
    CHECK(tf_type_store_create(&s, NULL, NULL) == TF_HOST_ERROR && s == NULL);
    CHECK(tf_type_store_create(&s, r, &(TfAllocator){0}) == TF_HOST_ERROR && s == NULL);
    CHECK(tf_type_store_create(&s, r, NULL) == TF_OK && s != NULL);
    ordinary(r, s);
    invalid(r, s, other);
    deep_and_wide(s);
    tf_type_store_destroy(s);
    injected = allocation_failures(r, annotation(r, "memory"));
    decision_cycle(manager, directory);
    tf_resolution_destroy(invalid_resolution);
    tf_resolution_destroy(other);
    tf_resolution_destroy(r);
    tf_source_manager_destroy(manager);
    CHECK(remove(bad) == 0 && remove(library) == 0 && remove(path) == 0);
#ifdef _WIN32
    CHECK(RemoveDirectoryA(directory) != 0);
#else
    CHECK(rmdir(directory) == 0);
#endif
    printf("type store: %zu checks, %zu injected allocation failures\n", checks, injected);
    return EXIT_SUCCESS;
}
