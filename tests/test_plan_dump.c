/* Independent decoded schema/round-trip assertions for machine plan JSON v1. */
#include "ternfold/plan_dump.h"
#include "../src/typed_signatures.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static size_t checks;
static void check(bool okay, int line, const char *expression) {
    ++checks;
    if (!okay) { fprintf(stderr,"test_plan_dump.c:%d: %s\n",line,expression); exit(1); }
}
#define CHECK(x) check((x),__LINE__,#x)
enum { NODES = 4096, STRINGS = 131072 };
typedef struct { char type; size_t next, offset, length; uint64_t number; } Node;
typedef struct {
    const unsigned char *input; size_t length, at, count, used;
    Node nodes[NODES]; unsigned char strings[STRINGS];
} Json;
static void ws(Json *j) {
    while (j->at < j->length && j->input[j->at] != 0 && strchr(" \t\r\n",j->input[j->at]) != NULL) ++j->at;
}
static unsigned hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10u;
    if (c >= 'A' && c <= 'F') return c-'A'+10u;
    CHECK(false); return 0;
}
static uint32_t unit(Json *j) {
    uint32_t out = 0;
    CHECK(j->length-j->at >= 4);
    for (unsigned i=0;i<4;++i) out=(out<<4)|hex(j->input[j->at++]);
    return out;
}
static void append(Json *j, unsigned char c) {
    CHECK(j->used < STRINGS); j->strings[j->used++]=c;
}
static void scalar(Json *j, uint32_t c) {
    CHECK(c <= 0x10ffffu && !(c >= 0xd800u && c <= 0xdfffu));
    if(c < 0x80u) append(j,(unsigned char)c);
    else if(c < 0x800u) {append(j,(unsigned char)(0xc0u|(c>>6)));append(j,(unsigned char)(0x80u|(c&63u)));}
    else if(c < 0x10000u) {append(j,(unsigned char)(0xe0u|(c>>12)));append(j,(unsigned char)(0x80u|((c>>6)&63u)));append(j,(unsigned char)(0x80u|(c&63u)));}
    else {append(j,(unsigned char)(0xf0u|(c>>18)));append(j,(unsigned char)(0x80u|((c>>12)&63u)));append(j,(unsigned char)(0x80u|((c>>6)&63u)));append(j,(unsigned char)(0x80u|(c&63u)));}
}
static size_t parse(Json *j, unsigned depth) {
    size_t id; unsigned char c;
    CHECK(depth < 32 && j->count < NODES); ws(j); CHECK(j->at < j->length);
    id=j->count++; c=j->input[j->at++]; j->nodes[id].type=(char)c;
    if(c=='"') {
        j->nodes[id].offset=j->used;
        for (;;) {
            CHECK(j->at < j->length); c=j->input[j->at++];
            if(c=='"') break;
            CHECK(c>=32);
            if(c=='\\') {
                CHECK(j->at < j->length); c=j->input[j->at++];
                if(c=='u') {
                    uint32_t cp=unit(j);
                    if(cp>=0xd800u && cp<=0xdbffu) {
                        uint32_t low;
                        CHECK(j->length-j->at>=2 && j->input[j->at]=='\\' && j->input[j->at+1]=='u');j->at+=2;
                        low=unit(j);CHECK(low>=0xdc00u&&low<=0xdfffu);
                        cp=0x10000u+((cp-0xd800u)<<10)+(low-0xdc00u);
                    }
                    scalar(j,cp); continue;
                }
                switch(c) {
                    case '"':case '\\':case '/':break;
                    case 'b':c='\b';break; case 'f':c='\f';break;
                    case 'n':c='\n';break; case 'r':c='\r';break;case 't':c='\t';break;
                    default:CHECK(false);
                }
            }
            append(j,c);
        }
        j->nodes[id].length=j->used-j->nodes[id].offset;
    } else if(c=='{' || c=='[') {
        unsigned char close=c=='{'?'}':']';ws(j);CHECK(j->at<j->length);
        if(j->input[j->at]!=close) for(;;) {
            if(c=='{') {size_t key=parse(j,depth+1);CHECK(j->nodes[key].type=='"');ws(j);CHECK(j->at<j->length&&j->input[j->at++]==':');}
            (void)parse(j,depth+1);ws(j);CHECK(j->at<j->length);
            if(j->input[j->at]==close) break;
            CHECK(j->input[j->at++]==',');
        }
        ++j->at;
    } else if(c=='t'||c=='f'||c=='n') {
        const char *tail=c=='t'?"rue":c=='f'?"alse":"ull";size_t n=strlen(tail);
        CHECK(n<=j->length-j->at&&!memcmp(j->input+j->at,tail,n));j->at+=n;
    } else {
        CHECK(c>='0'&&c<='9');j->nodes[id].type='0';j->nodes[id].number=c-'0';
        while(j->at<j->length&&j->input[j->at]>='0'&&j->input[j->at]<='9') {
            unsigned digit=j->input[j->at++]-'0';
            CHECK(c != '0');
            CHECK(j->nodes[id].number<=(UINT64_MAX-digit)/10u);
            j->nodes[id].number=j->nodes[id].number*10u+digit;
        }
    }
    j->nodes[id].next=j->count;return id;
}
static void text(const Json *j,size_t id,const void *data,size_t length) {
    CHECK(id<j->count&&j->nodes[id].type=='"'&&j->nodes[id].length==length);
    CHECK(length==0||!memcmp(j->strings+j->nodes[id].offset,data,length));
}
static size_t field(const Json *j,size_t object,const char *key) {
    size_t found=j->count,n=strlen(key);
    CHECK(object<j->count&&j->nodes[object].type=='{');
    for(size_t i=object+1;i<j->nodes[object].next;) {
        CHECK(j->nodes[i].type=='"');
        if(j->nodes[i].length==n&&!memcmp(j->strings+j->nodes[i].offset,key,n)) {CHECK(found==j->count);found=i+1;}
        i=j->nodes[i+1].next;
    }
    CHECK(found<j->count);return found;
}
static void members(const Json *j,size_t node,char type,size_t count) {
    size_t actual=0;
    CHECK(j->nodes[node].type==type);
    for(size_t i=node+1;i<j->nodes[node].next;i=j->nodes[i].next) ++actual;
    CHECK(actual==(type=='{'?count*2:count));
}
static void number(const Json *j,size_t id,size_t expected) {
    CHECK(j->nodes[id].type=='0'&&j->nodes[id].number==(uint64_t)expected);
}

/* Bounded independent decoder above is adapted from test_diagnostic_json.c.
 * It shares no rendering code. These checks intentionally freeze JSON v1. */
static size_t element(const Json *j, size_t array, size_t index) {
    size_t at = array + 1;
    CHECK(j->nodes[array].type == '[');
    while (index-- != 0) { CHECK(at < j->nodes[array].next); at = j->nodes[at].next; }
    CHECK(at < j->nodes[array].next);
    return at;
}

static void span(const Json *j, size_t id, TfSpan expected) {
    members(j,id,'{',2);
    number(j,field(j,id,"start_byte"),expected.start);
    number(j,field(j,id,"end_byte"),expected.end);
    CHECK(expected.source != NULL && expected.start <= expected.end && expected.end <= expected.source->length);
}

static void clause(const Json *j, size_t id, TfMachineClause expected) {
    members(j,id,'{',3);
    number(j,field(j,id,"node"),expected.node.index);
    number(j,field(j,id,"expression"),expected.expression.index);
    span(j,field(j,id,"span"),expected.span);
}

static void indexes(const Json *j, size_t id, const size_t *expected, size_t count) {
    members(j,id,'[',count);
    for (size_t i=0; i<count; ++i) number(j,element(j,id,i),expected[i]);
}

enum { OUTPUT_LIMIT = 131072 };
static char *read_stream(FILE *stream) {
    char *result = malloc(OUTPUT_LIMIT);
    size_t count;
    CHECK(result != NULL);
    CHECK(fflush(stream) == 0 && fseek(stream,0,SEEK_SET) == 0);
    count = fread(result,1,OUTPUT_LIMIT-1,stream);
    CHECK(!ferror(stream) && feof(stream));
    CHECK(memchr(result,'\0',count) == NULL);
    result[count] = '\0';
    return result;
}

static char *render(const TfMachineProgram *program, bool json) {
    FILE *stream = tmpfile();
    char *result;
    CHECK(stream != NULL);
    CHECK(tf_plan_dump_write(stream,program,json) == TF_OK);
    result = read_stream(stream);
    CHECK(fclose(stream) == 0);
    return result;
}

static Json *decode(const char *output) {
    Json *j = calloc(1,sizeof *j);
    CHECK(j != NULL);
    j->input = (const unsigned char *)output; j->length = strlen(output);
    CHECK(parse(j,0) == 0); ws(j); CHECK(j->at == j->length);
    return j;
}

typedef struct {
    TfSourceManager *manager;
    TfResolution *resolution;
    TfTypedSignatures *signatures;
    TfTypedProgram *typed;
    TfMachineProgram *program;
} Fixture;

static Fixture load(const char *path, size_t cap, const TfAllocator *allocator) {
    Fixture f = {0};
    CHECK(tf_source_manager_create(&f.manager,NULL) == TF_OK);
    TfStatus status = tf_resolve(&f.resolution,f.manager,path,NULL,NULL);
    if (status != TF_OK) fprintf(stderr,"Resolving %s returned %d\n",path,(int)status);
    CHECK(status == TF_OK);
    if (tf_diagnostic_phase_kind(tf_resolution_phase(f.resolution)) != TF_DIAG_PHASE_VALID)
        (void)tf_diagnostic_bag_write(stderr,tf_resolution_diagnostics(f.resolution),TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(f.resolution)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&f.signatures,f.resolution,100,NULL) == TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(f.signatures)));
    CHECK(tf_check(&f.typed,f.resolution,f.signatures,100,NULL) == TF_OK);
    if (tf_diagnostic_phase_kind(tf_typed_program_phase(f.typed)) != TF_DIAG_PHASE_VALID)
        (void)tf_diagnostic_bag_write(stderr,tf_typed_program_diagnostics(f.typed),TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
    CHECK(tf_diagnostic_phase_kind(tf_typed_program_phase(f.typed)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_machine_program_build(&f.program,f.typed,cap,allocator) == TF_OK);
    CHECK(f.program != NULL);
    return f;
}

static void destroy(Fixture f) {
    tf_machine_program_destroy(f.program);
    tf_typed_program_destroy(f.typed);
    tf_signatures_destroy(f.signatures);
    tf_resolution_destroy(f.resolution);
    tf_source_manager_destroy(f.manager);
}

static void valid(const Fixture *f) {
    if (tf_diagnostic_phase_kind(tf_machine_program_phase(f->program)) != TF_DIAG_PHASE_VALID)
        (void)tf_diagnostic_bag_write(stderr,tf_machine_program_diagnostics(f->program),TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
    CHECK(tf_diagnostic_phase_kind(tf_machine_program_phase(f->program)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_diagnostic_phase_payload(tf_machine_program_phase(f->program)) == f->program);
}

static Fixture source(const char *bytes, size_t cap, const TfAllocator *allocator) {
    /* Deliberately unique from other native test executables. build exists. */
    const char *path = "build/m07-plan-dump-case.tfd";
    FILE *stream = fopen(path,"wb");
    CHECK(stream != NULL);
    CHECK(fwrite(bytes,1,strlen(bytes),stream) == strlen(bytes));
    CHECK(fclose(stream) == 0);
    return load(path,cap,allocator);
}

static const char *read_kind(TfReferenceKind kind) {
    switch (kind) {
    case TF_REFERENCE_OLD: return "old";
    case TF_REFERENCE_INPUT: return "input";
    case TF_REFERENCE_CANDIDATE: return "candidate";
    default: CHECK(false); return NULL;
    }
}

static const char *read_context(TfMachineReadContext context) {
    switch (context) {
    case TF_MACHINE_INITIALIZER: return "initializer";
    case TF_MACHINE_WRITER: return "writer";
    case TF_MACHINE_GUARD: return "guard";
    case TF_MACHINE_REJECTION: return "rejection";
    case TF_MACHINE_EMISSION: return "emission";
    }
    CHECK(false); return NULL;
}

static void string(const Json *j, size_t id, const char *expected) {
    text(j,id,expected,strlen(expected));
}

/* Compare every exported field, array cardinality and identity, independently
 * of field order. Source expectations below additionally constrain the plan. */
static void schema(const Fixture *f, const Json *j) {
    size_t modules = field(j,0,"modules"), machines = field(j,0,"machines");
    members(j,0,'{',4);
    number(j,field(j,0,"schema_version"),1);
    CHECK(j->nodes[field(j,0,"guards_proven")].type == 'f');
    members(j,modules,'[',tf_resolution_module_count(f->resolution));
    for (size_t i=0; i<tf_resolution_module_count(f->resolution); ++i) {
        size_t object = element(j,modules,i);
        const TfResolvedModule *m = tf_resolution_module_at(f->resolution,i);
        members(j,object,'{',2);
        number(j,field(j,object,"id"),m->id.index);
        string(j,field(j,object,"path"),m->source->canonical_path);
    }
    members(j,machines,'[',tf_machine_program_count(f->program));
    for (size_t i=0; i<tf_machine_program_count(f->program); ++i) {
        const TfMachinePlan *p = tf_machine_program_at(f->program,i);
        size_t object = element(j,machines,i), array;
        members(j,object,'{',12);
        number(j,field(j,object,"module"),p->type.module_index);
        number(j,field(j,object,"declaration"),p->declaration.index);
        text(j,field(j,object,"name"),p->name.data,p->name.length);
        span(j,field(j,object,"span"),p->span);
        number(j,field(j,object,"input_symbol"),p->input.index);
        indexes(j,field(j,object,"schedule"),p->schedule,p->schedule_count);
        array = field(j,object,"cells"); members(j,array,'[',p->cell_count);
        for (size_t k=0; k<p->cell_count; ++k) {
            const TfMachineCell *c = &p->cells[k]; size_t item = element(j,array,k);
            members(j,item,'{',6);
            number(j,field(j,item,"id"),c->id);
            number(j,field(j,item,"symbol"),c->symbol.index);
            text(j,field(j,item,"name"),c->name.data,c->name.length);
            number(j,field(j,item,"type"),c->type.index);
            clause(j,field(j,item,"declaration"),c->declaration);
            indexes(j,field(j,item,"writers"),c->writers,c->writer_count);
        }
        array = field(j,object,"writers"); members(j,array,'[',p->writer_count);
        for (size_t k=0; k<p->writer_count; ++k) {
            size_t item = element(j,array,k); members(j,item,'{',2);
            number(j,field(j,item,"cell"),p->writers[k].cell);
            clause(j,field(j,item,"clause"),p->writers[k].clause);
        }
        array = field(j,object,"reads"); members(j,array,'[',p->read_count);
        for (size_t k=0; k<p->read_count; ++k) {
            const TfMachineRead *r = &p->reads[k]; size_t item = element(j,array,k);
            members(j,item,'{',7);
            string(j,field(j,item,"kind"),read_kind(r->kind));
            string(j,field(j,item,"context"),read_context(r->context));
            number(j,field(j,item,"clause_index"),r->clause_index);
            number(j,field(j,item,"symbol"),r->symbol.index);
            number(j,field(j,item,"node"),r->node.index);
            if (r->cell == SIZE_MAX) CHECK(j->nodes[field(j,item,"cell")].type == 'n');
            else number(j,field(j,item,"cell"),r->cell);
            span(j,field(j,item,"span"),r->span);
        }
        array = field(j,object,"dependencies"); members(j,array,'[',p->dependency_count);
        for (size_t k=0; k<p->dependency_count; ++k) {
            const TfMachineDependency *d = &p->dependencies[k]; size_t item = element(j,array,k);
            members(j,item,'{',3);
            number(j,field(j,item,"prerequisite"),d->prerequisite);
            number(j,field(j,item,"dependent"),d->dependent);
            indexes(j,field(j,item,"occurrences"),d->occurrences,d->occurrence_count);
        }
        array = field(j,object,"guards"); members(j,array,'[',p->guard_count);
        for (size_t k=0; k<p->guard_count; ++k) {
            const TfMachineGuard *g = &p->guards[k]; size_t item = element(j,array,k);
            const TfAstNode *rejection = tf_ast_get(g->rejection.owner,g->rejection);
            CHECK(rejection != NULL);
            members(j,item,'{',3);
            clause(j,field(j,item,"clause"),g->clause);
            number(j,field(j,item,"rejection"),g->rejection.index);
            span(j,field(j,item,"rejection_span"),rejection->span);
        }
        array = field(j,object,"emissions"); members(j,array,'[',p->emission_count);
        for (size_t k=0; k<p->emission_count; ++k) clause(j,element(j,array,k),p->emissions[k]);
    }
}

static void roundtrip(const Fixture *f) {
    char *a = render(f->program,true), *b = render(f->program,true);
    Json *j = decode(a);
    CHECK(strcmp(a,b) == 0); schema(f,j); free(j); free(b); free(a);
    a = render(f->program,false); b = render(f->program,false);
    CHECK(strcmp(a,b) == 0);
    CHECK(strstr(a,"guards are runtime conditions, not proven assertions.") != NULL);
    free(b); free(a);
}

static bool named(TfAstText name, const char *expected) {
    return name.length == strlen(expected) && !memcmp(name.data,expected,name.length);
}

static const TfMachinePlan *machine(const Fixture *f, const char *name) {
    for (size_t i=0; i<tf_machine_program_count(f->program); ++i) {
        const TfMachinePlan *p = tf_machine_program_at(f->program,i);
        if (named(p->name,name)) return p;
    }
    CHECK(false); return NULL;
}

static size_t cell(const TfMachinePlan *p, const char *name) {
    for (size_t i=0; i<p->cell_count; ++i) if (named(p->cells[i].name,name)) return i;
    CHECK(false); return SIZE_MAX;
}

static void schedule(const TfMachinePlan *p, const char *const *names, size_t count) {
    CHECK(p->schedule_count == count && p->cell_count == count);
    for (size_t i=0; i<count; ++i) CHECK(p->schedule[i] == cell(p,names[i]));
}

static void raw_span(TfSpan where, const char *expected) {
    CHECK(where.source != NULL && where.end <= where.source->length && where.start <= where.end);
    CHECK(where.end-where.start == strlen(expected));
    CHECK(!memcmp(where.source->bytes+where.start,expected,strlen(expected)));
}

static TfSpan node_span(TfNodeId id) {
    const TfAstNode *node = tf_ast_get(id.owner,id);
    CHECK(node != NULL);
    return node->span;
}

static void edge(const TfMachinePlan *p, const char *from, const char *to, size_t count) {
    size_t found = 0, lexical = 0;
    size_t prerequisite = cell(p,from), dependent = cell(p,to);
    for (size_t i=0; i<p->read_count; ++i) {
        const TfMachineRead *r = &p->reads[i];
        if (r->context == TF_MACHINE_WRITER) CHECK(r->clause_index < p->writer_count);
        if (r->kind == TF_REFERENCE_CANDIDATE && r->context == TF_MACHINE_WRITER &&
            r->cell == prerequisite && p->writers[r->clause_index].cell == dependent) ++lexical;
    }
    CHECK(lexical == count);
    for (size_t i=0; i<p->dependency_count; ++i) {
        const TfMachineDependency *d = &p->dependencies[i];
        if (d->prerequisite != prerequisite || d->dependent != dependent) continue;
        ++found; CHECK(d->occurrence_count == count);
        lexical = 0;
        for (size_t k=0; k<p->read_count; ++k) {
            const TfMachineRead *r = &p->reads[k];
            if (r->kind == TF_REFERENCE_CANDIDATE && r->context == TF_MACHINE_WRITER &&
                r->cell == prerequisite && p->writers[r->clause_index].cell == dependent) {
                CHECK(d->occurrences[lexical] == k); ++lexical;
            }
        }
    }
    CHECK(found == 1);
}

static void fixture_path(char *out, size_t capacity, const char *directory, const char *name) {
    int n = snprintf(out,capacity,"%s/%s",directory,name);
    CHECK(n > 0 && (size_t)n < capacity);
}

static void supplied_fixtures(const char *directory) {
    static const char *const ordered[] = {"a","B","b","z"};
    static const char *const swap[] = {"left","right"};
    static const char *const dead[] = {"a","z","values"};
    char path[4096];
    fixture_path(path,sizeof path,directory,"ordered.tfd");
    Fixture f = load(path,100,NULL); valid(&f); roundtrip(&f);
    /* Separate owners also catch address-dependent IDs or iteration order. */
    Fixture independent = load(path,100,NULL); valid(&independent);
    for (unsigned json=0; json<2; ++json) {
        char *first = render(f.program,json != 0), *second = render(independent.program,json != 0);
        CHECK(strcmp(first,second) == 0);
        free(second); free(first);
    }
    destroy(independent);
    CHECK(tf_machine_program_count(f.program) == 3);
    const TfMachinePlan *p = machine(&f,"Ordered");
    schedule(p,ordered,4); CHECK(p->dependency_count == 3);
    edge(p,"a","z",1); edge(p,"B","z",1); edge(p,"a","B",1);
    CHECK(p->read_count == 10);
    CHECK(p->reads[2].kind == TF_REFERENCE_INPUT && p->reads[2].clause_index == 1);
    CHECK(p->reads[3].kind == TF_REFERENCE_OLD && p->reads[3].clause_index == 1 && p->reads[3].cell == cell(p,"z"));
    CHECK(p->reads[4].kind == TF_REFERENCE_INPUT && p->reads[4].clause_index == 2);
    raw_span(p->reads[2].span,"request"); raw_span(p->reads[3].span,"z"); raw_span(p->reads[4].span,"request");
    CHECK(p->guard_count == 2 && p->emission_count == 2);
    raw_span(p->cells[0].declaration.span,"state z: Int = 0;");
    raw_span(p->writers[0].clause.span,"next z = next.a + next.B;");
    raw_span(p->guards[0].clause.span,"guard next.z > 0 else \"first\";");
    raw_span(p->guards[1].clause.span,"guard b > 0 else \"second\";");
    raw_span(node_span(p->guards[0].rejection),"\"first\"");
    raw_span(node_span(p->guards[1].rejection),"\"second\"");
    raw_span(p->emissions[0].span,"emit next.z;");
    raw_span(p->emissions[1].span,"emit b;");
    raw_span(node_span(p->emissions[0].expression),"next.z");
    raw_span(node_span(p->emissions[1].expression),"b");
    char *output = render(f.program,false);
    CHECK(strstr(output,"  schedule: a B b z\n") != NULL);
    CHECK(strstr(output,"  dependency a -> B reads=") != NULL);
    const char *g0 = strstr(output,"  guard 0 "), *g1 = strstr(output,"  guard 1 ");
    const char *e0 = strstr(output,"  emission 0 "), *e1 = strstr(output,"  emission 1 ");
    CHECK(g0 != NULL && g1 != NULL && e0 != NULL && e1 != NULL && g0 < g1 && g1 < e0 && e0 < e1);
    const char *reject = strstr(g0,"reject-on-false=");
    CHECK(reject != NULL && reject < g1); free(output);
    p = machine(&f,"Swap"); schedule(p,swap,2); CHECK(p->dependency_count == 0);
    CHECK(p->read_count == 4);
    CHECK(p->reads[0].kind == TF_REFERENCE_OLD && p->reads[1].kind == TF_REFERENCE_OLD);
    raw_span(p->reads[0].span,"left"); raw_span(p->reads[1].span,"right");
    p = machine(&f,"DeadBranch"); schedule(p,dead,3); CHECK(p->dependency_count == 2);
    edge(p,"a","z",1); edge(p,"z","values",1);
    destroy(f);
    fixture_path(path,sizeof path,directory,"permuted.tfd");
    f = load(path,100,NULL); valid(&f); roundtrip(&f);
    p = machine(&f,"Ordered"); schedule(p,ordered,4); CHECK(p->dependency_count == 3);
    edge(p,"a","z",1); edge(p,"B","z",1); edge(p,"a","B",1);
    output = render(f.program,false); CHECK(strstr(output,"  schedule: a B b z\n") != NULL);
    free(output); destroy(f);
}

/* UTF-8 + CRLF prefix distinguishes original byte offsets from scalar/line
 * offsets. False guards and a faulting emission remain inspection-only. */
static const char contexts_source[] =
    "/* \xc3\xa9 */\r\n"
    "machine Contexts(request: Int) yields Int rejects Int {\r\n"
    " state a: Int = 0; state b: Int = 0;\r\n"
    " next b = if false { next.a + next.a } else {\r\n"
    "   let values = for item in [request] { next.a + item };\r\n"
    "   a + request + len(values) };\r\n"
    " next a = request;\r\n"
    " guard false && next.b > a else next.a + request;\r\n"
    " guard next.a > 0 else b;\r\n"
    " emit next.b; emit 1 div 0;\r\n"
    "}\r\n";

static void contexts(void) {
    static const char *const names[] = {"a","b"};
    static const struct { const char *spelling; TfReferenceKind kind; TfMachineReadContext context; size_t clause; const char *target; } expected[] = {
        {"next.a",TF_REFERENCE_CANDIDATE,TF_MACHINE_WRITER,0,"a"},
        {"next.a",TF_REFERENCE_CANDIDATE,TF_MACHINE_WRITER,0,"a"},
        {"request",TF_REFERENCE_INPUT,TF_MACHINE_WRITER,0,NULL},
        {"next.a",TF_REFERENCE_CANDIDATE,TF_MACHINE_WRITER,0,"a"},
        {"a",TF_REFERENCE_OLD,TF_MACHINE_WRITER,0,"a"},
        {"request",TF_REFERENCE_INPUT,TF_MACHINE_WRITER,0,NULL},
        {"request",TF_REFERENCE_INPUT,TF_MACHINE_WRITER,1,NULL},
        {"next.b",TF_REFERENCE_CANDIDATE,TF_MACHINE_GUARD,0,"b"},
        {"a",TF_REFERENCE_OLD,TF_MACHINE_GUARD,0,"a"},
        {"next.a",TF_REFERENCE_CANDIDATE,TF_MACHINE_REJECTION,0,"a"},
        {"request",TF_REFERENCE_INPUT,TF_MACHINE_REJECTION,0,NULL},
        {"next.a",TF_REFERENCE_CANDIDATE,TF_MACHINE_GUARD,1,"a"},
        {"b",TF_REFERENCE_OLD,TF_MACHINE_REJECTION,1,"b"},
        {"next.b",TF_REFERENCE_CANDIDATE,TF_MACHINE_EMISSION,0,"b"}
    };
    Fixture f = source(contexts_source,100,NULL); valid(&f); roundtrip(&f);
    const TfMachinePlan *p = machine(&f,"Contexts");
    schedule(p,names,2); CHECK(p->dependency_count == 1); edge(p,"a","b",3);
    char *output = render(f.program,false);
    CHECK(strstr(output,"  dependency a -> b reads=[0,1,3]\n") != NULL);
    free(output);
    CHECK(p->read_count == sizeof expected/sizeof expected[0]);
    /* Search starts inside the first writer, beyond declarations. Exact spans
     * below are anchored by lexical order and full projection spelling. */
    const char *cursor = strstr(contexts_source,"next b =");
    for (size_t i=0; i<p->read_count; ++i) {
        const TfMachineRead *r = &p->reads[i];
        const char *at = strstr(cursor,expected[i].spelling);
        CHECK(at != NULL); raw_span(r->span,expected[i].spelling);
        CHECK(r->span.start == (size_t)(at-contexts_source));
        CHECK(r->kind == expected[i].kind && r->context == expected[i].context && r->clause_index == expected[i].clause);
        CHECK(r->cell == (expected[i].target == NULL ? SIZE_MAX : cell(p,expected[i].target)));
        CHECK(r->symbol.index == (expected[i].target == NULL ? p->input.index : p->cells[r->cell].symbol.index));
        cursor = at + strlen(expected[i].spelling);
    }
    CHECK(p->guard_count == 2 && p->emission_count == 2);
    raw_span(node_span(p->guards[0].rejection),"next.a + request");
    raw_span(node_span(p->guards[1].rejection),"b");
    raw_span(node_span(p->emissions[1].expression),"1 div 0");
    destroy(f);
}

static void imports(const char *directory) {
    char path[4096]; fixture_path(path,sizeof path,directory,"import-root.tfd");
    Fixture f = load(path,100,NULL); valid(&f); roundtrip(&f);
    CHECK(tf_resolution_module_count(f.resolution) == 2);
    CHECK(tf_machine_program_count(f.program) == 2);
    const TfMachinePlan *local = machine(&f,"Local");
    const TfMachinePlan *remote = machine(&f,"Imported");
    CHECK(local->type.module_index != remote->type.module_index);
    const TfResolvedModule *root = tf_resolution_module(f.resolution,tf_resolution_root(f.resolution));
    CHECK(local->type.module_index == root->id.index);
    const TfResolvedModule *imported = tf_resolution_module(f.resolution,(TfModuleId){f.resolution,remote->type.module_index});
    CHECK(imported != NULL && strcmp(imported->relative_path,"helper.tfd") == 0);
    CHECK(local->declaration.owner == root->ast && remote->declaration.owner == imported->ast);
    CHECK(local->declaration.owner != remote->declaration.owner);
    CHECK(local->span.source != remote->span.source);
    CHECK(local->cell_count == 1 && remote->cell_count == 1);
    CHECK(local->dependency_count == 0 && remote->dependency_count == 0);
    destroy(f);
}

static void refused(const TfMachineProgram *program) {
    for (unsigned json=0; json<2; ++json) {
        FILE *stream = tmpfile(); char *output;
        CHECK(stream != NULL); CHECK(fputs("sentinel",stream) != EOF);
        CHECK(tf_plan_dump_write(stream,program,json != 0) == TF_HOST_ERROR);
        CHECK(ftell(stream) == 8);
        output = read_stream(stream); CHECK(strcmp(output,"sentinel") == 0);
        free(output); CHECK(fclose(stream) == 0);
    }
}

static void boundaries(void) {
    Fixture f = source("fn library() -> Int { 7 }\n",100,NULL);
    valid(&f); CHECK(tf_machine_program_count(f.program) == 0); roundtrip(&f);
    char *output = render(f.program,false);
    CHECK(strcmp(output,"Machine plan; guards are runtime conditions, not proven assertions.\n") == 0);
    free(output);
    CHECK(tf_plan_dump_write(NULL,f.program,true) == TF_HOST_ERROR);
    CHECK(tf_plan_dump_write(NULL,f.program,false) == TF_HOST_ERROR);
    refused(NULL);
    /* An unbuffered stream opened read-only must surface a write failure in
     * the call, without relying on platform-specific broken-pipe behavior. */
    for (unsigned json=0; json<2; ++json) {
        FILE *stream = fopen("build/m07-plan-dump-case.tfd","rb");
        CHECK(stream != NULL); CHECK(setvbuf(stream,NULL,_IONBF,0) == 0);
        CHECK(tf_plan_dump_write(stream,f.program,json != 0) == TF_HOST_ERROR);
        CHECK(ferror(stream)); CHECK(fclose(stream) == 0);
    }
    destroy(f);
    static const char *const invalid[] = {
        ("machine Valid(r: Unit) yields Int rejects Text { state a: Int = 0; next a = a; }\n"
         "machine Missing(r: Unit) yields Int rejects Text { state a: Int = 0; state b: Int = 0; next a = a; }"),
        "machine Duplicate(r: Unit) yields Int rejects Text { state a: Int = 0; next a = 1; next a = 2; }",
        "machine Cycle(r: Unit) yields Int rejects Text { state a: Int = 0; next a = next.a; }"
    };
    for (size_t i=0; i<sizeof invalid/sizeof invalid[0]; ++i) for (unsigned cap=0; cap<2; ++cap) {
        static const char *const codes[] = {"E401","E402","E404"};
        f = source(invalid[i],cap == 0 ? 0 : 100,NULL);
        CHECK(tf_diagnostic_phase_kind(tf_machine_program_phase(f.program)) == TF_DIAG_PHASE_ERROR);
        CHECK(tf_diagnostic_phase_payload(tf_machine_program_phase(f.program)) == NULL);
        const TfDiagnosticBag *bag = tf_machine_program_diagnostics(f.program);
        CHECK(tf_diagnostic_bag_count(bag) == cap && tf_diagnostic_bag_suppressed(bag) == 1u-cap);
        if (cap != 0) CHECK(strcmp(tf_diagnostic_bag_get(bag,0)->code,codes[i]) == 0);
        refused(f.program); destroy(f);
    }
}

typedef struct { bool fail; size_t attempts, live; } Allocation;
static void *allocate(void *context, size_t size) {
    Allocation *a = context; ++a->attempts;
    if (a->fail) return NULL;
    void *result = malloc(size);
    if (result != NULL) ++a->live;
    return result;
}
static void deallocate(void *context, void *pointer) {
    Allocation *a = context;
    if (pointer != NULL) { CHECK(a->live != 0); --a->live; free(pointer); }
}

static void allocation_free(void) {
    Allocation state = {0}; TfAllocator allocator = {&state,allocate,deallocate};
    Fixture f = source(contexts_source,100,&allocator); valid(&f);
    size_t attempts = state.attempts, live = state.live;
    CHECK(attempts != 0 && live != 0);
    char *before_json = render(f.program,true), *before_text = render(f.program,false);
    state.fail = true;
    roundtrip(&f);
    char *after_json = render(f.program,true), *after_text = render(f.program,false);
    CHECK(state.attempts == attempts && state.live == live);
    CHECK(strcmp(before_json,after_json) == 0 && strcmp(before_text,after_text) == 0);
    free(before_json); free(before_text); free(after_json); free(after_text);
    destroy(f); CHECK(state.live == 0);
}

int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 2);
    supplied_fixtures(argc == 2 ? argv[1] : "tests/fixtures/plan");
    contexts(); imports(argc == 2 ? argv[1] : "tests/fixtures/plan");
    boundaries(); allocation_free();
    printf("machine plan dump: %zu checks passed\n",checks);
    return 0;
}
