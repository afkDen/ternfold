#include "ternfold/core_optimize.h"
#include "ternfold/core_lower.h"
#include "ternfold/interpreter.h"
#include "ternfold/c_emit.h"
#include "../src/typed_signatures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static size_t checks;
#define CHECK(e) do { ++checks; if (!(e)) { \
    fprintf(stderr, "optimization:%d: %s\n", __LINE__, #e); exit(1); } } while (0)

typedef struct { size_t calls, fail, live; } Allocation;
static void *allocate(void *context, size_t bytes) {
    Allocation *a = context;
    if (++a->calls == a->fail) return NULL;
    void *p = malloc(bytes);
    if (p != NULL) ++a->live;
    return p;
}
static void deallocate(void *context, void *p) {
    Allocation *a = context;
    CHECK(p != NULL && a->live != 0);
    --a->live;
    free(p);
}

static const TfCoreType types[] = {
    {.kind=TF_CORE_TYPE_INT}, {.kind=TF_CORE_TYPE_FLOAT}, {.kind=TF_CORE_TYPE_BOOL},
    {.kind=TF_CORE_TYPE_CHAR}, {.kind=TF_CORE_TYPE_UNIT}
};
enum { INT=1, FLOAT, BOOL, CHAR, UNIT };
static TfCoreConstant integer(int64_t n) {
    return (TfCoreConstant){.kind=TF_CORE_CONSTANT_INT,.type=INT,.as.integer=n};
}
static TfCoreConstant floating(uint64_t n) {
    return (TfCoreConstant){.kind=TF_CORE_CONSTANT_FLOAT,.type=FLOAT,.as.float_bits=n};
}

static void failures(const TfVerifiedCore *verified) {
    for (unsigned level=0; level<=1; ++level) {
        Allocation a={0};
        const TfAllocator allocator={&a,allocate,deallocate};
        TfCoreOptimization *out=NULL;
        CHECK(tf_core_optimize(&out,verified,(TfCoreOptLevel)level,&allocator)==TF_OK);
        tf_core_optimization_destroy(out);
        CHECK(a.live==0);
        const size_t calls=a.calls;
        for (size_t i=1;i<=calls;++i) {
            a=(Allocation){.fail=i}; out=NULL;
            CHECK(tf_core_optimize(&out,verified,(TfCoreOptLevel)level,&allocator)==TF_HOST_ERROR);
            CHECK(out==NULL && a.live==0);
        }
        printf("optimization level %u: %zu allocation failpoints\n",level,calls);
    }
}

static void operation(TfCoreConstant a, TfCoreConstant b, TfCoreOperationKind kind,
                      unsigned op, TfCoreTypeId result_type, bool safe, TfCoreConstant expected) {
    const TfCoreConstant constants[]={a,b};
    const TfCoreValueId args[]={1,2};
    TfCoreLet lets[]={
        {.value=1,.type=a.type,.operation={.kind=TF_CORE_OP_CONST,.as.constant=1}},
        {.value=2,.type=b.type,.operation={.kind=TF_CORE_OP_CONST,.as.constant=2}},
        {.value=3,.type=result_type,.operation={.kind=kind,.operands={args,kind==TF_CORE_OP_UNARY ? 1u:2u}}}
    };
    if (kind==TF_CORE_OP_UNARY) lets[2].operation.as.unary=(TfCoreUnary)op;
    else lets[2].operation.as.binary=(TfCoreBinary)op;
    const TfCoreBlock block={lets,3,{TF_CORE_YIELD,3,0},0};
    const TfCoreFunction fn={.kind=TF_CORE_FUNCTION_ORDINARY,.result=result_type,
        .blocks=&block,.block_count=1,.body=1,.value_count=3};
    const TfCoreProgramData data={.types=types,.type_count=5,.constants=constants,
        .constant_count=2,.functions=&fn,.function_count=1};
    TfCoreProgram *owner=NULL; TfVerifiedCore *verified=NULL;
    CHECK(tf_core_program_copy(&owner,&data,NULL)==TF_OK);
    CHECK(tf_core_verify(&verified,owner,NULL,NULL)==TF_OK);
    for (unsigned level=0;level<=1;++level) {
        TfCoreOptimization *out=NULL;
        CHECK(tf_core_optimize(&out,verified,(TfCoreOptLevel)level,NULL)==TF_OK);
        const TfCoreProgramData *d=tf_verified_core_data(tf_core_optimization_verified(out));
        CHECK(d!=tf_verified_core_data(verified));
        CHECK(d->functions[0].value_count==3 && d->functions[0].blocks[0].let_count==3);
        for (size_t i=0;i<3;++i) {
            CHECK(d->functions[0].blocks[0].lets[i].value==i+1);
            CHECK(d->functions[0].blocks[0].lets[i].type==lets[i].type);
            CHECK(d->functions[0].blocks[0].lets[i].span==lets[i].span);
        }
        const TfCoreOperation *r=&d->functions[0].blocks[0].lets[2].operation;
        if (level==1 && safe) {
            CHECK(tf_core_optimization_fold_count(out)==1 && d->constant_count==3);
            CHECK(r->kind==TF_CORE_OP_CONST && r->operands.count==0);
            const TfCoreConstant *c=tf_core_constant(d,r->as.constant);
            CHECK(c!=NULL && c->kind==expected.kind && c->type==result_type);
            switch(c->kind) {
            case TF_CORE_CONSTANT_INT: CHECK(c->as.integer==expected.as.integer); break;
            case TF_CORE_CONSTANT_FLOAT: CHECK(c->as.float_bits==expected.as.float_bits); break;
            case TF_CORE_CONSTANT_BOOL: CHECK(c->as.boolean==expected.as.boolean); break;
            default: CHECK(false);
            }
        } else {
            CHECK(tf_core_optimization_fold_count(out)==0 && r->kind==kind);
            CHECK(r->operands.count==lets[2].operation.operands.count);
        }
        tf_core_optimization_destroy(out);
    }
    static bool injected;
    if (!injected) { failures(verified); injected=true; }
    TfCoreOptimization *invalid=NULL;
    const TfAllocator bad={0};
    CHECK(tf_core_optimize(&invalid,verified,(TfCoreOptLevel)2,NULL)==TF_HOST_ERROR && invalid==NULL);
    CHECK(tf_core_optimize(&invalid,verified,TF_CORE_OPT_LEVEL_0,&bad)==TF_HOST_ERROR && invalid==NULL);
    CHECK(tf_verified_core_data(verified)->functions[0].blocks[0].lets[2].operation.kind==kind);
    tf_verified_core_destroy(verified); tf_core_program_destroy(owner);
}

static TfCoreConstant boolean(bool value) {
    return (TfCoreConstant){.kind=TF_CORE_CONSTANT_BOOL,.type=BOOL,.as.boolean=(uint8_t)value};
}
static void scalar_matrix(void) {
    const struct { TfCoreBinary op; int64_t a,b,r; bool safe; } cases[]={
        {TF_CORE_BINARY_ADD,40,2,42,true}, {TF_CORE_BINARY_ADD,INT64_MAX,1,0,false},
        {TF_CORE_BINARY_SUBTRACT,INT64_MIN,1,0,false}, {TF_CORE_BINARY_SUBTRACT,40,-2,42,true},
        {TF_CORE_BINARY_MULTIPLY,-7,6,-42,true}, {TF_CORE_BINARY_MULTIPLY,INT64_MIN,-1,0,false},
        {TF_CORE_BINARY_FLOOR_DIVIDE,-7,3,-3,true}, {TF_CORE_BINARY_REMAINDER,-7,3,-1,true},
        {TF_CORE_BINARY_FLOOR_DIVIDE,INT64_MIN,-1,0,false}, {TF_CORE_BINARY_REMAINDER,INT64_MIN,-1,0,false},
        {TF_CORE_BINARY_FLOOR_DIVIDE,1,0,0,false}, {TF_CORE_BINARY_REMAINDER,1,0,0,false},
        {TF_CORE_BINARY_POWER,2,62,INT64_C(4611686018427387904),true},
        {TF_CORE_BINARY_POWER,2,63,0,false}, {TF_CORE_BINARY_POWER,2,-1,0,false},
        {TF_CORE_BINARY_POWER,0,0,1,true}, {TF_CORE_BINARY_POWER,-2,63,INT64_MIN,true}
    };
    for(size_t i=0;i<sizeof cases/sizeof *cases;++i)
        operation(integer(cases[i].a),integer(cases[i].b),TF_CORE_OP_BINARY,
            (unsigned)cases[i].op,INT,cases[i].safe,integer(cases[i].r));
    operation(integer(INT64_MIN),integer(0),TF_CORE_OP_UNARY,TF_CORE_UNARY_NEGATE,INT,false,integer(0));
    operation(integer(-42),integer(0),TF_CORE_OP_UNARY,TF_CORE_UNARY_NEGATE,INT,true,integer(42));
    operation(boolean(true),boolean(false),TF_CORE_OP_UNARY,TF_CORE_UNARY_NOT,BOOL,true,boolean(false));
    for(unsigned op=TF_CORE_BINARY_LESS;op<=TF_CORE_BINARY_NOT_EQUAL;++op) {
        const bool expected[]={true,true,false,false,false,true};
        operation(integer(-2),integer(7),TF_CORE_OP_BINARY,op,BOOL,true,boolean(expected[op-TF_CORE_BINARY_LESS]));
        operation(floating(UINT64_C(0xc000000000000000)),floating(UINT64_C(0x401c000000000000)),
            TF_CORE_OP_BINARY,op,BOOL,true,boolean(expected[op-TF_CORE_BINARY_LESS]));
    }
    operation(boolean(true),boolean(false),TF_CORE_OP_BINARY,TF_CORE_BINARY_EQUAL,BOOL,true,boolean(false));
    const TfCoreConstant ch={.kind=TF_CORE_CONSTANT_CHAR,.type=CHAR,.as.character=UINT32_C(0x10ffff)};
    const TfCoreConstant unit={.kind=TF_CORE_CONSTANT_UNIT,.type=UNIT};
    operation(ch,ch,TF_CORE_OP_BINARY,TF_CORE_BINARY_EQUAL,BOOL,true,boolean(true));
    operation(unit,unit,TF_CORE_OP_BINARY,TF_CORE_BINARY_NOT_EQUAL,BOOL,true,boolean(false));
    const uint64_t one=UINT64_C(0x3ff0000000000000), two=UINT64_C(0x4000000000000000);
    operation(floating(one),floating(one),TF_CORE_OP_BINARY,TF_CORE_BINARY_ADD,FLOAT,true,floating(two));
    operation(floating(one),floating(one),TF_CORE_OP_BINARY,TF_CORE_BINARY_SUBTRACT,FLOAT,true,floating(0));
    operation(floating(two),floating(one),TF_CORE_OP_BINARY,TF_CORE_BINARY_MULTIPLY,FLOAT,true,floating(two));
    operation(floating(one),integer(-1),TF_CORE_OP_BINARY,TF_CORE_BINARY_POWER,FLOAT,true,floating(one));
    operation(floating(1),floating(UINT64_C(0x3fe0000000000000)),TF_CORE_OP_BINARY,
        TF_CORE_BINARY_MULTIPLY,FLOAT,true,floating(0)); /* tie underflow */
    operation(floating(UINT64_C(0x7fefffffffffffff)),floating(two),TF_CORE_OP_BINARY,
        TF_CORE_BINARY_MULTIPLY,FLOAT,false,floating(0));
    operation(integer(5),integer(2),TF_CORE_OP_BINARY,TF_CORE_BINARY_REAL_DIVIDE,FLOAT,true,
        floating(UINT64_C(0x4004000000000000)));
    operation(floating(one),integer(0),TF_CORE_OP_BINARY,TF_CORE_BINARY_REAL_DIVIDE,FLOAT,false,floating(0));
    operation(floating(0),integer(-1),TF_CORE_OP_BINARY,TF_CORE_BINARY_POWER,FLOAT,false,floating(0));
    operation(floating(0),integer(0),TF_CORE_OP_UNARY,TF_CORE_UNARY_NEGATE,FLOAT,true,floating(0));
}

typedef struct { TfSourceManager *sources; TfCoreProgram *core; TfVerifiedCore *verified; } Program;
static Program load(const char *path) {
    Program p={0};
    TfResolution *resolution=NULL; TfTypedSignatures *signatures=NULL;
    TfTypedProgram *typed=NULL; TfMachineProgram *machines=NULL;
    CHECK(tf_source_manager_create(&p.sources,NULL)==TF_OK);
    const TfStatus resolved=tf_resolve(&resolution,p.sources,path,NULL,NULL);
    if(resolved!=TF_OK) fprintf(stderr,"cannot resolve %s (status %d)\n",path,(int)resolved);
    CHECK(resolved==TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(resolution))==TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&signatures,resolution,100,NULL)==TF_OK);
    CHECK(tf_check(&typed,resolution,signatures,100,NULL)==TF_OK);
    CHECK(tf_machine_program_build(&machines,typed,100,NULL)==TF_OK);
    CHECK(tf_core_lower(&p.core,machines,NULL,NULL)==TF_OK);
    CHECK(tf_core_verify(&p.verified,p.core,NULL,NULL)==TF_OK);
    tf_machine_program_destroy(machines); tf_typed_program_destroy(typed);
    tf_signatures_destroy(signatures); tf_resolution_destroy(resolution);
    return p;
}
static void destroy(Program *p) {
    tf_verified_core_destroy(p->verified); tf_core_program_destroy(p->core);
    tf_source_manager_destroy(p->sources);
}

static void preservation(const TfCoreProgramData *a, const TfCoreProgramData *b) {
    CHECK(a->type_count==b->type_count && a->function_count==b->function_count);
    CHECK(a->span_count==b->span_count && a->entry==b->entry);
    CHECK(a->trace_machine_count==b->trace_machine_count && a->trace_point_count==b->trace_point_count);
    for(size_t i=0;i<a->span_count;++i)
        CHECK(a->spans[i].source==b->spans[i].source && a->spans[i].start==b->spans[i].start && a->spans[i].end==b->spans[i].end);
    for(size_t i=0;i<a->trace_point_count;++i) {
        const TfCoreTracePoint *x=&a->trace_points[i], *y=&b->trace_points[i];
        CHECK(x->function==y->function && x->block==y->block && x->after_let==y->after_let &&
            x->value==y->value && x->kind==y->kind && x->ordinal==y->ordinal && x->span==y->span);
    }
    for(size_t f=0;f<a->function_count;++f) {
        const TfCoreFunction *x=&a->functions[f],*y=&b->functions[f];
        CHECK(x->block_count==y->block_count && x->value_count==y->value_count && x->body==y->body);
        for(size_t j=0;j<x->block_count;++j) {
            CHECK(x->blocks[j].let_count==y->blocks[j].let_count);
            CHECK(x->blocks[j].terminator.value==y->blocks[j].terminator.value);
            for(size_t k=0;k<x->blocks[j].let_count;++k) {
                CHECK(x->blocks[j].lets[k].value==y->blocks[j].lets[k].value);
                CHECK(x->blocks[j].lets[k].span==y->blocks[j].lets[k].span);
            }
        }
    }
}

static void source_equivalence(const char *path) {
    Program p=load(path);
    TfCoreOptimization *out=NULL;
    CHECK(tf_core_optimize(&out,p.verified,TF_CORE_OPT_LEVEL_1,NULL)==TF_OK);
    const TfVerifiedCore *opt=tf_core_optimization_verified(out);
    preservation(tf_verified_core_data(p.verified),tf_verified_core_data(opt));
    TfCoreOptimization *again=NULL;
    CHECK(tf_core_optimize(&again,opt,TF_CORE_OPT_LEVEL_1,NULL)==TF_OK);
    CHECK(tf_core_optimization_fold_count(again)==0);
    CHECK(tf_verified_core_data(tf_core_optimization_verified(again))->constant_count==tf_verified_core_data(opt)->constant_count);
    tf_core_optimization_destroy(again);
    TfTraceRecord records[2][128]; TfTraceRecorder trace[2];
    TfInterpreterResult *results[2]={NULL,NULL}; TfRuntimeFault faults[2]; TfStatus status[2];
    for(size_t i=0;i<2;++i) {
        CHECK(tf_trace_recorder_init(&trace[i],records[i],128));
        status[i]=tf_interpret_entry_traced(&results[i],i==0?p.verified:opt,NULL,0,NULL,NULL,&faults[i],&trace[i]);
    }
    CHECK(status[0]==TF_OK || status[0]==TF_SOURCE_ERROR);
    CHECK(status[0]==status[1] && faults[0].code==faults[1].code && faults[0].span==faults[1].span);
    CHECK(tf_interpreter_result_count(results[0])==tf_interpreter_result_count(results[1]));
    for(size_t i=0;i<tf_interpreter_result_count(results[0]);++i) {
        TfCoreText a,b;
        CHECK(tf_interpreter_result_line(results[0],i,&a) && tf_interpreter_result_line(results[1],i,&b));
        CHECK(a.length==b.length && (a.length==0 || memcmp(a.bytes,b.bytes,a.length)==0));
    }
    CHECK(trace[0].count==trace[1].count && trace[0].truncated==trace[1].truncated);
    if(tf_verified_core_data(p.verified)->trace_machine_count!=0) CHECK(trace[0].count!=0);
    for(size_t i=0;i<trace[0].count;++i) {
        const TfTraceRecord *a=&records[0][i],*b=&records[1][i];
        CHECK(a->event.kind==b->event.kind && a->event.function_id==b->event.function_id &&
            a->event.span_id==b->event.span_id && a->event.let_id==b->event.let_id &&
            a->event.invocation_id==b->event.invocation_id && a->event.parent_invocation_id==b->event.parent_invocation_id);
        CHECK(a->preview_length==b->preview_length && memcmp(a->preview,b->preview,a->preview_length)==0);
    }
    for(size_t i=0;i<2;++i) tf_interpreter_result_destroy(results[i]);
    /* Reject malformed observation metadata before the optimizer can receive it. */
    const TfCoreProgramData *data=tf_verified_core_data(p.verified);
    if(data->trace_point_count!=0) {
        static bool injected_source;
        if(!injected_source) { failures(p.verified); injected_source=true; }
        TfCoreProgramData bad=*data;
        TfCoreTracePoint *points=malloc(data->trace_point_count*sizeof(*points)); CHECK(points!=NULL);
        memcpy(points,data->trace_points,data->trace_point_count*sizeof(*points)); bad.trace_points=points;
        for(unsigned mutation=0;mutation<4;++mutation) {
            points[0]=data->trace_points[0];
            if(mutation==0) points[0].value=SIZE_MAX;
            if(mutation==1) points[0].after_let=SIZE_MAX;
            if(mutation==2) points[0].block=SIZE_MAX;
            if(mutation==3) points[0].function=SIZE_MAX;
            TfCoreProgram *owner=NULL; TfVerifiedCore *v=NULL; TfCoreVerifyIssue issue={0};
            CHECK(tf_core_program_copy(&owner,&bad,NULL)==TF_OK);
            CHECK(tf_core_verify(&v,owner,NULL,&issue)==TF_SOURCE_ERROR && v==NULL);
            CHECK(issue.code==TF_CORE_VERIFY_OBSERVATION);
            tf_core_program_destroy(owner);
        }
        free(points);
    }
    /* Output stays usable after input Core and verified handle are destroyed. */
    tf_verified_core_destroy(p.verified); p.verified=NULL;
    tf_core_program_destroy(p.core); p.core=NULL;
    CHECK(tf_verified_core_data(opt)->function_count!=0);
    tf_core_optimization_destroy(out); destroy(&p);
}

static void graph_rejections(const char *path,const char *code) {
    TfSourceManager *sources=NULL; TfResolution *resolution=NULL; TfTypedSignatures *sig=NULL;
    TfTypedProgram *typed=NULL; TfMachineProgram *machines=NULL; TfCoreProgram *core=NULL;
    CHECK(tf_source_manager_create(&sources,NULL)==TF_OK);
    CHECK(tf_resolve(&resolution,sources,path,NULL,NULL)==TF_OK);
    CHECK(tf_signatures_build(&sig,resolution,100,NULL)==TF_OK);
    CHECK(tf_check(&typed,resolution,sig,100,NULL)==TF_OK);
    CHECK(tf_machine_program_build(&machines,typed,100,NULL)==TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_machine_program_phase(machines))==TF_DIAG_PHASE_ERROR);
    const TfDiagnosticBag *bag=tf_machine_program_diagnostics(machines);
    CHECK(tf_diagnostic_bag_count(bag)!=0);
    CHECK(strcmp(tf_diagnostic_bag_get(bag,0)->code,code)==0);
    CHECK(tf_core_lower(&core,machines,NULL,NULL)==TF_HOST_ERROR && core==NULL);
    /* No verified handle exists: neither optimization level admits this graph. */
    for(unsigned level=0;level<=1;++level) {
        TfCoreOptimization *out=NULL;
        CHECK(tf_core_optimize(&out,NULL,(TfCoreOptLevel)level,NULL)==TF_HOST_ERROR && out==NULL);
    }
    tf_machine_program_destroy(machines); tf_typed_program_destroy(typed); tf_signatures_destroy(sig);
    tf_resolution_destroy(resolution); tf_source_manager_destroy(sources);
}

static int command(int argc,char **argv) {
    CHECK(argc==5 && (strcmp(argv[1],"--emit")==0 || strcmp(argv[1],"--reference")==0));
    CHECK(strcmp(argv[3],"0")==0 || strcmp(argv[3],"1")==0);
    Program p=load(argv[2]); TfCoreOptimization *out=NULL;
    CHECK(tf_core_optimize(&out,p.verified,strcmp(argv[3],"0")==0?TF_CORE_OPT_LEVEL_0:TF_CORE_OPT_LEVEL_1,NULL)==TF_OK);
    const TfVerifiedCore *v=tf_core_optimization_verified(out);
    FILE *file=fopen(argv[4],"wb"); CHECK(file!=NULL);
    int exit_code=0;
    if(strcmp(argv[1],"--emit")==0) CHECK(tf_c_emit(file,v)==TF_OK);
    else {
        TfInterpreterResult *r=NULL; TfRuntimeFault fault={0};
        const TfStatus status=tf_interpret_entry(&r,v,NULL,0,NULL,NULL,&fault);
        CHECK(status==TF_OK || status==TF_SOURCE_ERROR);
        if(status==TF_OK) {
            for(size_t i=0;i<tf_interpreter_result_count(r);++i) {
                TfCoreText line; CHECK(tf_interpreter_result_line(r,i,&line));
                CHECK(fwrite(line.bytes,1,line.length,file)==line.length && fputc('\n',file)!=EOF);
            }
        } else {
            const TfSpan *span=tf_core_span(tf_verified_core_data(v),fault.span);
            CHECK(span!=NULL);
            fprintf(stderr,"R%03u [bytes %zu..%zu)\n",(unsigned)fault.code,span->start,span->end);
            exit_code=3; /* Match the established native/CLI runtime-fault exit. */
        }
        tf_interpreter_result_destroy(r);
    }
    CHECK(fclose(file)==0);
    tf_core_optimization_destroy(out); destroy(&p); return exit_code;
}

int main(int argc,char **argv) {
    if(argc!=1) return command(argc,argv);
    CHECK(tf_core_optimize(NULL,NULL,TF_CORE_OPT_LEVEL_0,NULL)==TF_HOST_ERROR);
    TfCoreOptimization *out=NULL;
    CHECK(tf_core_optimize(&out,NULL,(TfCoreOptLevel)2,NULL)==TF_HOST_ERROR && out==NULL);
    CHECK(tf_core_optimization_verified(NULL)==NULL && tf_core_optimization_fold_count(NULL)==0);
    tf_core_optimization_destroy(NULL);
    scalar_matrix();
    const char *fixtures[]={"benchmarks/scalars.tfd","examples/intro/counter.tfd","examples/features/snapshot_swap.tfd",
        "examples/features/dependency_order.tfd","examples/features/guard_short_circuit.tfd",
        "examples/features/fold_and_closure.tfd","tests/fixtures/differential/numeric.tfd",
        "tests/fixtures/types/interpreter-candidate-order.tfd","tests/fixtures/types/interpreter-late-guard.tfd",
        "tests/fixtures/types/interpreter-late-emission.tfd","tests/fixtures/types/interpreter-negative-power.tfd",
        "tests/fixtures/differential/backend-edge-record-div-first.tfd",
        "tests/fixtures/differential/backend-edge-record-overflow-first.tfd"};
    for(size_t i=0;i<sizeof fixtures/sizeof *fixtures;++i) source_equivalence(fixtures[i]);
    graph_rejections("benchmarks/cycle.tfd","E404");
    graph_rejections("benchmarks/duplicate-writer.tfd","E402");
    printf("optimization: %zu checks passed\n",checks); return 0;
}
