#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "ternfold_rt_io.h"
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

#define CHECK(ok) do { if (!(ok)) { fprintf(stderr,"test_c_output_fault:%d: %s\n",__LINE__,#ok); exit(1); } } while (0)
static tf_io_status allocation_fault(tf_context *ctx,FILE *out,tf_value lines);
/* Rewrite test-only identifiers, not the generated operations or semantics.
 * The wrapper injects a genuine allocator failure into output validation. */
#define main tf_generated_main
#define tf_output_publish allocation_fault
#include "c_output_fixture.c"
#undef main
#undef tf_output_publish

static size_t failed_calls;
static void *fail_allocate(void *user,size_t bytes) {
    (void)user; (void)bytes; ++failed_calls; return NULL;
}
static tf_io_status allocation_fault(tf_context *ctx,FILE *out,tf_value lines) {
    void *(*original)(void *,size_t)=ctx->config.allocator.allocate;
    ctx->config.allocator.allocate=fail_allocate;
    const tf_io_status status=tf_output_publish(ctx,out,lines);
    ctx->config.allocator.allocate=original;
    CHECK(status==TF_IO_FAULT && tf_context_fault(ctx)!=NULL &&
          tf_context_fault(ctx)->code==TF_R003);
    return status;
}

int main(void) {
    FILE *output=tmpfile(),*errors=tmpfile();
    CHECK(output!=NULL && errors!=NULL && fflush(stdout)==0 && fflush(stderr)==0);
    const int saved_out=DUP(FD(stdout)),saved_err=DUP(FD(stderr));
    CHECK(saved_out>=0 && saved_err>=0);
    CHECK(DUP2(FD(output),FD(stdout))>=0 && DUP2(FD(errors),FD(stderr))>=0);
    char name[]="native-output-fault";
    char *args[]={name,NULL};
    const int code=tf_generated_main(1,args);
    CHECK(fflush(stdout)==0 && fflush(stderr)==0);
    CHECK(DUP2(saved_out,FD(stdout))>=0 && DUP2(saved_err,FD(stderr))>=0);
    CHECK(CLOSE(saved_out)==0 && CLOSE(saved_err)==0);
    CHECK(code==3 && failed_calls==1);
    CHECK(fseek(output,0,SEEK_SET)==0 && fgetc(output)==EOF && !ferror(output));
    CHECK(fseek(errors,0,SEEK_SET)==0);
    static const char expected[]="error[R003]: execution resource limit exceeded\n";
    for (size_t i=0;i<sizeof expected-1;++i) CHECK(fgetc(errors)==(unsigned char)expected[i]);
    CHECK(fgetc(errors)==EOF && !ferror(errors) && fclose(errors)==0 && fclose(output)==0);
    puts("Generated launcher: output-validation allocation fault is R003/exit 3 with no stdout");
    return 0;
}
