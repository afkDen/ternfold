#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "ternfold/source_manager.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#endif

static size_t checks;
static char directory[4096], relative_directory[256];
static void check(bool okay, const char *expression, int line) {
    ++checks;
    if (!okay) { (void)fprintf(stderr, "test_source_manager.c:%d: %s\n", line, expression); exit(1); }
}
#define CHECK(x) check((x), #x, __LINE__)
static void path_for(char out[4096], const char *name) {
    int n = snprintf(out, 4096, "%s/%s", directory, name);
    CHECK(n > 0 && n < 4096);
}
static void write_bytes(const char *path, const void *bytes, size_t length) {
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(bytes, 1, length, file) == length);
    CHECK(fclose(file) == 0);
}
static void setup(void) {
    int n;
#ifdef _WIN32
    DWORD got;
    n = snprintf(relative_directory, sizeof(relative_directory), "build/tf-sm-%lu-%llu",
                 (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    CHECK(n > 0 && (size_t)n < sizeof(relative_directory));
    CHECK(CreateDirectoryA(relative_directory, NULL) != 0);
    got = GetFullPathNameA(relative_directory, (DWORD)sizeof(directory), directory, NULL);
    CHECK(got > 0 && got < sizeof(directory));
#else
    char cwd[3072];
    n = snprintf(relative_directory, sizeof(relative_directory), "build/tf-sm-%ld-%lld",
                 (long)getpid(), (long long)time(NULL));
    CHECK(n > 0 && (size_t)n < sizeof(relative_directory));
    CHECK(mkdir(relative_directory, 0700) == 0);
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
    n = snprintf(directory, sizeof(directory), "%s/%s", cwd, relative_directory);
    CHECK(n > 0 && (size_t)n < sizeof(directory));
#endif
}
static void test_snapshots(void) {
    TfSourceManager *m, *other;
    const TfManagedSource *first, *again, *second;
    char path[4096], relative[4096], dot[4096], canonical[4096];
    TfSpan borrowed;
    unsigned char digest[32];
    path_for(path, "snapshot.any");
    write_bytes(path, "old", 3);
    CHECK(tf_source_manager_create(&m, NULL) == TF_OK);
    CHECK(tf_source_manager_create(&other, NULL) == TF_OK);
    CHECK(tf_source_manager_cached(m, path, &again) == TF_OK && again == NULL);
    CHECK(tf_source_manager_count(m) == 0);
    CHECK(tf_source_manager_load(m, path, &first) == TF_OK);
    CHECK(first->source->length == 3);
    CHECK(first->status == TF_OK && first->id.index != 0);
    CHECK(tf_source_span(first->source, 0, 3, &borrowed));
    memcpy(digest, first->sha256, sizeof(digest));
    CHECK(strlen(first->canonical_path) < sizeof(canonical));
    (void)strcpy(canonical, first->canonical_path);
    CHECK(snprintf(relative, sizeof(relative), "%s/snapshot.any", relative_directory) > 0);
    CHECK(snprintf(dot, sizeof(dot), "%s/./snapshot.any", directory) > 0);
    CHECK(tf_source_manager_load(m, relative, &again) == TF_OK && again == first);
    CHECK(tf_source_manager_load(m, dot, &again) == TF_OK && again == first);
    CHECK(tf_source_manager_load(other, path, &second) == TF_OK);
    CHECK(tf_source_manager_lookup(m, first->id) == first);
    CHECK(tf_source_manager_lookup(other, first->id) == NULL);
    CHECK(tf_source_manager_lookup(m, second->id) == NULL);
    CHECK(tf_source_manager_lookup(m, (TfSourceId){0}) == NULL);
    CHECK(tf_source_manager_lookup(m, (TfSourceId){m, SIZE_MAX}) == NULL);
    CHECK(tf_source_manager_lookup(NULL, first->id) == NULL);
    write_bytes(path, "new content", 11);
    CHECK(tf_source_manager_load(m, path, &again) == TF_OK && again == first);
    CHECK(first->source->length == 3 && memcmp(first->source->bytes, "old", 3) == 0);
    CHECK(remove(path) == 0);
    CHECK(tf_source_manager_cached(m, relative, &again) == TF_OK && again == first);
    CHECK(tf_source_manager_cached(m, dot, &again) == TF_OK && again == first);
    CHECK(tf_source_manager_load(m, path, &again) == TF_OK && again == first);
    CHECK(tf_source_manager_load(m, relative, &again) == TF_OK && again == first);
    CHECK(tf_source_manager_load(m, dot, &again) == TF_OK && again == first);
    CHECK(tf_source_manager_load(m, canonical, &again) == TF_OK && again == first);
    CHECK(memcmp(digest, again->sha256, 32) == 0);
    CHECK(borrowed.source == first->source && memcmp(borrowed.source->bytes, "old", 3) == 0);
    CHECK(tf_source_manager_count(m) == 1);
    tf_source_manager_destroy(other);
    tf_source_manager_destroy(m);
}
static void test_invalid(void) {
    static const unsigned char invalid[][4] = {{0xff,0,0,0}, {0xef,0xbb,0xbf,0}, {'x','\r',0,0}};
    static const size_t lengths[] = {1,3,2};
    static const TfError codes[] = {TF_E001,TF_E002,TF_E003};
    TfSourceManager *m;
    size_t i;
    CHECK(tf_source_manager_create(&m, NULL) == TF_OK);
    for (i = 0; i < 3; ++i) {
        const TfManagedSource *entry, *again;
        char path[4096], name[64];
        CHECK(snprintf(name, sizeof(name), "invalid-%zu", i) > 0);
        path_for(path, name);
        write_bytes(path, invalid[i], lengths[i]);
        CHECK(tf_source_manager_load(m, path, &entry) == TF_SOURCE_ERROR);
        CHECK(!entry->source->valid && entry->status == TF_SOURCE_ERROR);
        CHECK(entry->source->error.code == codes[i]);
        CHECK(entry->source->error.span.source == entry->source);
        CHECK(entry->source->length == lengths[i]);
        CHECK(memcmp(entry->source->bytes, invalid[i], lengths[i]) == 0);
        write_bytes(path, "valid", 5);
        CHECK(tf_source_manager_load(m, path, &again) == TF_SOURCE_ERROR && again == entry);
        CHECK(remove(path) == 0);
        CHECK(tf_source_manager_cached(m, path, &again) == TF_SOURCE_ERROR && again == entry);
        CHECK(tf_source_manager_load(m, path, &again) == TF_SOURCE_ERROR && again == entry);
        CHECK(tf_source_manager_lookup(m, entry->id) == entry);
    }
    CHECK(tf_source_manager_count(m) == 3);
    tf_source_manager_destroy(m);
}
static void test_growth(void) {
    TfSourceManager *m;
    const TfManagedSource *entries[96], *again;
    TfSpan spans[96];
    size_t i;
    char path[4096], name[64];
    CHECK(tf_source_manager_create(&m, NULL) == TF_OK);
    for (i = 0; i < 96; ++i) {
        CHECK(snprintf(name, sizeof(name), "growth-%zu", i) > 0);
        path_for(path, name);
        write_bytes(path, "abc", 3);
        CHECK(tf_source_manager_load(m, path, &entries[i]) == TF_OK);
        CHECK(entries[i]->id.index == i + 1);
        CHECK(tf_source_span(entries[i]->source, 0, 3, &spans[i]));
        CHECK(remove(path) == 0);
    }
    CHECK(tf_source_manager_count(m) == 96);
    for (i = 0; i < 96; ++i) {
        CHECK(tf_source_manager_lookup(m, entries[i]->id) == entries[i]);
        CHECK(spans[i].source == entries[i]->source);
        CHECK(memcmp(spans[i].source->bytes, "abc", 3) == 0);
        CHECK(snprintf(name, sizeof(name), "growth-%zu", i) > 0);
        path_for(path, name);
        CHECK(tf_source_manager_load(m, path, &again) == TF_OK && again == entries[i]);
        if (i != 0) CHECK(!tf_span_join(spans[0], spans[i], &spans[i]));
    }
    tf_source_manager_destroy(m);
}
static void test_digest(void) {
    static const struct { const char *text, *hex; } vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
         "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"}
    };
    TfSourceManager *m;
    size_t i, j;
    char path[4096], actual[65];
    unsigned char *million = malloc(1000000);
    CHECK(million != NULL);
    CHECK(tf_source_manager_create(&m, NULL) == TF_OK);
    for (i = 0; i <= sizeof(vectors)/sizeof(vectors[0]); ++i) {
        const TfManagedSource *entry;
        char name[64];
        const char *expected;
        CHECK(snprintf(name, sizeof(name), "digest-%zu", i) > 0);
        path_for(path, name);
        if (i < sizeof(vectors)/sizeof(vectors[0])) {
            write_bytes(path, vectors[i].text, strlen(vectors[i].text)); expected = vectors[i].hex;
        } else {
            memset(million, 'a', 1000000);
            write_bytes(path, million, 1000000);
            expected = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
        }
        CHECK(tf_source_manager_load(m, path, &entry) == TF_OK);
        for (j = 0; j < 32; ++j) CHECK(snprintf(actual + 2*j, 3, "%02x", (unsigned)entry->sha256[j]) == 2);
        CHECK(strcmp(actual, expected) == 0);
        CHECK(remove(path) == 0);
    }
    tf_source_manager_destroy(m);
    free(million);
}

typedef struct { size_t calls, fail_at, live; } Memory;
static void *allocate(void *context, size_t size) {
    Memory *memory = context;
    void *p;
    ++memory->calls;
    if (memory->calls == memory->fail_at) return NULL;
    p = malloc(size);
    if (p != NULL) ++memory->live;
    return p;
}
static void deallocate(void *context, void *p) {
    Memory *memory = context;
    CHECK(p != NULL && memory->live != 0);
    --memory->live;
    free(p);
}
static void test_failures(void) {
    size_t scenario, position;
    char path[4096], prior_path[4096];
    static const unsigned char bad[] = {0xef,0xbb,0xbf};
    unsigned char bytes[9000];
    memset(bytes, 'a', sizeof(bytes));
    path_for(path, "allocation");
    path_for(prior_path, "prior");
    write_bytes(prior_path, "prior", 5);
    for (scenario = 0; scenario < 4; ++scenario) {
        bool reached_success = false;
        if (scenario == 1) write_bytes(path, bad, sizeof(bad));
        else write_bytes(path, bytes, sizeof(bytes));
        for (position = 1; position < 80; ++position) {
            Memory memory = {0};
            TfAllocator a = {&memory, allocate, deallocate};
            TfSourceManager *m;
            const TfManagedSource *prior = NULL, *out = NULL;
            TfStatus status;
            size_t before;
            CHECK(tf_source_manager_create(&m, &a) == TF_OK);
            if (scenario != 0) CHECK(tf_source_manager_load(m, prior_path, &prior) == TF_OK);
            if (scenario == 3) {
                size_t i;
                /* Sixteen entries exhaust both entry and alias capacities. */
                for (i = 1; i < 16; ++i) {
                    char name[64], extra[4096];
                    CHECK(snprintf(name, sizeof(name), "seed-%zu", i) > 0);
                    path_for(extra, name); write_bytes(extra, "seed", 4);
                    CHECK(tf_source_manager_load(m, extra, &out) == TF_OK);
                    CHECK(remove(extra) == 0);
                }
            }
            before = tf_source_manager_count(m);
            memory.fail_at = memory.calls + position;
            status = tf_source_manager_load(m, path, &out);
            if (status == TF_HOST_ERROR) {
                CHECK(out == NULL && tf_source_manager_count(m) == before);
                if (prior != NULL) {
                    CHECK(tf_source_manager_lookup(m, prior->id) == prior);
                    CHECK(memcmp(prior->source->bytes, "prior", 5) == 0);
                }
                /* Retry observes new disk data: failed loads publish no alias. */
                memory.fail_at = 0;
                write_bytes(path, "retry", 5);
                CHECK(tf_source_manager_load(m, path, &out) == TF_OK);
                CHECK(out->source->length == 5 && memcmp(out->source->bytes, "retry", 5) == 0);
                CHECK(tf_source_manager_count(m) == before + 1);
                if (scenario == 1) write_bytes(path, bad, sizeof(bad));
                else write_bytes(path, bytes, sizeof(bytes));
            } else {
                CHECK(status == (scenario == 1 ? TF_SOURCE_ERROR : TF_OK));
                CHECK(out != NULL && tf_source_manager_count(m) == before + 1);
                reached_success = true;
            }
            tf_source_manager_destroy(m);
            CHECK(memory.live == 0);
            if (reached_success) break;
        }
        CHECK(reached_success);
    }
    CHECK(remove(path) == 0); CHECK(remove(prior_path) == 0);
    {
        Memory memory = {0,1,0};
        TfAllocator a = {&memory, allocate, deallocate};
        TfSourceManager *m = NULL;
        CHECK(tf_source_manager_create(&m, &a) == TF_HOST_ERROR && m == NULL);
        CHECK(memory.live == 0);
        a.allocate = NULL;
        CHECK(tf_source_manager_create(&m, &a) == TF_HOST_ERROR && m == NULL);
    }
}
static void test_host_errors(void) {
    TfSourceManager *m;
    const TfManagedSource *out = NULL;
    char path[4096];
    CHECK(tf_source_manager_create(&m, NULL) == TF_OK);
    path_for(path, "missing");
    CHECK(tf_source_manager_load(m, path, &out) == TF_HOST_ERROR && out == NULL);
    CHECK(tf_source_manager_load(m, directory, &out) == TF_HOST_ERROR && out == NULL);
    CHECK(tf_source_manager_count(m) == 0);
    CHECK(tf_source_manager_load(NULL, path, &out) == TF_HOST_ERROR && out == NULL);
    CHECK(tf_source_manager_load(m, NULL, &out) == TF_HOST_ERROR && out == NULL);
    CHECK(tf_source_manager_load(m, "", &out) == TF_HOST_ERROR && out == NULL);
    CHECK(tf_source_manager_load(m, path, NULL) == TF_HOST_ERROR);
    CHECK(tf_source_manager_create(NULL, NULL) == TF_HOST_ERROR);
    CHECK(tf_source_manager_count(NULL) == 0);
    write_bytes(path, "now exists", 10);
    CHECK(tf_source_manager_load(m, path, &out) == TF_OK);
    CHECK(remove(path) == 0);
    tf_source_manager_destroy(m);
    tf_source_manager_destroy(NULL);
}

static void test_path_aliases(void) {
    char first_path[4096], second_path[4096], alias[4096];
    TfSourceManager *m;
    const TfManagedSource *first, *second, *linked, *again;
    path_for(first_path, "link-first"); path_for(second_path, "link-second");
    path_for(alias, "link-alias");
    write_bytes(first_path, "first", 5); write_bytes(second_path, "second", 6);
#ifdef _WIN32
    CHECK(CreateHardLinkA(alias, first_path, NULL) != 0);
#else
    CHECK(link(first_path, alias) == 0);
#endif
    CHECK(tf_source_manager_create(&m, NULL) == TF_OK);
    CHECK(tf_source_manager_load(m, first_path, &first) == TF_OK);
    CHECK(tf_source_manager_load(m, second_path, &second) == TF_OK);
    CHECK(tf_source_manager_load(m, alias, &linked) == TF_OK);
    CHECK(linked != first && linked != second); /* canonical paths, not inodes */
    CHECK(memcmp(linked->sha256, first->sha256, 32) == 0);
    CHECK(remove(alias) == 0);
#ifdef _WIN32
    CHECK(CreateHardLinkA(alias, second_path, NULL) != 0);
#else
    CHECK(link(second_path, alias) == 0);
#endif
    CHECK(tf_source_manager_load(m, alias, &again) == TF_OK && again == linked);
    CHECK(again->source->length == 5 && memcmp(again->source->bytes, "first", 5) == 0);
    CHECK(remove(alias) == 0); CHECK(remove(first_path) == 0); CHECK(remove(second_path) == 0);
    tf_source_manager_destroy(m);
#ifndef _WIN32
    {
        char side[4096], target[4096], deep[4096], symlink_path[4096], traversal[4096];
        path_for(side, "side"); path_for(target, "target"); path_for(deep, "target/deep");
        path_for(symlink_path, "side/link");
        CHECK(mkdir(side, 0700) == 0); CHECK(mkdir(target, 0700) == 0); CHECK(mkdir(deep, 0700) == 0);
        path_for(first_path, "side/file"); path_for(second_path, "target/file");
        write_bytes(first_path, "wrong", 5); write_bytes(second_path, "right", 5);
        CHECK(symlink(deep, symlink_path) == 0);
        CHECK(tf_source_manager_create(&m, NULL) == TF_OK);
        path_for(traversal, "side/link/../file");
        CHECK(tf_source_manager_load(m, traversal, &first) == TF_OK);
        CHECK(memcmp(first->source->bytes, "right", 5) == 0);
        /* Canonical spelling was cached even though first load used a symlink. */
        CHECK(remove(second_path) == 0);
        CHECK(tf_source_manager_load(m, first->canonical_path, &again) == TF_OK && again == first);
        CHECK(remove(symlink_path) == 0);
        CHECK(symlink(side, symlink_path) == 0);
        CHECK(tf_source_manager_load(m, traversal, &again) == TF_OK && again == first);
        path_for(traversal, "side/nonexistent/../file");
        CHECK(tf_source_manager_load(m, traversal, &again) == TF_HOST_ERROR && again == NULL);
        CHECK(tf_source_manager_load(m, first_path, &second) == TF_OK);
        path_for(traversal, "side/file/.");
        CHECK(tf_source_manager_load(m, traversal, &again) == TF_HOST_ERROR && again == NULL);
        path_for(traversal, "side/file/");
        CHECK(tf_source_manager_load(m, traversal, &again) == TF_HOST_ERROR && again == NULL);
        CHECK(remove(symlink_path) == 0); CHECK(remove(first_path) == 0);
        CHECK(rmdir(deep) == 0); CHECK(rmdir(target) == 0); CHECK(rmdir(side) == 0);
        tf_source_manager_destroy(m);
    }
#endif
}

#ifdef _WIN32
static void test_case_rename(void) {
    char lower[4096], upper[4096];
    TfSourceManager *m;
    const TfManagedSource *first, *again;
    unsigned char digest[32];
    TfSpan span;
    path_for(lower, "rename-case.tfd"); path_for(upper, "RENAME-CASE.TFD");
    write_bytes(lower, "abc", 3);
    CHECK(tf_source_manager_create(&m, NULL) == TF_OK);
    CHECK(tf_source_manager_load(m, lower, &first) == TF_OK);
    CHECK(tf_source_span(first->source, 0, 3, &span));
    memcpy(digest, first->sha256, sizeof(digest));
    CHECK(MoveFileExA(lower, upper, 0) != 0);
    write_bytes(upper, "new bytes", 9);
    CHECK(tf_source_manager_load(m, upper, &again) == TF_OK);
    CHECK(again == first);
    CHECK(tf_source_manager_count(m) == 1);
    CHECK(again->source->length == 3 && memcmp(again->source->bytes, "abc", 3) == 0);
    CHECK(memcmp(again->sha256, digest, sizeof(digest)) == 0);
    CHECK(tf_source_manager_lookup(m, again->id) == first);
    CHECK(span.source == again->source);
    CHECK(remove(upper) == 0);
    CHECK(tf_source_manager_load(m, upper, &again) == TF_OK && again == first);
    tf_source_manager_destroy(m);
}
static void test_case_replacement(void) {
    char lower[4096], upper[4096], parked[4096];
    TfSourceManager *m;
    const TfManagedSource *first, *second, *again;
    TfSource replacement;
    unsigned char digest[32];
    path_for(lower, "distinct-case.tfd"); path_for(upper, "DISTINCT-CASE.TFD");
    path_for(parked, "parked-case.tfd");
    write_bytes(lower, "first", 5);
    CHECK(tf_source_manager_create(&m, NULL) == TF_OK);
    CHECK(tf_source_manager_load(m, lower, &first) == TF_OK);
    memcpy(digest, first->sha256, sizeof(digest));
    /* Keep the first file alive to prevent native file-ID recycling, then
     * create a distinct file whose canonical spelling differs only in case. */
    CHECK(MoveFileExA(lower, parked, 0) != 0);
    write_bytes(upper, "second", 6);
    CHECK(tf_source_load(&replacement, upper, NULL) == TF_OK);
    CHECK(first->source->origin.device != replacement.origin.device ||
          first->source->origin.index != replacement.origin.index);
    tf_source_destroy(&replacement);
    CHECK(tf_source_manager_load(m, upper, &second) == TF_OK);
    /* A case-insensitive canonical module path retains its snapshot even if
     * replacement changed file identity. File IDs do not identify modules. */
    CHECK(second == first && tf_source_manager_count(m) == 1);
    CHECK(second->source->length == 5 && memcmp(second->source->bytes, "first", 5) == 0);
    CHECK(memcmp(second->sha256, digest, sizeof(digest)) == 0);
    CHECK(tf_source_manager_lookup(m, second->id) == first);
    CHECK(tf_source_manager_load(m, lower, &again) == TF_OK && again == first);
    CHECK(remove(upper) == 0); CHECK(remove(parked) == 0);
    CHECK(tf_source_manager_load(m, upper, &again) == TF_OK && again == first);
    tf_source_manager_destroy(m);
}
#endif

static void test_coordinate_properties(void) {
    static const struct { unsigned char bytes[4]; size_t length, scalars; } pieces[] = {
        {{'A',0,0,0},1,1}, {{0xc2,0xa2,0,0},2,1}, {{0xe2,0x82,0xac,0},3,1},
        {{0xf0,0x9f,0x99,0x82},4,1}, {{'\t',0,0,0},1,1}, {{'\r','\n',0,0},2,2}
    };
    uint32_t state = UINT32_C(0x51a7c0de);
    size_t sample;
    TfSource other;
    CHECK(tf_source_init(&other, "other", "x", 1, NULL) == TF_OK);
    for (sample = 0; sample < 1000; ++sample) {
        unsigned char bytes[256];
        TfLocation expected[257], location;
        bool boundary[257] = {false};
        size_t length = 0, scalar = 0, line = 1, column = 1, display = 1, i, count;
        TfSource source;
        TfSpan whole, joined;
        /* Every nonempty case contains ASCII, 2/3/4-byte scalars, TAB, CRLF;
         * then a deterministic sequence in varying orders. Case zero is EOF. */
        count = sample == 0 ? 0 : 6 + sample % 27;
        for (i = 0; i < count; ++i) {
            size_t choice, part;
            state = state * UINT32_C(1664525) + UINT32_C(1013904223);
            choice = i < 6 ? (i + sample) % 6 : (size_t)(state % 6u);
            for (part = 0; part < pieces[choice].scalars; ++part) {
                size_t width = pieces[choice].scalars == 2 ? 1 : pieces[choice].length;
                unsigned char lead = pieces[choice].bytes[part];
                expected[scalar] = (TfLocation){length,scalar,line,column,display};
                boundary[length] = true;
                memcpy(bytes + length, pieces[choice].bytes + part, width);
                length += width; ++scalar;
                if (lead == '\n') { ++line; column = 1; display = 1; }
                else { ++column; display += lead == '\t' ? 4 - (display - 1) % 4 : 1; }
            }
        }
        boundary[length] = true;
        expected[scalar] = (TfLocation){length,scalar,line,column,display};
        CHECK(tf_source_init(&source, "generated", bytes, length, NULL) == TF_OK);
        CHECK(tf_source_span(&source, 0, length, &whole));
        for (i = 0; i <= scalar; ++i) {
            size_t byte;
            TfSpan point;
            CHECK(tf_source_location(&source, expected[i].byte, &location));
            CHECK(location.byte == expected[i].byte && location.scalar == i &&
                  location.line == expected[i].line && location.column == expected[i].column &&
                  location.display_column == expected[i].display_column);
            CHECK(tf_source_byte(&source, location.line, location.column, &byte));
            CHECK(byte == expected[i].byte);
            CHECK(tf_source_span(&source, byte, byte, &point));
            CHECK(tf_span_join(point, whole, &joined) && joined.start == 0 && joined.end == length);
        }
        for (i = 0; i < length; ++i) if (!boundary[i]) {
            TfSpan forged = {&source,i,i};
            CHECK(!tf_source_location(&source, i, &location));
            CHECK(!tf_source_span(&source, i, i, &joined));
            CHECK(!tf_span_join(whole, forged, &joined));
            CHECK(!tf_span_join(forged, whole, &joined));
        }
        CHECK(!tf_source_location(&source, length + 1, &location));
        CHECK(!tf_span_join(whole, (TfSpan){&source,0,length + 1}, &joined));
        CHECK(!tf_span_join((TfSpan){&source,length + 1,length + 1}, whole, &joined));
        CHECK(!tf_span_join(whole, (TfSpan){&source,SIZE_MAX,SIZE_MAX}, &joined));
        CHECK(!tf_span_join(whole, (TfSpan){NULL,0,0}, &joined));
        CHECK(!tf_span_join(whole, (TfSpan){&other,0,1}, &joined));
        CHECK(!tf_span_join(whole, (TfSpan){&source,1,0}, &joined));
        tf_source_destroy(&source);
    }
    tf_source_destroy(&other);
}

int main(void) {
    setup();
    test_snapshots(); test_invalid(); test_growth(); test_digest();
    test_failures(); test_host_errors(); test_path_aliases();
#ifdef _WIN32
    test_case_rename();
    test_case_replacement();
#endif
    test_coordinate_properties();
#ifdef _WIN32
    CHECK(RemoveDirectoryA(directory) != 0);
#else
    CHECK(rmdir(directory) == 0);
#endif
    (void)printf("source manager: %zu checks passed\n", checks);
    return 0;
}
