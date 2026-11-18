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
static void reviewed_protocol_regressions(void) {
    FILE *in=tmpfile(),*out=tmpfile(); CHECK(in && out);
    frame(in,"{\r\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    char type[256]="Int", next[256], source[768], message[2048];
    for (size_t i=0;i<40;++i) {
        CHECK(snprintf(next,sizeof next,"List[%s]",type)>0);
        CHECK(strlen(next)<sizeof type); strcpy(type,next);
    }
    CHECK(snprintf(source,sizeof source,"fn f(x: %s) -> %s { x }",type,type)>0);
    CHECK(snprintf(message,sizeof message,
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{"
        "\"uri\":\"file:///deep.tfd\",\"version\":-2,\"text\":\"%s\"}}}",source)>0);
    frame(in,message);
    CHECK(snprintf(message,sizeof message,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/hover\",\"params\":{"
        "\"textDocument\":{\"uri\":\"file:///deep.tfd\"},\"position\":{\"line\":0,\"character\":%zu}}}",
        strlen(source)-3)>0);
    frame(in,message);
    frame(in,"{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{"
        "\"textDocument\":{\"uri\":\"file:///deep.tfd\",\"version\":-1},"
        "\"contentChanges\":[{\"text\":\"fn f()->Int{true}\"},{\"text\":\"fn f()->Int{9}\"}]}}");
    frame(in,"{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\",\"params\":{\"id\":\"a\"}}");
    frame(in,"{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\",\"params\":{\"id\":\"b\"}}");
    frame(in,"{\"jsonrpc\":\"2.0\",\"id\":\"a\",\"method\":\"textDocument/hover\",\"params\":{}}");
    frame(in,"{\"jsonrpc\":\"2.0\",\"id\":\"b\",\"method\":\"textDocument/hover\",\"params\":{}}");
    frame(in,"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/hover\",\"params\":{"
        "\"textDocument\":{\"uri\":\"file:///deep.tfd\"},\"position\":{\"line\":0,\"character\":12}}}");
    rewind(in); CHECK(tf_editor_serve(in,out,".",NULL)==TF_OK);
    char *result=read_all(out);
    CHECK(strstr(result,"\"version\":-2,\"diagnostics\":[]"));
    CHECK(strstr(result,"\"version\":-1,\"diagnostics\":[]"));
    CHECK(strstr(result,"...") && strstr(result,"\"id\":5,\"result\":"));
    CHECK(strstr(result,"\"value\":\"Int\""));
    CHECK(strstr(result,"\"id\":\"a\",\"error\":{\"code\":-32800"));
    CHECK(strstr(result,"\"id\":\"b\",\"error\":{\"code\":-32800"));
    CHECK(!strstr(result,"-32700"));
    free(result); CHECK(fclose(in)==0 && fclose(out)==0);
}
static void wide_hover_budget(void) {
    FILE *in=tmpfile(), *out=tmpfile(); CHECK(in && out);
    char type[4352]="", source[9000], message[9600];
    size_t n=0;
    for (size_t i=0;i<32;++i) { memcpy(type+n,"List[",5); n+=5; }
    memcpy(type+n,"fn(",3); n+=3;
    for (size_t i=0;i<999;++i) { memcpy(type+n,"Int,",4); n+=4; }
    memcpy(type+n,"Int)->Int",9); n+=9;
    memset(type+n,']',32); type[n+32]=0;
    const int length=snprintf(source,sizeof source,"fn f(x:%s)->%s{x}",type,type);
    CHECK(length>2 && (size_t)length<sizeof source);
    frame(in,"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    const int size=snprintf(message,sizeof message,
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{"
        "\"uri\":\"file:///wide.tfd\",\"version\":1,\"text\":\"%s\"}}}",source);
    CHECK(size>0 && (size_t)size<sizeof message);
    frame(in,message);
    for (int i=2;i<=3;++i) {
        const int request=snprintf(message,sizeof message,
            "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"textDocument/hover\",\"params\":{"
            "\"textDocument\":{\"uri\":\"file:///wide.tfd\"},\"position\":{\"line\":0,\"character\":%d}}}",i,length-2);
        CHECK(request>0 && (size_t)request<sizeof message);
        frame(in,message);
    }
    rewind(in); CHECK(tf_editor_serve(in,out,".",NULL)==TF_OK);
    char *result=read_all(out);
    CHECK(strstr(result,"\"version\":1,\"diagnostics\":[]"));
    const char *first=strstr(result,"\"id\":2,\"result\":{\"contents\":");
    const char *second=strstr(result,"\"id\":3,\"result\":{\"contents\":");
    CHECK(first && second && first<second);
    const char *value=strstr(first,"\"value\":\"");
    CHECK(value && value<second); value+=sizeof "\"value\":\""-1;
    const char *end=strchr(value,'\"');
    CHECK(end && end<second && (size_t)(end-value)<1024);
    CHECK(strstr(value,"...") && strstr(value,"...")<end);
    free(result); CHECK(fclose(in)==0 && fclose(out)==0);
}
static void padded(FILE *file,size_t length) {
    static const char declaration[]="pub fn value()->Int{1}\n";
    CHECK(length>=sizeof declaration-1);
    CHECK(fwrite(declaration,1,sizeof declaration-1,file)==sizeof declaration-1);
    for (size_t i=sizeof declaration-1;i<length;++i) CHECK(fputc(' ',file)!=EOF);
    CHECK(fclose(file)==0);
}
static void dependency_limits(void) {
    char path[128],path2[128],code[512]; FILE *file=NULL;
    for (unsigned i=0;i<64 && !file;++i) {
        CHECK(snprintf(path,sizeof path,"build/editor-budget-%u.tfd",i)>0);
        file=fopen(path,"wbx");
    }
    CHECK(file);
    CHECK(snprintf(code,sizeof code,"import \"%s\" as b; fn f()->Int{b::value()}",path)>0);
    const size_t near=1048576-strlen(code);
    padded(file,near);
    Fault state={0,SIZE_MAX,0}; TfAllocator allocator={&state,allocate,deallocate};
    TfEditorSnapshot *old=NULL,*next=NULL;
    CHECK(tf_editor_snapshot_create(&old,"budget",code,strlen(code),-3,".",&allocator)==TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_editor_diagnostics(old,-3)));
    file=fopen(path,"wb"); CHECK(file); padded(file,near+1);
    CHECK(tf_editor_snapshot_create(&next,"budget",code,strlen(code),-2,".",&allocator)==TF_HOST_ERROR);
    CHECK(!next && tf_editor_diagnostics(old,-3));
    file=fopen(path,"wb"); CHECK(file); padded(file,600000);
    CHECK(snprintf(path2,sizeof path2,"%s.second.tfd",path)>0);
    file=fopen(path2,"wbx"); CHECK(file); padded(file,600000);
    CHECK(snprintf(code,sizeof code,"import \"%s\" as b; import \"%s\" as c;"
                   "fn f()->Int{b::value()+c::value()}",path,path2)>0);
    CHECK(tf_editor_snapshot_create(&next,"budget",code,strlen(code),-1,".",&allocator)==TF_HOST_ERROR);
    CHECK(!next && tf_editor_diagnostics(old,-3));
    tf_editor_snapshot_destroy(old); CHECK(state.live==0);

    file=fopen(path,"wb"); CHECK(file);
    CHECK(fputs("\n\npub fn value()->Int{true}\n",file)>=0 && fclose(file)==0);
    CHECK(snprintf(code,sizeof code,"import \\\"%s\\\" as b; fn f()->Int{b::value()}",path)>0);
    FILE *in=tmpfile(),*out=tmpfile(); CHECK(in && out);
    frame(in,"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    char message[2048];
    CHECK(snprintf(message,sizeof message,"{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///imports.tfd\",\"version\":1,\"text\":\"%s\"}}}",code)>0);
    frame(in,message);
    frame(in,"{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\","
        "\"params\":{\"textDocument\":{\"uri\":\"file:///imports.tfd\"}}}");
    rewind(in); CHECK(tf_editor_serve(in,out,".",NULL)==TF_OK);
    char *result=read_all(out);
    CHECK(strstr(result,"E302") && strstr(result,"\"line\":2"));
    CHECK(strstr(result,"editor-budget-") && strstr(result,"\"uri\":\"file:///"));
    CHECK(strstr(result,"\"uri\":\"file:///imports.tfd\",\"version\":1,\"diagnostics\":[]"));
    free(result); CHECK(fclose(in)==0 && fclose(out)==0);
    CHECK(remove(path)==0 && remove(path2)==0);
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
    coordinates(); snapshots(); transcript(); reviewed_protocol_regressions();
    wide_hover_budget(); dependency_limits(); malformed(); protocol_allocations();
    printf("PASS editor: %zu checks\n", checks);
    return 0;
}
