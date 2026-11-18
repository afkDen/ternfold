#include "ternfold_rt_io.h"

#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(ok) do { ++checks; if (!(ok)) { \
    fprintf(stderr,"test_runtime_io:%d: %s\n",__LINE__,#ok); exit(1); \
} } while (0)

typedef struct { size_t calls, fail, live; } Counts;
static void *allocate(void *user,size_t bytes) {
    Counts *c=user; if (++c->calls==c->fail) return NULL;
    void *ptr=malloc(bytes); if (ptr!=NULL) ++c->live; return ptr;
}
static void deallocate(void *user,void *ptr) {
    Counts *c=user; CHECK(ptr!=NULL && c->live!=0); --c->live; free(ptr);
}
static const tf_rt_type types[]={ {.kind=TF_RT_TYPE_UNIT}, {.kind=TF_RT_TYPE_TEXT},
                                  {.kind=TF_RT_TYPE_LIST,.as.element=2} };
static const tf_rt_program program={types,3,NULL,0};

static FILE *input(const char *bytes,size_t length) {
    FILE *stream=tmpfile(); CHECK(stream!=NULL);
    CHECK(fwrite(bytes,1,length,stream)==length && fflush(stream)==0 && fseek(stream,0,SEEK_SET)==0);
    return stream;
}

int main(void) {
    static const char bytes[]="a\r\n\n\xf0\x9f\x99\x82\0z\nfinal\r";
    static const char expected[]="a\n\n\xf0\x9f\x99\x82\0z\nfinal\r\n";
    tf_context ctx;
    tf_value snapshot=tf_unit();
    Counts counter={0};
    const tf_rt_config config={{allocate,deallocate,&counter},100000,32};
    FILE *source=input(bytes,sizeof bytes-1), *output=tmpfile(); CHECK(output!=NULL);
    CHECK(tf_context_init(&ctx,&program,&config)==TF_OK);
    CHECK(tf_input_capture(&ctx,3,source,&snapshot)==TF_IO_OK);
    const size_t failpoints=counter.calls;
    size_t count;
    CHECK(tf_list_length(&ctx,0,snapshot,&count)==TF_OK && count==4);
    CHECK(tf_output_publish(&ctx,output,snapshot)==TF_IO_OK && fflush(output)==0 && fseek(output,0,SEEK_SET)==0);
    for (size_t i=0;i<sizeof expected-1;++i) CHECK(fgetc(output)==(unsigned char)expected[i]);
    CHECK(fgetc(output)==EOF && feof(output));
    CHECK(fclose(output)==0);
    tf_release(&snapshot); CHECK(counter.live==0 && tf_context_live_bytes(&ctx)==0);
    for (size_t i=1;i<=failpoints;++i) {
        counter=(Counts){.fail=i};
        CHECK(tf_context_init(&ctx,&program,&config)==TF_OK && fseek(source,0,SEEK_SET)==0);
        CHECK(tf_input_capture(&ctx,3,source,&snapshot)==TF_IO_FAULT && snapshot.tag==TF_VALUE_UNIT);
        CHECK(tf_context_fault(&ctx)->code==TF_R003 && counter.live==0 && tf_context_live_bytes(&ctx)==0);
    }
    CHECK(fclose(source)==0);

    source=input("\xc0\x80",2);
    counter=(Counts){0}; CHECK(tf_context_init(&ctx,&program,&config)==TF_OK);
    CHECK(tf_input_capture(&ctx,3,source,&snapshot)==TF_IO_INVALID_UTF8 && snapshot.tag==TF_VALUE_UNIT && counter.live==0);
    CHECK(tf_context_fault(&ctx)==NULL && fclose(source)==0);

    unsigned char split[4099];
    memset(split,'x',4095);
    split[4095]=0xf0; split[4096]=0x9f; split[4097]=0x99; split[4098]=0x82;
    source=input((const char *)split,sizeof split);
    CHECK(tf_input_capture(&ctx,3,source,&snapshot)==TF_IO_OK);
    tf_value long_line=tf_unit();
    CHECK(tf_entry_input(&ctx,0,snapshot,0,&long_line)==TF_OK);
    const unsigned char *view=NULL; size_t length=0;
    CHECK(tf_text_view(&ctx,0,long_line,&view,&length)==TF_OK && length==sizeof split &&
          memcmp(view,split,sizeof split)==0);
    tf_release(&long_line); tf_release(&snapshot);
    CHECK(fclose(source)==0 && counter.live==0);

    source=input("",0); output=tmpfile(); CHECK(output!=NULL);
    CHECK(tf_input_capture(&ctx,3,source,&snapshot)==TF_IO_OK);
    CHECK(tf_list_length(&ctx,0,snapshot,&count)==TF_OK && count==0);
    CHECK(tf_output_publish(&ctx,output,snapshot)==TF_IO_OK && ftell(output)==0);
    tf_value absent=tf_unit();
    CHECK(tf_entry_input(&ctx,99,snapshot,0,&absent)==TF_FAULT && absent.tag==TF_VALUE_UNIT);
    CHECK(tf_context_fault(&ctx)->code==TF_R006 && tf_context_fault(&ctx)->span==99);
    CHECK(tf_output_publish(&ctx,output,snapshot)==TF_IO_FAULT && ftell(output)==0);
    tf_release(&snapshot);
    CHECK(fclose(source)==0 && fclose(output)==0 && counter.live==0);
    printf("Native host I/O: %zu checks, %zu capture allocation failures\n",checks,failpoints);
    return 0;
}
