#include "ternfold/c_emit.h"
#include "ternfold/core_lower.h"
#include "../src/typed_signatures.h"

#ifdef TF_TEST_EMIT_ALLOC
#undef malloc
#undef calloc
#undef free
#endif
#include <stdlib.h>
#include <string.h>

#define CHECK(ok) do { if (!(ok)) { fprintf(stderr,"test_c_emit:%d: %s\n",__LINE__,#ok); exit(1); } } while (0)

#ifdef TF_TEST_EMIT_ALLOC
static bool tracking;
static size_t allocations, failed_allocation, live;
void *tf_emit_test_malloc(size_t size) {
    if (tracking && ++allocations==failed_allocation) return NULL;
    void *p=malloc(size);
    if (tracking && p!=NULL) ++live;
    return p;
}
void *tf_emit_test_calloc(size_t count,size_t size) {
    if (tracking && ++allocations==failed_allocation) return NULL;
    void *p=calloc(count,size);
    if (tracking && p!=NULL) ++live;
    return p;
}
void tf_emit_test_free(void *p) {
    if (tracking && p!=NULL) { CHECK(live!=0); --live; }
    free(p);
}
static void allocation_failures(const TfVerifiedCore *program) {
    static bool tested;
    if (tested) return;
    tested=true;
    FILE *stream=tmpfile();
    CHECK(stream!=NULL);
    tracking=true; allocations=0; failed_allocation=0;
    const TfStatus status=tf_c_emit(stream,program);
    tracking=false;
    CHECK(status==TF_OK && live==0 && fclose(stream)==0);
    const size_t count=allocations;
    CHECK(count!=0);
    for (size_t i=1;i<=count;++i) {
        stream=tmpfile(); CHECK(stream!=NULL);
        tracking=true; allocations=0; failed_allocation=i;
        const TfStatus failed=tf_c_emit(stream,program);
        tracking=false;
        CHECK(failed==TF_HOST_ERROR && live==0 && fclose(stream)==0);
    }
    printf("C17 emission: %zu injected allocation failures\n",count);
}
#endif

static void emit(const char *source, FILE *out) {
    TfSourceManager *manager=NULL;
    TfResolution *resolution=NULL;
    TfTypedSignatures *signatures=NULL;
    TfTypedProgram *typed=NULL;
    TfMachineProgram *machines=NULL;
    TfCoreProgram *core=NULL;
    TfVerifiedCore *verified=NULL;
    CHECK(tf_source_manager_create(&manager,NULL)==TF_OK);
    CHECK(tf_resolve(&resolution,manager,source,NULL,NULL)==TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(resolution))==TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&signatures,resolution,100,NULL)==TF_OK);
    CHECK(tf_check(&typed,resolution,signatures,100,NULL)==TF_OK);
    CHECK(tf_machine_program_build(&machines,typed,100,NULL)==TF_OK);
    CHECK(tf_core_lower(&core,machines,NULL,NULL)==TF_OK);
    CHECK(tf_core_verify(&verified,core,NULL,NULL)==TF_OK);
    tf_machine_program_destroy(machines); tf_typed_program_destroy(typed);
    tf_signatures_destroy(signatures); tf_resolution_destroy(resolution);
#ifdef TF_TEST_EMIT_ALLOC
    allocation_failures(verified);
#endif
    CHECK(tf_c_emit(out,verified)==TF_OK && fflush(out)==0);
    tf_verified_core_destroy(verified); tf_core_program_destroy(core); tf_source_manager_destroy(manager);
}

static char *read_text(FILE *stream) {
    CHECK(fseek(stream,0,SEEK_END)==0);
    const long length=ftell(stream);
    CHECK(length>=0 && (uintmax_t)length<SIZE_MAX && fseek(stream,0,SEEK_SET)==0);
    char *data=malloc((size_t)length+1);
    CHECK(data!=NULL && fread(data,1,(size_t)length,stream)==(size_t)length);
    data[length]='\0'; return data;
}

int main(int argc,char **argv) {
    if (argc==3) {
        FILE *out=fopen(argv[2],"wb"); CHECK(out!=NULL);
        emit(argv[1],out); CHECK(fclose(out)==0); return 0;
    }
    CHECK(argc==1);
    FILE *a=tmpfile(), *b=tmpfile(); CHECK(a!=NULL && b!=NULL);
    CHECK(tf_c_emit(a,NULL)==TF_HOST_ERROR && ftell(a)==0);
    emit("tests/fixtures/types/interpreter-core.tfd",a);
    emit("tests/fixtures/types/interpreter-core.tfd",b);
    char *first=read_text(a), *second=read_text(b);
    CHECK(strcmp(first,second)==0);
    CHECK(strstr(first,"tf_stack_call_closure(s, f,")!=NULL);
    CHECK(strstr(first,"tf_loop_begin(s, f,")!=NULL);
    CHECK(strstr(first,"tf_variant_tag(s->ctx,")!=NULL);
    CHECK(strstr(first,"tf_machine(s->ctx,")!=NULL);
    free(first); free(second); CHECK(fclose(a)==0 && fclose(b)==0);
    puts("C17 emission: stable verified source and native continuation operations passed");
    return 0;
}
