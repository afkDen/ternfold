#include "host_io.h"
#include "../internal.h"

#include <stdlib.h>
#include <string.h>

void tf_input_snapshot_destroy(TfInputSnapshot *snapshot) {
    if (snapshot==NULL) return;
    free(snapshot->lines); free(snapshot->bytes);
    *snapshot=(TfInputSnapshot){0};
}

TfStatus tf_input_snapshot_read(TfInputSnapshot *out, FILE *input, size_t max_bytes) {
    if (out==NULL) return TF_HOST_ERROR;
    *out=(TfInputSnapshot){0};
    if (input==NULL) return TF_HOST_ERROR;
    unsigned char block[4096];
    size_t length=0, capacity=0, lines=0;
    TfStatus status=TF_HOST_ERROR;
    for (;;) {
        const size_t read=fread(block,1,sizeof block,input);
        if (read>max_bytes-length) goto fail;
        const size_t needed=length+read;
        if (needed>capacity) {
            size_t next=capacity!=0 ? capacity : sizeof block;
            if (next>max_bytes) next=max_bytes;
            while (next<needed) next=next>max_bytes/2 ? max_bytes : next*2;
            void *bytes=realloc(out->bytes,next);
            if (bytes==NULL) goto fail;
            out->bytes=bytes; capacity=next;
        }
        if (read!=0) memcpy(out->bytes+length,block,read);
        length=needed;
        if (read<sizeof block) {
            if (ferror(input) || !feof(input)) goto fail;
            break;
        }
    }
    for (size_t i=0,next; i<length; i=next) {
        uint32_t scalar;
        if (!tf_decode(out->bytes,length,i,&scalar,&next)) { status=TF_SOURCE_ERROR; goto fail; }
        if (scalar=='\n') ++lines;
    }
    if (length!=0 && out->bytes[length-1]!='\n') ++lines;
    if (lines==0) return TF_OK;
    if (lines>(max_bytes-length)/sizeof *out->lines) goto fail;
    /* If unused byte capacity would exceed the combined logical budget, give
     * it back before allocating descriptors. */
    if (capacity>max_bytes-lines*sizeof *out->lines) {
        void *smaller=realloc(out->bytes,length);
        if (smaller==NULL) goto fail;
        out->bytes=smaller;
    }
    out->lines=malloc(lines*sizeof *out->lines);
    if (out->lines==NULL) goto fail;
    size_t start=0;
    for (size_t i=0; i<length; ++i) {
        if (out->bytes[i]!='\n') continue;
        const size_t end=i>start && out->bytes[i-1]=='\r' ? i-1 : i;
        out->lines[out->count++]=(TfCoreText){out->bytes+start,end-start};
        start=i+1;
    }
    if (start<length) out->lines[out->count++]=(TfCoreText){out->bytes+start,length-start};
    return TF_OK;
fail:
    tf_input_snapshot_destroy(out);
    return status;
}

TfStatus tf_interpreter_output_write(FILE *stream, const TfInterpreterResult *result) {
    if (stream==NULL || result==NULL) return TF_HOST_ERROR;
    const size_t lines=tf_interpreter_result_count(result);
    for (size_t i=0; i<lines; ++i) {
        TfCoreText line;
        if (!tf_interpreter_result_line(result,i,&line) ||
            (line.length!=0 && fwrite(line.bytes,1,line.length,stream)!=line.length) ||
            fputc('\n',stream)==EOF) return TF_HOST_ERROR;
    }
    return ferror(stream) ? TF_HOST_ERROR : TF_OK;
}
