/* Standalone generated suite; share the existing argv, timeout, capture and
 * scratch-directory implementation without changing the fixed suite's entry. */
#define main tf_fixed_differential_main
#include "../test_differential.c"
#undef main
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>

enum { MAX_TERMS=6, MAX_CELLS=4, SOURCE_CAP=4096, OUTPUT_CAP=256,
       CORPUS=18, SHRINK_BUDGET=24 };

typedef enum { SCALAR, ACCEPT, REJECT, OVERFLOW, ZERO_DIVISOR, NEGATIVE_POWER } Family;
typedef enum { ADD, SUB, MUL, FLOOR_DIV, REM, CHOOSE, OP_COUNT } Op;
typedef struct { Op op; int arg; } Term;
typedef struct {
    Family family;
    int initial, count, fault_variant;
    Term terms[MAX_TERMS];
    int cells, old[MAX_CELLS], parent[MAX_CELLS], order[MAX_CELLS];
} Program;
typedef struct {
    char source[SOURCE_CAP], output[OUTPUT_CAP];
    size_t used, begin, end;
    const char *code, *message;
} Render;
typedef struct {
    int status, code; /* observed runtime code number (0 if none) */
    bool output_wrong, report_wrong;
} Observation;
typedef struct { Observation ref, c; } Failure;
typedef struct { const char *compiler, *cc, *root; bool sanitize; } Toolchain;

static uint32_t random_word(uint32_t *state) {
    /* Fixed-width unsigned arithmetic gives the same stream on every host. */
    *state=*state*UINT32_C(1664525)+UINT32_C(1013904223);
    return *state ^ (*state >> 16);
}
static int draw(uint32_t *state, uint32_t n) {
    return (int)(random_word(state)%n);
}
static Program generate(uint32_t seed, unsigned index) {
    uint32_t state=seed ^ ((uint32_t)(index+1u)*UINT32_C(0x9e3779b9));
    Program p={0};
    p.family=(Family)(index%6u);
    p.initial=draw(&state,19)-9;
    p.count=1+draw(&state,MAX_TERMS);
    for (int i=0;i<p.count;++i) {
        p.terms[i].op=(Op)draw(&state,OP_COUNT);
        p.terms[i].arg=1+draw(&state,3);
    }
    p.fault_variant=draw(&state,2);
    p.cells=2+draw(&state,MAX_CELLS-1);
    for (int i=0;i<p.cells;++i) {
        p.old[i]=draw(&state,19)-9;
        p.parent[i]=draw(&state,(uint32_t)(i+1))-1;
        p.order[i]=i;
    }
    for (int i=p.cells-1;i>0;--i) {
        const int j=draw(&state,(uint32_t)(i+1));
        const int saved=p.order[i];
        p.order[i]=p.order[j]; p.order[j]=saved;
    }
    return p;
}
static void emit(Render *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    const int n=vsnprintf(r->source+r->used,sizeof r->source-r->used,fmt,ap);
    va_end(ap);
    CHECK(n>=0 && (size_t)n<sizeof r->source-r->used);
    r->used+=(size_t)n;
}

static int terms(Render *r, const Program *p) {
    static const char *const symbols[]={"+","-","*","div","%"};
    int value=p->initial;
    emit(r,"    let v0 = %d;\n",value);
    for (int i=0;i<p->count;++i) {
        const Term t=p->terms[i];
        if (t.op==CHOOSE) {
            /* A typed Bool selects between two safe Int values. This never turns
             * a value case into an expected-fault case. */
            emit(r,"    let v%d = if v%d < %d { v%d } else { %d };\n",
                 i+1,i,t.arg,i,t.arg);
            if (value>=t.arg) value=t.arg;
        } else {
            emit(r,"    let v%d = v%d %s %d;\n",i+1,i,symbols[t.op],t.arg);
            /* At most six operations: |value| <= 9 * 3^6 = 6561.
             * Four candidate cells still fit even a 16-bit C int. */
            int next=value;
            switch (t.op) {
            case ADD: next+=t.arg; break;
            case SUB: next-=t.arg; break;
            case MUL: next*=t.arg; break;
            case FLOOR_DIV: next=(next < 0 ? next-(t.arg-1) : next)/t.arg; break;
            case REM: next%=t.arg; break;
            default: CHECK(false);
            }
            CHECK(next>=-6561 && next<=6561);
            value=next;
        }
    }
    return value;
}

static void emit_machine(Render *r, const Program *p) {
    emit(r,"machine Graph(delta: Int) yields Int rejects Text {\n");
    for (int i=0;i<p->cells;++i)
        emit(r,"    state c%d: Int = %d;\n",i,p->old[i]);
    for (int j=0;j<p->cells;++j) {
        const int i=p->order[j], parent=p->parent[i];
        /* Read an old cell even when that cell also has a writer. Every
         * candidate edge points to a lower ID; written order is shuffled. */
        emit(r,"    next c%d = c%d + delta",i,(i+1)%p->cells);
        if (parent>=0) emit(r," + next.c%d",parent);
        emit(r,";\n");
    }
    if (p->family==REJECT)
        emit(r,"    guard false else \"first\";\n    guard false else \"second\";\n");
    for (int i=0;i<p->cells;++i) emit(r,"    emit next.c%d;\n",i);
    emit(r,"}\n");
}
static void machine_result(Render *r, const Program *p, int delta) {
    int candidate[MAX_CELLS]={0};
    emit(r,"    match step(Graph(), v%d) {\n",p->count);
    emit(r,"        Decision::Accept(snap, values) => { map(values, int_text)"
           " ++ [int_text(snap.c0)] },\n"
           "        Decision::Reject(snap, reason) => { [reason, int_text(snap.c0)] }\n"
           "    }\n");
    if (p->family==REJECT) {
        const int n=snprintf(r->output,sizeof r->output,"first\n%d\n",p->old[0]);
        CHECK(n>0 && (size_t)n<sizeof r->output);
    } else {
        size_t used=0;
        for (int i=0;i<=p->cells;++i) {
            if (i<p->cells) {
                const int parent=p->parent[i];
                candidate[i]=p->old[(i+1)%p->cells]+delta+(parent<0 ? 0 : candidate[parent]);
            }
            const int n=snprintf(r->output+used,sizeof r->output-used,"%d\n",
                                 candidate[i%p->cells]);
            CHECK(n>0 && (size_t)n<sizeof r->output-used);
            used+=(size_t)n;
        }
    }
}
static void emit_fault(Render *r, const Program *p) {
    const char *lhs, *op, *rhs, *later;
    switch (p->family) {
    case OVERFLOW:
        lhs=p->fault_variant ? "-9223372036854775808" : "9223372036854775807";
        op=p->fault_variant ? "-" : "+"; rhs="1"; later="7 div 0";
        r->code="R001"; r->message="arithmetic overflow or nonfinite result"; break;
    case ZERO_DIVISOR:
        lhs="7"; op=p->fault_variant ? "%" : "div"; rhs="0"; later="2 ^ -1";
        r->code="R002"; r->message="division or remainder by zero"; break;
    case NEGATIVE_POWER:
        lhs=p->fault_variant ? "0" : "2"; op="^"; rhs="-1"; later="9223372036854775807 + 1";
        r->code="R005"; r->message="negative exponent for Int power"; break;
    default: CHECK(false); return;
    }
    /* Keep both the first fault and its competing fault during reduction.
     * The source map is recomputed when preceding bindings are removed. */
    emit(r,"    output int_text(v%d);\n    let first = %s ",p->count,lhs);
    r->begin=r->used;
    emit(r,"%s",op);
    r->end=r->used;
    emit(r," %s;\n    let second = %s;\n    [int_text(first), int_text(second)]\n",rhs,later);
}

static Render render(const Program *p) {
    Render r={0};
    const bool machine=p->family==ACCEPT || p->family==REJECT;
    if (machine) emit_machine(&r,p);
    emit(&r,"pub fn main() -> List[Text] {\n");
    const int value=terms(&r,p);
    if (machine) machine_result(&r,p,value);
    else if (p->family==SCALAR) {
        emit(&r,"    [int_text(v%d), int_text(-9223372036854775808), "
                "int_text(9223372036854775807)]\n",p->count);
        const int n=snprintf(r.output,sizeof r.output,
                             "%d\n-9223372036854775808\n9223372036854775807\n",value);
        CHECK(n>0 && (size_t)n<sizeof r.output);
    } else emit_fault(&r,p);
    emit(&r,"}\n");
    return r;
}

static bool bytes_equal(Bytes bytes, const char *expected) {
    const size_t n=strlen(expected);
    return bytes.length==n && memcmp(bytes.data,expected,n)==0;
}

/* Generated source is ASCII (no tabs): byte-derived scalar columns and
 * display columns coincide. The compiler renders paths differently in JSON
 * and in the native launcher; neither expected report is copied from a peer. */
static bool fault_matches(Bytes bytes, const Render *r, const char *path, bool ref) {
    char quoted[PATH_CAP*6+1], report[PATH_CAP*6+512];
    size_t n=0;
    for (const unsigned char *p=(const unsigned char *)path;*p;++p) {
        const unsigned char ch=*p;
        if (ref && (ch=='\\' || ch=='"')) {
            quoted[n++]='\\'; quoted[n++]=(char)ch;
        } else if (ref && (ch<32 || ch==127)) {
            CHECK(snprintf(quoted+n,sizeof quoted-n,"\\u%04x",(unsigned)ch)==6);
            n+=6;
        } else if (!ref && (ch<32 || ch>126 || ch=='\\' || ch=='"')) {
            CHECK(snprintf(quoted+n,sizeof quoted-n,"\\x%02x",(unsigned)ch)==4);
            n+=4;
        } else quoted[n++]=(char)ch;
    }
    quoted[n]='\0';
    const Location a=location(r->source,r->begin), b=location(r->source,r->end);
    int length;
    if (ref) {
        length=snprintf(report,sizeof report,
            "{\"schema_version\":2,\"diagnostics\":[{\"code\":\"%s\",\"severity\":\"error\","
            "\"message\":\"%s\",\"labels\":[{\"primary\":true,\"message\":\"\",\"span\":{"
            "\"source\":\"%s\",\"start_byte\":%zu,\"end_byte\":%zu,"
            "\"start\":{\"line\":%zu,\"column\":%zu,\"display_column\":%zu},"
            "\"end\":{\"line\":%zu,\"column\":%zu,\"display_column\":%zu},"
            "\"source_text\":\"%.*s\",\"raw\":false}}],\"notes\":[],\"help\":[],\"edits\":[]}],"
            "\"suppressed\":0,\"has_errors\":true}\n",
            r->code,r->message,quoted,r->begin,r->end,a.line,a.column,a.column,
            b.line,b.column,b.column,(int)(r->end-r->begin),r->source+r->begin);
    } else {
        length=snprintf(report,sizeof report,"error[%s]: %s\n  --> \"%s\":%zu:%zu..%zu:%zu [bytes %zu..%zu)\n",
                        r->code,r->message,quoted,a.line,a.column,b.line,b.column,r->begin,r->end);
    }
    CHECK(length>=0 && (size_t)length<sizeof report);
    return bytes_equal(bytes,report);
}

static int observed_code(Bytes bytes, bool ref) {
    const char *prefix=ref ? "{\"schema_version\":2,\"diagnostics\":[{\"code\":\"" : "error[";
    const size_t n=strlen(prefix);
    if (bytes.length<n+5 || memcmp(bytes.data,prefix,n)!=0 ||
        bytes.data[n]!='R' || bytes.data[n+1]!='0' || bytes.data[n+2]!='0' ||
        bytes.data[n+3]<'1' || bytes.data[n+3]>'9' ||
        bytes.data[n+4]!=(ref ? '"' : ']')) return 0;
    return bytes.data[n+3]-'0';
}

static Observation observe(int status, const Render *r, const char *path, bool ref,
                           const char *capture_prefix) {
    /* Static rejections, host failures and resource-limit faults fail hard.
     * Never confuse them with a shrinkable semantic disagreement. */
    CHECK(status==0 || status==3);
    const Bytes stdout_bytes=read_bytes(out), stderr_bytes=read_bytes(err);
    const int code=observed_code(stderr_bytes,ref);
    CHECK(code!=3);
    const Observation result={status,code,!bytes_equal(stdout_bytes,r->output),
        !(r->code==NULL ? bytes_equal(stderr_bytes,ref ? LF_OK : "") :
          fault_matches(stderr_bytes,r,path,ref))};
    if (capture_prefix!=NULL) {
        char capture[PATH_CAP], leaf[64];
        int n=snprintf(leaf,sizeof leaf,"%s.stdout",capture_prefix);
        CHECK(n>0 && (size_t)n<sizeof leaf); join(capture,work,leaf);
        write_bytes(capture,stdout_bytes.data,stdout_bytes.length);
        n=snprintf(leaf,sizeof leaf,"%s.stderr",capture_prefix);
        CHECK(n>0 && (size_t)n<sizeof leaf); join(capture,work,leaf);
        write_bytes(capture,stderr_bytes.data,stderr_bytes.length);
    }
    free(stdout_bytes.data); free(stderr_bytes.data);
    return result;
}

static Failure execute_case(const Toolchain *tools, const Render *r, unsigned capture) {
    char source[PATH_CAP], absolute[PATH_CAP];
    join(source,work,"generated.tfd");
    origin_path(source,absolute);
    write_bytes(source,r->source,r->used);
    const char *ref_args[]={tools->compiler,"run",source,"--root",work,"--diagnostics-json",NULL};
    const Observation ref=observe(spawn(ref_args),r,absolute,true,capture==2 ? "min-reference" : capture==1 ? "reference" : NULL);
    build(tools->compiler,tools->cc,tools->root,source,work,tools->sanitize,0);
    expect(err,"",0);
    const char *native_args[]={native,NULL};
    const Observation c=observe(spawn(native_args),r,absolute,false,capture==2 ? "min-native" : capture==1 ? "native" : NULL);
    remove_native();
    return (Failure){ref,c};
}

static bool equal_observation(Observation a, Observation b) {
    return a.status==b.status && a.code==b.code &&
           a.output_wrong==b.output_wrong && a.report_wrong==b.report_wrong;
}
static bool equal_failure(Failure a, Failure b) {
    return equal_observation(a.ref,b.ref) && equal_observation(a.c,b.c);
}
static bool passed(const Render *r, Failure observed) {
    const Observation expected={r->code==NULL ? 0 : 3,r->code==NULL ? 0 : r->code[3]-'0',false,false};
    return equal_failure(observed,(Failure){expected,expected});
}

typedef Failure (*Evaluate)(void *,const Render *);
static bool same_counterexample(const Program *p, Failure target, Evaluate evaluate, void *context) {
    const Render r=render(p);
    const Failure actual=evaluate(context,&r);
    return !passed(&r,actual) && equal_failure(target,actual);
}
static Program shrink(Program p, Failure target, Evaluate evaluate, void *context, unsigned *attempts) {
    /* Remove whole typed bindings and then reduce their literals. Each
     * accepted edit decreases (term count, nonzero seed, operand sum).
     * Keep the guard policy and both fault sites fixed. */
    for (int i=0;i<p.count && *attempts<SHRINK_BUDGET;) {
        Program q=p;
        --q.count;
        for (int j=i;j<q.count;++j) q.terms[j]=q.terms[j+1];
        ++*attempts;
        if (same_counterexample(&q,target,evaluate,context)) { p=q; i=0; }
        else ++i;
    }
    if (p.initial!=0 && *attempts<SHRINK_BUDGET) {
        Program q=p;
        q.initial=0;
        ++*attempts;
        if (same_counterexample(&q,target,evaluate,context)) p=q;
    }
    for (int i=0;i<p.count && *attempts<SHRINK_BUDGET;++i) {
        if (p.terms[i].arg==1) continue;
        Program q=p;
        q.terms[i].arg=1;
        ++*attempts;
        if (same_counterexample(&q,target,evaluate,context)) p=q;
    }
    if (p.family==ACCEPT || p.family==REJECT) {
        while (p.cells>2 && *attempts<SHRINK_BUDGET) {
            Program q=p;
            --q.cells;
            int kept=0;
            for (int i=0;i<p.cells;++i)
                if (p.order[i]!=q.cells) q.order[kept++]=p.order[i];
            CHECK(kept==q.cells);
            ++*attempts;
            if (!same_counterexample(&q,target,evaluate,context)) break;
            p=q;
        }
        for (int i=0;i<p.cells && *attempts<SHRINK_BUDGET;++i) {
            if (p.parent[i]<0) continue;
            Program q=p;
            q.parent[i]=-1;
            ++*attempts;
            if (same_counterexample(&q,target,evaluate,context)) p=q;
        }
    }
    return p;
}
static Failure evaluate_case(void *context, const Render *r) {
    return execute_case(context,r,0);
}

static Failure shrink_probe(void *context, const Render *r) {
    (void)context;
    CHECK(strcmp(r->code,"R002")==0 && r->end-r->begin==3 &&
          memcmp(r->source+r->begin,"div",3)==0 && strstr(r->source,"2 ^ -1")!=NULL);
    const Observation good={3,2,false,false};
    /* Simulate an earlier competing R005 after a particular deletion.
     * The shrinker must retain the mutation that still yields the original
     * native output failure and must reject the different fault class. */
    const Observation bad=strstr(r->source," * 1;")!=NULL ?
        (Observation){3,2,true,false} : (Observation){3,5,false,true};
    return (Failure){good,bad};
}
static Failure graph_probe(void *context, const Render *r) {
    (void)context;
    CHECK(r->code==NULL && strstr(r->source,"guard false")==NULL);
    const char *writer=strstr(r->source,"next c2 = ");
    const char *edge=writer==NULL ? NULL : strstr(writer," + next.c1;");
    const Observation ok={0,0,false,false};
    const Observation peer={0,0,edge!=NULL && edge<strchr(writer,'\n'),false};
    return (Failure){ok,peer};
}
static void self_test(void) {
    Program p={.family=ZERO_DIVISOR,.initial=9,.count=3,.terms={{ADD,3},{MUL,1},{SUB,2}}};
    const Render original=render(&p);
    const Failure target=shrink_probe(NULL,&original);
    unsigned a=0,b=0;
    const Program first=shrink(p,target,shrink_probe,NULL,&a);
    const Program second=shrink(p,target,shrink_probe,NULL,&b);
    const Render x=render(&first), y=render(&second);
    CHECK(first.count==1 && first.initial==0 && first.terms[0].op==MUL);
    CHECK(a>0 && a<=SHRINK_BUDGET && a==b && strcmp(x.source,y.source)==0);
    CHECK(x.begin<original.begin && same_counterexample(&first,target,shrink_probe,NULL));
    CHECK(!passed(&x,(Failure){{3,2,false,false},{3,2,true,false}}));

    const Program graph={.family=ACCEPT,.cells=4,.old={4,3,2,1},
                         .parent={-1,0,1,2},.order={3,2,1,0}};
    const Render g=render(&graph);
    const Failure graph_target=graph_probe(NULL,&g);
    CHECK(!passed(&g,graph_target));
    unsigned attempts=0;
    const Program smaller=shrink(graph,graph_target,graph_probe,NULL,&attempts);
    CHECK(smaller.cells==3 && smaller.parent[1]==-1 && smaller.parent[2]==1);
    CHECK(smaller.order[0]==2 && smaller.order[1]==1 && smaller.order[2]==0);
}

static bool decimal(const char *arg, uint32_t *result) {
    if (*arg=='\0') return false;
    uint32_t n=0;
    for (;*arg;++arg) {
        if (*arg<'0' || *arg>'9') return false;
        const uint32_t digit=(uint32_t)(*arg-'0');
        if (n>(UINT32_MAX-digit)/10u) return false;
        n=n*10u+digit;
    }
    *result=n;
    return true;
}
int main(int argc, char **argv) {
    uint32_t seed=UINT32_C(141592653), only=CORPUS;
    const bool sanitize=argc>=5 && strcmp(argv[4],"--sanitize")==0;
    const int positional=sanitize ? 5 : 4;
    if (argc<4 || argc>positional+2 ||
        (argc>positional && !decimal(argv[positional],&seed)) ||
        (argc>positional+1 && (!decimal(argv[positional+1],&only) || only>=CORPUS))) {
        fprintf(stderr,"usage: %s COMPILER CC RUNTIME_ROOT [--sanitize] [SEED [CASE 0..%d]]\n",
                argv[0],CORPUS-1);
        return 2;
    }
    Toolchain tools={argv[1],argv[2],argv[3],sanitize};
    current="M14 shrinker self-test";
    self_test();
    setup();
    unsigned values=0,faults=0;
    for (unsigned i=0;i<CORPUS;++i) {
        if (only!=CORPUS && i!=only) continue;
        char label[96];
        const int n=snprintf(label,sizeof label,"M14 seed=%" PRIu32 " case=%u",seed,i);
        CHECK(n>0 && (size_t)n<sizeof label);
        current=label;
        const Program p=generate(seed,i);
        const Render r=render(&p);
        const Failure observed=execute_case(&tools,&r,1);
        if (!passed(&r,observed)) {
            char artifact[PATH_CAP];
            join(artifact,work,"original.tfd");
            write_bytes(artifact,r.source,r.used);
            fprintf(stderr,"%s failed: reference=(%d,R%03d,output=%d,report=%d), "
                    "native=(%d,R%03d,output=%d,report=%d); captures in %s\n",
                    current,observed.ref.status,observed.ref.code,observed.ref.output_wrong,
                    observed.ref.report_wrong,observed.c.status,observed.c.code,
                    observed.c.output_wrong,observed.c.report_wrong,work);
            unsigned attempts=0;
            const Program reduced=shrink(p,observed,evaluate_case,&tools,&attempts);
            const Render small=render(&reduced);
            join(artifact,work,"minimized.tfd");
            write_bytes(artifact,small.source,small.used);
            CHECK(equal_failure(observed,execute_case(&tools,&small,2)));
            fprintf(stderr,"%s: %zu -> %zu source bytes in %u shrink attempts\n",
                    current,r.used,small.used,attempts);
            return 1;
        }
        if (r.code==NULL) ++values; else ++faults;
    }
    current="M14 cleanup";
    static const char *const files[]={"generated.tfd","reference.stdout","reference.stderr","native.stdout","native.stderr"};
    for (size_t i=0;i<sizeof files/sizeof *files;++i) {
        char file[PATH_CAP]; join(file,work,files[i]); CHECK(remove(file)==0);
    }
    CHECK(remove(out)==0); CHECK(remove(err)==0); CHECK(REMOVE_DIR(work)==0);
    printf("M14 generated seed=%" PRIu32 ": %u value and %u fault cases passed (shrink budget %d)\n",
           seed,values,faults,SHRINK_BUDGET);
    return 0;
}
