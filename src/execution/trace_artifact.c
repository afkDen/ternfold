#include "ternfold/trace_artifact.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static char *companion(const char *path) {
    if (!path) return NULL;
    const size_t length=strlen(path);
    if (length==0 || length>4096) return NULL;
    char *out=malloc(length+sizeof ".origins");
    if (out) { memcpy(out,path,length); memcpy(out+length,".origins",sizeof ".origins"); }
    return out;
}
static bool escaped(FILE *out,const unsigned char *bytes,size_t length) {
    for (size_t i=0;i<length;++i) {
        if (bytes[i]>=32 && bytes[i]<=126 && bytes[i]!='\\' && bytes[i]!='"') {
            if (fputc(bytes[i],out)==EOF) return false;
        } else if (fprintf(out,"\\x%02X",(unsigned)bytes[i])<0) return false;
    }
    return true;
}
TfTraceIoStatus tf_trace_artifact_write(const char *path,const TfTraceRecorder *recorder,
                                       const TfTraceOrigin *origins,size_t count) {
    if (count>32768 || (count && !origins)) return TF_TRACE_IO_LIMIT;
    size_t bytes=0;
    for (size_t i=0;i<count;++i) {
        const TfTraceOrigin *o=&origins[i];
        if (o->path_length>4096 || (o->path_length && !o->path) || o->start>o->end ||
            o->path_length>(TF_TRACE_IO_MAX_BYTES-bytes-256)/4) return TF_TRACE_IO_LIMIT;
        bytes+=o->path_length*4+256;
        if (bytes>TF_TRACE_IO_MAX_BYTES-256) return TF_TRACE_IO_LIMIT;
    }
    TfTraceSerialized serialized={0};
    TfTraceIoStatus status=tf_trace_serialize(&serialized,recorder,NULL,NULL);
    if (status!=TF_TRACE_IO_OK) return status;
    char *name=companion(path);
    if (!name) { tf_trace_serialized_destroy(&serialized); return TF_TRACE_IO_ALLOCATION; }
    FILE *trace=fopen(path,"wbx"), *map=NULL;
    if (trace) map=fopen(name,"wbx");
    if (!trace || !map) status=TF_TRACE_IO_STREAM;
    else {
        bool ok=fputs("TFTRACE ORIGINS 1\n",map)!=EOF;
        for (size_t i=0;i<count && ok;++i) {
            const TfTraceOrigin *o=&origins[i];
            ok=fprintf(map,"s%zu source=\"",i+1)>=0 && escaped(map,o->path,o->path_length) &&
               fprintf(map,"\" bytes=%zu..%zu location=%zu:%zu..%zu:%zu\n",
                       o->start,o->end,o->start_line,o->start_column,o->end_line,o->end_column)>=0;
        }
        if (!ok || fwrite(serialized.bytes,1,serialized.length,trace)!=serialized.length)
            status=TF_TRACE_IO_STREAM;
    }
    if (map && fclose(map)!=0) status=TF_TRACE_IO_STREAM;
    if (trace && fclose(trace)!=0) status=TF_TRACE_IO_STREAM;
    free(name); tf_trace_serialized_destroy(&serialized);
    return status;
}
TfTraceIoStatus tf_trace_artifact_replay(FILE *out,const char *path) {
    static const char *const kinds[]={"?","begin","writer","guard","private-emission","accept","reject","fault"};
    if (!out || !path) return TF_TRACE_IO_INVALID;
    FILE *input=fopen(path,"rb");
    if (!input) return TF_TRACE_IO_STREAM;
    TfTraceReplay replay={0};
    TfTraceIoStatus status=tf_trace_replay_read(&replay,input,NULL,NULL);
    if (fclose(input)!=0) status=TF_TRACE_IO_STREAM;
    if (status!=TF_TRACE_IO_OK) { tf_trace_replay_destroy(&replay); return status; }
    bool ok=fprintf(out,"Trace: %zu events, %zu invocations, %s\n",replay.recorder.count,
                    replay.invocation_count,replay.unknown ? "UNKNOWN (truncated/incomplete)" : "complete")>=0;
    for (size_t i=0;i<replay.recorder.count && ok;++i) {
        const TfTraceRecord *r=&replay.recorder.records[i];
        ok=fprintf(out,"#%" PRIu64 " step=%" PRIu64 " parent=%" PRIu64 " %s f%zu s%zu item=%zu \"",
                   r->sequence_id,r->event.invocation_id,r->event.parent_invocation_id,
                   kinds[r->event.kind],r->event.function_id,r->event.span_id,r->event.ordinal)>=0 &&
           escaped(out,r->preview,r->preview_length) &&
           fputs(r->preview_truncated ? "\" ...\n" : "\"\n",out)!=EOF;
    }
    char *name=companion(path);
    input=name ? fopen(name,"rb") : NULL;
    if (input) {
        ok=ok && fputs("Companion source map (descriptive, not authenticated):\n",out)!=EOF;
        size_t count=0; int c;
        while (ok && (c=fgetc(input))!=EOF) {
            if (++count>TF_TRACE_IO_MAX_BYTES) { status=TF_TRACE_IO_LIMIT; break; }
            const unsigned char byte=(unsigned char)c;
            if (byte=='\n') ok=fputc('\n',out)!=EOF;
            else ok=byte>=32 && byte<=126 ? fputc(byte,out)!=EOF : escaped(out,&byte,1);
        }
        const bool failed=ferror(input)!=0;
        const bool closed=fclose(input)==0;
        if (failed || !closed) status=TF_TRACE_IO_STREAM;
    } else ok=ok && fputs("Source map unavailable; numeric origins only.\n",out)!=EOF;
    free(name); tf_trace_replay_destroy(&replay);
    if (!ok || fflush(out)!=0) status=TF_TRACE_IO_STREAM;
    return status;
}
