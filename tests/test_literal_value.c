#include "../src/literal_value.h"
#include "ternfold/parser.h"

#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { fprintf(stderr,"test_literal_value:%d: %s\n",__LINE__,#condition); exit(1); } \
} while (0)

typedef struct {
    const char *expression;
    TfStatus status;
    uint64_t bits;
    bool pattern, minimum;
} NumericCase;

static void number(NumericCase test) {
    char program[192];
    int n = snprintf(program,sizeof program,test.pattern ?
        "fn f() -> Int { match 0 { %s => { 0 }, _ => { 1 } } }" :
        "fn f() -> Int { %s }",test.expression);
    TfSource source;
    TfLexResult lex;
    TfParseResult *parsed;
    const TfAst *ast;
    const TfAstNode *node = NULL, *parent = NULL;
    TfLiteralValue value = {0};
    CHECK(n > 0 && (size_t)n < sizeof program);
    CHECK(tf_source_init(&source,"numeric.tfd",program,(size_t)n,NULL) == TF_OK);
    CHECK(tf_lex(&lex,&source,100,NULL) == TF_OK);
    CHECK(tf_parse(&parsed,&lex,NULL,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_parse_phase(parsed)) == TF_DIAG_PHASE_VALID);
    ast = tf_parse_ast(parsed);
    for (size_t i=0; i<tf_ast_count(ast); ++i) {
        const TfAstNode *candidate = tf_ast_at(ast,i);
        if (candidate->kind == (test.pattern ? TF_AST_PATTERN_LITERAL : TF_AST_LITERAL)) {
            CHECK(node == NULL);
            node = candidate;
        }
        if (candidate->kind == TF_AST_UNARY) parent = candidate;
    }
    CHECK(node != NULL);
    CHECK(tf_literal_value(node,parent,&value) == test.status);
    if (test.status == TF_OK) {
        CHECK(value.minimum_magnitude == test.minimum);
        if (node->as.literal.token == TF_FLOAT)
            CHECK(value.kind == TF_LITERAL_FLOAT && value.as.float_bits == test.bits);
        else if (test.pattern)
            CHECK(value.kind == TF_LITERAL_INT && (uint64_t)value.as.signed_pattern == test.bits);
        else CHECK(value.kind == TF_LITERAL_INT && value.as.int_magnitude == test.bits);
    }
    tf_parse_destroy(parsed);
    tf_lex_destroy(&lex);
    tf_source_destroy(&source);
}

int main(void) {
    const NumericCase cases[] = {
        {"9223372036854775807",TF_OK,UINT64_C(9223372036854775807),false,false},
        {"9223372036854775808",TF_SOURCE_ERROR,0,false,false},
        {"-9223372036854775808",TF_OK,UINT64_C(1)<<63,false,true},
        {"- 9223372036854775808",TF_SOURCE_ERROR,0,false,false},
        {"-/* gap */9223372036854775808",TF_SOURCE_ERROR,0,false,false},
        {"-(9223372036854775808)",TF_SOURCE_ERROR,0,false,false},
        {"-9223372036854775809",TF_SOURCE_ERROR,0,false,false},
        {"-9223372036854775808",TF_OK,UINT64_C(1)<<63,true,true},
        {"- 9223372036854775808",TF_SOURCE_ERROR,0,true,false},
        {"-/* gap */9223372036854775808",TF_SOURCE_ERROR,0,true,false},
        {"- 5",TF_OK,(uint64_t)(int64_t)-5,true,false},
        {"-0.0",TF_OK,0,true,false},
        {"-1.25",TF_OK,UINT64_C(0xbff4000000000000),true,false},
        {"-1.25",TF_OK,UINT64_C(0x3ff4000000000000),false,false},
        {"1e999",TF_SOURCE_ERROR,0,false,false},
        {"1e999",TF_SOURCE_ERROR,0,true,false}
    };
    for (size_t i=0; i<sizeof cases/sizeof *cases; ++i) number(cases[i]);
    printf("typed literal boundaries: %zu checks passed\n",checks);
    return 0;
}
