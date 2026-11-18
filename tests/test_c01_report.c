#include "ternfold/lexer.h"

#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(ok) do { ++checks; if (!(ok)) { \
    fprintf(stderr,"test_c01_report:%d: %s\n",__LINE__,#ok); exit(1); \
} } while (0)

typedef struct { TfTokenKind kind; const char *category, *spelling; } Operator;
static const Operator inventory[] = {
    {TF_PLUS,"PLUS","+"}, {TF_MINUS,"MINUS","-"}, {TF_SLASH,"SLASH","/"},
    {TF_STAR,"STAR","*"}, {TF_PERCENT,"PERCENT","%"}, {TF_DIV,"DIV","div"},
    {TF_CARET,"CARET","^"}, {TF_AND_AND,"AND_AND","&&"}, {TF_OR_OR,"OR_OR","||"},
    {TF_BANG,"BANG","!"}, {TF_GREATER,"GREATER",">"}, {TF_LESS,"LESS","<"},
    {TF_EQUAL_EQUAL,"EQUAL_EQUAL","=="}, {TF_BANG_EQUAL,"BANG_EQUAL","!="},
    {TF_GREATER_EQUAL,"GREATER_EQUAL",">="}, {TF_LESS_EQUAL,"LESS_EQUAL","<="}
};

static void check_report(FILE *generated, const TfLexResult *lex) {
    char line[512], expected[512];
    bool seen[sizeof inventory/sizeof *inventory] = {false};
    size_t previous = 0, rows = 0;
    FILE *golden = fopen("tests/fixtures/numerical/Symbol Table.txt","rb");
    CHECK(golden != NULL);
    CHECK(fseek(generated,0,SEEK_SET) == 0);
    CHECK(fgets(line,sizeof line,generated) != NULL && strcmp(line,"TOKEN TYPE\tLEXEME\n") == 0);
    CHECK(fgets(expected,sizeof expected,golden) != NULL && strcmp(line,expected) == 0);
    for (size_t i=0; i<lex->token_count; ++i) {
        const TfToken *token = &lex->tokens[i];
        if (token->kind == TF_EOF) continue;
        const TfSpan span = token->span;
        CHECK(span.source == lex->source && previous <= span.start && span.start < span.end &&
              span.end <= lex->source->length);
        CHECK(fgets(line,sizeof line,generated) != NULL && fgets(expected,sizeof expected,golden) != NULL);
        CHECK(strcmp(line,expected) == 0 && strchr(line,'\r') == NULL);
        char *tab = strchr(line,'\t'), *lf = strchr(line,'\n');
        CHECK(tab != NULL && lf != NULL && tab<lf && lf[1]=='\0' && strchr(tab+1,'\t') == NULL);
        /* This fixture has no escaped/control lexemes. Independently match
         * every row to the original source bytes at the token's true span. */
        const size_t length = span.end-span.start;
        CHECK((size_t)(lf-tab-1) == length);
        CHECK(memcmp(tab+1,lex->source->bytes+span.start,length) == 0);
        for (size_t j=0; j<sizeof inventory/sizeof *inventory; ++j) {
            const Operator *op = &inventory[j];
            if (token->kind != op->kind) continue;
            CHECK(length == strlen(op->spelling) && memcmp(tab+1,op->spelling,length) == 0);
            CHECK((size_t)(tab-line) == strlen(op->category) &&
                  memcmp(line,op->category,(size_t)(tab-line)) == 0);
            seen[j] = true;
            break;
        }
        previous = span.end;
        ++rows;
    }
    CHECK(rows != 0 && fgetc(generated) == EOF && fgetc(golden) == EOF);
    CHECK(!ferror(generated) && !ferror(golden));
    for (size_t i=0; i<sizeof seen/sizeof *seen; ++i) CHECK(seen[i]);
    CHECK(fclose(golden) == 0);
    printf("C01 operator inventory: %zu data rows, 7 arithmetic and 9 Boolean/relational operators\n",rows);
}

int main(void) {
    TfSource source = {0};
    TfLexResult lex = {0};
    FILE *generated = tmpfile();
    CHECK(generated != NULL);
    CHECK(tf_source_load(&source,"examples/course/operator_inventory.tfd",NULL) == TF_OK);
    CHECK(tf_lex(&lex,&source,100,NULL) == TF_OK && !lex.has_errors);
    CHECK(tf_report_write_classroom(generated,&lex) == TF_OK);
    check_report(generated,&lex);
    CHECK(fclose(generated) == 0);
    tf_lex_destroy(&lex);
    tf_source_destroy(&source);
    printf("C01 classroom report: %zu checks passed\n",checks);
    return 0;
}
