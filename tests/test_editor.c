#include "ternfold/editor.h"
#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(e) do { ++checks; if (!(e)) { fprintf(stderr,"editor:%d: %s\n",__LINE__,#e); exit(1); } } while (0)
typedef struct { size_t calls, fail, live; } Fault;
static void *allocate(void *c, size_t n) {
    Fault *f = c;
    if (f->calls++ == f->fail) return NULL;
    void *p = malloc(n);
    if (p != NULL) ++f->live;
    return p;
}
static void deallocate(void *c, void *p) {
    Fault *f = c;
    if (p != NULL) { CHECK(f->live != 0); --f->live; free(p); }
}
static void frame(FILE *f, const char *s) {
    size_t n = strlen(s);
    CHECK(fprintf(f, "Content-Length: %zu\r\n\r\n", n) > 0);
    CHECK(fwrite(s, 1, n, f) == n);
}
static char *read_all(FILE *f) {
    long n;
    CHECK(fflush(f) == 0 && fseek(f, 0, SEEK_END) == 0);
    n = ftell(f); CHECK(n >= 0 && n < 1048576);
    char *s = malloc((size_t)n + 1); CHECK(s != NULL);
    rewind(f); CHECK(fread(s, 1, (size_t)n, f) == (size_t)n); s[n] = 0;
    return s;
}
static void coordinates(void) {
    const char text[] = "a\xf0\x9f\x99\x82\t\xc3\xb1\r\nz\n";
    TfSource s = {0}; TfEditorPosition p; size_t byte;
    CHECK(tf_source_init(&s, "unicode", text, sizeof text - 1, NULL) == TF_OK);
    CHECK(tf_editor_position(&s, 5, &p) && p.line == 0 && p.character == 3);
    CHECK(tf_editor_position(&s, 6, &p) && p.character == 4);
    CHECK(tf_editor_byte(&s, (TfEditorPosition){0, 5}, &byte) && byte == 8);
    CHECK(!tf_editor_byte(&s, (TfEditorPosition){0, 2}, &byte));
    CHECK(!tf_editor_position(&s, 2, &p));
    CHECK(!tf_editor_position(&s, 9, &p)); /* CRLF interior */
    CHECK(!tf_editor_byte(&s, (TfEditorPosition){0, 6}, &byte));
    CHECK(tf_editor_byte(&s, (TfEditorPosition){1, 0}, &byte) && byte == 10);
    CHECK(tf_editor_byte(&s, (TfEditorPosition){2, 0}, &byte) && byte == sizeof text - 1);
    CHECK(!tf_editor_byte(&s, (TfEditorPosition){3, 0}, &byte));
    for (size_t i = 0; i <= s.length; ++i) {
        if (tf_editor_position(&s, i, &p)) CHECK(tf_editor_byte(&s, p, &byte) && byte == i);
    }
    tf_source_destroy(&s);
}
static void snapshots(void) {
    const char *text = "fn f(x: Int) -> Int { x + 1 }";
    TfEditorSnapshot *s = NULL, *next = NULL;
    TfEditorRange range;
    CHECK(tf_editor_snapshot_create(&s, "doc.tfd", text, strlen(text), 1, ".", NULL) == TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_editor_diagnostics(s, 1)));
    TfConcreteTypeId t = tf_editor_hover(s, 1, (TfEditorPosition){0, 22}, &range);
    CHECK(t.owner != NULL && tf_type_store_get(tf_editor_types(s), t)->kind == TF_CONCRETE_INT);
    CHECK(tf_editor_hover(s, 2, (TfEditorPosition){0, 22}, &range).owner == NULL);
    CHECK(tf_editor_diagnostics(s, 2) == NULL);
    const char *bad = "fn f() -> Int { true }";
    CHECK(tf_editor_snapshot_create(&next, "doc.tfd", bad, strlen(bad), 2, ".", NULL) == TF_OK);
    CHECK(tf_diagnostic_bag_has_errors(tf_editor_diagnostics(next, 2)));
    CHECK(tf_editor_hover(next, 2, (TfEditorPosition){0, 16}, &range).owner == NULL);
    CHECK(memcmp(tf_editor_source(s)->bytes, text, strlen(text)) == 0);
    CHECK(tf_editor_diagnostics(s, 1) != NULL);
    tf_editor_cancel(s); CHECK(tf_editor_diagnostics(s, 1) == NULL);
    CHECK(tf_editor_hover(s, 1, (TfEditorPosition){0, 22}, &range).owner == NULL);
    tf_editor_snapshot_destroy(next); tf_editor_snapshot_destroy(s);
    const char unicode[] = "fn f()->Int{let s=\"\xf0\x9f\x99\x82\"; 7}";
    CHECK(tf_editor_snapshot_create(&s, "unicode.tfd", unicode, sizeof unicode - 1, 9, ".", NULL) == TF_OK);
    TfEditorPosition p;
    CHECK(tf_editor_position(tf_editor_source(s), (size_t)(strstr(unicode, "7}") - unicode), &p));
    CHECK(p.character == 24); /* astral scalar takes two UTF-16 units */
    t = tf_editor_hover(s, 9, p, &range);
    CHECK(t.owner != NULL && tf_type_store_get(tf_editor_types(s), t)->kind == TF_CONCRETE_INT);
    CHECK(range.start.character == 24 && range.end.character == 25);
    tf_editor_snapshot_destroy(s);
    static const char *const bads[] = {
        "fn", "fn f()->Int{unknown}", "fn f()->Int{true}",
        "machine M(i:Int)yields Int rejects Text{state n:Int=0;next n=next.n;}",
        "import \"missing-editor-fixture.tfd\" as q;", "\xff"
    };
    for (size_t i = 0; i < sizeof bads / sizeof bads[0]; ++i) {
        CHECK(tf_editor_snapshot_create(&s, "doc.tfd", bads[i], strlen(bads[i]), 0, ".", NULL) == TF_OK);
        CHECK(tf_diagnostic_bag_has_errors(tf_editor_diagnostics(s, 0)));
        tf_editor_snapshot_destroy(s);
    }
    Fault baseline = {0, SIZE_MAX, 0}; TfAllocator a = {&baseline, allocate, deallocate};
    CHECK(tf_editor_snapshot_create(&s, "oom.tfd", text, strlen(text), 0, ".", &a) == TF_OK);
    tf_editor_snapshot_destroy(s); CHECK(baseline.live == 0);
    for (size_t i = 0; i < baseline.calls; ++i) {
        Fault f = {0, i, 0}; a.context = &f;
        CHECK(tf_editor_snapshot_create(&s, "oom.tfd", text, strlen(text), 0, ".", &a) == TF_HOST_ERROR);
        CHECK(s == NULL && f.live == 0);
    }
    printf("editor snapshot allocation failpoints: %zu\n", baseline.calls);
}
static void protocol_allocations(void) {
    FILE *in = tmpfile(), *out = tmpfile(); CHECK(in != NULL && out != NULL);
    frame(in, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    frame(in, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file:///oom.tfd\",\"version\":1,\"text\":\"fn f()->Int{7}\"}}}");
    rewind(in);
    Fault baseline = {0, SIZE_MAX, 0}; TfAllocator a = {&baseline, allocate, deallocate};
    CHECK(tf_editor_serve(in, out, ".", &a) == TF_OK && baseline.live == 0);
    CHECK(fclose(out) == 0);
    for (size_t i = 0; i < baseline.calls; ++i) {
        Fault f = {0, i, 0}; a.context = &f;
        rewind(in); out = tmpfile(); CHECK(out != NULL);
        CHECK(tf_editor_serve(in, out, ".", &a) == TF_HOST_ERROR);
        CHECK(f.live == 0); CHECK(fclose(out) == 0);
    }
    CHECK(fclose(in) == 0);
    printf("editor protocol allocation failpoints: %zu\n", baseline.calls);
}
static void transcript(void) {
    FILE *in = tmpfile(), *out = tmpfile(); CHECK(in != NULL && out != NULL);
    frame(in, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
    frame(in, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file:///doc.tfd\",\"version\":1,\"text\":\"fn f()->Int{true}\"}}}");
    frame(in, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"file:///doc.tfd\",\"version\":2},\"contentChanges\":[{\"text\":\"fn f()->Int{7}\"}]}}");
    frame(in, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"file:///doc.tfd\",\"version\":1},\"contentChanges\":[{\"text\":\"fn broken\"}]}}");
    frame(in, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"file:///doc.tfd\"},\"position\":{\"line\":0,\"character\":12}}}");
    frame(in, "{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\",\"params\":{\"id\":\"cancelled\"}}");
    frame(in, "{\"jsonrpc\":\"2.0\",\"id\":\"cancelled\",\"method\":\"textDocument/hover\",\"params\":{}}");
    frame(in, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"unknown\"}");
    frame(in, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\",\"params\":{\"textDocument\":{\"uri\":\"file:///doc.tfd\"}}}");
    frame(in, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"shutdown\"}");
    frame(in, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
    rewind(in); CHECK(tf_editor_serve(in, out, ".", NULL) == TF_OK);
    char *s = read_all(out);
    CHECK(strstr(s, "\"hoverProvider\":true") != NULL);
    CHECK(strstr(s, "\"version\":1,\"diagnostics\":[{\"range\":") != NULL);
    CHECK(strstr(s, "\"version\":2,\"diagnostics\":[]") != NULL);
    CHECK(strstr(s, "\"value\":\"Int\"") != NULL);
    CHECK(strstr(s, "\"id\":\"cancelled\",\"error\":{\"code\":-32800") != NULL);
    CHECK(strstr(s, "\"code\":-32601") != NULL);
    const char *p = s; size_t publishes = 0;
    while ((p = strstr(p, "textDocument/publishDiagnostics")) != NULL) { ++publishes; ++p; }
    CHECK(publishes == 3); /* opening, current version, close; no stale revision */
    free(s); CHECK(fclose(in) == 0 && fclose(out) == 0);
}
static void malformed(void) {
    static const char *const jsons[] = {
        "{", "[]", "{\"jsonrpc\":\"2.0\",\"id\":1,\"id\":2}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"\\ud800\"}", "{\"n\":01}",
        "{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"initialize\"}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"x\"}x"
    };
    for (size_t i = 0; i < sizeof jsons / sizeof jsons[0]; ++i) {
        FILE *in = tmpfile(), *out = tmpfile(); CHECK(in != NULL && out != NULL);
        frame(in, jsons[i]); rewind(in);
        CHECK(tf_editor_serve(in, out, ".", NULL) == TF_OK);
        char *s = read_all(out); CHECK(strstr(s, "\"error\":") != NULL); free(s);
        CHECK(fclose(in) == 0 && fclose(out) == 0);
    }
    static const char *const frames[] = {
        "Content-Length: 1048577\r\n\r\n", "Content-Length: 1\r\nContent-Length: 1\r\n\r\nx",
        "Content-Length: 20\r\n\r\n{}", "Content-Length: -1\r\n\r\n", "Content-Length: 1\n\nx", "Oops\r\n\r\n"
    };
    for (size_t i = 0; i < sizeof frames / sizeof frames[0]; ++i) {
        FILE *in = tmpfile(), *out = tmpfile(); CHECK(in != NULL && out != NULL);
        CHECK(fputs(frames[i], in) >= 0); rewind(in);
        CHECK(tf_editor_serve(in, out, ".", NULL) == TF_HOST_ERROR);
        CHECK(fclose(in) == 0 && fclose(out) == 0);
    }
    for (size_t i = 0; i < 3; ++i) {
        FILE *in = tmpfile(), *out = tmpfile(); CHECK(in != NULL && out != NULL);
        Fault f = {0, i, 0}; TfAllocator a = {&f, allocate, deallocate};
        CHECK(tf_editor_serve(in, out, ".", &a) == TF_HOST_ERROR && f.live == 0);
        CHECK(fclose(in) == 0 && fclose(out) == 0);
    }
}
int main(void) {
    coordinates(); snapshots(); transcript(); malformed(); protocol_allocations();
    printf("PASS editor: %zu checks\n", checks);
    return 0;
}
