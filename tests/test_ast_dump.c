#include "ternfold/parser.h"
#include "ternfold/ast_dump.h"
#include <stdlib.h>
#include <string.h>

static size_t checks;
static void check(bool ok,int line){++checks;if(!ok){(void)fprintf(stderr,"ast_dump:%d failed\n",line);exit(1);}}
#define CHECK(x) check((x),__LINE__)
static char *capture(const TfAst *a,bool errors,bool json){
    FILE *f=tmpfile();long length;char *result;
    CHECK(f!=NULL);CHECK(tf_ast_dump_write(f,a,errors,json)==TF_OK);CHECK(fflush(f)==0);
    CHECK(fseek(f,0,SEEK_END)==0);length=ftell(f);CHECK(length>=0&&(uintmax_t)length<SIZE_MAX);
    result=malloc((size_t)length+1);CHECK(result!=NULL);CHECK(fseek(f,0,SEEK_SET)==0);
    CHECK(fread(result,1,(size_t)length,f)==(size_t)length);result[(size_t)length]=0;
    CHECK(memchr(result,0,(size_t)length)==NULL);CHECK(fclose(f)==0);return result;
}
static TfParseResult *parse(TfSource *source,const char *text){
    TfLexResult lex;TfParseResult *r=NULL;
    CHECK(tf_source_init(source,"dump.tfd",text,strlen(text),NULL)==TF_OK);
    CHECK(tf_lex(&lex,source,100,NULL)!=TF_HOST_ERROR);CHECK(tf_parse(&r,&lex,NULL,NULL)==TF_OK);
    tf_lex_destroy(&lex);return r;
}
static void empty_golden(void){
    TfSource source;TfParseResult *r=parse(&source,"");const TfAst *a=tf_parse_ast(r);char *json,*text;
    json=capture(a,false,true);text=capture(a,false,false);
    CHECK(strcmp(json,"{\"schema_version\":1,\"root\":1,\"has_errors\":false,\"nodes\":[{\"id\":1,\"kind\":\"PROGRAM\",\"start_byte\":0,\"end_byte\":0,\"declarations\":[]}]}\n")==0);
    CHECK(strcmp(text,"AST root=1 has_errors=false nodes=1\n{\"id\":1,\"kind\":\"PROGRAM\",\"start_byte\":0,\"end_byte\":0,\"declarations\":[]}\n")==0);
    CHECK(tf_ast_dump_write(NULL,a,false,true)==TF_HOST_ERROR);
    {FILE *f=tmpfile();CHECK(f!=NULL);CHECK(tf_ast_dump_write(f,NULL,false,true)==TF_HOST_ERROR);CHECK(fclose(f)==0);}
    free(json);free(text);tf_parse_destroy(r);tf_source_destroy(&source);
}
static void payload_and_recovery(void){
    TfSource source;TfParseResult *r=parse(&source,"pub fn main() -> List[Text] { input x: Text from stdin; output \"a\\u{0}\\n\\\"\\\\é\" to stdout; [x] }");
    const TfAst *a=tf_parse_ast(r);char *first,*second,*text;
    CHECK(tf_diagnostic_phase_kind(tf_parse_phase(r))==TF_DIAG_PHASE_VALID);
    first=capture(a,false,true);second=capture(a,false,true);text=capture(a,false,false);
    CHECK(strcmp(first,second)==0);CHECK(strstr(first,"\"has_errors\":false")!=NULL);
    CHECK(strstr(first,"a\\u0000\\u000a\\\"\\\\é")!=NULL);
    CHECK(strstr(first,"\"from_stdin\":true")!=NULL);CHECK(strstr(first,"\"to_stdout\":true")!=NULL);
    CHECK(strstr(first,"\"name\":\"main\",\"public\":true")!=NULL);
    for(size_t i=0;i<tf_ast_count(a);++i){
        const TfAstNode *n=tf_ast_at(a,i);char needle[160];
        CHECK(snprintf(needle,sizeof(needle),"{\"id\":%zu,\"kind\":\"%s\",\"start_byte\":%zu,\"end_byte\":%zu",n->id.index,tf_ast_kind_name(n->kind),n->span.start,n->span.end)>0);
        CHECK(strstr(first,needle)!=NULL);CHECK(strstr(text,needle)!=NULL);
    }
    free(first);free(second);free(text);tf_parse_destroy(r);tf_source_destroy(&source);
    r=parse(&source,"fn bad() -> Int { 1 + ; fn good() -> Int { 2 }");a=tf_parse_ast(r);
    CHECK(tf_diagnostic_phase_kind(tf_parse_phase(r))==TF_DIAG_PHASE_ERROR);
    first=capture(a,true,true);CHECK(strstr(first,"\"has_errors\":true")!=NULL);
    CHECK(strstr(first,"\"kind\":\"ERROR\"")!=NULL);CHECK(strstr(first,"\"kind\":\"MISSING\"")!=NULL);
    CHECK(strstr(first,"\"name\":\"good\"")!=NULL);
    free(first);tf_parse_destroy(r);tf_source_destroy(&source);
}
static void write_failure(void){
    TfSource source;TfParseResult *r=parse(&source,"");FILE *f=fopen("tests/fixtures/parser/surface.tfd","rb");
    CHECK(f!=NULL);CHECK(setvbuf(f,NULL,_IONBF,0)==0);
    CHECK(tf_ast_dump_write(f,tf_parse_ast(r),false,true)==TF_HOST_ERROR);
    CHECK(fclose(f)==0);tf_parse_destroy(r);tf_source_destroy(&source);
}
int main(void){empty_golden();payload_and_recovery();write_failure();(void)printf("AST dump: %zu checks passed\n",checks);return 0;}
