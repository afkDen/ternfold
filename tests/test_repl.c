#include "../src/repl.h"

#include <stdlib.h>
#include <string.h>

static size_t assertions;
#define CHECK(ok) do { ++assertions; if (!(ok)) { fprintf(stderr,"test_repl:%d: %s\n",__LINE__,#ok); exit(1); } } while (0)

static char *read_all(FILE *stream) {
    CHECK(fseek(stream,0,SEEK_END)==0);
    const long length=ftell(stream);
    CHECK(length>=0 && length<=65536 && fseek(stream,0,SEEK_SET)==0);
    char *value=malloc((size_t)length+1);
    CHECK(value!=NULL && fread(value,1,(size_t)length,stream)==(size_t)length);
    CHECK(memchr(value,'\0',(size_t)length)==NULL);
    value[length]='\0';
    return value;
}
static void exercise(const char *script,const char *expected,const char *const diagnostics[],size_t count) {
    FILE *in=tmpfile(),*out=tmpfile(),*err=tmpfile();
    CHECK(in!=NULL && out!=NULL && err!=NULL);
    CHECK(fputs(script,in)!=EOF && fflush(in)==0 && fseek(in,0,SEEK_SET)==0);
    CHECK(tf_repl(in,out,err,NULL,false)==0);
    char *stdout_bytes=read_all(out), *stderr_bytes=read_all(err);
    if (strcmp(stdout_bytes,expected)!=0) fprintf(stderr,"expected stdout:\n%sactual stdout:\n%s",expected,stdout_bytes);
    CHECK(strcmp(stdout_bytes,expected)==0);
    const char *cursor=stderr_bytes;
    if (count==0) CHECK(*cursor=='\0');
    for (size_t i=0;i<count;++i) {
        const char *next=strstr(cursor,diagnostics[i]);
        if (next==NULL) fprintf(stderr,"missing %s in:\n%s",diagnostics[i],cursor);
        CHECK(next!=NULL);
        cursor=next+strlen(diagnostics[i]);
    }
    free(stdout_bytes); free(stderr_bytes);
    CHECK(fclose(in)==0 && fclose(out)==0 && fclose(err)==0);
}

int main(void) {
    static const char *const faults[]={"error[R002]","\"<repl>\"","[bytes 192..195)",
        "error[E204]","error[E208]","error[E203]"};
    exercise(
        "fn twice(value: Int) -> Int {\n"
        "    value * 2\n"
        "}\n"
        "let x = 21;\n"
        "let copied = x;\n"
        "let apply = fn(value: Int) -> Int { twice(value) + x };\n"
        "apply(copied)\n"
        ":type apply\n"
        "let failed = 1 div 0;\n"
        "x\n"
        ":load tests/fixtures/repl/not-present.tfd\n"
        "x\n"
        ":load examples/intro/counter.tfd\n"
        ":type main\n"
        "main()\n"
        ":load examples/course/main_io.tfd\n"
        "main\n"
        ":reset\n"
        ":type x\n"
        ":quit\n"
        "not_evaluated_after_quit\n",
        "defined\n21\n21\n<closure>\n63\nfn(Int) -> Int\n21\n21\n"
        "loaded\nfn() -> List[Text]\n[\"4\"]\nloaded\nreset\n",
        faults,sizeof faults/sizeof *faults);

    static const char *const recovery[]={"error[E202]","expected exactly one complete expression or binding",
        "error[E203]","unknown or incomplete command","error[E103]",
        "incomplete unit at end of input"};
    exercise(
        "let n = 9;\n"
        "let n = 11;\n"
        "let candidate = 1; let stray = 2;\n"
        "candidate\n"
        ":phantom\n"
        "\"unclosed\n"
        ":type (1 div 0)\n"
        "n +\n"
        " 1\n"
        "[\n",
        "9\nInt\n10\n",recovery,sizeof recovery/sizeof *recovery);

    exercise(
        ":load examples/intro/counter.tfd\n:plan\nmain()\n",
        "loaded\nMachine plan; guards are runtime conditions, not proven assertions.\n"
        "machine 1:Counter node=2 span=0..183\n"
        "  schedule: value\n"
        "  cell 0 value type=1 initializer=13 writer=0\n"
        "  read 0 old context=writer:0 symbol=24 node=15 span=97..102\n"
        "  read 1 input context=writer:0 symbol=23 node=18 span=105..110\n"
        "  read 2 candidate context=guard:0 symbol=24 node=21 span=122..132\n"
        "  read 3 candidate context=emission:0 symbol=24 node=26 span=170..180\n"
        "  guard 0 predicate=22 reject-on-false=24\n"
        "  emission 0 expression=26\n[\"4\"]\n",NULL,0);

    exercise(
        "machine Gauge(increment: Int) yields Int rejects Unit {\n"
        "    state count: Int = 2;\n"
        "    next count = count + increment;\n"
        "    guard next.count >= 0 else ();\n"
        "    emit next.count;\n"
        "}\n"
        "let initial = Gauge();\n"
        "let denied = step(initial, -3);\n"
        "match denied { Decision::Reject(snapshot, _) => { snapshot.count }, Decision::Accept(_, _) => { 0 } }\n"
        "step(initial, 4)\n"
        "\"line\\n\\u{0}end\"\n",
        "defined\nGauge {count: 2}\nReject(Gauge {count: 2}, ())\n2\n"
        "Accept(Gauge {count: 6}, [6])\n\"line\\n\\u{0}end\"\n",NULL,0);
    exercise(
        "if true\n"
        "{ 1 }\n"
        "else { 2 }\n"
        "for value in [1, 2]\n"
        "{ value + 1 }\n"
        "match true\n"
        "{ true => { 1 }, false => { 0 } }\n",
        "1\n[2, 3]\n1\n",NULL,0);
    static const char *const bad_header[]={"error[E110]","42"};
    exercise(
        "fn broken() -> 42\n"
        "fn good() -> Int { 7 }\n"
        "good()\n",
        "defined\n7\n",bad_header,sizeof bad_header/sizeof *bad_header);
    exercise(
        "let held = 8;\n"
        "let abandoned = 1\n"
        " :cancel \r\n"
        "held\n"
        "let again = 2\n"
        " :quit\n"
        ";\n"
        "77\n",
        "8\ncancelled\n8\n",NULL,0);
    exercise(
        ":load tests/fixtures/repl/nominal-root.tfd\n"
        ":type fn(l: first::R, r: second::R) -> first::R { l }\n",
        "loaded\nfn(\"first.tfd\"::R, \"second.tfd\"::R) -> \"first.tfd\"::R\n",NULL,0);
    printf("Transactional REPL: %zu transcript assertions passed\n",assertions);
    return 0;
}
