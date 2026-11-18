#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "ternfold/core_lower.h"
#include "ternfold/interpreter.h"
#include "../src/repl_resolve.h"
#include "../src/typed_signatures.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define FD _fileno
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#else
#include <unistd.h>
#define FD fileno
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#endif

static size_t failed_trace_allocations;
static void *fail_trace_calloc(size_t count,size_t width) {
    (void)count; (void)width; ++failed_trace_allocations; return NULL;
}
/* Affect only the host trace-buffer allocation, not the compiler/VM allocator. */
#define calloc fail_trace_calloc
#include "../src/execution/driver_run.c"
#undef calloc

static size_t checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"trace execution:%d: %s\n",__LINE__,#x); exit(1); } } while (0)
typedef struct {
    TfSource source;
    TfSourceManager *manager;
    TfResolution *resolution;
    TfTypedSignatures *signatures;
    TfTypedProgram *typed;
    TfMachineProgram *machine;
    TfCoreProgram *core;
    TfVerifiedCore *verified;
} Program;

static void prepare(Program *p,const char *text) {
    *p=(Program){0};
    CHECK(tf_source_init(&p->source,"<trace>",text,strlen(text),NULL)==TF_OK);
    CHECK(tf_source_manager_create(&p->manager,NULL)==TF_OK);
    CHECK(tf_resolve_virtual(&p->resolution,p->manager,&p->source,NULL,NULL,NULL)==TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(p->resolution))==TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&p->signatures,p->resolution,100,NULL)==TF_OK);
    CHECK(tf_check(&p->typed,p->resolution,p->signatures,100,NULL)==TF_OK);
    if (tf_diagnostic_bag_has_errors(tf_typed_program_diagnostics(p->typed)))
        (void)tf_diagnostic_bag_write(stderr,tf_typed_program_diagnostics(p->typed),TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
    CHECK(tf_machine_program_build(&p->machine,p->typed,100,NULL)==TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_machine_program_phase(p->machine))==TF_DIAG_PHASE_VALID);
    CHECK(tf_core_lower(&p->core,p->machine,NULL,NULL)==TF_OK);
    TfCoreVerifyIssue issue={0};
    const TfStatus status=tf_core_verify(&p->verified,p->core,NULL,&issue);
    if (status!=TF_OK) fprintf(stderr,"verify %u f%zu b%zu v%zu\n",(unsigned)issue.code,issue.function,issue.block,issue.value);
    CHECK(status==TF_OK);
}
static void destroy(Program *p) {
    tf_verified_core_destroy(p->verified); tf_core_program_destroy(p->core);
    tf_machine_program_destroy(p->machine); tf_typed_program_destroy(p->typed);
    tf_signatures_destroy(p->signatures); tf_resolution_destroy(p->resolution);
    tf_source_manager_destroy(p->manager); tf_source_destroy(&p->source);
}
static void same_results(TfInterpreterResult *a,TfInterpreterResult *b) {
    CHECK(tf_interpreter_result_count(a)==tf_interpreter_result_count(b));
    for (size_t i=0;i<tf_interpreter_result_count(a);++i) {
        TfCoreText x={0},y={0};
        CHECK(tf_interpreter_result_line(a,i,&x) && tf_interpreter_result_line(b,i,&y));
        CHECK(x.length==y.length && memcmp(x.bytes,y.bytes,x.length)==0);
    }
}
static void same_fault(TfRuntimeFault a,TfRuntimeFault b) {
    CHECK(a.code==b.code && a.span==b.span && a.frame_count==b.frame_count && a.omitted_frames==b.omitted_frames);
    for (size_t i=0;i<a.frame_count;++i)
        CHECK(a.frames[i].function==b.frames[i].function && a.frames[i].invocation==b.frames[i].invocation);
}
static TfStatus compare(Program *p,TfTraceRecorder *trace,const TfInterpreterLimits *limits) {
    TfInterpreterResult *plain=NULL,*observed=NULL;
    TfRuntimeFault a,b;
    const TfStatus x=tf_interpret_entry(&plain,p->verified,NULL,0,limits,NULL,&a);
    const TfStatus y=tf_interpret_entry_traced(&observed,p->verified,NULL,0,limits,NULL,&b,trace);
    CHECK(x==y);
    same_fault(a,b);
    if (x==TF_OK) same_results(plain,observed);
    else CHECK(plain==NULL && observed==NULL);
    tf_interpreter_result_destroy(plain); tf_interpreter_result_destroy(observed);
    return x;
}

static const char prefix[]=
    "fn divide(n: Int) -> Int { n div 0 }\n"
    "machine M(d: Int) yields Int rejects Text {\n"
    " state a: Int = 1; state b: Int = 2;\n";
static void scenario(const char *writers,const char *guards,const char *emissions,int input,
                     const TfTraceEventKind *expected,size_t count,TfStatus status) {
    char text[4096];
    const int n=snprintf(text,sizeof text,"%s%s%s%s}\n"
        "pub fn main() -> List[Text] { match step(M(), %d) {"
        " Decision::Accept(s, es) => { map(es, int_text) },"
        " Decision::Reject(s, reason) => { [reason] } } }",
        prefix,writers,guards,emissions,input);
    CHECK(n>0 && (size_t)n<sizeof text);
    Program p; prepare(&p,text);
    TfTraceRecord records[128];
    TfTraceRecorder trace;
    CHECK(tf_trace_recorder_init(&trace,records,128));
    CHECK(compare(&p,&trace,NULL)==status && trace.count==count && !trace.truncated);
    const TfCoreProgramData *data=tf_verified_core_data(p.verified);
    CHECK(data->trace_machine_count==1 && data->trace_point_count>=2);
    for (size_t i=0;i<count;++i) {
        CHECK(records[i].event.kind==expected[i]);
        CHECK(records[i].event.invocation_id==1 && records[i].event.parent_invocation_id==0);
        CHECK(records[i].sequence_id==i+1 && records[i].event.span_id>0);
        CHECK(tf_core_trace_machine(data,records[i].event.function_id)!=NULL);
    }
    for (size_t cap=0;cap<3;++cap) {
        CHECK(tf_trace_recorder_init(&trace,records,cap));
        const TfStatus observed=compare(&p,&trace,NULL);
        if (observed!=status || trace.count!=cap || trace.truncated!=(count>cap))
            fprintf(stderr,"capacity %zu result=%u expected=%u count=%zu omitted=%llu\n",
                    cap,(unsigned)observed,(unsigned)status,trace.count,(unsigned long long)trace.omitted_events);
        CHECK(observed==status && trace.count==cap && trace.truncated==(count>cap));
        CHECK(trace.omitted_events==count-cap);
    }
    for (uint64_t steps=0;steps<130;steps+=7) {
        TfInterpreterLimits limits=tf_interpreter_default_limits(); limits.max_steps=steps;
        CHECK(tf_trace_recorder_init(&trace,records,128));
        (void)compare(&p,&trace,&limits);
    }
    destroy(&p);
}
static void metadata_rejected(const TfCoreProgramData *data) {
    TfCoreProgram *copy=NULL; TfVerifiedCore *verified=NULL;
    CHECK(tf_core_program_copy(&copy,data,NULL)==TF_OK);
    TfCoreVerifyIssue issue={0};
    CHECK(tf_core_verify(&verified,copy,NULL,&issue)==TF_SOURCE_ERROR && verified==NULL);
    CHECK(issue.code==TF_CORE_VERIFY_OBSERVATION);
    tf_core_program_destroy(copy);
}
static void metadata(void) {
    Program p;
    prepare(&p,"machine M(d: Int) yields Int rejects Text { state a: Int = 1;"
               " next a = a; guard d > 0 else \"no\"; emit next.a; }"
               " pub fn main() -> List[Text] { [] }");
    const TfCoreProgramData *original=tf_verified_core_data(p.verified);
    CHECK(original->trace_point_count==3);
    TfCoreTracePoint points[3];
    for (unsigned trial=0;trial<12;++trial) {
        memcpy(points,original->trace_points,sizeof points);
        TfCoreProgramData data=*original; data.trace_points=points;
        switch (trial) {
        case 0: points[0].function=0; break;
        case 1: points[0].block=SIZE_MAX; break;
        case 2: points[0].after_let=SIZE_MAX; break;
        case 3: points[0].value=SIZE_MAX; break;
        case 4: points[0].span=0; break;
        case 5: points[0].ordinal=0; break;
        case 6: points[0].ordinal=SIZE_MAX; break;
        case 7: points[0].kind=(TfCoreTraceKind)99; break;
        case 8: data.trace_point_count=2; break;
        case 9: points[1]=points[0]; break;
        case 10: points[0].value=points[1].value; break; /* future guard result */
        case 11: { TfCoreTracePoint temp=points[0]; points[0]=points[2]; points[2]=temp; break; }
        }
        metadata_rejected(&data);
    }
    TfCoreProgramData data=*original;
    TfCoreTraceMachine machine=*data.trace_machines;
    data.trace_machines=&machine; machine.machine=0; metadata_rejected(&data);
    machine=*original->trace_machines; machine.guard_count=SIZE_MAX; metadata_rejected(&data);
    data=*original; data.trace_machine_count=0; metadata_rejected(&data);
    destroy(&p);
}
static void nesting(void) {
    Program p;
    prepare(&p,
        "machine Inner(d: Int) yields Int rejects Text { state a: Int = 1; next a = a+d; emit next.a; }"
        "fn helper(d: Int) -> Int { match step(Inner(),d) { Decision::Accept(s, es) => { s.a },"
        " Decision::Reject(s, reason) => { 0 } } }"
        "machine Outer(d: Int) yields Int rejects Text { state a: Int = 2;"
        " next a = helper(d); emit next.a; }"
        "pub fn main() -> List[Text] { match step(Outer(),3) { Decision::Accept(s, es) => { map(es,int_text) },"
        " Decision::Reject(s, r) => { [r] } } }");
    TfTraceRecord records[32]; TfTraceRecorder trace;
    CHECK(tf_trace_recorder_init(&trace,records,32) && compare(&p,&trace,NULL)==TF_OK);
    CHECK(trace.count==8 && records[0].event.invocation_id==1 &&
          records[1].event.invocation_id==2 && records[1].event.parent_invocation_id==1);
    CHECK(records[4].event.kind==TF_TRACE_ACCEPT && records[4].event.invocation_id==2);
    CHECK(records[5].event.kind==TF_TRACE_WRITER && records[5].event.invocation_id==1);
    destroy(&p);
}
static char *capture(FILE *stream) {
    CHECK(fflush(stream)==0 && fseek(stream,0,SEEK_END)==0);
    const long count=ftell(stream);
    CHECK(count>=0 && count<65536 && fseek(stream,0,SEEK_SET)==0);
    char *bytes=malloc((size_t)count+1); CHECK(bytes);
    CHECK(fread(bytes,1,(size_t)count,stream)==(size_t)count);
    bytes[count]=0; return bytes;
}
static void trace_allocation_noninterference(void) {
    const char *sources[]={
        "pub fn main() -> List[Text] { [int_text(2 ^ 3)] }",
        "pub fn main() -> List[Text] { let n = 1 div 0; [int_text(n)] }"
    };
    for (size_t i=0;i<2;++i) {
        Program p; prepare(&p,sources[i]);
        char *plain_out=NULL,*plain_err=NULL;
        for (unsigned traced=0;traced<2;++traced) {
            FILE *output=tmpfile(),*errors=tmpfile(); CHECK(output && errors);
            CHECK(fflush(stdout)==0 && fflush(stderr)==0);
            const int saved_out=DUP(FD(stdout)),saved_err=DUP(FD(stderr));
            CHECK(saved_out>=0 && saved_err>=0);
            CHECK(DUP2(FD(output),FD(stdout))>=0 && DUP2(FD(errors),FD(stderr))>=0);
            const char *failure=NULL;
            const int code=tf_run_verified_traced(p.verified,tf_typed_program_diagnostics(p.typed),
                NULL,TF_DIAG_TEXT,&failure,traced ? "build/unused-allocation.trace" : NULL);
            const bool flushed=fflush(stdout)==0 && fflush(stderr)==0;
            CHECK(DUP2(saved_out,FD(stdout))>=0 && DUP2(saved_err,FD(stderr))>=0);
            CHECK(CLOSE(saved_out)==0 && CLOSE(saved_err)==0 && flushed);
            char *out=capture(output),*err=capture(errors);
            if (!traced) {
                CHECK(code==(i ? 3 : 0) && !failure);
                plain_out=out; plain_err=err;
            } else {
                CHECK(code==2 && failure && strstr(failure,"trace"));
                CHECK(strcmp(out,plain_out)==0 && strcmp(err,plain_err)==0);
                CHECK(i ? strstr(err,"R002")!=NULL : strcmp(out,"8\n")==0);
                free(out); free(err); free(plain_out); free(plain_err);
            }
            CHECK(fclose(output)==0 && fclose(errors)==0);
        }
        destroy(&p);
    }
    CHECK(failed_trace_allocations==2);
}
int main(void) {
    static const TfTraceEventKind accepted[]={TF_TRACE_STEP_BEGIN,TF_TRACE_WRITER,TF_TRACE_WRITER,TF_TRACE_GUARD,TF_TRACE_EMISSION,TF_TRACE_ACCEPT};
    static const TfTraceEventKind rejected[]={TF_TRACE_STEP_BEGIN,TF_TRACE_WRITER,TF_TRACE_WRITER,TF_TRACE_GUARD,TF_TRACE_REJECT};
    static const TfTraceEventKind candidate[]={TF_TRACE_STEP_BEGIN,TF_TRACE_FAULT};
    static const TfTraceEventKind guard[]={TF_TRACE_STEP_BEGIN,TF_TRACE_WRITER,TF_TRACE_WRITER,TF_TRACE_FAULT};
    static const TfTraceEventKind reason[]={TF_TRACE_STEP_BEGIN,TF_TRACE_WRITER,TF_TRACE_WRITER,TF_TRACE_GUARD,TF_TRACE_FAULT};
    static const TfTraceEventKind emission[]={TF_TRACE_STEP_BEGIN,TF_TRACE_WRITER,TF_TRACE_WRITER,TF_TRACE_GUARD,TF_TRACE_EMISSION,TF_TRACE_FAULT};
    const char *writers=" next b = next.a * 2; next a = a + d;";
    scenario(writers," guard next.a >= 0 else \"negative\";"," emit next.b;",3,accepted,6,TF_OK);
    scenario(writers," guard next.a >= 0 else \"negative\";"," emit next.b;",-5,rejected,5,TF_OK);
    scenario(" next b = next.a * 2; next a = divide(d);",""," emit next.b;",3,candidate,2,TF_SOURCE_ERROR);
    scenario(writers," guard divide(d) > 0 else \"negative\";"," emit next.b;",3,guard,4,TF_SOURCE_ERROR);
    scenario(writers," guard false else int_text(divide(d));"," emit next.b;",3,reason,5,TF_SOURCE_ERROR);
    scenario(writers," guard true else \"negative\";"," emit next.b; emit divide(d);",3,emission,6,TF_SOURCE_ERROR);
    nesting(); metadata(); trace_allocation_noninterference();
    printf("trace execution: %zu assertions; ordered events, exact faults, truncation, budgets and malformed metadata passed\n",checks);
    return 0;
}
