#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
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
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"native trace allocation:%d: %s\n",__LINE__,#x); exit(1); } } while (0)
static size_t failures;
static void *fail_trace_calloc(size_t count,size_t width) {
    (void)count; (void)width; ++failures; return NULL;
}
#define calloc fail_trace_calloc
#define main generated_main
#ifdef TF_TEST_TRACE_LANGUAGE_FAULT
#include "c_trace_fault_fixture.c"
#else
#include "c_output_fixture.c"
#endif
#undef main
#undef calloc
static char *read_all(FILE *stream) {
    CHECK(fflush(stream)==0 && fseek(stream,0,SEEK_END)==0);
    const long n=ftell(stream); CHECK(n>=0 && n<65536 && fseek(stream,0,SEEK_SET)==0);
    char *bytes=malloc((size_t)n+1); CHECK(bytes);
    CHECK(fread(bytes,1,(size_t)n,stream)==(size_t)n); bytes[n]=0;
    return bytes;
}
int main(void) {
    char *plain_out=NULL,*plain_err=NULL;
    for (unsigned traced=0;traced<2;++traced) {
        FILE *out=tmpfile(),*err=tmpfile(); CHECK(out && err);
        CHECK(fflush(stdout)==0 && fflush(stderr)==0);
        const int saved_out=DUP(FD(stdout)),saved_err=DUP(FD(stderr));
        CHECK(saved_out>=0 && saved_err>=0);
        CHECK(DUP2(FD(out),FD(stdout))>=0 && DUP2(FD(err),FD(stderr))>=0);
        char *args[]={"native-trace-test","--trace","build/unused-native.trace",NULL};
        const int status=generated_main(traced ? 3 : 1,args);
        const bool flushed=fflush(stdout)==0 && fflush(stderr)==0;
        CHECK(DUP2(saved_out,FD(stdout))>=0 && DUP2(saved_err,FD(stderr))>=0);
        CHECK(CLOSE(saved_out)==0 && CLOSE(saved_err)==0 && flushed);
        char *output=read_all(out),*errors=read_all(err);
        if (!traced) {
#ifdef TF_TEST_TRACE_LANGUAGE_FAULT
            CHECK(status==3 && !*output && strstr(errors,"error[R002]"));
#else
            CHECK(status==0 && strcmp(output,"Hello, Ternfold.\n")==0 && !*errors);
#endif
            plain_out=output; plain_err=errors;
        } else {
            static const char suffix[]="cannot write new trace artifact and source map\n";
            CHECK(status==2 && strcmp(output,plain_out)==0);
            CHECK(strncmp(errors,plain_err,strlen(plain_err))==0);
            CHECK(strcmp(errors+strlen(plain_err),suffix)==0);
            free(output); free(errors); free(plain_out); free(plain_err);
        }
        CHECK(fclose(out)==0 && fclose(err)==0);
    }
    CHECK(failures==1);
    puts("Native tracing: buffer allocation failure preserves language output/fault before observation error");
    return 0;
}
