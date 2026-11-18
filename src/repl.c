#include "repl.h"
#include "repl_input.h"
#include "repl_resolve.h"
#include "source_manager_internal.h"
#include "core/repl_lower.h"
#include "execution/repl_value.h"
#include "ternfold/check.h"
#include "ternfold/core_lower.h"
#include "ternfold/core_verify.h"
#include "ternfold/plan_dump.h"
#include "execution/driver_run.h"
#include "typed_signatures.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* The session holds source, never a pointer into a finished VM arena. User
 * submissions have no side effects outside a designated host entry, so
 * replaying the immutable binding prefix preserves their values. */
enum { TURN_BYTES=64*1024, TURN_SCAN_BYTES=32*1024*1024, SESSION_BYTES=1024*1024,
       CACHE_BYTES=64*1024*1024 };
typedef struct { char *data; size_t length,capacity; } Buffer;
typedef enum { DECLARATION, BINDING, EXPRESSION, TYPE_QUERY, PLAN_QUERY } TurnKind;
typedef struct {
    Buffer source;
    char name[64];
    size_t function_start,unit_start,unit_end,final_start;
} VirtualUnit;
typedef struct {
    Buffer declarations,bindings;
    TfSourceManager *sources;
    const TfManagedSource *loaded_root;
    size_t *cached_bytes; /* stable allocator context while the session moves */
} Session;
typedef struct {
    TfResolution *resolution;
    TfTypedSignatures *signatures;
    TfTypedProgram *typed;
    TfMachineProgram *machines;
    TfCoreProgram *core;
    TfVerifiedCore *verified;
} Pipeline;

static void buffer_destroy(Buffer *b) { free(b->data); *b=(Buffer){0}; }
static bool append(Buffer *b,const void *bytes,size_t length,size_t limit) {
    if (b->length>limit || length>limit-b->length) return false;
    const size_t total=b->length+length;
    void *storage=b->data;
    if (!tf_grow(&storage,&b->capacity,total+1,1,tf_allocator(NULL))) return false;
    b->data=storage;
    if (length!=0) memcpy(b->data+b->length,bytes,length);
    b->length=total;
    b->data[total]=0;
    return true;
}
static bool put(Buffer *b,const char *value) { return append(b,value,strlen(value),SESSION_BYTES); }

typedef union { size_t bytes; max_align_t alignment; } CacheAllocation;
static void *cache_allocate(void *context,size_t bytes) {
    size_t *live=context;
    if (bytes>CACHE_BYTES-sizeof(CacheAllocation) ||
        bytes+sizeof(CacheAllocation)>CACHE_BYTES-*live) return NULL;
    CacheAllocation *p=malloc(bytes+sizeof *p);
    if (p==NULL) return NULL;
    p->bytes=bytes+sizeof *p;
    *live+=p->bytes;
    return p+1;
}
static void cache_deallocate(void *context,void *address) {
    if (address==NULL) return;
    size_t *live=context;
    CacheAllocation *allocation=(CacheAllocation *)address-1;
    *live-=allocation->bytes;
    free(allocation);
}
static void session_destroy(Session *s) {
    buffer_destroy(&s->declarations); buffer_destroy(&s->bindings);
    tf_source_manager_destroy(s->sources); free(s->cached_bytes); *s=(Session){0};
}
static bool session_create(Session *s) {
    s->cached_bytes=calloc(1,sizeof *s->cached_bytes);
    if (s->cached_bytes==NULL) return false;
    const TfAllocator allocator={s->cached_bytes,cache_allocate,cache_deallocate};
    if (tf_source_manager_create(&s->sources,&allocator)==TF_OK) return true;
    session_destroy(s);
    return false;
}
static void pipeline_destroy(Pipeline *p) {
    tf_verified_core_destroy(p->verified); tf_core_program_destroy(p->core);
    tf_machine_program_destroy(p->machines); tf_typed_program_destroy(p->typed);
    tf_signatures_destroy(p->signatures); tf_resolution_destroy(p->resolution);
    *p=(Pipeline){0};
}
static bool message(FILE *out,const char *value) { return fputs(value,out)!=EOF && fflush(out)==0; }

/* Resolver successful results can still contain diagnostics. Stop at the
 * first rejected phase; its bag borrows source from the active transaction. */
static int check_through_plan(Pipeline *p,const TfDiagnosticBag **diagnostics) {
    *diagnostics=tf_resolution_diagnostics(p->resolution);
    if (tf_diagnostic_bag_has_errors(*diagnostics)) return 1;
    if (tf_signatures_build(&p->signatures,p->resolution,100,NULL)!=TF_OK) return 2;
    *diagnostics=tf_signatures_diagnostics(p->signatures);
    if (tf_diagnostic_bag_has_errors(*diagnostics)) return 1;
    if (tf_check(&p->typed,p->resolution,p->signatures,100,NULL)!=TF_OK ||
        tf_diagnostic_phase_kind(tf_typed_program_phase(p->typed))==TF_DIAG_PHASE_INVALID) return 2;
    *diagnostics=tf_typed_program_diagnostics(p->typed);
    if (tf_diagnostic_bag_has_errors(*diagnostics)) return 1;
    if (tf_machine_program_build(&p->machines,p->typed,100,NULL)!=TF_OK) return 2;
    *diagnostics=tf_machine_program_diagnostics(p->machines);
    return tf_diagnostic_bag_has_errors(*diagnostics) ? 1 : 0;
}
static bool show_diagnostics(FILE *err,const TfDiagnosticBag *bag) {
    return tf_diagnostic_bag_write(err,bag,TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER)==TF_OK &&
           fflush(err)==0;
}

static const char probe_prefix[]="ternfold_repl_internal_";
enum { PROBE_IDS=65536 };
static void mark_probe_names(unsigned char marks[static PROBE_IDS/8],const Buffer *b) {
    const size_t n=sizeof probe_prefix-1;
    if (b->length<=n) return;
    for (size_t i=0;i<b->length-n;++i) {
        if (b->data[i]!=probe_prefix[0] || memcmp(b->data+i,probe_prefix,n)!=0) continue;
        size_t at=i+n;
        unsigned id=0;
        while (at<b->length && b->data[at]>='0' && b->data[at]<='9' && id<PROBE_IDS) {
            id=id*10u+(unsigned)(b->data[at++]-'0');
        }
        if (at!=i+n && id<PROBE_IDS) marks[id/8u]|=(unsigned char)(1u<<(id%8u));
    }
}

/* Guard the textual assembly with exact checked AST spans before interpreting
 * its selected let. Cheap name exclusion also covers identifiers in inactive
 * source branches. Existing declarations and bindings are not transformed. */
static bool compose(VirtualUnit *unit,const Session *s,const Buffer *incoming,TurnKind kind) {
    Buffer *b=&unit->source;
    if (!append(b,s->declarations.data,s->declarations.length,SESSION_BYTES) || !put(b,"\n")) return false;
    if (kind==DECLARATION)
        return append(b,incoming->data,incoming->length,SESSION_BYTES) && put(b,"\n");
    if (kind==PLAN_QUERY) return true;
    unsigned char marks[PROBE_IDS/8]={0};
    mark_probe_names(marks,&s->declarations); mark_probe_names(marks,&s->bindings);
    mark_probe_names(marks,incoming);
    unsigned id=0;
    while (id<PROBE_IDS && (marks[id/8u]&(1u<<(id%8u)))!=0u) ++id;
    if (id==PROBE_IDS) return false;
    const int length=snprintf(unit->name,sizeof unit->name,"%s%u",probe_prefix,id);
    if (length<0 || (size_t)length>=sizeof unit->name) return false;
    unit->function_start=b->length;
    if (!put(b,"fn ") || !put(b,unit->name) || !put(b,"() -> Unit {\n") ||
        !append(b,s->bindings.data,s->bindings.length,SESSION_BYTES) || !put(b,"\n")) return false;
    unit->unit_start=b->length;
    if (kind!=BINDING && !put(b,"let _ = (\n")) return false;
    if (!append(b,incoming->data,incoming->length,SESSION_BYTES) || !put(b,"\n")) return false;
    if (kind!=BINDING && !put(b,");\n")) return false;
    unit->unit_end=b->length;
    unit->final_start=b->length;
    return put(b,"()\n}\n");
}

static bool find_probe(const Pipeline *p,const VirtualUnit *unit,TurnKind kind,
                       TfNodeId *function,TfNodeId *binding,TfConcreteTypeId *type) {
    const TfResolvedModule *module=tf_resolution_module(p->resolution,tf_resolution_root(p->resolution));
    if (module==NULL || module->ast==NULL) return false;
    const TfAst *ast=module->ast;
    const TfAstNode *program=tf_ast_get(ast,tf_ast_root(ast));
    if (program==NULL || program->kind!=TF_AST_PROGRAM || program->as.program.declarations.count==0) return false;
    const TfNodeList declarations=program->as.program.declarations;
    const TfAstNode *fn=tf_ast_get(ast,declarations.items[declarations.count-1]);
    if (fn==NULL || fn->kind!=TF_AST_FUNCTION || fn->as.function.public_decl ||
        fn->as.function.parameters.count!=0 || fn->span.start!=unit->function_start ||
        fn->span.end!=unit->source.length-1 || fn->as.function.name.length!=strlen(unit->name) ||
        memcmp(fn->as.function.name.data,unit->name,fn->as.function.name.length)!=0) return false;
    const TfAstNode *body=tf_ast_get(ast,fn->as.function.body);
    if (body==NULL || body->kind!=TF_AST_BLOCK) return false;
    const TfAstNode *end=tf_ast_get(ast,body->as.block.final_expression);
    if (end==NULL || end->kind!=TF_AST_UNIT || end->span.start!=unit->final_start) return false;
    const TfNodeList statements=body->as.block.statements;
    const TfAstNode *let=NULL;
    for (size_t i=0;i<statements.count;++i) {
        const TfAstNode *statement=tf_ast_get(ast,statements.items[i]);
        if (statement==NULL || statement->kind!=TF_AST_LET || statement->span.end>unit->unit_end) return false;
        if (statement->span.start<unit->unit_start) {
            if (statement->span.end>unit->unit_start) return false;
        } else {
            if (let!=NULL || i+1!=statements.count) return false;
            let=statement;
        }
    }
    if (let==NULL) return false;
    const TfAstNode *value=tf_ast_get(ast,let->as.binding.value);
    if (value==NULL) return false;
    if (kind!=BINDING && (!let->as.binding.discard || let->span.start!=unit->unit_start ||
                         value->kind!=TF_AST_GROUP || value->span.start!=unit->unit_start+8 ||
                         value->span.end!=unit->unit_end-2)) return false;
    const TfTypedNode *typed=tf_typed_program_node(p->typed,value->id);
    if (typed==NULL || typed->type.index==0) return false;
    *function=fn->id; *binding=let->id; *type=typed->type;
    return true;
}

static bool format_type(Buffer *out,const Pipeline *p,TfConcreteTypeId id,unsigned depth) {
    if (out->length>=2048) return true;
    if (depth==24) return put(out,"...");
    const TfConcreteType *t=tf_type_store_get(tf_typed_program_store(p->typed),id);
    if (t==NULL) return false;
    static const char *const names[]={"Int","Float","Bool","Text","Char","Unit"};
    if ((size_t)t->kind<sizeof names/sizeof *names) return put(out,names[t->kind]);
    switch (t->kind) {
    case TF_CONCRETE_LIST: case TF_CONCRETE_OPTION:
        return put(out,t->kind==TF_CONCRETE_LIST ? "List[" : "Option[") &&
            format_type(out,p,t->as.element,depth+1) && put(out,"]");
    case TF_CONCRETE_DECISION:
        return put(out,"Decision[") && format_type(out,p,t->as.decision.machine,depth+1) &&
            put(out,", ") && format_type(out,p,t->as.decision.output,depth+1) &&
            put(out,", ") && format_type(out,p,t->as.decision.rejection,depth+1) && put(out,"]");
    case TF_CONCRETE_RECORD: case TF_CONCRETE_ENUM: case TF_CONCRETE_MACHINE: {
        const TfResolvedType *nominal=tf_resolution_type(p->resolution,t->as.nominal);
        if (nominal==NULL) return false;
        const TfModuleId owner={p->resolution,t->as.nominal.module_index};
        const TfResolvedModule *module=tf_resolution_module(p->resolution,owner);
        if (module==NULL) return false;
        if (!module->is_root) {
            if (!put(out,"\"")) return false;
            const unsigned char *path=(const unsigned char *)module->relative_path;
            for (size_t i=0;path[i]!='\0';++i) {
                if (i==128) { if (!put(out,"...")) return false; break; }
                if (path[i]>=32 && path[i]<=126 && path[i]!='"' && path[i]!='\\') {
                    if (!append(out,path+i,1,SESSION_BYTES)) return false;
                } else {
                    char escape[5];
                    if (snprintf(escape,sizeof escape,"\\x%02x",(unsigned)path[i])!=4 ||
                        !put(out,escape)) return false;
                }
            }
            if (!put(out,"\"::")) return false;
        }
        const size_t length=nominal->name.length>128 ? 128 : nominal->name.length;
        return append(out,nominal->name.data,length,SESSION_BYTES) &&
               (length==nominal->name.length || put(out,"..."));
    }
    case TF_CONCRETE_FUNCTION:
        if (!put(out,"fn(")) return false;
        for (size_t i=0;i<t->as.function.arity;++i) {
            if (out->length>=2048) { if (!put(out,"...")) return false; break; }
            if ((i!=0 && !put(out,", ")) || !format_type(out,p,t->as.function.parameters[i],depth+1)) return false;
        }
        return put(out,") -> ") && format_type(out,p,t->as.function.result,depth+1);
    default: return false;
    }
}

/* Replace the session atomically. Loading checks a program and pins its
 * reachable imports; it does not evaluate any function or read its stdin. */
static int load_session(Session *s,const char *path,const char *root,FILE *out,FILE *err) {
    Session next={0};
    Pipeline p={0};
    const TfDiagnosticBag *diagnostics=NULL;
    const TfResolveOptions options={root,100};
    int status=2;
    if (!session_create(&next) ||
        tf_resolve(&p.resolution,next.sources,path,&options,NULL)!=TF_OK) goto done;
    status=check_through_plan(&p,&diagnostics);
    if (status!=0) goto done;
    const TfResolvedModule *module=tf_resolution_module(p.resolution,tf_resolution_root(p.resolution));
    if (module==NULL || module->source==NULL ||
        !append(&next.declarations,module->source->source->bytes,module->source->source->length,SESSION_BYTES)) {
        status=2; goto done;
    }
    next.loaded_root=module->source;
    if (!message(out,"loaded\n")) { status=-1; goto done; }
    session_destroy(s);
    *s=next;
    next=(Session){0};
done:
    if (status==1 && !show_diagnostics(err,diagnostics)) status=-1;
    else if (status==2 && !message(err,"repl: cannot load source (host or resource failure)\n")) status=-1;
    pipeline_destroy(&p);
    session_destroy(&next);
    return status;
}

static TfSymbolId symbol_for(const TfResolution *resolution,TfNodeId fn) {
    for (size_t i=0;i<tf_resolution_symbol_count(resolution);++i) {
        const TfResolvedSymbol *symbol=tf_resolution_symbol_at(resolution,i);
        if (symbol!=NULL && symbol->declaration.owner==fn.owner && symbol->declaration.index==fn.index)
            return symbol->id;
    }
    return (TfSymbolId){0};
}

static int run_turn(Session *s,const Buffer *incoming,TurnKind kind,const char *root,FILE *out,FILE *err) {
    const TfSourceManagerMark mark=tf_source_manager_mark(s->sources);
    VirtualUnit unit={0};
    Pipeline p={0};
    TfSource source={0};
    Buffer replacement={0}, rendered_type={0};
    bool commit=false, reported=false;
    int result=2;
    const TfDiagnosticBag *diagnostics=NULL;
    const TfResolveOptions options={root,100};
    if (!compose(&unit,s,incoming,kind) ||
        tf_source_init(&source,"<repl>",unit.source.data,unit.source.length,NULL)==TF_HOST_ERROR ||
        tf_resolve_virtual(&p.resolution,s->sources,&source,s->loaded_root,&options,NULL)!=TF_OK) goto done;
    result=check_through_plan(&p,&diagnostics);
    if (result!=0) goto done;

    if (kind==PLAN_QUERY) {
        if (tf_plan_dump_write(out,p.machines,false)!=TF_OK || fflush(out)!=0) result=-1;
        goto done;
    }
    if (kind==DECLARATION) {
        /* The candidate includes only the old module, a separator and the new
         * complete declaration(s). No evaluation or hidden code was added. */
        if (!show_diagnostics(err,diagnostics) || !message(out,"defined\n")) { result=-1; goto done; }
        buffer_destroy(&s->declarations);
        s->declarations=unit.source; unit.source=(Buffer){0};
        commit=true;
        goto done;
    }

    TfNodeId fn={0},let={0};
    TfConcreteTypeId type={0};
    if (!find_probe(&p,&unit,kind,&fn,&let,&type)) {
        result=message(err,"repl: expected exactly one complete expression or binding\n") ? 1 : -1;
        reported=true;
        goto done;
    }
    if (kind==TYPE_QUERY) {
        if (!format_type(&rendered_type,&p,type,0) || !put(&rendered_type,"\n")) { result=2; goto done; }
        if (!show_diagnostics(err,diagnostics) || !message(out,rendered_type.data)) result=-1;
        goto done;
    }
    if (kind==BINDING &&
        (!append(&replacement,s->bindings.data,s->bindings.length,SESSION_BYTES) ||
         !append(&replacement,incoming->data,incoming->length,SESSION_BYTES) || !put(&replacement,"\n"))) {
        result=2; goto done;
    }
    const TfSymbolId wrapper=symbol_for(p.resolution,fn);
    TfCoreFunctionId function=0;
    if (wrapper.index==0 ||
        tf_core_lower_repl(&p.core,&function,p.machines,wrapper,let,NULL,NULL)!=TF_OK ||
        tf_core_verify(&p.verified,p.core,NULL,NULL)!=TF_OK) { result=2; goto done; }
    TfReplValue view={0};
    TfRuntimeFault fault={0};
    const TfStatus executed=tf_interpret_repl(&view,p.verified,function,NULL,NULL,&fault);
    if (executed==TF_SOURCE_ERROR) {
        result=tf_driver_report_runtime_fault(err,tf_verified_core_data(p.verified),diagnostics,TF_DIAG_TEXT,fault) ? 3 : -1;
        goto done;
    }
    if (executed!=TF_OK) { result=2; goto done; }
    if (!show_diagnostics(err,diagnostics) || fwrite(view.bytes,1,view.length,out)!=view.length ||
        !message(out,"\n")) { result=-1; goto done; }
    if (kind==BINDING) {
        buffer_destroy(&s->bindings);
        s->bindings=replacement; replacement=(Buffer){0};
        commit=true;
    }
done:
    if (result==1 && !reported && !show_diagnostics(err,diagnostics)) result=-1;
    if (result==2 && !message(err,"repl: cannot process unit (host or resource failure)\n")) result=-1;
    pipeline_destroy(&p); tf_source_destroy(&source);
    if (!commit && !tf_source_manager_rollback(s->sources,mark)) result=-1;
    buffer_destroy(&unit.source); buffer_destroy(&replacement); buffer_destroy(&rendered_type);
    return result;
}

/* Read a bounded physical line; on an oversize line, drain it before retry.
 * Preserve line endings for the lexer, including a final unterminated line. */
static int physical_line(FILE *in,Buffer *line) {
    line->length=0;
    int ch;
    bool overflow=false;
    while ((ch=fgetc(in))!=EOF) {
        const unsigned char byte=(unsigned char)ch;
        if (!overflow) {
            if (line->length==TURN_BYTES) overflow=true;
            else if (!append(line,&byte,1,TURN_BYTES)) return -1;
        }
        if (ch=='\n') break;
    }
    if (ferror(in)) return -1;
    if (overflow) return 2;
    return line->length!=0 ? 1 : 0;
}
static bool horizontal(char ch) { return ch==' ' || ch=='\t'; }
static bool command_is(const char *start,size_t n,const char *name) {
    return n==strlen(name) && memcmp(start,name,n)==0;
}
static bool line_command(const Buffer *line,const char *name) {
    size_t start=0,end=line->length;
    while (start<end && horizontal(line->data[start])) ++start;
    while (end>start && (horizontal(line->data[end-1]) || line->data[end-1]=='\r' || line->data[end-1]=='\n')) --end;
    return command_is(line->data+start,end-start,name);
}
/* 0 means an ordinary source line; 1 means a handled command; 2 is quit. */
static int command(Session *s,Buffer *line,const char *root,FILE *out,FILE *err) {
    size_t start=0;
    while (start<line->length && horizontal(line->data[start])) ++start;
    if (start==line->length || line->data[start]!=':') return 0;
    if (memchr(line->data,0,line->length)!=NULL)
        return message(err,"repl: NUL in command\n") ? 1 : -1;
    size_t end=line->length;
    while (end>start && (horizontal(line->data[end-1]) || line->data[end-1]=='\r' || line->data[end-1]=='\n')) --end;
    size_t split=start;
    while (split<end && !horizontal(line->data[split])) ++split;
    size_t arg=split;
    while (arg<end && horizontal(line->data[arg])) ++arg;
    const size_t count=split-start;
    const char *name=line->data+start;
    if (command_is(name,count,":quit") && arg==end) return 2;
    if (command_is(name,count,":cancel") && arg==end)
        return message(out,"cancelled\n") ? 1 : -1;
    if (command_is(name,count,":help") && arg==end)
        return message(out,":help  :quit  :cancel  :reset  :load FILE  :type EXPR  :plan\n") ? 1 : -1;
    if (command_is(name,count,":reset") && arg==end) {
        Session next={0};
        if (!session_create(&next))
            return message(err,"repl: cannot reset session\n") ? 1 : -1;
        if (!message(out,"reset\n")) { session_destroy(&next); return -1; }
        session_destroy(s); *s=next;
        return 1;
    }
    if (command_is(name,count,":load") && arg<end) {
        line->data[end]='\0';
        return load_session(s,line->data+arg,root,out,err)<0 ? -1 : 1;
    }
    if (command_is(name,count,":type") && arg<end) {
        const Buffer expression={line->data+arg,end-arg,0};
        return run_turn(s,&expression,TYPE_QUERY,root,out,err)<0 ? -1 : 1;
    }
    if (command_is(name,count,":plan") && arg==end)
        return run_turn(s,NULL,PLAN_QUERY,root,out,err)<0 ? -1 : 1;
    return message(err,"repl: unknown or incomplete command; use :help\n") ? 1 : -1;
}

int tf_repl(FILE *in,FILE *out,FILE *err,const char *root,bool prompt) {
    Session session={0};
    Buffer line={0}, pending={0};
    size_t scanned=0;
    int status=2;
    if (!session_create(&session)) goto done;
    for (;;) {
        if (prompt && !message(out,pending.length==0 ? "ternfold> " : "... ")) goto done;
        const int next=physical_line(in,&line);
        if (next<0) { (void)message(err,"repl: cannot read input\n"); goto done; }
        if (next==0) {
            if (pending.length!=0 && !message(err,"repl: incomplete unit at end of input\n")) goto done;
            status=0; break;
        }
        if (next==2) {
            pending.length=0; scanned=0;
            if (!message(err,"repl: unit exceeds 64 KiB\n")) goto done;
            continue;
        }
        if (pending.length!=0 && line_command(&line,":quit")) { status=0; break; }
        if (pending.length!=0 && line_command(&line,":cancel")) {
            pending.length=0; scanned=0;
            if (!message(out,"cancelled\n")) goto done;
            continue;
        }
        if (line.length>TURN_BYTES-pending.length) {
            pending.length=0; scanned=0;
            if (!message(err,"repl: unit exceeds 64 KiB\n")) goto done;
            continue;
        }
        if (pending.length==0) {
            const int handled=command(&session,&line,root,out,err);
            if (handled<0) goto done;
            if (handled==2) { status=0; break; }
            if (handled==1) continue;
        }
        if (!append(&pending,line.data,line.length,TURN_BYTES)) goto done;
        if (pending.length>TURN_SCAN_BYTES-scanned) {
            pending.length=0; scanned=0;
            if (!message(err,"repl: multiline analysis budget exceeded\n")) goto done;
            continue;
        }
        scanned+=pending.length;
        TfSource source={0};
        const TfStatus read=tf_source_init(&source,"<repl>",pending.data,pending.length,NULL);
        const TfReplUnitKind classified=read==TF_HOST_ERROR ? TF_REPL_UNIT_HOST_ERROR : tf_repl_classify(&source);
        tf_source_destroy(&source);
        if (classified==TF_REPL_UNIT_INCOMPLETE) continue;
        if (classified==TF_REPL_UNIT_HOST_ERROR) {
            if (!message(err,"repl: cannot classify unit\n")) goto done;
        } else if (classified!=TF_REPL_UNIT_EMPTY) {
            const TurnKind kind=classified==TF_REPL_UNIT_DECLARATION ? DECLARATION :
                                classified==TF_REPL_UNIT_BINDING ? BINDING : EXPRESSION;
            if (run_turn(&session,&pending,kind,root,out,err)<0) goto done;
        }
        pending.length=0; scanned=0;
    }
done:
    session_destroy(&session); buffer_destroy(&line); buffer_destroy(&pending);
    return status;
}
