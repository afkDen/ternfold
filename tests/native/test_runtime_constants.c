#include "ternfold_rt_constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(ok) do { ++checks; if (!(ok)) { \
    fprintf(stderr,"test_runtime_constants:%d: %s\n",__LINE__,#ok); exit(1); \
} } while (0)

typedef struct { size_t calls,fail,live; } Counts;
static void *allocate(void *user,size_t bytes) {
    Counts *count=user;
    if (++count->calls==count->fail) return NULL;
    void *ptr=malloc(bytes); if (ptr!=NULL) ++count->live; return ptr;
}
static void deallocate(void *user,void *ptr) {
    Counts *count=user;
    CHECK(ptr!=NULL && count->live!=0); --count->live; free(ptr);
}

enum { UNIT=1,INT,TEXT,TEXTS,VARIANT };
static const tf_type_id text_payload[]={TEXT};
static const tf_rt_shape payloads[]={ {text_payload,1},{0} };
static const tf_rt_type types[]={
    {.kind=TF_RT_TYPE_UNIT},{.kind=TF_RT_TYPE_INT},{.kind=TF_RT_TYPE_TEXT},
    {.kind=TF_RT_TYPE_LIST,.as.element=TEXT},
    {.kind=TF_RT_TYPE_VARIANT,.as.variants={payloads,2}}
};
static const tf_rt_program program={types,sizeof types/sizeof *types,NULL,0};

int main(void) {
    tf_context ctx;
    Counts count={0};
    tf_rt_config config={{allocate,deallocate,&count},100000,32};
    unsigned char text[]={'a',0,'b'};
    const size_t child[]={1};
    tf_rt_constant descriptors[]={
        {.kind=TF_RT_CONST_TEXT,.type=TEXT,.span=1,.as.text={text,sizeof text}},
        {.kind=TF_RT_CONST_LIST,.type=TEXTS,.span=2,.as.children={child,1,0}},
        {.kind=TF_RT_CONST_VARIANT,.type=VARIANT,.span=3,.as.children={child,1,0}},
        {.kind=TF_RT_CONST_VARIANT,.type=VARIANT,.span=4,.as.children={NULL,0,1}}
    };
    tf_constant_pool *pool=NULL;
    CHECK(tf_context_init(&ctx,&program,&config)==TF_OK);
    CHECK(tf_constants_create(&ctx,descriptors,4,&pool)==TF_OK);
    const size_t failpoints=count.calls;
    tf_value list=tf_unit(),element=tf_unit();
    CHECK(tf_constant_get(pool,9,2,&list)==TF_OK);
    tf_constants_destroy(pool);
    CHECK(tf_list_at(&ctx,10,list,0,&element)==TF_OK);
    tf_release(&list);
    memset(text,'x',sizeof text);
    const unsigned char *bytes=NULL; size_t length=0;
    CHECK(tf_text_view(&ctx,11,element,&bytes,&length)==TF_OK && length==3 && memcmp(bytes,"a\0b",3)==0);
    tf_release(&element);
    CHECK(count.live==0 && tf_context_live_bytes(&ctx)==0);
    for (size_t i=1; i<=failpoints; ++i) {
        count=(Counts){.fail=i};
        CHECK(tf_context_init(&ctx,&program,&config)==TF_OK);
        CHECK(tf_constants_create(&ctx,descriptors,4,&pool)==TF_FAULT && pool==NULL);
        CHECK(tf_context_fault(&ctx)->code==TF_R003 && count.live==0 && tf_context_live_bytes(&ctx)==0);
    }
    const size_t self[]={2};
    descriptors[1].as.children.ids=self;
    count=(Counts){0};
    CHECK(tf_context_init(&ctx,&program,&config)==TF_OK);
    CHECK(tf_constants_create(&ctx,descriptors,4,&pool)==TF_FAULT && pool==NULL);
    CHECK(tf_context_fault(&ctx)->code==TF_R004 && tf_context_fault(&ctx)->span==2 && count.live==0);
    printf("Native constants: %zu checks, %zu injected allocation failures\n",checks,failpoints);
    return 0;
}
