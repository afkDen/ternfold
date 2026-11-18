#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "ternfold/resolve.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

enum { PATH_CAP = 4096, FILE_CAP = 16 };
static const char *case_name;
static size_t checks, skips;

static void check(bool ok, const char *what, int line) {
    ++checks;
    if (!ok) {
        fprintf(stderr, "test_resolution:%d [%s]: %s\n", line, case_name, what);
        exit(EXIT_FAILURE);
    }
}
#define CHECK(e) check((e), #e, __LINE__)

typedef struct {
    char directory[PATH_CAP];
    char files[FILE_CAP][PATH_CAP];
    size_t count;
} Fixture;

static void join(char out[PATH_CAP], const char *directory, const char *name) {
    int n = snprintf(out, PATH_CAP, "%s/%s", directory, name);
    CHECK(n > 0 && n < PATH_CAP);
}

static void fixture_init(Fixture *f) {
    f->count = 0;
#ifdef _WIN32
    {
        static unsigned serial;
        char name[128];
        int n = snprintf(name, sizeof name, "build/tf-resolve-%lu-%llu-%u",
                         (unsigned long)GetCurrentProcessId(),
                         (unsigned long long)GetTickCount64(), ++serial);
        DWORD size;
        CHECK(n > 0 && (size_t)n < sizeof name);
        size = GetFullPathNameA(name, PATH_CAP, f->directory, NULL);
        CHECK(size > 0 && size < PATH_CAP);
        CHECK(CreateDirectoryA(f->directory, NULL) != 0);
    }
#else
    {
        char cwd[PATH_CAP];
        CHECK(getcwd(cwd, sizeof cwd) != NULL);
        join(f->directory, cwd, "build/tf-resolve-XXXXXX");
        CHECK(mkdtemp(f->directory) != NULL);
    }
#endif
}

static void mkdir_checked(const char *path) {
#ifdef _WIN32
    CHECK(CreateDirectoryA(path, NULL) != 0);
#else
    CHECK(mkdir(path, 0700) == 0);
#endif
}

static void rmdir_checked(const char *path) {
#ifdef _WIN32
    CHECK(RemoveDirectoryA(path) != 0);
#else
    CHECK(rmdir(path) == 0);
#endif
}

static char *fixture_path(Fixture *f, const char *name) {
    CHECK(f->count < FILE_CAP);
    join(f->files[f->count], f->directory, name);
    return f->files[f->count++];
}

static const char *put(Fixture *f, const char *name, const char *source) {
    const char *path = fixture_path(f, name);
    FILE *file = fopen(path, "wbx");
    size_t n = strlen(source);
    CHECK(file != NULL);
    CHECK(fwrite(source, 1, n, file) == n);
    CHECK(fclose(file) == 0);
    return path;
}

static void fixture_remove_files(Fixture *f) {
    while (f->count > 0) CHECK(remove(f->files[--f->count]) == 0);
}

static void fixture_destroy(Fixture *f) {
    fixture_remove_files(f);
    rmdir_checked(f->directory);
}

static TfSourceManager *manager_new(void) {
    TfSourceManager *manager = NULL;
    CHECK(tf_source_manager_create(&manager, NULL) == TF_OK && manager != NULL);
    return manager;
}

static TfResolution *resolve(TfSourceManager *manager, const char *path,
                             const TfResolveOptions *options) {
    TfResolution *result = NULL;
    TfStatus status = tf_resolve(&result, manager, path, options, NULL);
    if (status != TF_OK) fprintf(stderr, "tf_resolve(%s): status %d\n", path, (int)status);
    CHECK(status == TF_OK && result != NULL);
    return result;
}

static void phase(const TfResolution *r, bool valid, size_t count, size_t suppressed) {
    const TfDiagnosticBag *bag = tf_resolution_diagnostics(r);
    const TfDiagnosticPhaseResult *p = tf_resolution_phase(r);
    if (tf_diagnostic_bag_count(bag) != count || tf_diagnostic_bag_has_errors(bag) == valid)
        (void)tf_diagnostic_bag_write(stderr, bag, TF_DIAG_TEXT, TF_DIAG_COLOR_NEVER);
    CHECK(bag != NULL && tf_diagnostic_phase_bag(p) == bag);
    CHECK(tf_diagnostic_phase_kind(p) == (valid ? TF_DIAG_PHASE_VALID : TF_DIAG_PHASE_ERROR));
    CHECK(tf_diagnostic_phase_payload(p) == (valid ? r : NULL));
    CHECK(tf_diagnostic_bag_has_errors(bag) == !valid);
    CHECK(tf_diagnostic_bag_count(bag) == count);
    CHECK(tf_diagnostic_bag_suppressed(bag) == suppressed);
}

static const TfResolvedModule *root_module(const TfResolution *r) {
    const TfResolvedModule *m = tf_resolution_module(r, tf_resolution_root(r));
    CHECK(m != NULL && m->is_root && m->ast != NULL);
    return m;
}

static const TfResolvedModule *module_named(const TfResolution *r, const char *name) {
    for (size_t i = 0; i < tf_resolution_module_count(r); ++i) {
        const TfResolvedModule *m = tf_resolution_module_at(r, i);
        if (strcmp(m->relative_path, name) == 0) return m;
    }
    CHECK(false);
    return NULL;
}

/* Locate a known spelling inside an independently supplied unique source
 * fragment. This checks exact byte boundaries even with repeated names. */
static TfSpan span_of(const TfSource *src, const char *fragment, size_t skip, size_t width) {
    const unsigned char *first = NULL;
    size_t n = strlen(fragment);
    CHECK(skip <= n && width <= n - skip && n <= src->length);
    for (size_t i = 0; i <= src->length - n; ++i) {
        if (memcmp(src->bytes + i, fragment, n) == 0) {
            CHECK(first == NULL);
            first = src->bytes + i;
        }
    }
    CHECK(first != NULL);
    {
        size_t at = (size_t)(first - src->bytes) + skip;
        return (TfSpan){src, at, at + width};
    }
}

static bool same_span(TfSpan a, TfSpan b) {
    return a.source == b.source && a.start == b.start && a.end == b.end;
}

static const TfAstNode *node_at(const TfResolvedModule *m, TfAstKind kind, TfSpan span) {
    const TfAstNode *found = NULL;
    for (size_t i = 0; i < tf_ast_count(m->ast); ++i) {
        const TfAstNode *n = tf_ast_at(m->ast, i);
        if (n->kind == kind && same_span(n->span, span) &&
            (kind != TF_AST_PATH || n->as.path.segments.count != 0)) {
            CHECK(found == NULL);
            found = n;
        }
    }
    if (found == NULL)
        fprintf(stderr, "AST missing %s at '%.*s'\n", tf_ast_kind_name(kind),
                (int)(span.end - span.start), span.source->bytes + span.start);
    CHECK(found != NULL);
    return found;
}

static const TfAstNode *node_in(const TfResolvedModule *m, TfAstKind kind,
                                const char *fragment, size_t skip, size_t width) {
    return node_at(m, kind, span_of(m->source->source, fragment, skip, width));
}

static const TfResolvedReference *reference(const TfResolution *r, TfNodeId node,
                                            TfReferenceKind kind) {
    const TfResolvedReference *ref = tf_resolution_reference(r, node);
    CHECK(ref != NULL && ref->kind == kind);
    CHECK(ref->node.owner == node.owner && ref->node.index == node.index);
    return ref;
}

static const TfResolvedSymbol *target(const TfResolution *r, TfNodeId node,
                                      TfReferenceKind kind, TfSymbolKind symbol_kind) {
    const TfResolvedSymbol *s = tf_resolution_symbol(r, reference(r, node, kind)->symbol);
    CHECK(s != NULL && s->kind == symbol_kind);
    return s;
}

static bool text_is(TfAstText text, const char *expected) {
    return text.length == strlen(expected) && memcmp(text.data, expected, text.length) == 0;
}

static const TfResolvedSymbol *named_symbol(const TfResolution *r, const TfResolvedModule *m,
                                            const char *name, TfSymbolKind kind) {
    const TfResolvedSymbol *found = NULL;
    for (size_t i = 0; i < tf_resolution_symbol_count(r); ++i) {
        const TfResolvedSymbol *s = tf_resolution_symbol_at(r, i);
        if (s->module.owner == r && s->module.index == m->id.index &&
            s->kind == kind && text_is(s->name, name)) {
            CHECK(found == NULL);
            found = s;
        }
    }
    CHECK(found != NULL && tf_resolution_symbol(r, found->id) == found);
    return found;
}

static const TfDiagnosticRecord *diagnostic(const TfResolution *r, size_t index,
                                            const char *code, size_t labels) {
    const TfDiagnosticBag *bag = tf_resolution_diagnostics(r);
    const TfDiagnosticRecord *d = tf_diagnostic_bag_get(bag, index);
    if (d == NULL || strcmp(d->code, code) != 0 || d->label_count != labels)
        (void)tf_diagnostic_bag_write(stderr, bag, TF_DIAG_TEXT, TF_DIAG_COLOR_NEVER);
    CHECK(d != NULL && strcmp(d->code, code) == 0 && d->severity == TF_DIAG_ERROR);
    CHECK(d->label_count == labels);
    return d;
}

static void label(const TfDiagnosticRecord *d, size_t at, bool primary, TfSpan span) {
    CHECK(at < d->label_count);
    CHECK(d->labels[at].primary == primary);
    if (!same_span(d->labels[at].span, span))
        fprintf(stderr, "%s label %zu: expected [%zu,%zu), got [%zu,%zu)\n",
                d->code, at, span.start, span.end, d->labels[at].span.start, d->labels[at].span.end);
    CHECK(same_span(d->labels[at].span, span));
}


static void graph(void) {
    Fixture f;
    char sub[PATH_CAP];
    TfSourceManager *sm = manager_new();
    TfResolution *r, *again;
    const TfResolvedModule *m, *dep, *shared, *other;
    const TfResolvedSymbol *a, *b, *make, *variant, *ctor;
    const TfResolvedType *left, *right;
    const TfAstNode *n;
    const char *entry;

    fixture_init(&f);
    join(sub, f.directory, "sub");
    mkdir_checked(sub);
    entry = put(&f, "entry.tfd",
        "import \"sub/dependency.tfd\" as dep;\n"
        "import \"sub/shared.tfd\" as first;\n"
        "import \"sub/./shared.tfd\" as second;\n"
        "import \"other.tfd\" as other;\n"
        "fn construct() -> first::Shape { second::Shape(side: dep::side()) }\n"
        "fn keep(v: other::Shape) -> other::Shape { v }\n"
        "fn flag() -> second::Flag { first::Flag::On }\n");
    put(&f, "sub/dependency.tfd",
        "import \"shared.tfd\" as inner;\n"
        "pub fn side() -> Int { inner::number() }\n");
    put(&f, "sub/shared.tfd",
        "pub record Shape { side: Int }\n"
        "pub enum Flag { On, Off }\n"
        "pub fn number() -> Int { 4 }\n");
    put(&f, "other.tfd", "pub record Shape { side: Int }\n");

    r = resolve(sm, entry, NULL);
    phase(r, true, 0, 0);
    CHECK(tf_resolution_module_count(r) == 4);
    CHECK(tf_source_manager_count(sm) == 4);
    m = root_module(r);
    CHECK(m == module_named(r, "entry.tfd"));
    dep = module_named(r, "sub/dependency.tfd");
    shared = module_named(r, "sub/shared.tfd");
    other = module_named(r, "other.tfd");
    CHECK(!dep->is_root && !shared->is_root && !other->is_root);
    CHECK(tf_ast_source(shared->ast) == shared->source->source);
    a = named_symbol(r, m, "first", TF_SYMBOL_ALIAS);
    b = named_symbol(r, m, "second", TF_SYMBOL_ALIAS);
    CHECK(a->id.index != b->id.index);
    CHECK(tf_resolution_module(r, a->target_module) == shared);
    CHECK(tf_resolution_module(r, b->target_module) == shared);
    CHECK(tf_resolution_module(r, named_symbol(r, dep, "inner", TF_SYMBOL_ALIAS)->target_module) == shared);
    make = named_symbol(r, m, "construct", TF_SYMBOL_FUNCTION);
    n = tf_ast_get(m->ast, make->declaration);
    left = tf_resolution_type(r, reference(r, n->as.function.result_type, TF_REFERENCE_TYPE)->type);
    CHECK(left != NULL && left->kind == TF_TYPE_RECORD && text_is(left->name, "Shape"));
    CHECK(left->id.module_index == shared->id.index);
    n = node_in(m, TF_AST_TYPE_PATH, "v: other::Shape", 3, strlen("other::Shape"));
    right = tf_resolution_type(r, reference(r, n->id, TF_REFERENCE_TYPE)->type);
    CHECK(right != NULL && right != left && text_is(right->name, "Shape"));
    CHECK(right->id.module_index == other->id.index);
    n = node_in(m, TF_AST_PATH, "second::Shape(side:", 0, strlen("second::Shape"));
    ctor = target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_CONSTRUCTOR);
    CHECK(tf_resolution_type(r, ctor->type) == left);
    CHECK(target(r, n->as.path.segments.items[0], TF_REFERENCE_MODULE, TF_SYMBOL_ALIAS) == b);
    n = node_in(m, TF_AST_PATH, "first::Flag::On", 0, strlen("first::Flag::On"));
    variant = target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_VARIANT);
    CHECK(variant->module.index == shared->id.index && text_is(variant->name, "On"));
    CHECK(target(r, n->as.path.segments.items[0], TF_REFERENCE_MODULE, TF_SYMBOL_ALIAS) == a);
    CHECK(tf_resolution_type(r, reference(r, n->as.path.segments.items[1], TF_REFERENCE_TYPE)->type)
          == tf_resolution_type(r, variant->type));
    n = node_in(dep, TF_AST_PATH, "inner::number()", 0, strlen("inner::number"));
    CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_FUNCTION)
          == named_symbol(r, shared, "number", TF_SYMBOL_FUNCTION));
    n = node_in(m, TF_AST_ARGUMENT, "side: dep::side()", 0, strlen("side: dep::side()"));
    CHECK(tf_resolution_reference(r, n->id) == NULL); /* M06 owns argument labels. */

    fixture_remove_files(&f);
    again = resolve(sm, entry, NULL);
    phase(again, true, 0, 0);
    CHECK(module_named(again, "sub/shared.tfd")->source == shared->source);
    CHECK(module_named(again, "sub/shared.tfd")->id.index == shared->id.index);
    CHECK(tf_resolution_module(again, shared->id) == NULL);
    CHECK(tf_resolution_symbol(again, a->id) == NULL);
    CHECK(tf_resolution_scope(again, a->scope) == NULL);
    CHECK(tf_resolution_type(again, left->id) == NULL);
    CHECK(tf_resolution_reference(again, n->id) == NULL);
    CHECK(tf_resolution_module(r, (TfModuleId){0}) == NULL);
    CHECK(tf_resolution_symbol(r, (TfSymbolId){0}) == NULL);
    CHECK(tf_resolution_scope(r, (TfScopeId){0}) == NULL);
    CHECK(tf_resolution_type(r, (TfTypeId){0}) == NULL);
    CHECK(tf_resolution_reference(r, (TfNodeId){0}) == NULL);
    tf_resolution_destroy(again);
    tf_resolution_destroy(r);
    tf_source_manager_destroy(sm);
    fixture_remove_files(&f);
    rmdir_checked(sub);
    rmdir_checked(f.directory);
}

static void cycle(void) {
    Fixture f;
    TfSourceManager *sm = manager_new();
    TfResolution *r;
    const TfDiagnosticRecord *d;
    const char *entry;
    const char *const expected[] = {"a.tfd", "b.tfd", "c.tfd", "a.tfd"};
    fixture_init(&f);
    entry = put(&f, "a.tfd", "import \"b.tfd\" as b;\n");
    put(&f, "b.tfd", "import \"c.tfd\" as c;\n");
    put(&f, "c.tfd", "import \"a.tfd\" as a;\n");
    r = resolve(sm, entry, NULL);
    phase(r, false, 1, 0);
    CHECK(tf_resolution_module_count(r) == 3 && tf_source_manager_count(sm) == 3);
    d = diagnostic(r, 0, "E205", 3);
    label(d, 0, true, span_of(module_named(r, "c.tfd")->source->source, "\"a.tfd\"", 0, 7));
    label(d, 1, false, span_of(module_named(r, "a.tfd")->source->source, "\"b.tfd\"", 0, 7));
    label(d, 2, false, span_of(module_named(r, "b.tfd")->source->source, "\"c.tfd\"", 0, 7));
    CHECK(d->note_count == sizeof expected / sizeof expected[0]);
    for (size_t i = 0; i < d->note_count; ++i) {
        CHECK(d->notes[i].length == strlen(expected[i]));
        CHECK(memcmp(d->notes[i].data, expected[i], d->notes[i].length) == 0);
    }
    tf_resolution_destroy(r);
    tf_source_manager_destroy(sm);
    fixture_destroy(&f);
}

static void paths(void) {
    static const char *const forbidden[] = {
        "\"missing.tfd\"", "\"../root-other/outside.tfd\"", "\"dir/../good.tfd\"",
        "\"/outside.tfd\"", "\"https://example.invalid/outside.tfd\"",
        "\"dir\\\\inside.tfd\"", "\"embedded\\u{0}nul.tfd\"", "\"good.txt\""
    };
    Fixture f;
    char base[PATH_CAP], other[PATH_CAP], nested[PATH_CAP], input[128];
    TfSourceManager *sm = manager_new();
    const char *outside;
    TfResolveOptions opts;
    TfResolution *r;
    fixture_init(&f);
    join(base, f.directory, "root");
    join(other, f.directory, "root-other");
    join(nested, f.directory, "root/dir");
    mkdir_checked(base);
    mkdir_checked(other);
    mkdir_checked(nested);
    opts = (TfResolveOptions){base, 100};
#ifndef _WIN32
    /* A backslash is part of a POSIX filename, never a containment separator. */
    r = resolve(sm, put(&f, "root\outside.tfd", ""), &opts);
    phase(r, false, 1, 0);
    (void)diagnostic(r, 0, "E204", 0);
    CHECK(tf_resolution_module_count(r) == 0 && tf_source_manager_count(sm) == 0);
    tf_resolution_destroy(r);
#endif
    outside = put(&f, "root-other/outside.tfd", "fn ignored() -> Int { 1 }\n");
    put(&f, "root/good.tfd", "");
    put(&f, "root/dir/inside.tfd", "");
    r = resolve(sm, outside, &opts);
    phase(r, false, 1, 0);
    (void)diagnostic(r, 0, "E204", 0);
    CHECK(tf_resolution_module_count(r) == 0 && tf_source_manager_count(sm) == 0);
    tf_resolution_destroy(r);
    for (size_t i = 0; i < sizeof forbidden / sizeof forbidden[0]; ++i) {
        char name[64];
        const char *entry;
        int n = snprintf(name, sizeof name, "root/invalid-%zu.tfd", i);
        CHECK(n > 0 && (size_t)n < sizeof name);
        n = snprintf(input, sizeof input, "import %s as bad;\n", forbidden[i]);
        CHECK(n > 0 && (size_t)n < sizeof input);
        entry = put(&f, name, input);
        r = resolve(sm, entry, &opts);
        phase(r, false, 1, 0);
        label(diagnostic(r, 0, "E204", 1), 0, true,
              span_of(root_module(r)->source->source, forbidden[i], 0, strlen(forbidden[i])));
        CHECK(tf_resolution_module_count(r) == 1 && tf_source_manager_count(sm) == i + 1);
        tf_resolution_destroy(r);
    }
    tf_source_manager_destroy(sm);
    fixture_remove_files(&f);
    rmdir_checked(nested);
    rmdir_checked(base);
    rmdir_checked(other);
    rmdir_checked(f.directory);
}


static bool make_link(Fixture *f, const char *name, const char *target_path) {
    const char *path = fixture_path(f, name);
#ifdef _WIN32
    if (CreateSymbolicLinkA(path, target_path, 2) != 0) return true;
    if (GetLastError() == ERROR_INVALID_PARAMETER &&
        CreateSymbolicLinkA(path, target_path, 0) != 0) return true;
    CHECK(GetLastError() == ERROR_PRIVILEGE_NOT_HELD || GetLastError() == ERROR_NOT_SUPPORTED);
#else
    if (symlink(target_path, path) == 0) return true;
    CHECK(errno == EPERM || errno == EACCES || errno == ENOSYS || errno == EOPNOTSUPP);
#endif
    --f->count;
    ++skips;
    fprintf(stderr, "SKIP [%s]: host cannot create symbolic links\n", case_name);
    return false;
}

static void symlinks(void) {
    Fixture f;
    char sub[PATH_CAP];
    TfSourceManager *sm;
    TfResolution *r;
    const TfResolvedModule *m, *shared;
    const char *outside, *inside, *entry, *escape;
    fixture_init(&f);
    join(sub, f.directory, "root");
    mkdir_checked(sub);
    outside = put(&f, "outside.tfd", "fn secret() -> Int { 0 }\n");
    inside = put(&f, "root/inside.tfd", "pub fn value() -> Int { 1 }\n");
    if (make_link(&f, "root/alias.tfd", inside) && make_link(&f, "root/escape.tfd", outside)) {
        entry = put(&f, "root/entry.tfd",
                    "import \"alias.tfd\" as a;\nimport \"inside.tfd\" as b;\n"
                    "fn value() -> Int { a::value() + b::value() }\n");
        escape = put(&f, "root/probe.tfd", "import \"escape.tfd\" as escape;\n");
        sm = manager_new();
        r = resolve(sm, entry, NULL);
        phase(r, true, 0, 0);
        CHECK(tf_resolution_module_count(r) == 2 && tf_source_manager_count(sm) == 2);
        m = root_module(r);
        shared = module_named(r, "inside.tfd");
        CHECK(tf_resolution_module(r, named_symbol(r, m, "a", TF_SYMBOL_ALIAS)->target_module) == shared);
        CHECK(tf_resolution_module(r, named_symbol(r, m, "b", TF_SYMBOL_ALIAS)->target_module) == shared);
        tf_resolution_destroy(r);
        r = resolve(sm, escape, NULL);
        phase(r, false, 1, 0);
        m = root_module(r);
        label(diagnostic(r, 0, "E204", 1), 0, true,
              span_of(m->source->source, "\"escape.tfd\"", 0, 12));
        CHECK(tf_resolution_module_count(r) == 1 && tf_source_manager_count(sm) == 3);
        tf_resolution_destroy(r);
        tf_source_manager_destroy(sm);
    }
    fixture_remove_files(&f);
    rmdir_checked(sub);
    rmdir_checked(f.directory);
}

static void namespaces(void) {
    Fixture f;
    TfSourceManager *sm = manager_new();
    TfResolution *r;
    const TfResolvedModule *m;
    const TfAstNode *n;
    const TfResolvedType *tag;
    const TfResolvedSymbol *function, *variant;
    fixture_init(&f);
    r = resolve(sm, put(&f, "library.tfd",
        "fn forward() -> Tag { later() }\n"
        "fn Tag() -> Tag { Tag::Here }\n"
        "enum Tag { Here }\n"
        "fn later() -> Tag { if true { Tag() } else { forward() } }\n"), NULL);
    phase(r, true, 0, 0);
    m = root_module(r);
    function = named_symbol(r, m, "Tag", TF_SYMBOL_FUNCTION);
    n = tf_ast_get(m->ast, function->declaration);
    tag = tf_resolution_type(r, reference(r, n->as.function.result_type, TF_REFERENCE_TYPE)->type);
    CHECK(tag != NULL && tag->kind == TF_TYPE_ENUM && text_is(tag->name, "Tag"));
    n = node_in(m, TF_AST_PATH, "{ Tag::Here }", 2, 9);
    variant = target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_VARIANT);
    CHECK(tf_resolution_type(r, variant->type) == tag);
    n = node_in(m, TF_AST_PATH, "{ Tag() }", 2, 3);
    CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_FUNCTION) == function);
    n = node_in(m, TF_AST_PATH, "{ later() }", 2, 5);
    CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_FUNCTION)
          == named_symbol(r, m, "later", TF_SYMBOL_FUNCTION));
    n = node_in(m, TF_AST_PATH, "{ forward() }", 2, 7);
    CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_FUNCTION)
          == named_symbol(r, m, "forward", TF_SYMBOL_FUNCTION));
    tf_resolution_destroy(r);
    tf_source_manager_destroy(sm);
    fixture_destroy(&f);
}

typedef struct {
    const char *code, *use;
    size_t skip, width;
    const char *declaration;
    size_t decl_skip, decl_width;
    bool imported_declaration;
} Error;

static void errors(const TfResolution *r, const Error *expected, size_t count) {
    const TfSource *src = root_module(r)->source->source;
    phase(r, false, count, 0);
    for (size_t i = 0; i < count; ++i) {
        const Error *e = &expected[i];
        const TfDiagnosticRecord *d = diagnostic(r, i, e->code, e->declaration == NULL ? 1 : 2);
        label(d, 0, true, span_of(src, e->use, e->skip, e->width));
        if (e->declaration != NULL) {
            const TfSource *decl = e->imported_declaration ? module_named(r, "lib.tfd")->source->source : src;
            label(d, 1, false, span_of(decl, e->declaration, e->decl_skip, e->decl_width));
        }
    }
}

static void privacy(void) {
    static const Error expected[] = {
        {"E206", "lib::Hidden", 5, 6, "record Hidden", 7, 6, true},
        {"E207", "List[Local]", 5, 5, "record Local", 7, 5, false},
        {"E206", "lib::secret", 0, 11, "fn secret", 3, 6, true},
        {"E203", "{ open() }", 2, 4, NULL, 0, 0, false}
    };
    Fixture f;
    TfSourceManager *sm = manager_new();
    TfResolution *r;
    fixture_init(&f);
    put(&f, "lib.tfd",
        "record Hidden {}\n"
        "fn secret() -> Int { 0 }\n"
        "pub fn open() -> Int { secret() }\n");
    r = resolve(sm, put(&f, "entry.tfd",
        "import \"lib.tfd\" as lib;\n"
        "record Local {}\n"
        "fn hidden(x: lib::Hidden) -> Int { 0 }\n"
        "pub fn leak() -> List[Local] { [] }\n"
        "fn denied() -> Int { lib::secret() }\n"
        "fn unqualified() -> Int { open() }\n"), NULL);
    errors(r, expected, sizeof expected / sizeof expected[0]);
    tf_resolution_destroy(r);
    tf_source_manager_destroy(sm);
    fixture_destroy(&f);
}

static void failed_import(void) {
    Fixture f;
    TfSourceManager *sm = manager_new();
    TfResolution *r;
    const TfDiagnosticRecord *d;
    const TfResolvedModule *healthy;
    fixture_init(&f);
    put(&f, "bad.tfd", "@");
    put(&f, "healthy.tfd", "pub fn use() -> Int { absent }");
    r = resolve(sm,put(&f,"entry.tfd",
        "import \"bad.tfd\" as bad; import \"healthy.tfd\" as healthy; "
        "fn f(x: bad::Thing) -> Int { bad::helper() }"),NULL);
    phase(r,false,3,0);
    d = diagnostic(r,0,"E101",1);
    label(d,0,true,span_of(module_named(r,"bad.tfd")->source->source,"@",0,1));
    d = diagnostic(r,1,"E110",1);
    label(d,0,true,span_of(module_named(r,"bad.tfd")->source->source,"@",0,1));
    healthy = module_named(r,"healthy.tfd");
    d = diagnostic(r,2,"E203",1);
    label(d,0,true,span_of(healthy->source->source,"absent",0,6));
    tf_resolution_destroy(r);
    tf_source_manager_destroy(sm);
    fixture_destroy(&f);
}

static void wrong_variant(void) {
    Fixture f;
    TfSourceManager *sm = manager_new();
    TfResolution *r;
    const TfDiagnosticRecord *d;
    fixture_init(&f);
    put(&f,"lib.tfd","pub fn helper() -> Int { 0 }");
    r = resolve(sm,put(&f,"entry.tfd",
        "import \"lib.tfd\" as lib; fn f() -> Int { match 1 { lib::helper => { 0 }, _ => { 1 } } }"),NULL);
    phase(r,false,1,0);
    d = diagnostic(r,0,"E312",2);
    label(d,0,true,span_of(root_module(r)->source->source,"lib::helper",0,11));
    label(d,1,false,span_of(module_named(r,"lib.tfd")->source->source,"helper",0,6));
    tf_resolution_destroy(r);
    tf_source_manager_destroy(sm);
    fixture_destroy(&f);
}

static void nominal_ordinal(void) {
    Fixture f;
    TfSourceManager *sm = manager_new();
    TfResolution *a, *b;
    const TfResolvedSymbol *ca, *cb;
    fixture_init(&f);
    put(&f,"lib.tfd","");
    a = resolve(sm,put(&f,"a.tfd","fn f() -> Int { 0 } record Item {}"),NULL);
    b = resolve(sm,put(&f,"b.tfd",
        "import \"lib.tfd\" as lib; fn f() -> Int { 0 } record Item {}"),NULL);
    phase(a,true,0,0);
    phase(b,true,0,0);
    ca = named_symbol(a,root_module(a),"Item",TF_SYMBOL_CONSTRUCTOR);
    cb = named_symbol(b,root_module(b),"Item",TF_SYMBOL_CONSTRUCTOR);
    CHECK(ca->type.declaration_index == 2 && cb->type.declaration_index == 2);
    CHECK(tf_resolution_type(a,cb->type) == NULL);
    tf_resolution_destroy(b);
    tf_resolution_destroy(a);
    tf_source_manager_destroy(sm);
    fixture_destroy(&f);
}

static void duplicates(void) {
    static const Error expected[] = {
        {"E202", "as alias; // two", 3, 5, "as alias; // one", 3, 5, false},
        {"E202", "enum alias", 5, 5, "as alias; // one", 3, 5, false},
        {"E202", "fn Item", 3, 4, "record Item", 7, 4, false},
        {"E201", "fn len", 3, 3, NULL, 0, 0, false},
        {"E202", "let x = 1", 4, 1, "x: Int", 0, 1, false},
        {"E203", "= absent;", 2, 6, NULL, 0, 0, false}
    };
    Fixture f;
    TfSourceManager *sm = manager_new();
    TfResolution *r;
    fixture_init(&f);
    put(&f, "lib.tfd", "");
    r = resolve(sm, put(&f, "entry.tfd",
        "import \"lib.tfd\" as alias; // one\n"
        "import \"lib.tfd\" as alias; // two\n"
        "enum alias { One }\n"
        "record Item {}\n"
        "fn Item() -> Int { 0 }\n"
        "fn len() -> Int { 0 }\n"
        "fn bad(x: Int) -> Int { let x = 1; let absent = absent; x }\n"), NULL);
    errors(r, expected, sizeof expected / sizeof expected[0]);
    tf_resolution_destroy(r);
    tf_source_manager_destroy(sm);
    fixture_destroy(&f);
}


static void scoping(void) {
    Fixture f;
    TfSourceManager *sm = manager_new();
    TfResolution *r;
    const TfResolvedModule *m;
    const TfAstNode *n, *loop;
    const TfResolvedSymbol *outer, *shadow, *loop_symbol, *pattern_symbol;
    size_t captured_outer = 0, captured_shadow = 0;
    fixture_init(&f);
    r = resolve(sm, put(&f, "scope.tfd",
        "fn helper() -> Int { 0 }\n"
        "fn collect(x: Int) -> List[Int] {\n"
        "  let vals = [x];\n"
        "  let _ = helper();\n"
        "  for vals in vals {\n"
        "    if true {\n"
        "      let x = x + vals;\n"
        "      let f = fn () -> Int {\n"
        "        let g = fn () -> Int { x + x + helper() };\n"
        "        g()\n"
        "      };\n"
        "      f()\n"
        "    } else { match vals { item => { item } } }\n"
        "  }\n"
        "}\n"), NULL);
    phase(r, true, 0, 0);
    m = root_module(r);
    outer = named_symbol(r, m, "x", TF_SYMBOL_PARAMETER);
    shadow = named_symbol(r, m, "x", TF_SYMBOL_LOCAL);
    loop_symbol = named_symbol(r, m, "vals", TF_SYMBOL_LOOP);
    pattern_symbol = named_symbol(r, m, "item", TF_SYMBOL_PATTERN);
    CHECK(outer != shadow && outer->scope.index != shadow->scope.index);
    n = node_in(m, TF_AST_PATH, "let x = x + vals", 8, 1);
    CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_PARAMETER) == outer);
    n = node_in(m, TF_AST_PATH, "let x = x + vals", 12, 4);
    CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_LOOP) == loop_symbol);
    n = node_in(m, TF_AST_PATH, "for vals in vals", 12, 4);
    CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_LOCAL)
          == named_symbol(r, m, "vals", TF_SYMBOL_LOCAL));
    n = node_in(m, TF_AST_PATH, "item => { item }", 10, 4);
    CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_PATTERN) == pattern_symbol);
    n = node_in(m, TF_AST_LET, "let _ = helper();", 0, strlen("let _ = helper();"));
    CHECK(tf_resolution_reference(r, n->id) == NULL);
    loop = tf_ast_get(m->ast, loop_symbol->declaration);
    CHECK(loop != NULL && loop->kind == TF_AST_FOR);
    CHECK(target(r, loop->id, TF_REFERENCE_DECLARATION, TF_SYMBOL_LOOP) == loop_symbol);
    {
        TfScopeId ancestor = shadow->scope;
        size_t distance = 0;
        while (ancestor.index != loop_symbol->scope.index) {
            const TfResolvedScope *scope = tf_resolution_scope(r, ancestor);
            CHECK(scope != NULL && distance < tf_resolution_scope_count(r));
            ancestor = scope->parent;
            ++distance;
        }
        CHECK(distance != 0 && ancestor.owner == loop_symbol->scope.owner);
    }

    /* The inner free use needs x in both closure environments, once each.
     * Neither the unshadowed parameter nor the global helper is captured. */
    CHECK(tf_resolution_capture_count(r) == 2);
    for (size_t i = 0; i < tf_resolution_capture_count(r); ++i) {
        const TfResolvedCapture *cap = tf_resolution_capture_at(r, i);
        const TfAstNode *closure = tf_ast_get(m->ast, cap->closure);
        CHECK(cap->kind == TF_REFERENCE_VALUE && tf_resolution_symbol(r, cap->symbol) == shadow);
        CHECK(closure != NULL && closure->kind == TF_AST_CLOSURE);
        if (closure->span.start == span_of(m->source->source, "fn () -> Int {\n", 0, 0).start)
            ++captured_outer;
        if (closure->span.start == span_of(m->source->source, "fn () -> Int { x + x", 0, 0).start)
            ++captured_shadow;
    }
    CHECK(captured_outer == 1 && captured_shadow == 1);
    tf_resolution_destroy(r);
    tf_source_manager_destroy(sm);
    fixture_destroy(&f);
}

static void machine(void) {
    Fixture f;
    TfSourceManager *sm = manager_new();
    TfResolution *r;
    const TfResolvedModule *m;
    const TfResolvedSymbol *input, *cell, *other;
    const TfAstNode *n;
    fixture_init(&f);
    r = resolve(sm, put(&f, "machine.tfd",
        "fn score() -> Int { 0 }\n"
        "machine Counter(delta: Int) yields Int rejects Unit {\n"
        "  state score: Int = score();\n"
        "  state total: Int = 0;\n"
        "  next total = next.score + total;\n"
        "  next score = score + delta;\n"
        "  guard next.total >= 0 else ();\n"
        "  emit next.total;\n"
        "}\n"), NULL);
    phase(r, true, 0, 0);
    m = root_module(r);
    input = named_symbol(r, m, "delta", TF_SYMBOL_MACHINE_INPUT);
    cell = named_symbol(r, m, "score", TF_SYMBOL_CELL);
    other = named_symbol(r, m, "total", TF_SYMBOL_CELL);
    n = node_in(m, TF_AST_PATH, "= score();", 2, 5);
    CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_FUNCTION)
          == named_symbol(r, m, "score", TF_SYMBOL_FUNCTION));
    n = node_in(m, TF_AST_PATH, "= score + delta", 2, 5);
    CHECK(target(r, n->id, TF_REFERENCE_OLD, TF_SYMBOL_CELL) == cell);
    n = node_in(m, TF_AST_PATH, "+ delta;", 2, 5);
    CHECK(target(r, n->id, TF_REFERENCE_INPUT, TF_SYMBOL_MACHINE_INPUT) == input);
    n = node_in(m, TF_AST_NEXT, "next.score + total", 0, 10);
    CHECK(target(r, n->id, TF_REFERENCE_CANDIDATE, TF_SYMBOL_CELL) == cell);
    n = node_in(m, TF_AST_PATH, "+ total;", 2, 5);
    CHECK(target(r, n->id, TF_REFERENCE_OLD, TF_SYMBOL_CELL) == other);
    n = node_in(m, TF_AST_NEXT, "guard next.total", 6, 10);
    CHECK(target(r, n->id, TF_REFERENCE_CANDIDATE, TF_SYMBOL_CELL) == other);
    n = node_in(m, TF_AST_NEXT, "emit next.total", 5, 10);
    CHECK(target(r, n->id, TF_REFERENCE_CANDIDATE, TF_SYMBOL_CELL) == other);
    n = node_in(m, TF_AST_WRITER, "next score = score + delta;", 0, 27);
    CHECK(target(r, n->id, TF_REFERENCE_WRITER, TF_SYMBOL_CELL) == cell);
    n = node_in(m, TF_AST_WRITER, "next total = next.score + total;", 0, 32);
    CHECK(target(r, n->id, TF_REFERENCE_WRITER, TF_SYMBOL_CELL) == other);
    CHECK(tf_resolution_type(r, cell->type)->kind == TF_TYPE_MACHINE);
    tf_resolution_destroy(r);
    tf_source_manager_destroy(sm);
    fixture_destroy(&f);
}

static void machine_errors(void) {
    static const Error expected[] = {
        {"E405", "next.outside", 0, 12, NULL, 0, 0, false},
        {"E403", "next delta =", 5, 5, NULL, 0, 0, false},
        {"E403", "next.unknown", 5, 7, NULL, 0, 0, false},
        {"E406", "next.score", 0, 10, "state score", 6, 5, false}
    };
    Fixture f;
    TfSourceManager *sm = manager_new();
    TfResolution *r;
    fixture_init(&f);
    r = resolve(sm, put(&f, "machine.tfd",
        "fn outside() -> Int { next.outside }\n"
        "machine Counter(delta: Int) yields Int rejects Unit {\n"
        "  state score: Int = 0;\n"
        "  next delta = next.unknown;\n"
        "  next score = { let f = fn () -> Int { next.score }; f() };\n"
        "}\n"), NULL);
    errors(r, expected, sizeof expected / sizeof expected[0]);
    tf_resolution_destroy(r);
    tf_source_manager_destroy(sm);
    fixture_destroy(&f);
}

static void entry(void) {
    static const Error expected[] = {
        {"E208", "let f = main;", 8, 4, "pub fn main", 7, 4, false},
        {"E208", "{ main() }", 2, 4, "pub fn main", 7, 4, false}
    };
    Fixture f;
    TfSourceManager *sm = manager_new();
    TfResolution *r;
    const TfResolvedModule *m;
    const TfAstNode *n;
    const TfResolvedSymbol *main_symbol;
    fixture_init(&f);
    put(&f, "lib.tfd", "pub fn main() -> List[Text] { [] }\n");
    r = resolve(sm, put(&f, "pure.tfd",
        "import \"lib.tfd\" as lib;\n"
        "pub fn main() -> List[Text] { lib::main() }\n"
        "fn caller() -> List[Text] { main() }\n"), NULL);
    phase(r, true, 0, 0);
    m = root_module(r);
    main_symbol = named_symbol(r, m, "main", TF_SYMBOL_FUNCTION);
    CHECK(!main_symbol->entry_only);
    n = node_in(m, TF_AST_PATH, "{ main() }", 2, 4);
    CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_FUNCTION) == main_symbol);
    n = node_in(m, TF_AST_PATH, "{ lib::main() }", 2, 9);
    CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_FUNCTION)
          == named_symbol(r, module_named(r, "lib.tfd"), "main", TF_SYMBOL_FUNCTION));
    tf_resolution_destroy(r);

    r = resolve(sm, put(&f, "io.tfd",
        "pub fn main() -> List[Text] {\n"
        "  input visitor: Text from stdin;\n"
        "  output visitor to stdout;\n"
        "  [visitor]\n"
        "}\n"
        "fn by_value() -> List[Text] { let f = main; f() }\n"
        "fn by_call() -> List[Text] { main() }\n"
        "fn shadow(main: Text) -> Text { main }\n"), NULL);
    errors(r, expected, sizeof expected / sizeof expected[0]);
    m = root_module(r);
    CHECK(named_symbol(r, m, "main", TF_SYMBOL_FUNCTION)->entry_only);
    n = node_in(m, TF_AST_PATH, "output visitor", 7, 7);
    CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_ENTRY_INPUT)
          == named_symbol(r, m, "visitor", TF_SYMBOL_ENTRY_INPUT));
    tf_resolution_destroy(r);

    /* The same I/O syntax is invalid when loaded as an imported module. */
    put(&f, "imported-io.tfd", "pub fn main() -> List[Text] { output \"x\"; [] }\n");
    r = resolve(sm, put(&f, "importing.tfd", "import \"imported-io.tfd\" as unused;\n"), NULL);
    phase(r, false, 1, 0);
    label(diagnostic(r, 0, "E208", 1), 0, true,
          span_of(module_named(r, "imported-io.tfd")->source->source, "output", 0, 6));
    tf_resolution_destroy(r);
    tf_source_manager_destroy(sm);
    fixture_destroy(&f);
}




/* Flat chains give the parser a shallow syntax workload but make a deep
 * left spine in the AST. Avoid recursive traversal in the test itself. */
static void flat_chain(bool postfix) {
    enum { HOPS = 10000 };
    static const char prefix[] = "fn chain(x: Int) -> Int { x";
    static const char suffix[] = " }\n";
    const char *hop = postfix ? ".field" : " + x";
    size_t hop_len = strlen(hop);
    size_t length = sizeof prefix - 1 + HOPS * hop_len + sizeof suffix;
    char *program = malloc(length);
    Fixture f;
    TfSourceManager *sm = manager_new();
    TfResolution *r;
    const TfResolvedModule *m;
    const TfResolvedSymbol *parameter;
    const TfAstNode *n;
    size_t offset = sizeof prefix - 1;

    CHECK(program != NULL);
    memcpy(program, prefix, offset);
    for (size_t i = 0; i < HOPS; ++i) {
        memcpy(program + offset, hop, hop_len);
        offset += hop_len;
    }
    memcpy(program + offset, suffix, sizeof suffix);
    fixture_init(&f);
    r = resolve(sm, put(&f, "chain.tfd", program), NULL);
    free(program);
    phase(r, true, 0, 0);
    m = root_module(r);
    parameter = named_symbol(r, m, "x", TF_SYMBOL_PARAMETER);
    {
        const TfSource *src = m->source->source;
        size_t first = sizeof prefix - 2;
        n = node_at(m, TF_AST_PATH, (TfSpan){src, first, first + 1});
        CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_PARAMETER) == parameter);
        n = node_at(m, postfix ? TF_AST_PROJECT : TF_AST_BINARY, (TfSpan){src, first, offset});
        CHECK(tf_resolution_reference(r, n->id) == NULL);
        if (!postfix) {
            n = node_at(m, TF_AST_PATH, (TfSpan){src, offset - 1, offset});
            CHECK(target(r, n->id, TF_REFERENCE_VALUE, TF_SYMBOL_PARAMETER) == parameter);
        }
    }
    {
        size_t references = 0;
        for (size_t i = 0; i < tf_resolution_reference_count(r); ++i) {
            const TfResolvedReference *ref = tf_resolution_reference_at(r, i);
            const TfAstNode *use = tf_ast_get(m->ast, ref->node);
            if (ref->kind == TF_REFERENCE_VALUE && use->kind == TF_AST_PATH &&
                use->as.path.segments.count != 0) {
                CHECK(tf_resolution_symbol(r, ref->symbol) == parameter);
                ++references;
            }
        }
        CHECK(references == (postfix ? 1 : HOPS + 1));
    }
    tf_resolution_destroy(r);
    tf_source_manager_destroy(sm);
    fixture_destroy(&f);
}

static void flat_binary(void) { flat_chain(false); }
static void flat_postfix(void) { flat_chain(true); }

static void diagnostic_limit(void) {
    enum { CLAUSES = 101 };
    static const char clause[] = "  output \"x\";\n";
    Fixture f;
    TfSourceManager *sm = manager_new();
    TfResolution *r;
    TfResolveOptions opts = {NULL, 2};
    char program[2048] = "fn forbidden() -> Int {\n";
    size_t offset = strlen(program), first = offset;
    const TfSource *source;
    fixture_init(&f);
    CHECK(offset + CLAUSES * (sizeof clause - 1) + sizeof "  0\n}\n" <= sizeof program);
    for (size_t i = 0; i < CLAUSES; ++i) {
        memcpy(program + offset, clause, sizeof clause - 1);
        offset += sizeof clause - 1;
    }
    memcpy(program + offset, "  0\n}\n", sizeof "  0\n}\n");
    r = resolve(sm, put(&f, "limit.tfd", program), &opts);
    source = root_module(r)->source->source;
    for (size_t i = 0; i < opts.diagnostic_cap; ++i) {
        size_t start = first + i * (sizeof clause - 1) + 2;
        label(diagnostic(r, i, "E208", 1), 0, true, (TfSpan){source, start, start + 6});
    }
    if (tf_diagnostic_bag_suppressed(tf_resolution_diagnostics(r)) != CLAUSES - opts.diagnostic_cap)
        fprintf(stderr, "expected %zu suppressed diagnostics, got %zu\n",
                CLAUSES - opts.diagnostic_cap,
                tf_diagnostic_bag_suppressed(tf_resolution_diagnostics(r)));
    phase(r, false, opts.diagnostic_cap, CLAUSES - opts.diagnostic_cap);
    tf_resolution_destroy(r);
    tf_source_manager_destroy(sm);
    fixture_destroy(&f);
}

typedef struct { size_t attempts, fail_at, live; } AllocationProbe;

static void *probe_alloc(void *context, size_t size) {
    AllocationProbe *p = context;
    void *ptr;
    if (++p->attempts == p->fail_at) return NULL;
    ptr = malloc(size);
    CHECK(ptr != NULL);
    ++p->live;
    return ptr;
}

static void probe_free(void *context, void *ptr) {
    AllocationProbe *p = context;
    if (ptr != NULL) {
        CHECK(p->live > 0);
        --p->live;
        free(ptr);
    }
}

static void oom(void) {
    Fixture f;
    const char *entry;
    size_t last;
    fixture_init(&f);
    entry = put(&f, "entry.tfd", "import \"dep.tfd\" as dep;\n");
    put(&f, "dep.tfd", "");
    {
        AllocationProbe p = {0};
        TfAllocator a = {&p, probe_alloc, probe_free};
        TfSourceManager *sm = NULL;
        TfResolution *r = NULL;
        CHECK(tf_source_manager_create(&sm, &a) == TF_OK && sm != NULL);
        CHECK(tf_resolve(&r, sm, entry, NULL, &a) == TF_OK && r != NULL);
        phase(r, true, 0, 0);
        CHECK(tf_resolution_module_count(r) == 2);
        last = p.attempts;
        CHECK(last > 1 && last < 1024);
        tf_resolution_destroy(r);
        tf_source_manager_destroy(sm);
        CHECK(p.live == 0);
    }
    for (size_t fail = 1; fail <= last; ++fail) {
        AllocationProbe p = {0, fail, 0};
        TfAllocator a = {&p, probe_alloc, probe_free};
        TfSourceManager *sm = NULL;
        TfResolution *r = NULL;
        TfStatus status = tf_source_manager_create(&sm, &a);
        if (status == TF_OK) {
            CHECK(sm != NULL);
            status = tf_resolve(&r, sm, entry, NULL, &a);
            if (status != TF_HOST_ERROR || r != NULL)
                fprintf(stderr, "allocation %zu/%zu: tf_resolve status %d, result %s\n",
                        fail, last, (int)status, r == NULL ? "NULL" : "non-NULL");
            CHECK(status == TF_HOST_ERROR && r == NULL);
            /* A failed resolver may leave complete immutable snapshots cached. */
            for (size_t i = 0; i < tf_source_manager_count(sm); ++i) {
                const TfManagedSource *s = tf_source_manager_lookup(sm, (TfSourceId){sm, i + 1});
                CHECK(s != NULL && s->status == TF_OK && s->source->valid);
            }
        } else {
            CHECK(status == TF_HOST_ERROR && sm == NULL);
        }
        CHECK(p.attempts >= fail);
        tf_resolution_destroy(r);
        tf_source_manager_destroy(sm);
        CHECK(p.live == 0);
    }
    printf("test_resolution: injected %zu allocation failures\n", last);
    fixture_destroy(&f);
}

int main(int argc, char **argv) {
    static const struct { const char *name; void (*run)(void); } cases[] = {
        {"graph", graph}, {"cycle", cycle}, {"paths", paths}, {"symlinks", symlinks},
        {"namespaces", namespaces}, {"privacy", privacy}, {"duplicates", duplicates},
        {"failed-import", failed_import}, {"wrong-variant", wrong_variant}, {"nominal-ordinal", nominal_ordinal},
        {"scoping", scoping}, {"machine", machine}, {"machine-errors", machine_errors},
        {"entry", entry}, {"diagnostic-limit", diagnostic_limit}, {"oom", oom},
        {"flat-binary", flat_binary}, {"flat-postfix", flat_postfix}
    };
    size_t ran = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        if (argc == 1 || (argc == 2 && strcmp(argv[1], cases[i].name) == 0)) {
            case_name = cases[i].name;
            cases[i].run();
            ++ran;
        }
    }
    if (ran == 0) {
        fprintf(stderr, "usage: test_resolution [case]\n");
        return EXIT_FAILURE;
    }
    printf("test_resolution: %zu checks in %zu cases, %zu capability skips\n", checks, ran, skips);
    return EXIT_SUCCESS;
}
