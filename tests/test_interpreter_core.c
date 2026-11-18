#include "ternfold/core_lower.h"
#include "ternfold/interpreter.h"
#include "../src/typed_signatures.h"

#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(ok) do { ++checks; if (!(ok)) { \
    fprintf(stderr,"test_interpreter_core:%d: %s\n",__LINE__,#ok); exit(1); \
} } while (0)

typedef struct { TfSourceManager *manager; TfCoreProgram *core; TfVerifiedCore *verified; } Program;
typedef struct { size_t attempts, fail, live; } Counts;

static void *allocate(void *context, size_t bytes) {
    Counts *counts=context;
    if (++counts->attempts==counts->fail) return NULL;
    void *p=malloc(bytes);
    if (p!=NULL) ++counts->live;
    return p;
}

static void deallocate(void *context, void *p) {
    Counts *counts=context;
    CHECK(counts->live!=0 && p!=NULL);
    --counts->live; free(p);
}

static Program compile(const char *file) {
    Program program={0};
    TfResolution *resolution=NULL;
    TfTypedSignatures *signatures=NULL;
    TfTypedProgram *typed=NULL;
    TfMachineProgram *machines=NULL;
    CHECK(tf_source_manager_create(&program.manager,NULL)==TF_OK);
    CHECK(tf_resolve(&resolution,program.manager,file,NULL,NULL)==TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(resolution))==TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&signatures,resolution,100,NULL)==TF_OK);
    CHECK(tf_check(&typed,resolution,signatures,100,NULL)==TF_OK);
    const TfDiagnosticBag *bag=tf_typed_program_diagnostics(typed);
    if (tf_diagnostic_bag_has_errors(bag)) (void)tf_diagnostic_bag_write(stderr,bag,TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
    CHECK(tf_machine_program_build(&machines,typed,100,NULL)==TF_OK);
    CHECK(tf_core_lower(&program.core,machines,NULL,NULL)==TF_OK);
    CHECK(tf_core_verify(&program.verified,program.core,NULL,NULL)==TF_OK);
    tf_machine_program_destroy(machines); tf_typed_program_destroy(typed);
    tf_signatures_destroy(signatures); tf_resolution_destroy(resolution);
    return program;
}

static void release(Program p) {
    tf_verified_core_destroy(p.verified); tf_core_program_destroy(p.core); tf_source_manager_destroy(p.manager);
}

static void line(const TfInterpreterResult *r, size_t index, const char *bytes, size_t length) {
    TfCoreText actual={0};
    CHECK(tf_interpreter_result_line(r,index,&actual));
    CHECK(actual.length==length && (length==0 || memcmp(actual.bytes,bytes,length)==0));
}

static bool spells(Program p, TfCoreSpanId origin, const char *text) {
    const TfSpan *s=tf_core_span(tf_verified_core_data(p.verified),origin);
    const size_t n=strlen(text);
    return s!=NULL && s->end-s->start==n && memcmp(s->source->bytes+s->start,text,n)==0;
}

static size_t transitions(void) {
    Program p=compile("tests/fixtures/types/interpreter-core.tfd");
    TfInterpreterResult *result=NULL;
    TfRuntimeFault fault;
    Counts counts={0};
    const TfAllocator allocator={&counts,allocate,deallocate};
    CHECK(tf_interpret_entry(&result,p.verified,NULL,1,NULL,&allocator,&fault)==TF_OK);
    CHECK(result!=NULL && fault.code==TF_RUNTIME_NONE && counts.live==1);
    const size_t allocations=counts.attempts;
    tf_interpreter_result_destroy(result); CHECK(counts.live==0);
    for (size_t i=1; i<=allocations; ++i) {
        counts=(Counts){.fail=i};
        CHECK(tf_interpret_entry(&result,p.verified,NULL,0,NULL,&allocator,&fault)==TF_SOURCE_ERROR);
        CHECK(result==NULL && fault.code==TF_RUNTIME_RESOURCE && counts.live==0);
    }
    CHECK(tf_interpret_entry(&result,p.verified,NULL,0,NULL,NULL,&fault)==TF_OK);
    release(p); /* Completed output owns every byte independently of Core. */
    CHECK(tf_interpreter_result_count(result)==5);
    line(result,0,"accepted",8); line(result,1,"old snapshot",12); line(result,2,"rejected",8);
    line(result,3,"checked",7); line(result,4,"a\0b",3);
    TfCoreText no={0}; CHECK(!tf_interpreter_result_line(result,5,&no));
    tf_interpreter_result_destroy(result);
    return allocations;
}

static void staging(void) {
    Program p=compile("tests/fixtures/types/core-entry.tfd");
    const TfCoreText input[]={{(const unsigned char *)"A",1},{(const unsigned char *)"B",1}};
    TfInterpreterResult *result=NULL;
    TfRuntimeFault fault;
    CHECK(tf_interpret_entry(&result,p.verified,input,2,NULL,NULL,&fault)==TF_OK);
    CHECK(tf_interpreter_result_count(result)==4);
    line(result,0,"first:A",7); line(result,1,"B",1); line(result,2,"B",1); line(result,3,"A",1);
    tf_interpreter_result_destroy(result); release(p);

    p=compile("tests/fixtures/types/interpreter-fault.tfd");
    CHECK(tf_interpret_entry(&result,p.verified,input,1,NULL,NULL,&fault)==TF_SOURCE_ERROR);
    CHECK(result==NULL && fault.code==TF_RUNTIME_INSUFFICIENT_INPUT && spells(p,fault.span,"input second: Text;"));
    CHECK(tf_interpret_entry(&result,p.verified,input,2,NULL,NULL,&fault)==TF_SOURCE_ERROR);
    CHECK(result==NULL && fault.code==TF_RUNTIME_ZERO_DIVISOR && spells(p,fault.span,"div"));
    release(p);
}

static void recursion(void) {
    Program p=compile("tests/fixtures/types/interpreter-recursion.tfd");
    TfInterpreterResult *result=NULL;
    TfInterpreterLimits limits=tf_interpreter_default_limits();
    TfRuntimeFault fault;
    limits.max_call_depth=100;
    CHECK(tf_interpret_entry(&result,p.verified,NULL,0,&limits,NULL,&fault)==TF_SOURCE_ERROR);
    CHECK(result==NULL && fault.code==TF_RUNTIME_RESOURCE && spells(p,fault.span,"recurse(x + 1)"));
    CHECK(fault.frame_count==TF_INTERPRETER_FAULT_FRAMES && fault.omitted_frames==100-TF_INTERPRETER_FAULT_FRAMES);
    CHECK(fault.frames[0].function==tf_verified_core_data(p.verified)->entry && fault.frames[0].invocation==0);
    limits.max_steps=2;
    CHECK(tf_interpret_entry(&result,p.verified,NULL,0,&limits,NULL,&fault)==TF_SOURCE_ERROR);
    CHECK(result==NULL && fault.code==TF_RUNTIME_RESOURCE && fault.frame_count==2);
    release(p);
}

static void prelude(void) {
    const struct { const char *bytes; size_t length; } expected[]={
#define EXPECT(text) {text,sizeof(text)-1}
        EXPECT("-2"),EXPECT("-1"),EXPECT("0"),EXPECT("3"),EXPECT("123"),EXPECT("7"),EXPECT("0"),
        EXPECT("8"),EXPECT("none"),EXPECT("-9223372036854775808"),EXPECT("none"),EXPECT("none"),
        EXPECT("none"),EXPECT("-2"),EXPECT("1.0000000000000001e-1"),EXPECT("4.9406564584124654e-324"),
        EXPECT("4"),EXPECT("\xf0\x9f\x99\x82"),EXPECT("\0b"),EXPECT(""),EXPECT("none"),EXPECT("false")
#undef EXPECT
    };
    Program p=compile("tests/fixtures/types/interpreter-prelude.tfd");
    TfInterpreterResult *result=NULL;
    TfRuntimeFault fault;
    CHECK(tf_interpret_entry(&result,p.verified,NULL,0,NULL,NULL,&fault)==TF_OK);
    CHECK(tf_interpreter_result_count(result)==sizeof expected/sizeof *expected && fault.code==TF_RUNTIME_NONE);
    for (size_t i=0; i<sizeof expected/sizeof *expected; ++i) line(result,i,expected[i].bytes,expected[i].length);
    tf_interpreter_result_destroy(result); release(p);
}

static void ordered_faults(void) {
    static const struct { const char *path; TfRuntimeCode code; const char *origin,*context; } cases[]={
        {"tests/fixtures/types/interpreter-late-guard.tfd",TF_RUNTIME_ZERO_DIVISOR,"div","guard 1 "},
        {"tests/fixtures/types/interpreter-late-emission.tfd",TF_RUNTIME_ZERO_DIVISOR,"div","emit int_text"},
        {"tests/fixtures/types/interpreter-candidate-order.tfd",TF_RUNTIME_OVERFLOW,"+","next a"},
        {"tests/fixtures/types/interpreter-negative-power.tfd",TF_RUNTIME_NEGATIVE_EXPONENT,"^","int_text"}
    };
    for (size_t i=0; i<sizeof cases/sizeof *cases; ++i) {
        Program p=compile(cases[i].path);
        TfInterpreterResult *result=NULL;
        TfRuntimeFault fault;
        CHECK(tf_interpret_entry(&result,p.verified,NULL,0,NULL,NULL,&fault)==TF_SOURCE_ERROR);
        CHECK(result==NULL && fault.code==cases[i].code && spells(p,fault.span,cases[i].origin));
        const TfSpan *span=tf_core_span(tf_verified_core_data(p.verified),fault.span);
        const size_t n=strlen(cases[i].context);
        bool found=false;
        for (size_t start=span->start>80 ? span->start-80 : 0; start+n<=span->start; ++start)
            if (memcmp(span->source->bytes+start,cases[i].context,n)==0) found=true;
        CHECK(found);
        release(p);
    }
}

int main(void) {
    const size_t allocations=transitions();
    staging(); recursion(); prelude(); ordered_faults();
    printf("Core interpreter slice: %zu checks, %zu injected allocation failures\n",checks,allocations);
    return 0;
}
