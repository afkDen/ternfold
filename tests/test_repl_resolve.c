#include "../src/repl_resolve.h"
#include "../src/source_manager_internal.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define ROOT "build/repl-resolve-fixture"
#define NESTED ROOT "/nested"
#define ENTRY NESTED "/root.tfd"
#define DEP NESTED "/dep.tfd"
#define SIBLING ROOT "/sibling.tfd"
#define ALIAS NESTED "/alias.tfd"
#define DEP_SOURCE "pub fn value() -> Int { 7 }"

#define CHECK(e) do { if (!(e)) { \
    fprintf(stderr,"test_repl_resolve:%d: %s\n",__LINE__,#e); exit(EXIT_FAILURE); \
} } while (0)

static void make_dir(const char *path) {
#ifdef _WIN32
    CHECK(_mkdir(path) == 0);
#else
    CHECK(mkdir(path,0700) == 0);
#endif
}
static void remove_dir(const char *path) {
#ifdef _WIN32
    CHECK(_rmdir(path) == 0);
#else
    CHECK(rmdir(path) == 0);
#endif
}
static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path,"wb");
    CHECK(file != NULL);
    CHECK(fputs(text,file) >= 0);
    CHECK(fclose(file) == 0);
}
static void init(TfSource *source, const char *text) {
    CHECK(tf_source_init(source,"<repl>",text,strlen(text),NULL) == TF_OK);
}
static const TfDiagnosticRecord *diagnostic(const TfResolution *resolution, const char *code) {
    const TfDiagnosticBag *bag = tf_resolution_diagnostics(resolution);
    size_t i;
    for (i = 0; i < tf_diagnostic_bag_count(bag); ++i) {
        const TfDiagnosticRecord *d = tf_diagnostic_bag_get(bag,i);
        if (strcmp(d->code,code) == 0) return d;
    }
    return NULL;
}
static void transactional_cache(void) {
    TfSourceManager *manager;
    TfSourceManagerMark mark;
    TfSourceId removed;
    const TfManagedSource *pinned, *source;
    TfResolution *resolution;
    TfSource virtual_source;
    TfResolveOptions opts = {NESTED,100};
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    CHECK(tf_source_manager_load(manager,DEP,&pinned) == TF_OK);
    mark = tf_source_manager_mark(manager);
    /* Exercise an extra request alias to an old entry, independently of host
     * symlink privileges, through the resolver's canonical-load handoff. */
    CHECK(tf_source_manager_load_resolved(manager,ALIAS,pinned->canonical_path,&source) == TF_OK);
    CHECK(source == pinned);
    CHECK(tf_source_manager_load(manager,ENTRY,&source) == TF_OK);
    removed = source->id;
    CHECK(tf_source_manager_cached(manager,ALIAS,&source) == TF_OK && source == pinned);
    CHECK(tf_source_manager_count(manager) == 2);
    source = NULL;
    CHECK(tf_source_manager_rollback(manager,mark));
    CHECK(tf_source_manager_count(manager) == 1);
    CHECK(tf_source_manager_lookup(manager,removed) == NULL);
    CHECK(tf_source_manager_cached(manager,ENTRY,&source) == TF_OK && source == NULL);
    CHECK(tf_source_manager_cached(manager,ALIAS,&source) == TF_OK && source == NULL);
    CHECK(tf_source_manager_lookup(manager,pinned->id) == pinned);
    CHECK(remove(DEP) == 0);
    init(&virtual_source,"import \"dep.tfd\" as dep; fn f() -> Int { dep::value() }");
    CHECK(tf_resolve_virtual(&resolution,manager,&virtual_source,NULL,&opts,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_resolution_module_count(resolution) == 2);
    CHECK(tf_resolution_module_at(resolution,1)->source == pinned);
    CHECK(strcmp(tf_resolution_module_at(resolution,0)->relative_path,"<repl>") == 0);
    CHECK(tf_resolution_module_at(resolution,0)->source->id.index == 0);
    CHECK(tf_resolution_module_at(resolution,0)->source->source == &virtual_source);
    CHECK(tf_source_manager_count(manager) == 1);
    CHECK(pinned->source->length == strlen(DEP_SOURCE));
    CHECK(memcmp(pinned->source->bytes,DEP_SOURCE,pinned->source->length) == 0);
    tf_resolution_destroy(resolution);
    tf_source_destroy(&virtual_source);
    tf_source_manager_destroy(manager);
}
static void virtual_diagnostics(void) {
    TfSourceManager *manager;
    TfResolution *resolution;
    TfSource source;
    const TfDiagnosticRecord *d;
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    init(&source,"import \"<repl>\" as self; pub fn main() -> List[Text] { "
                 "output \"entry\"; [] } fn f() -> List[Text] { main() }");
    CHECK(tf_resolve_virtual(&resolution,manager,&source,NULL,NULL,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) == TF_DIAG_PHASE_ERROR);
    CHECK(diagnostic(resolution,"E204") != NULL);
    d = diagnostic(resolution,"E208");
    CHECK(d != NULL && d->label_count == 2);
    CHECK(d->labels[0].span.source == &source);
    CHECK(strcmp(d->labels[0].span.source->name,"<repl>") == 0);
    CHECK(d->labels[1].span.source == &source);
    CHECK(tf_source_manager_count(manager) == 0);
    tf_resolution_destroy(resolution);
    tf_source_destroy(&source);
    tf_source_manager_destroy(manager);
}
static void loaded_overlay(void) {
    TfSourceManager *manager;
    TfResolution *resolution;
    TfSource virtual_source;
    const TfManagedSource *pinned, *sibling, *alias;
    const TfResolvedModule *module;
    TfResolveOptions wide_root = {ROOT,100};
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    CHECK(tf_source_manager_load(manager,ENTRY,&pinned) == TF_OK);
    CHECK(tf_source_manager_load(manager,SIBLING,&sibling) == TF_OK);
    CHECK(tf_source_manager_load_resolved(manager,ALIAS,sibling->canonical_path,&alias) == TF_OK);
    CHECK(alias == sibling);
    init(&virtual_source,"import \"alias.tfd\" as sibling; fn f() -> Int { sibling::value() }");
    CHECK(tf_resolve_virtual(&resolution,manager,&virtual_source,pinned,&wide_root,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) == TF_DIAG_PHASE_VALID);
    module = tf_resolution_module(resolution,tf_resolution_root(resolution));
    CHECK(module != NULL && strcmp(module->relative_path,"nested/root.tfd") == 0);
    CHECK(module->source != pinned && module->source->source == &virtual_source);
    CHECK(strcmp(module->source->canonical_path,pinned->canonical_path) == 0);
    CHECK(memcmp(module->source->sha256,pinned->sha256,sizeof(pinned->sha256)) != 0);
    tf_resolution_destroy(resolution);
    CHECK(tf_resolve_virtual(&resolution,manager,&virtual_source,pinned,NULL,NULL) == TF_OK);
    CHECK(diagnostic(resolution,"E204") != NULL);
    tf_resolution_destroy(resolution);
    tf_source_destroy(&virtual_source);
    write_file(DEP,"import \"root.tfd\" as back; pub fn value() -> Int { 1 }");
    init(&virtual_source,"import \"dep.tfd\" as dep; fn f() -> Int { dep::value() }");
    CHECK(remove(ENTRY) == 0);
    CHECK(tf_resolve_virtual(&resolution,manager,&virtual_source,pinned,&wide_root,NULL) == TF_OK);
    CHECK(diagnostic(resolution,"E205") != NULL);
    CHECK(tf_resolution_module_count(resolution) == 2);
    CHECK(tf_source_manager_count(manager) == 3);
    CHECK(tf_source_manager_lookup(manager,pinned->id) == pinned);
    tf_resolution_destroy(resolution);
    tf_source_destroy(&virtual_source);
    tf_source_manager_destroy(manager);
}

typedef struct { size_t calls, fail_at, live; } AllocationTest;
static void *test_allocate(void *context, size_t size) {
    AllocationTest *test = context;
    void *pointer;
    if (++test->calls == test->fail_at) return NULL;
    pointer = malloc(size);
    CHECK(pointer != NULL);
    ++test->live;
    return pointer;
}
static void test_free(void *context, void *pointer) {
    AllocationTest *test = context;
    CHECK(test->live != 0 && pointer != NULL);
    --test->live;
    free(pointer);
}
static void allocation_failures(void) {
    TfSource source;
    TfResolveOptions opts = {NESTED,100};
    size_t fail_at;
    init(&source,"import \"dep.tfd\" as dep; fn f() -> Int { dep::value() }");
    write_file(DEP,DEP_SOURCE);
    for (fail_at = 1; ; ++fail_at) {
        AllocationTest test = {0,0,0};
        TfAllocator allocator = {&test,test_allocate,test_free};
        TfSourceManager *manager;
        TfSourceManagerMark mark;
        TfResolution *resolution;
        TfStatus status;
        CHECK(tf_source_manager_create(&manager,&allocator) == TF_OK);
        mark = tf_source_manager_mark(manager);
        test.calls = 0;
        test.fail_at = fail_at;
        status = tf_resolve_virtual(&resolution,manager,&source,NULL,&opts,&allocator);
        if (status == TF_HOST_ERROR) CHECK(resolution == NULL && test.calls >= fail_at);
        else {
            CHECK(status == TF_OK && test.calls < fail_at);
            CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) == TF_DIAG_PHASE_VALID);
        }
        tf_resolution_destroy(resolution);
        CHECK(tf_source_manager_rollback(manager,mark));
        CHECK(tf_source_manager_count(manager) == 0);
        tf_source_manager_destroy(manager);
        CHECK(test.live == 0);
        if (status == TF_OK) break;
    }
    tf_source_destroy(&source);
}
int main(void) {
    make_dir(ROOT);
    make_dir(NESTED);
    write_file(ENTRY,"fn original() -> Int { 1 }");
    write_file(DEP,DEP_SOURCE);
    write_file(SIBLING,DEP_SOURCE);
    transactional_cache();
    virtual_diagnostics();
    loaded_overlay();
    allocation_failures();
    CHECK(remove(DEP) == 0);
    CHECK(remove(SIBLING) == 0);
    remove_dir(NESTED);
    remove_dir(ROOT);
    puts("REPL resolver tests passed");
    return EXIT_SUCCESS;
}
