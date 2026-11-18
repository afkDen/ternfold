#include "ternfold/core_lower.h"
#include "ternfold/core_verify.h"
#include "../src/typed_signatures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define PROCESS_ID _getpid
#else
#include <unistd.h>
#define PROCESS_ID getpid
#endif

static size_t checks;
#define CHECK(ok) do { ++checks; if (!(ok)) { \
    fprintf(stderr,"test_core_match:%d: %s\n",__LINE__,#ok); exit(1); \
} } while (0)

typedef struct {
    TfResolution *resolution;
    TfTypedSignatures *signatures;
    TfTypedProgram *typed;
    TfMachineProgram *machines;
} Front;

static Front prepare(TfSourceManager *manager, const char *path) {
    Front f={0};
    CHECK(tf_resolve(&f.resolution,manager,path,NULL,NULL)==TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(f.resolution))==TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&f.signatures,f.resolution,100,NULL)==TF_OK);
    CHECK(tf_check(&f.typed,f.resolution,f.signatures,100,NULL)==TF_OK);
    return f;
}

static void release(Front f) {
    tf_machine_program_destroy(f.machines);
    tf_typed_program_destroy(f.typed);
    tf_signatures_destroy(f.signatures);
    tf_resolution_destroy(f.resolution);
}

typedef struct { size_t calls, fail, live; } Count;

static void *allocate(void *context, size_t bytes) {
    Count *c=context;
    if (++c->calls==c->fail) return NULL;
    void *ptr=malloc(bytes);
    if (ptr != NULL) ++c->live;
    return ptr;
}

static void deallocate(void *context, void *ptr) {
    Count *c=context;
    CHECK(ptr != NULL && c->live != 0);
    --c->live;
    free(ptr);
}

static size_t failpoints(const TfMachineProgram *machines) {
    TfCoreProgram *core=NULL;
    TfCoreLowerIssue issue;
    Count count={0};
    const TfAllocator allocator={&count,allocate,deallocate};
    CHECK(tf_core_lower(&core,machines,&allocator,&issue)==TF_OK);
    tf_core_program_destroy(core);
    CHECK(count.live==0);
    const size_t calls=count.calls;
    for (size_t i=1; i<=calls; ++i) {
        count=(Count){.fail=i};
        CHECK(tf_core_lower(&core,machines,&allocator,&issue)==TF_HOST_ERROR);
        CHECK(core==NULL && count.live==0 && issue.code==TF_CORE_LOWER_NONE);
    }
    return calls;
}

static const TfCoreFunction *find(const TfCoreProgramData *d, const char *name) {
    const size_t len=strlen(name);
    for (size_t i=0; i<d->function_count; ++i) {
        const TfCoreText t=d->functions[i].name;
        if (t.length==len && memcmp(t.bytes,name,len)==0) return &d->functions[i];
    }
    CHECK(false);
    return NULL;
}

static const TfCoreBlock *block(const TfCoreFunction *f, TfCoreBlockId id) {
    const TfCoreBlock *b=tf_core_block(f,id);
    CHECK(b != NULL && b->terminator.kind==TF_CORE_YIELD);
    return b;
}

static const TfCoreLet *let_at(const TfCoreBlock *b, size_t at, TfCoreOperationKind kind) {
    CHECK(at<b->let_count && b->lets[at].operation.kind==kind);
    return &b->lets[at];
}

static const TfCoreOperation *last(const TfCoreBlock *b, TfCoreOperationKind kind) {
    CHECK(b->let_count != 0 && b->terminator.value==b->lets[b->let_count-1].value);
    return &let_at(b,b->let_count-1,kind)->operation;
}

static void subject(const TfCoreOperation *op, TfCoreValueId value) {
    CHECK(op->operands.count==1 && op->operands.items[0]==value);
}

static const TfCoreBlock *tag(const TfCoreFunction *f, const TfCoreOperation *op, size_t which) {
    for (size_t i=0; i<op->as.switch_tag.case_count; ++i)
        if (op->as.switch_tag.cases[i].tag==which)
            return block(f,op->as.switch_tag.cases[i].block);
    CHECK(false);
    return NULL;
}

static TfCoreValueId payload(const TfCoreBlock *b, TfCoreValueId source, size_t which, size_t slot) {
    for (size_t i=0; i<b->let_count; ++i) {
        const TfCoreOperation *op=&b->lets[i].operation;
        if (op->kind==TF_CORE_OP_PAYLOAD && op->as.payload.index==slot) {
            CHECK(op->as.payload.tag==which);
            subject(op,source);
            return b->lets[i].value;
        }
    }
    CHECK(false);
    return 0;
}

static const TfCoreBlock *yes(const TfCoreFunction *f, const TfCoreOperation *op) {
    return block(f,op->as.branch.then_block);
}

static const TfCoreBlock *no(const TfCoreFunction *f, const TfCoreOperation *op) {
    return block(f,op->as.branch.else_block);
}

static const TfCoreConstant *constant(const TfCoreProgramData *d, const TfCoreLet *binding,
                                       TfCoreConstantKind kind) {
    CHECK(binding->operation.kind==TF_CORE_OP_CONST);
    const TfCoreConstant *k=tf_core_constant(d,binding->operation.as.constant);
    CHECK(k != NULL && k->kind==kind);
    return k;
}

static void integer(const TfCoreProgramData *d, const TfCoreBlock *b, int64_t value) {
    const TfCoreOperation *op=last(b,TF_CORE_OP_CONST);
    const TfCoreConstant *k=tf_core_constant(d,op->as.constant);
    CHECK(k != NULL && k->kind==TF_CORE_CONSTANT_INT && k->as.integer==value);
}

static const TfCoreConstant *equality(const TfCoreProgramData *d, const TfCoreBlock *b,
                                       TfCoreValueId source, const TfCoreOperation **branch,
                                       TfCoreConstantKind kind, const char *spelling) {
    CHECK(b->let_count>=3);
    const size_t end=b->let_count;
    const TfCoreLet *literal=let_at(b,end-3,TF_CORE_OP_CONST);
    const TfCoreLet *compare=let_at(b,end-2,TF_CORE_OP_BINARY);
    const TfCoreOperation *op=&compare->operation;
    CHECK(op->as.binary==TF_CORE_BINARY_EQUAL && op->operands.count==2);
    CHECK(op->operands.items[0]==source && op->operands.items[1]==literal->value);
    *branch=last(b,TF_CORE_OP_IF);
    subject(*branch,compare->value);
    const TfSpan *span=tf_core_span(d,compare->span);
    const size_t length=strlen(spelling);
    CHECK(span != NULL && span->source != NULL && span->end-span->start==length);
    CHECK(memcmp(span->source->bytes+span->start,spelling,length)==0);
    return constant(d,literal,kind);
}

static void nested(const TfCoreProgramData *d) {
    const TfCoreFunction *f=find(d,"nested");
    const TfCoreBlock *root=block(f,f->body);
    CHECK(root->let_count==2);
    const TfCoreLet *call=let_at(root,0,TF_CORE_OP_CALL);
    subject(&call->operation,f->parameters[0].value);
    CHECK(tf_core_function(d,call->operation.as.function)==find(d,"once"));
    const TfCoreOperation *outer=last(root,TF_CORE_OP_SWITCH_TAG);
    subject(outer,call->value);
    CHECK(outer->as.switch_tag.case_count==2 && outer->as.switch_tag.default_block==0);
    integer(d,tag(f,outer,TF_CORE_OPTION_NONE),13);
    const TfCoreBlock *some=tag(f,outer,TF_CORE_OPTION_SOME);
    const TfCoreValueId inner_value=payload(some,call->value,TF_CORE_OPTION_SOME,0);
    const TfCoreOperation *inner=last(some,TF_CORE_OP_SWITCH_TAG);
    subject(inner,inner_value);
    CHECK(inner->as.switch_tag.case_count==2 && inner->as.switch_tag.default_block==0);
    integer(d,tag(f,inner,TF_CORE_OPTION_NONE),12);
    some=tag(f,inner,TF_CORE_OPTION_SOME);
    const TfCoreOperation *test=last(some,TF_CORE_OP_IF);
    subject(test,payload(some,inner_value,TF_CORE_OPTION_SOME,0));
    integer(d,yes(f,test),10);
    integer(d,no(f,test),11);
}

static void priority(const TfCoreProgramData *d) {
    const TfCoreFunction *f=find(d,"priority");
    const TfCoreValueId parameter=f->parameters[0].value;
    const TfCoreOperation *outer=last(block(f,f->body),TF_CORE_OP_SWITCH_TAG);
    CHECK(outer->as.switch_tag.case_count==3 && outer->as.switch_tag.default_block==0);
    const TfCoreBlock *pair=tag(f,outer,1);
    const TfCoreOperation *second=last(pair,TF_CORE_OP_IF);
    subject(second,payload(pair,parameter,1,1));
    /* Pair(true,true) matches two rows. The first is selected without testing
     * the first payload. The other two results depend on that first payload. */
    integer(d,yes(f,second),20);
    const TfCoreOperation *first=last(no(f,second),TF_CORE_OP_IF);
    subject(first,payload(pair,parameter,1,0));
    integer(d,yes(f,first),21);
    integer(d,no(f,first),22);
    integer(d,tag(f,outer,0),24);
    const TfCoreBlock *wrapped=tag(f,outer,2);
    CHECK(wrapped->let_count==2); /* Option projection and switch; Unit needs neither. */
    const TfCoreValueId option=payload(wrapped,parameter,2,0);
    const TfCoreOperation *inner=last(wrapped,TF_CORE_OP_SWITCH_TAG);
    subject(inner,option);
    CHECK(inner->as.switch_tag.case_count==1);
    integer(d,block(f,inner->as.switch_tag.default_block),25);
    const TfCoreBlock *some=tag(f,inner,TF_CORE_OPTION_SOME);
    const TfCoreOperation *test=last(some,TF_CORE_OP_IF);
    subject(test,payload(some,option,TF_CORE_OPTION_SOME,0));
    integer(d,yes(f,test),23);
    integer(d,no(f,test),25);
}

static void mixed(const TfCoreProgramData *d) {
    const TfCoreFunction *f=find(d,"mixed");
    const TfCoreValueId parameter=f->parameters[0].value;
    const TfCoreOperation *outer=last(block(f,f->body),TF_CORE_OP_SWITCH_TAG);
    CHECK(outer->as.switch_tag.case_count==1);
    integer(d,block(f,outer->as.switch_tag.default_block),34);
    const TfCoreBlock *pair=tag(f,outer,0);
    const TfCoreValueId number=payload(pair,parameter,0,0), flag=payload(pair,parameter,0,1);
    const TfCoreOperation *test;
    CHECK(equality(d,pair,number,&test,TF_CORE_CONSTANT_INT,"-9223372036854775808")
              ->as.integer==INT64_MIN);
    const TfCoreOperation *bool_test=last(yes(f,test),TF_CORE_OP_IF);
    subject(bool_test,flag);
    integer(d,yes(f,bool_test),30);
    CHECK(no(f,bool_test)->terminator.value==number);
    CHECK(equality(d,no(f,test),number,&test,TF_CORE_CONSTANT_INT,"- 7")->as.integer==-7);
    integer(d,yes(f,test),31);
    CHECK(equality(d,no(f,test),number,&test,TF_CORE_CONSTANT_INT,"42")->as.integer==42);
    bool_test=last(yes(f,test),TF_CORE_OP_IF);
    subject(bool_test,flag);
    integer(d,no(f,bool_test),32);
    integer(d,yes(f,bool_test),34);
    bool_test=last(no(f,test),TF_CORE_OP_IF);
    subject(bool_test,flag);
    integer(d,yes(f,bool_test),34);
    CHECK(no(f,bool_test)->terminator.value==number);
}

static void open_literals(const TfCoreProgramData *d) {
    const TfCoreFunction *f=find(d,"floats");
    const TfCoreOperation *test;
    TfCoreValueId p=f->parameters[0].value;
    CHECK(equality(d,block(f,f->body),p,&test,TF_CORE_CONSTANT_FLOAT,"-1.25")->as.float_bits
          ==UINT64_C(0xbff4000000000000));
    integer(d,yes(f,test),40);
    CHECK(equality(d,no(f,test),p,&test,TF_CORE_CONSTANT_FLOAT,"-0.0")->as.float_bits==0);
    integer(d,yes(f,test),41);
    integer(d,no(f,test),42);
    f=find(d,"same_literal"); p=f->parameters[0].value;
    const TfCoreOperation *tag_test=last(block(f,f->body),TF_CORE_OP_SWITCH_TAG);
    CHECK(tag_test->as.switch_tag.case_count==1 && tag_test->as.switch_tag.default_block==0);
    const TfCoreBlock *pair=tag(f,tag_test,0);
    CHECK(equality(d,pair,payload(pair,p,0,0),&test,TF_CORE_CONSTANT_FLOAT,"-0.0")
          ->as.float_bits==0);
    integer(d,no(f,test),45);
    const TfCoreOperation *flag=last(yes(f,test),TF_CORE_OP_IF);
    subject(flag,payload(pair,p,0,1));
    integer(d,yes(f,flag),43);
    integer(d,no(f,flag),44);
    f=find(d,"texts"); p=f->parameters[0].value;
    TfCoreText text=equality(d,block(f,f->body),p,&test,TF_CORE_CONSTANT_TEXT,
                             "\"a\\u{0}b\"")->as.text;
    CHECK(text.length==3 && memcmp(text.bytes,"a\0b",3)==0);
    integer(d,yes(f,test),50);
    text=equality(d,no(f,test),p,&test,TF_CORE_CONSTANT_TEXT,"\"a\\u{0}c\"")->as.text;
    CHECK(text.length==3 && memcmp(text.bytes,"a\0c",3)==0);
    integer(d,yes(f,test),51);
    integer(d,no(f,test),52);
    f=find(d,"chars"); p=f->parameters[0].value;
    CHECK(equality(d,block(f,f->body),p,&test,TF_CORE_CONSTANT_CHAR,"'a'")->as.character=='a');
    integer(d,yes(f,test),60);
    CHECK(equality(d,no(f,test),p,&test,TF_CORE_CONSTANT_CHAR,"'\\u{1F642}'")->as.character==0x1f642u);
    integer(d,yes(f,test),61);
    integer(d,no(f,test),62);
    f=find(d,"unit");
    CHECK(block(f,f->body)->let_count==1);
    integer(d,block(f,f->body),70);
}

static void captures(const TfCoreProgramData *d) {
    const TfCoreFunction *f=find(d,"captured");
    const TfCoreValueId p=f->parameters[0].value, shadow=f->parameters[1].value;
    const TfCoreOperation *outer=last(block(f,f->body),TF_CORE_OP_SWITCH_TAG);
    CHECK(block(f,outer->as.switch_tag.default_block)->terminator.value==shadow);
    const TfCoreBlock *pair=tag(f,outer,1);
    const TfCoreOperation *test=last(pair,TF_CORE_OP_IF);
    subject(test,payload(pair,p,1,1));
    CHECK(no(f,test)->terminator.value==shadow);
    const TfCoreBlock *bound=yes(f,test);
    const TfCoreLet *closure=let_at(bound,0,TF_CORE_OP_CLOSURE);
    subject(&closure->operation,payload(pair,p,1,0));
    subject(last(bound,TF_CORE_OP_CALL_CLOSURE),closure->value);
    const TfCoreFunction *lambda=tf_core_function(d,closure->operation.as.function);
    CHECK(lambda != NULL && lambda->capture_count==1);
    CHECK(block(lambda,lambda->body)->terminator.value==lambda->captures[0].value);

    const TfCoreBlock *wrapped=tag(f,outer,2);
    const TfCoreValueId option=payload(wrapped,p,2,0);
    const TfCoreOperation *inner=last(wrapped,TF_CORE_OP_SWITCH_TAG);
    subject(inner,option);
    CHECK(block(f,inner->as.switch_tag.default_block)->terminator.value==shadow);
    bound=tag(f,inner,TF_CORE_OPTION_SOME);
    closure=let_at(bound,1,TF_CORE_OP_CLOSURE);
    subject(&closure->operation,payload(bound,option,TF_CORE_OPTION_SOME,0));
    subject(last(bound,TF_CORE_OP_CALL_CLOSURE),closure->value);

    f=find(d,"duplicated");
    size_t copies=0;
    for (size_t i=0; i<f->block_count; ++i) {
        const TfCoreBlock *b=&f->blocks[i];
        for (size_t j=0; j<b->let_count; ++j) {
            const TfCoreOperation *op=&b->lets[j].operation;
            if (op->kind != TF_CORE_OP_CLOSURE) continue;
            ++copies;
            subject(op,f->parameters[0].value);
            lambda=tf_core_function(d,op->as.function);
            CHECK(lambda != NULL && lambda->capture_count==1 &&
                  lambda->captures[0].type==f->parameters[0].type);
            CHECK(block(lambda,lambda->body)->terminator.value==lambda->captures[0].value);
        }
    }
    CHECK(copies==4);
}

static void invalid_patterns(TfSourceManager *manager) {
    Front f=prepare(manager,"tests/fixtures/types/core-match-invalid.tfd");
    CHECK(tf_diagnostic_phase_kind(tf_typed_program_phase(f.typed))==TF_DIAG_PHASE_ERROR);
    const TfDiagnosticBag *bag=tf_typed_program_diagnostics(f.typed);
    CHECK(tf_diagnostic_bag_count(bag)==2);
    CHECK(strcmp(tf_diagnostic_bag_get(bag,0)->code,"E310")==0);
    CHECK(strcmp(tf_diagnostic_bag_get(bag,1)->code,"E311")==0);
    CHECK(tf_machine_program_build(&f.machines,f.typed,100,NULL)==TF_HOST_ERROR && f.machines==NULL);
    release(f);
}

static void wide_open(TfSourceManager *manager) {
    enum { LITERALS=512 };
    char path[96];
    const int length=snprintf(path,sizeof path,"build/test-core-match-%ld.tfd",(long)PROCESS_ID());
    CHECK(length>0 && (size_t)length<sizeof path);
    FILE *stream=fopen(path,"wb");
    CHECK(stream != NULL);
    CHECK(fputs("fn wide(x: Int) -> Int { match x {\n",stream)>=0);
    for (int i=0; i<LITERALS; ++i) CHECK(fprintf(stream,"%d => { %d },\n",i,i)>=0);
    CHECK(fputs("_ => { -1 div 1 } } }\n",stream)>=0 && fclose(stream)==0);
    Front f=prepare(manager,path);
    CHECK(tf_machine_program_build(&f.machines,f.typed,100,NULL)==TF_OK);
    TfCoreProgram *core=NULL;
    CHECK(tf_core_lower(&core,f.machines,NULL,NULL)==TF_OK);
    release(f);
    CHECK(remove(path)==0);
    TfVerifiedCore *verified=NULL;
    CHECK(tf_core_verify(&verified,core,NULL,NULL)==TF_OK);
    const TfCoreProgramData *d=tf_verified_core_data(verified);
    const TfCoreFunction *fn=find(d,"wide");
    const TfCoreBlock *current=block(fn,fn->body);
    CHECK(fn->block_count==2u*LITERALS+1u);
    for (int i=0; i<LITERALS; ++i) {
        char spelling[16];
        const int written=snprintf(spelling,sizeof spelling,"%d",i);
        const TfCoreOperation *test;
        CHECK(written>0 && (size_t)written<sizeof spelling);
        CHECK(equality(d,current,fn->parameters[0].value,&test,TF_CORE_CONSTANT_INT,spelling)
              ->as.integer==i);
        integer(d,yes(fn,test),i);
        current=no(fn,test);
    }
    CHECK(last(current,TF_CORE_OP_BINARY)->as.binary==TF_CORE_BINARY_FLOOR_DIVIDE);
    tf_verified_core_destroy(verified);
    tf_core_program_destroy(core);
}

static void dead_without_projection(const TfCoreFunction *fn, TfCoreBlockId id) {
    const TfCoreBlock *b=tf_core_block(fn,id);
    CHECK(b!=NULL && b->terminator.kind==TF_CORE_UNREACHABLE && b->let_count==0);
}

static TfCoreBlockId case_id(const TfCoreOperation *op, size_t tag_value) {
    for (size_t i=0; i<op->as.switch_tag.case_count; ++i)
        if (op->as.switch_tag.cases[i].tag==tag_value) return op->as.switch_tag.cases[i].block;
    CHECK(false); return 0;
}

static size_t nested_facts(TfSourceManager *manager) {
    Front f=prepare(manager,"tests/fixtures/types/core-match-facts.tfd");
    CHECK(tf_machine_program_build(&f.machines,f.typed,100,NULL)==TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_machine_program_phase(f.machines))==TF_DIAG_PHASE_VALID);
    const size_t failures=failpoints(f.machines);
    TfCoreProgram *core=NULL;
    TfVerifiedCore *verified=NULL;
    CHECK(tf_core_lower(&core,f.machines,NULL,NULL)==TF_OK);
    release(f);
    CHECK(tf_core_verify(&verified,core,NULL,NULL)==TF_OK);
    const TfCoreProgramData *d=tf_verified_core_data(verified);
    const TfCoreFunction *fn=find(d,"explicit_fact");
    const TfCoreOperation *outer=last(block(fn,fn->body),TF_CORE_OP_SWITCH_TAG);
    const TfCoreOperation *inner=last(tag(fn,outer,TF_CORE_OPTION_NONE),TF_CORE_OP_SWITCH_TAG);
    dead_without_projection(fn,case_id(inner,TF_CORE_OPTION_SOME));
    integer(d,tag(fn,inner,TF_CORE_OPTION_NONE),0);

    fn=find(d,"default_fact");
    outer=last(block(fn,fn->body),TF_CORE_OP_SWITCH_TAG);
    inner=last(block(fn,outer->as.switch_tag.default_block),TF_CORE_OP_SWITCH_TAG);
    subject(outer,fn->parameters[0].value); subject(inner,fn->parameters[0].value);
    dead_without_projection(fn,case_id(inner,TF_CORE_OPTION_SOME));
    integer(d,tag(fn,inner,TF_CORE_OPTION_NONE),1);

    fn=find(d,"impossible_default");
    outer=last(block(fn,fn->body),TF_CORE_OP_SWITCH_TAG);
    inner=last(block(fn,outer->as.switch_tag.default_block),TF_CORE_OP_SWITCH_TAG);
    dead_without_projection(fn,inner->as.switch_tag.default_block);
    const TfCoreBlock *some=tag(fn,inner,1);
    CHECK(payload(some,fn->parameters[0].value,1,0)==some->terminator.value);

    fn=find(d,"independent_closure");
    outer=last(block(fn,fn->body),TF_CORE_OP_SWITCH_TAG);
    const TfCoreBlock *none=tag(fn,outer,TF_CORE_OPTION_NONE);
    const TfCoreFunction *lifted=tf_core_function(d,let_at(none,0,TF_CORE_OP_CLOSURE)->operation.as.function);
    CHECK(lifted!=NULL);
    inner=last(block(lifted,lifted->body),TF_CORE_OP_SWITCH_TAG);
    some=tag(lifted,inner,TF_CORE_OPTION_SOME);
    CHECK(payload(some,lifted->parameters[0].value,TF_CORE_OPTION_SOME,0)==some->terminator.value);

    fn=find(d,"known_bool");
    outer=last(block(fn,fn->body),TF_CORE_OP_IF);
    inner=last(yes(fn,outer),TF_CORE_OP_IF);
    dead_without_projection(fn,inner->as.branch.else_block);
    integer(d,yes(fn,inner),5);
    tf_verified_core_destroy(verified);
    tf_core_program_destroy(core);
    return failures;
}

static size_t valid(TfSourceManager *manager, const char *path, bool int_only) {
    Front f=prepare(manager,path);
    const TfDiagnosticBag *bag=tf_typed_program_diagnostics(f.typed);
    for (size_t i=0; i<tf_diagnostic_bag_count(bag); ++i)
        fprintf(stderr,"%s: %s\n",path,tf_diagnostic_bag_get(bag,i)->code);
    CHECK(tf_machine_program_build(&f.machines,f.typed,100,NULL)==TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_machine_program_phase(f.machines))==TF_DIAG_PHASE_VALID);
    size_t source_bools=0;
    const TfTypeStore *store=tf_typed_program_store(f.typed);
    for (size_t i=0; i<tf_type_store_count(store); ++i)
        if (tf_type_store_at(store,i)->kind==TF_CONCRETE_BOOL) ++source_bools;
    CHECK(source_bools==(int_only ? 0u : 1u));
    const size_t calls=failpoints(f.machines);
    TfCoreProgram *core=NULL;
    TfCoreLowerIssue issue;
    CHECK(tf_core_lower(&core,f.machines,NULL,&issue)==TF_OK && issue.code==TF_CORE_LOWER_NONE);
    release(f);
    TfVerifiedCore *verified=NULL;
    TfCoreVerifyIssue problem;
    CHECK(tf_core_verify(&verified,core,NULL,&problem)==TF_OK);
    const TfCoreProgramData *data=tf_verified_core_data(verified);
    size_t core_bools=0;
    for (size_t i=0; i<data->type_count; ++i)
        if (data->types[i].kind==TF_CORE_TYPE_BOOL) ++core_bools;
    CHECK(core_bools==1);
    for (size_t i=0; i<data->function_count; ++i)
        for (size_t j=0; j<data->functions[i].block_count; ++j)
            CHECK(data->functions[i].blocks[j].terminator.kind==TF_CORE_YIELD);
    if (int_only) {
        const TfCoreFunction *fn=find(data,"int_only");
        const TfCoreOperation *test;
        CHECK(equality(data,block(fn,fn->body),fn->parameters[0].value,&test,
                       TF_CORE_CONSTANT_INT,"-1")->as.integer==-1);
        integer(data,yes(fn,test),1);
        integer(data,no(fn,test),0);
    } else {
        nested(data); priority(data); mixed(data); open_literals(data); captures(data);
    }
    tf_verified_core_destroy(verified);
    tf_core_program_destroy(core);
    return calls;
}

int main(void) {
    TfSourceManager *manager=NULL;
    CHECK(tf_source_manager_create(&manager,NULL)==TF_OK);
    size_t calls=valid(manager,"tests/fixtures/types/core-match.tfd",false);
    calls+=valid(manager,"tests/fixtures/types/core-match-int-only.tfd",true);
    calls+=nested_facts(manager);
    invalid_patterns(manager);
    wide_open(manager);
    tf_source_manager_destroy(manager);
    printf("Core match lowering: %zu checks, %zu injected allocation failures\n",checks,calls);
    return EXIT_SUCCESS;
}
