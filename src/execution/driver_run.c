#include "driver_run.h"
#include "host_io.h"
#include "ternfold/trace_artifact.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static bool report(FILE *stream,const TfDiagnosticBag *front, TfDiagnosticFormat format, const char *code,
                   const char *message, const TfDiagnosticLabel *labels, size_t count) {
    TfDiagnosticBag *bag=NULL;
    bool ok=false;
    if (tf_diagnostic_bag_create(&bag,100,NULL)!=TF_OK) return false;
    /* The required failure is retained even if earlier warnings fill the cap. */
    const TfDiagnosticRecord record={.code=code,.severity=TF_DIAG_ERROR,
        .message={message,strlen(message)},.labels=labels,.label_count=count};
    if (tf_diagnostic_bag_add(bag,&record)!=TF_OK) goto done;
    for (size_t i=0; i<tf_diagnostic_bag_count(front); ++i)
        if (tf_diagnostic_bag_add(bag,tf_diagnostic_bag_get(front,i))!=TF_OK) goto done;
    ok=tf_diagnostic_bag_write(stream,bag,format,TF_DIAG_COLOR_NEVER)==TF_OK && fflush(stream)==0;
done:
    tf_diagnostic_bag_destroy(bag);
    return ok;
}

int tf_driver_missing_entry(const TfDiagnosticBag *frontend, TfDiagnosticFormat format,
                            const char *message, const char **failure) {
    if (!report(stderr,frontend,format,"E208",message,NULL,0)) *failure="cannot write entry diagnostic";
    return 1;
}

bool tf_driver_report_runtime_fault(FILE *stream,const TfCoreProgramData *data,const TfDiagnosticBag *front,
                                    TfDiagnosticFormat format,TfRuntimeFault fault) {
    static const struct { const char *code,*message; } messages[]={
        {"R004","unexpected interpreter state"}, {"R001","arithmetic overflow or nonfinite result"},
        {"R002","division or remainder by zero"}, {"R003","execution resource limit exceeded"},
        {"R004","unexpected interpreter state"}, {"R005","negative exponent for Int power"},
        {"R006","not enough input lines"}
    };
    const size_t index=(size_t)fault.code < sizeof messages/sizeof *messages ? (size_t)fault.code : 0;
    TfDiagnosticLabel labels[TF_INTERPRETER_FAULT_FRAMES+1];
    size_t used=0;
    const TfSpan *primary=tf_core_span(data,fault.span);
    if (primary!=NULL) labels[used++]=(TfDiagnosticLabel){*primary,true,{NULL,0}};
    for (size_t i=fault.frame_count; i>0; --i) {
        const TfSpan *call=tf_core_span(data,fault.frames[i-1].invocation);
        if (call==NULL || (primary!=NULL && call->source==primary->source &&
                           call->start==primary->start && call->end==primary->end)) continue;
        labels[used]=(TfDiagnosticLabel){*call,used==0,{"called here",sizeof "called here"-1}};
        ++used;
    }
    return report(stream,front,format,messages[index].code,messages[index].message,labels,used);
}

int tf_run_verified_traced(const TfVerifiedCore *program, const TfDiagnosticBag *front,
                    const char *input_path, TfDiagnosticFormat format, const char **failure,
                    const char *trace_path) {
    const TfCoreProgramData *data=tf_verified_core_data(program);
    const TfCoreFunction *entry=tf_core_function(data,data->entry);
    if (entry==NULL)
        return tf_driver_missing_entry(front,format,"run requires a root pub fn main() -> List[Text]",failure);
    const TfCoreBlock *body=tf_core_block(entry,entry->body);
    bool needs_input=false;
    if (entry->kind==TF_CORE_FUNCTION_ENTRY_ONLY)
        for (size_t i=0; i<body->let_count; ++i)
            if (body->lets[i].operation.kind==TF_CORE_OP_INPUT_AT) { needs_input=true; break; }
    TfInputSnapshot input={0};
    if (needs_input) {
        FILE *stream=stdin;
        if (input_path!=NULL) stream=fopen(input_path,"rb");
#ifdef _WIN32
        else if (_setmode(_fileno(stdin),_O_BINARY)==-1) stream=NULL;
#endif
        if (stream==NULL) { *failure="cannot open the input snapshot"; return 2; }
        const TfStatus captured=tf_input_snapshot_read(&input,stream,tf_interpreter_default_limits().max_bytes);
        const bool closed=stream==stdin || fclose(stream)==0;
        if (captured!=TF_OK || !closed) {
            tf_input_snapshot_destroy(&input);
            *failure=captured==TF_SOURCE_ERROR ? "input snapshot is not valid UTF-8" :
                "cannot capture input (read error or resource limit)";
            return 2;
        }
    }
    TfInterpreterResult *result=NULL;
    TfRuntimeFault fault;
    TfTraceRecord *records=trace_path ? calloc(TF_TRACE_IO_MAX_RECORDS,sizeof *records) : NULL;
    if (trace_path && !records) {
        tf_input_snapshot_destroy(&input); *failure="cannot allocate trace buffer"; return 2;
    }
    TfTraceRecorder trace;
    (void)tf_trace_recorder_init(&trace,records,trace_path ? TF_TRACE_IO_MAX_RECORDS : 0);
    const TfStatus status=tf_interpret_entry_traced(&result,program,input.lines,input.count,NULL,NULL,&fault,
                                                   trace_path ? &trace : NULL);
    tf_input_snapshot_destroy(&input);
    bool trace_failed=false;
    if (trace_path) {
        TfTraceOrigin *origins=data->span_count<=32768 ? calloc(data->span_count+1,sizeof *origins) : NULL;
        if (!origins) trace_failed=true;
        else {
            for (size_t i=0;i<data->span_count;++i) {
                const TfSpan *s=&data->spans[i];
                TfLocation start={0},end={0};
                if (!tf_source_location(s->source,s->start,&start) || !tf_source_location(s->source,s->end,&end)) {
                    trace_failed=true; break;
                }
                origins[i]=(TfTraceOrigin){(const unsigned char *)s->source->name,strlen(s->source->name),
                                          s->start,s->end,start.line,start.column,end.line,end.column};
            }
            if (!trace_failed) trace_failed=tf_trace_artifact_write(trace_path,&trace,origins,data->span_count)!=TF_TRACE_IO_OK;
            free(origins);
        }
    }
    free(records);
    if (status==TF_SOURCE_ERROR) {
        if (!tf_driver_report_runtime_fault(stderr,data,front,format,fault)) *failure="cannot write runtime diagnostic";
        if (trace_failed) *failure="cannot write new trace artifact and source map";
        return *failure ? 2 : 3;
    }
    if (status!=TF_OK) { *failure="cannot execute verified Core on this host"; return 2; }
    if (tf_diagnostic_bag_write(stderr,front,format,TF_DIAG_COLOR_NEVER)!=TF_OK || fflush(stderr)!=0)
        *failure="cannot write diagnostics";
    else if (tf_interpreter_output_write(stdout,result)!=TF_OK || fflush(stdout)!=0)
        *failure="cannot write program output";
    tf_interpreter_result_destroy(result);
    if (trace_failed) *failure="cannot write new trace artifact and source map";
    return *failure==NULL ? 0 : 2;
}

int tf_run_verified(const TfVerifiedCore *program, const TfDiagnosticBag *front,
                    const char *input_path, TfDiagnosticFormat format, const char **failure) {
    return tf_run_verified_traced(program,front,input_path,format,failure,NULL);
}
