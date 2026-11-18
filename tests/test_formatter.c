#include "ternfold/format.h"
#include "ternfold/ast_dump.h"
#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(e) do { ++checks; if (!(e)) { fprintf(stderr,"formatter:%d: %s\n",__LINE__,#e); exit(1); } } while (0)
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
/* Compare the production AST JSON schema with only byte-coordinate values
 * elided. Quoted literal values are copied as strings, so numeric text is
 * never mistaken for a coordinate. This covers every AST field/child list. */
static char *shape(const TfAst *ast) {
    FILE *f = tmpfile(); CHECK(f != NULL);
    CHECK(tf_ast_dump_write(f, ast, false, true) == TF_OK);
    CHECK(fflush(f) == 0 && fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f); CHECK(size >= 0);
    char *bytes = malloc((size_t)size + 1); CHECK(bytes != NULL);
    rewind(f); CHECK(fread(bytes, 1, (size_t)size, f) == (size_t)size);
    bytes[size] = 0; CHECK(fclose(f) == 0);
    size_t read = 0, write = 0;
    while (bytes[read] != 0) {
        if (bytes[read] == '"') {
            size_t start = read++;
            while (bytes[read] != 0 && bytes[read] != '"') {
                if (bytes[read] == '\\' && bytes[read + 1] != 0) ++read;
                ++read;
            }
            if (bytes[read] == '"') ++read;
            size_t length = read - start;
            bool coordinate = (length == 12 && memcmp(bytes + start, "\"start_byte\"", 12) == 0) ||
                              (length == 10 && memcmp(bytes + start, "\"end_byte\"", 10) == 0);
            memmove(bytes + write, bytes + start, length); write += length;
            if (coordinate && bytes[read] == ':') {
                bytes[write++] = bytes[read++];
                while (bytes[read] >= '0' && bytes[read] <= '9') ++read;
                bytes[write++] = '0';
            }
        } else bytes[write++] = bytes[read++];
    }
    bytes[write] = 0;
    return bytes;
}
static void equivalent(const TfSource *a, const TfSource *b) {
    TfLexResult x = {0}, y = {0};
    TfParseResult *p = NULL, *q = NULL;
    CHECK(tf_lex(&x, a, 100, NULL) == TF_OK);
    CHECK(tf_lex(&y, b, 100, NULL) == TF_OK);
    CHECK(x.token_count == y.token_count);
    for (size_t i = 0; i < x.token_count; ++i) {
        const TfToken *l = &x.tokens[i], *r = &y.tokens[i];
        CHECK(l->kind == r->kind && l->reserved == r->reserved);
        CHECK(l->span.end - l->span.start == r->span.end - r->span.start);
        CHECK(memcmp(a->bytes + l->span.start, b->bytes + r->span.start, l->span.end - l->span.start) == 0);
    }
    CHECK(tf_parse(&p, &x, NULL, NULL) == TF_OK);
    CHECK(tf_parse(&q, &y, NULL, NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_parse_phase(p)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_diagnostic_phase_kind(tf_parse_phase(q)) == TF_DIAG_PHASE_VALID);
    const TfAst *left = tf_parse_ast(p), *right = tf_parse_ast(q);
    char *left_shape = shape(left), *right_shape = shape(right);
    CHECK(strcmp(left_shape, right_shape) == 0);
    free(right_shape); free(left_shape);
    CHECK(tf_ast_count(left) == tf_ast_count(right));
    /* Parser construction order/IDs and child structure are deterministic for
     * the exact token stream. Also assert all node kinds and operator values. */
    for (size_t i = 0; i < tf_ast_count(left); ++i) {
        const TfAstNode *l = tf_ast_at(left, i), *r = tf_ast_at(right, i);
        CHECK(l->kind == r->kind && l->id.index == r->id.index);
        if (l->kind == TF_AST_BINARY) {
            CHECK(l->as.binary.op == r->as.binary.op);
            CHECK(l->as.binary.left.index == r->as.binary.left.index && l->as.binary.right.index == r->as.binary.right.index);
        }
        if (l->kind == TF_AST_UNARY) CHECK(l->as.unary.op == r->as.unary.op && l->as.unary.operand.index == r->as.unary.operand.index);
        if (l->kind == TF_AST_LITERAL || l->kind == TF_AST_PATTERN_LITERAL) {
            CHECK(l->as.literal.token == r->as.literal.token && l->as.literal.negative == r->as.literal.negative);
            CHECK(l->as.literal.value.length == r->as.literal.value.length);
            CHECK(memcmp(l->as.literal.value.data, r->as.literal.value.data, l->as.literal.value.length) == 0);
        }
    }
    tf_parse_destroy(q); tf_parse_destroy(p); tf_lex_destroy(&y); tf_lex_destroy(&x);
}
static void valid(const char *text) {
    TfSource source = {0}, formatted = {0};
    TfFormatResult first = {0}, second = {0};
    CHECK(tf_source_init(&source, "format.tfd", text, strlen(text), NULL) == TF_OK);
    CHECK(tf_format(&first, &source, NULL, NULL) == TF_OK);
    CHECK(first.bytes[first.length] == 0);
    CHECK(tf_source_init(&formatted, "format.tfd", first.bytes, first.length, NULL) == TF_OK);
    equivalent(&source, &formatted);
    CHECK(tf_format(&second, &formatted, NULL, NULL) == TF_OK);
    CHECK(first.length == second.length && memcmp(first.bytes, second.bytes, first.length) == 0);
    tf_format_destroy(&second); tf_source_destroy(&formatted); tf_format_destroy(&first); tf_source_destroy(&source);
}
int main(void) {
    static const char *const cases[] = {
        "", " \t\r\n ", "// comment at EOF", "/*block\r\n comment*/",
        "fn f()->Int{let x=-9223372036854775808; x}",
        "fn f()->Int{- 9223372036854775808}",
        "fn f()->Int{-/*separation*/9223372036854775808}",
        "fn f()->Int{2^-1+3*4 div 2%3}",
        "fn f()->Float{1.0e+2/2.0}",
        "fn f()->Text{\"a\\n\\u{0}/*literal*/\"}//tail",
        "fn f()->Char{'\xf0\x9f\x99\x82'}",
        "pub fn main()->List[Text]{input n:Text from stdin; output n to stdout; [n]}",
        "enum E{A,B(Int)} fn f(x:E)->Int{match x{E::A=>{1},E::B(v)=>{v},}}",
        "record R{x:Int,y:List[Int]} fn f()->R{R(x:1,y:[2,3,])}",
        "fn f(x:Int)->Int{if x<1{0}else{if x==1{1}else{2}}}",
        "fn f()->List[Int]{for x in [1,2]{let y=x+1; y}}",
        "machine M(i:Int)yields Int rejects Text{state n:Int=0;next n=n+i;guard next.n>0 else \"no\";emit next.n;}",
        "//leading\r\nfn f(/*one*/)->Int{ //body\r\n 1/*two\nthree*/}//end"
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) valid(cases[i]);
    static const char *const invalid[] = {"fn", "fn f()->Int{1;}", "/*", "fn f()->Int{01}", "@", "fn f()->Text{\"bad}"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        TfSource s = {0}; TfFormatResult result = {0};
        CHECK(tf_source_init(&s, "bad.tfd", invalid[i], strlen(invalid[i]), NULL) == TF_OK);
        CHECK(tf_format(&result, &s, NULL, NULL) == TF_SOURCE_ERROR);
        CHECK(result.bytes == NULL && result.length == 0);
        tf_format_destroy(&result); tf_source_destroy(&s);
    }
    TfSource s = {0};
    CHECK(tf_source_init(&s, "oom.tfd", cases[17], strlen(cases[17]), NULL) == TF_OK);
    Fault baseline = {0, SIZE_MAX, 0}; TfAllocator a = {&baseline, allocate, deallocate}; TfFormatResult result = {0};
    CHECK(tf_format(&result, &s, NULL, &a) == TF_OK);
    tf_format_destroy(&result); CHECK(baseline.live == 0);
    size_t failures = baseline.calls;
    for (size_t i = 0; i < failures; ++i) {
        Fault f = {0, i, 0}; a.context = &f;
        CHECK(tf_format(&result, &s, NULL, &a) == TF_HOST_ERROR);
        CHECK(result.bytes == NULL && result.length == 0 && f.live == 0);
        tf_format_destroy(&result);
    }
    tf_source_destroy(&s);
    unsigned char bad[] = {0xff};
    CHECK(tf_source_init(&s, "invalid", bad, sizeof bad, NULL) == TF_SOURCE_ERROR);
    CHECK(tf_format(&result, &s, NULL, NULL) == TF_SOURCE_ERROR); tf_source_destroy(&s);
    CHECK(tf_format(NULL, NULL, NULL, NULL) == TF_HOST_ERROR);
    printf("PASS formatter: %zu checks, %zu allocation failpoints\n", checks, failures);
    return 0;
}
