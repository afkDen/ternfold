#include "ternfold/c_emit.h"
#include "c_nodes.h"
#include "c_metadata.h"
#include "lifetimes.h"

#include <stdarg.h>
#include <stdlib.h>

typedef struct {
    FILE *out;
    const TfCoreProgramData *core;
    size_t next_helper;
    bool failed;
    TfNativeLifetimes **lifetimes;
} Emitter;

static void line(Emitter *e,const char *format,...) {
    if (e->failed) return;
    va_list ap;
    va_start(ap,format);
    if (vfprintf(e->out,format,ap)<0) e->failed=true;
    va_end(ap);
}

static const char *binary_name(TfCoreBinary op) {
    static const char *const names[]={NULL,"TF_RT_NUM_ADD","TF_RT_NUM_SUBTRACT","TF_RT_NUM_MULTIPLY",
        "TF_RT_NUM_REAL_DIVIDE","TF_RT_NUM_FLOOR_DIVIDE","TF_RT_NUM_REMAINDER","TF_RT_NUM_POWER",
        "TF_RT_NUM_LESS","TF_RT_NUM_LESS_EQUAL","TF_RT_NUM_GREATER","TF_RT_NUM_GREATER_EQUAL"};
    return (size_t)op<sizeof names/sizeof *names ? names[op] : NULL;
}

static const char *builtin_name(TfCoreBuiltin op) {
    static const char *const names[]={NULL,"TF_BUILTIN_LEN","TF_BUILTIN_AT","TF_BUILTIN_APPEND",NULL,NULL,
        "TF_BUILTIN_RANGE","TF_BUILTIN_INT_TEXT","TF_BUILTIN_INT_FLOAT","TF_BUILTIN_FLOAT_INT",
        "TF_BUILTIN_FLOAT_TEXT","TF_BUILTIN_BOOL_TEXT","TF_BUILTIN_PARSE_INT","TF_BUILTIN_TEXT_LEN",
        "TF_BUILTIN_TEXT_AT","TF_BUILTIN_TEXT_SLICE"};
    return (size_t)op<sizeof names/sizeof *names ? names[op] : NULL;
}

static bool callback_kind(const TfCoreOperation *op) {
    return op->kind==TF_CORE_OP_BUILTIN &&
           (op->as.builtin.id==TF_CORE_BUILTIN_MAP || op->as.builtin.id==TF_CORE_BUILTIN_FOLD);
}

static bool suspends(const TfCoreOperation *op) {
    return op->kind==TF_CORE_OP_CALL || op->kind==TF_CORE_OP_CALL_CLOSURE || callback_kind(op);
}

static void origin(Emitter *e,TfCoreSpanId span,const TfCoreFunction *fn) {
    if (fn->kind==TF_CORE_FUNCTION_PRELUDE_WRAPPER) line(e,"f->span");
    else line(e,"%zu",span);
}

static void checked(Emitter *e,const TfCoreFunction *fn,TfCoreSpanId span,const char *callee) {
    line(e,"    if (%s(s->ctx, ",callee);
    origin(e,span,fn);
}

static void check_end(Emitter *e,size_t dest) {
    line(e,", &v[%zu]) != TF_OK) return TF_FAULT;\n",dest);
}

static void staged_begin(Emitter *e,const TfCoreOperation *op) {
    line(e,"    {\n");
    for (size_t i=0;i<op->operands.count;++i) line(e,"        a[%zu] = v[%zu];\n",i,op->operands.items[i]-1);
}

static void staged_end(Emitter *e,const TfCoreOperation *op,bool suspend) {
    for (size_t i=0;i<op->operands.count;++i) line(e,"        a[%zu] = tf_unit();\n",i);
    line(e,suspend ? "        return status;\n    }\n" : "        if (status != TF_OK) return status;\n    }\n");
}

static void staged(Emitter *e,const TfCoreFunction *fn,const TfCoreLet *let) {
    const TfCoreOperation *op=&let->operation;
    const size_t dest=let->value-1,count=op->operands.count;
    const char *args=count!=0 ? "a" : "NULL";
    staged_begin(e,op);
    line(e,"        const tf_status status = ");
    switch (op->kind) {
    case TF_CORE_OP_RECORD: case TF_CORE_OP_MACHINE_VALUE: case TF_CORE_OP_LIST: case TF_CORE_OP_VARIANT:
        line(e,"%s(s->ctx, ",op->kind==TF_CORE_OP_RECORD ? "tf_record" :
             op->kind==TF_CORE_OP_MACHINE_VALUE ? "tf_machine" : op->kind==TF_CORE_OP_LIST ? "tf_list" : "tf_variant");
        origin(e,let->span,fn); line(e,", %zu",let->type);
        if (op->kind==TF_CORE_OP_VARIANT) line(e,", %zu",op->as.variant.tag);
        line(e,", %s, %zu, &v[%zu]);\n",args,count,dest);
        break;
    case TF_CORE_OP_CLOSURE:
        line(e,"tf_closure(s->ctx, "); origin(e,let->span,fn);
        line(e,", %zu, %s, %zu, &v[%zu]);\n",op->as.function,args,count,dest);
        break;
    case TF_CORE_OP_CALL: case TF_CORE_OP_CALL_CLOSURE:
        line(e,"%s(s, f, ",op->kind==TF_CORE_OP_CALL ? "tf_stack_call" : "tf_stack_call_closure");
        origin(e,let->span,fn);
        if (op->kind==TF_CORE_OP_CALL)
            line(e,", %zu, %s, %zu, %zu, %zu);\n",op->as.function,args,count,dest,let->value);
        else line(e,", a[0], %s, %zu, %zu, %zu);\n",count==1 ? "NULL" : "a + 1",count-1,dest,let->value);
        break;
    case TF_CORE_OP_BUILTIN:
        if (callback_kind(op)) {
            line(e,"tf_stack_call(s, f, "); origin(e,let->span,fn);
            line(e,", %zu, a, %zu, %zu, %zu);\n",++e->next_helper,count,dest,let->value);
        } else {
            const char *name=builtin_name(op->as.builtin.id);
            if (name==NULL) { e->failed=true; return; }
            line(e,"tf_builtin_call(s->ctx, &tf_builtins, "); origin(e,let->span,fn);
            line(e,", %s, %zu, a, %zu, &v[%zu]);\n",name,let->type,count,dest);
        }
        break;
    default: e->failed=true; return;
    }
    staged_end(e,op,suspends(op));
}

static void operation(Emitter *e,const TfCBody *body,const TfCoreLet *let) {
    const TfCoreFunction *fn=body->function;
    const TfCoreOperation *op=&let->operation;
    const size_t dest=let->value-1;
    const size_t a=op->operands.count!=0 ? op->operands.items[0]-1 : 0;
    const size_t b=op->operands.count>1 ? op->operands.items[1]-1 : 0;
    switch (op->kind) {
    case TF_CORE_OP_CONST:
        line(e,"    if (tf_constant_get(tf_active_pool(s), "); origin(e,let->span,fn);
        line(e,", tf_literal_id_%zu",op->as.constant); check_end(e,dest); break;
    case TF_CORE_OP_UNARY:
        checked(e,fn,let->span,op->as.unary==TF_CORE_UNARY_NOT ? "tf_numeric_not" : "tf_numeric_negate");
        line(e,", v[%zu]",a); check_end(e,dest); break;
    case TF_CORE_OP_BINARY:
        if (op->as.binary==TF_CORE_BINARY_EQUAL || op->as.binary==TF_CORE_BINARY_NOT_EQUAL) {
            checked(e,fn,let->span,"tf_value_equal");
            line(e,", v[%zu], v[%zu]",a,b); check_end(e,dest);
            if (op->as.binary==TF_CORE_BINARY_NOT_EQUAL) line(e,"    v[%zu] = tf_bool(!v[%zu].as.boolean);\n",dest,dest);
        } else if (op->as.binary==TF_CORE_BINARY_CONCAT) {
            checked(e,fn,let->span,tf_core_type(e->core,let->type)->kind==TF_CORE_TYPE_TEXT ? "tf_text_concat" : "tf_list_concat");
            line(e,", v[%zu], v[%zu]",a,b); check_end(e,dest);
        } else {
            const char *name=binary_name(op->as.binary);
            if (name==NULL) { e->failed=true; break; }
            checked(e,fn,let->span,"tf_numeric_binary");
            line(e,", %s, v[%zu], v[%zu]",name,a,b); check_end(e,dest);
        }
        break;
    case TF_CORE_OP_RECORD: case TF_CORE_OP_MACHINE_VALUE: case TF_CORE_OP_LIST: case TF_CORE_OP_VARIANT:
    case TF_CORE_OP_CLOSURE: case TF_CORE_OP_CALL: case TF_CORE_OP_CALL_CLOSURE: case TF_CORE_OP_BUILTIN:
        staged(e,fn,let); break;
    case TF_CORE_OP_PROJECT:
        checked(e,fn,let->span,"tf_project"); line(e,", v[%zu], %zu",a,op->as.field); check_end(e,dest); break;
    case TF_CORE_OP_PAYLOAD:
        checked(e,fn,let->span,"tf_payload"); line(e,", v[%zu], %zu, %zu",a,op->as.payload.tag,op->as.payload.index);
        check_end(e,dest); break;
    case TF_CORE_OP_IF:
        line(e,"    if (v[%zu].as.boolean != 0) goto block_%zu;\n    goto block_%zu;\n",
             a,op->as.branch.then_block,op->as.branch.else_block);
        break;
    case TF_CORE_OP_SWITCH_TAG:
        line(e,"    {\n        size_t tag;\n");
        checked(e,fn,let->span,"tf_variant_tag"); line(e,", v[%zu], &tag) != TF_OK) return TF_FAULT;\n",a);
        line(e,"        switch (tag) {\n");
        for (size_t i=0;i<op->as.switch_tag.case_count;++i)
            line(e,"        case %zu: goto block_%zu;\n",op->as.switch_tag.cases[i].tag,op->as.switch_tag.cases[i].block);
        if (op->as.switch_tag.default_block!=0) line(e,"        default: goto block_%zu;\n",op->as.switch_tag.default_block);
        else line(e,"        default: return tf_context_fail(s->ctx, TF_R004, %zu);\n",let->span);
        line(e,"        }\n    }\n");
        break;
    case TF_CORE_OP_INPUT_AT:
        checked(e,fn,let->span,"tf_entry_input"); line(e,", v[%zu], %zu",a,op->as.input_index); check_end(e,dest); break;
    case TF_CORE_OP_TAG:
        /* Verified internal Tag values have no value operand or result use.
         * The subject is already evaluated; reading its tag cannot fault. */
        break;
    case TF_CORE_OP_LIST_MAP:
        line(e,"    if (tf_loop_begin(s, f, %zu, v[%zu]) != TF_OK) return TF_FAULT;\n",let->span,a);
        line(e,"loop_%zu: ;\n    {\n        bool done;\n",let->value);
        line(e,"        if (tf_loop_element(s, f, %zu, v[%zu], &v[%zu], &done) != TF_OK) return TF_FAULT;\n",
             let->span,a,op->as.list_map.element.value-1);
        line(e,"        if (!done) goto block_%zu;\n    }\n",op->as.list_map.body);
        line(e,"    if (tf_loop_finish(s, f, %zu, %zu, &v[%zu]) != TF_OK) return TF_FAULT;\n",let->span,let->type,dest);
        break;
    }
    if (suspends(op) || op->kind==TF_CORE_OP_IF || op->kind==TF_CORE_OP_SWITCH_TAG)
        line(e,"cont_%zu: ;\n",let->value);
}

static void body(Emitter *e,const TfCBody *c,TfCoreFunctionId id) {
    const TfCoreFunction *fn=c->function;
    line(e,"\nstatic tf_status tf_body_%zu(tf_stack *s, tf_stack_frame *f) {\n    tf_value *v = f->slots;\n    (void)v;\n",id);
    if (c->argument_slots!=0) {
        line(e,"    tf_value *a = v + %zu;\n",fn->value_count);
        /* All non-staged operations can still contribute to the maximum. */
        line(e,"    (void)a;\n");
    }
    line(e,"    switch (f->pc) {\n    case 0: break;\n");
    for (size_t i=0;i<fn->block_count;++i)
        for (size_t j=0;j<fn->blocks[i].let_count;++j) {
            const TfCoreLet *let=&fn->blocks[i].lets[j];
            if (suspends(&let->operation)) line(e,"    case %zu: goto cont_%zu;\n",let->value,let->value);
        }
    line(e,"    default: return tf_context_fail(s->ctx, TF_R004, f->span);\n    }\n");
    for (size_t i=0;i<fn->parameter_count;++i) {
        checked(e,fn,fn->parameters[i].span,"tf_retain"); line(e,", f->args[%zu]",i);
        check_end(e,fn->parameters[i].value-1);
    }
    for (size_t i=0;i<fn->capture_count;++i) {
        checked(e,fn,fn->captures[i].span,"tf_retain"); line(e,", f->captures[%zu]",i);
        check_end(e,fn->captures[i].value-1);
    }
    if (tf_core_trace_machine(e->core,id)) line(e,"    tf_stack_trace_begin(s, f, %zu);\n",fn->span);
    line(e,"    goto block_%zu;\n",fn->body);
    for (size_t i=0;i<fn->block_count && !e->failed;++i) {
        const TfCoreBlock *block=&fn->blocks[i];
        const TfCBlockExit exit=c->exits[i];
        line(e,"block_%zu: ;\n",i+1);
        size_t observation_count=0, observation=0;
        const TfCoreTracePoint *points=tf_core_trace_points(e->core,id,i+1,&observation_count);
        for (size_t j=0;j<=block->let_count && !e->failed;++j) {
            while (observation<observation_count && points[observation].after_let==j) {
                const TfCoreTracePoint *p=&points[observation++];
                line(e,"    tf_stack_trace_value(s, f, %s, %zu, %zu, %zu);\n",
                     p->kind==TF_CORE_TRACE_WRITER ? "TF_TRACE_WRITER" :
                     p->kind==TF_CORE_TRACE_GUARD ? "TF_TRACE_GUARD" : "TF_TRACE_EMISSION",
                     p->span,p->ordinal,p->value-1);
            }
            if (e->lifetimes != NULL) {
                const TfCoreValueIds releases=tf_native_lifetimes_releases(e->lifetimes[id-1],j);
                for (size_t r=0;r<releases.count;++r)
                    line(e,"    tf_release(&v[%zu]);\n",releases.items[r]-1);
            }
            if (j<block->let_count) operation(e,c,&block->lets[j]);
        }
        if (block->terminator.kind==TF_CORE_UNREACHABLE) {
            line(e,"    return tf_context_fail(s->ctx, TF_R004, %zu);\n",block->terminator.span);
        } else if (exit.kind==TF_C_ROOT) {
            line(e,"    return tf_stack_return(s, f, %zu);\n",block->terminator.value-1);
        } else if (exit.kind==TF_C_BRANCH) {
            checked(e,fn,block->terminator.span,"tf_retain");
            line(e,", v[%zu]",block->terminator.value-1); check_end(e,exit.parent_value-1);
            for (size_t j=0;j<block->let_count;++j) line(e,"    tf_release(&v[%zu]);\n",block->lets[j].value-1);
            line(e,"    goto cont_%zu;\n",exit.parent_value);
        } else if (exit.kind==TF_C_ITERATION) {
            line(e,"    if (tf_loop_keep(s, f, %zu, v[%zu]) != TF_OK) return TF_FAULT;\n",
                 block->terminator.span,block->terminator.value-1);
            for (size_t j=0;j<block->let_count;++j) line(e,"    tf_release(&v[%zu]);\n",block->lets[j].value-1);
            line(e,"    tf_release(&v[%zu]);\n    goto loop_%zu;\n",exit.element-1,exit.parent_value);
        }
    }
    line(e,"}\n");
}

static size_t helper_count(const TfCoreProgramData *core) {
    size_t count=0;
    for (size_t i=0;i<core->function_count;++i) {
        const TfCoreFunction *fn=&core->functions[i];
        for (size_t j=0;j<fn->block_count;++j)
            for (size_t k=0;k<fn->blocks[j].let_count;++k)
                if (callback_kind(&fn->blocks[j].lets[k].operation)) ++count;
    }
    return count;
}

static void ids(Emitter *e,const char *prefix,size_t id,const TfCoreParameter *params,size_t count) {
    if (count==0) return;
    line(e,"static const tf_type_id tf_%s_%zu[] = {",prefix,id);
    for (size_t i=0;i<count;++i) line(e,"%s%zu",i!=0 ? ", " : "",params[i].type);
    line(e,"};\n");
}

static void shape(Emitter *e,const char *prefix,size_t id,size_t count) {
    if (count==0) line(e,"{NULL, 0}");
    else line(e,"{tf_%s_%zu, %zu}",prefix,id,count);
}

static void tables(Emitter *e,const TfCBody *bodies,size_t helpers,size_t functions) {
    const TfCoreProgramData *core=e->core;
    const size_t total=functions+helpers;
    const size_t generic=core->type_count+1;
    TfCoreTypeId *closure_types=calloc(functions+1,sizeof *closure_types);
    if (closure_types==NULL) { e->failed=true; return; }
    size_t helper=functions;
    for (size_t i=0;i<functions;++i) {
        const TfCoreFunction *fn=bodies[i].function;
        line(e,"static tf_status tf_body_%zu(tf_stack *, tf_stack_frame *);\n",i+1);
        ids(e,"params",i+1,fn->parameters,fn->parameter_count);
        ids(e,"captures",i+1,fn->captures,fn->capture_count);
        for (size_t j=0;j<fn->block_count;++j)
            for (size_t k=0;k<fn->blocks[j].let_count;++k) {
                const TfCoreLet *let=&fn->blocks[j].lets[k];
                const TfCoreOperation *op=&let->operation;
                if (op->kind==TF_CORE_OP_CLOSURE) closure_types[op->as.function]=let->type;
                if (callback_kind(op)) {
                    line(e,"static const tf_type_id tf_params_%zu[] = {",++helper);
                    for (size_t a=0;a<op->operands.count;++a)
                        line(e,"%s%zu",a!=0 ? ", " : "",bodies[i].types[op->operands.items[a]-1]);
                    line(e,"};\n");
                }
            }
    }
    line(e,"static const tf_rt_function tf_functions[] = {\n");
    if (total==0) line(e,"    {0, {NULL, 0}},\n");
    for (size_t i=0;i<total;++i) {
        const size_t type=i<functions && closure_types[i+1]!=0 ? closure_types[i+1] : generic;
        line(e,"    {%zu, ",type);
        shape(e,"captures",i+1,i<functions ? bodies[i].function->capture_count : 0);
        line(e,"},\n");
    }
    free(closure_types);
    line(e,"};\nstatic const tf_stack_body tf_bodies[] = {\n");
    if (total==0) line(e,"    {NULL, TF_RT_ORDINARY, {NULL, 0}, 0, 0},\n");
    for (size_t i=0;i<functions;++i) {
        const TfCoreFunction *fn=bodies[i].function;
        const char *kind=fn->kind==TF_CORE_FUNCTION_ORDINARY ? "TF_RT_ORDINARY" :
                         fn->kind==TF_CORE_FUNCTION_ENTRY_ONLY ? "TF_RT_ENTRY_ONLY" : "TF_RT_PRELUDE_WRAPPER";
        line(e,"    {tf_body_%zu, %s, ",i+1,kind);
        shape(e,"params",i+1,fn->parameter_count);
        line(e,", %zu, %zu},\n",fn->result,fn->value_count+bodies[i].argument_slots);
    }
    helper=functions;
    for (size_t i=0;i<functions;++i) {
        const TfCoreFunction *fn=bodies[i].function;
        for (size_t j=0;j<fn->block_count;++j)
            for (size_t k=0;k<fn->blocks[j].let_count;++k) {
                const TfCoreLet *let=&fn->blocks[j].lets[k];
                const TfCoreOperation *op=&let->operation;
                if (callback_kind(op))
                    line(e,"    {%s, TF_RT_ORDINARY, {tf_params_%zu, %zu}, %zu, 4},\n",
                         op->as.builtin.id==TF_CORE_BUILTIN_MAP ? "tf_stack_map_resume" : "tf_stack_fold_resume",
                         ++helper,op->operands.count,let->type);
            }
    }
    line(e,"};\nstatic const tf_rt_program tf_value_program = {tf_types, %zu, tf_functions, %zu};\n",generic,total);
    line(e,"static const tf_stack_program tf_body_program = {tf_bodies, %zu, %zu};\n",total,core->entry);
    line(e,"static const tf_builtin_env tf_builtins = {&tf_value_program, {0}, 268435456};\n");
}

static void launcher(Emitter *e) {
    const TfCoreProgramData *core=e->core;
    const TfCoreFunction *entry=tf_core_function(core,core->entry);
    bool input=false;
    if (entry!=NULL && entry->kind==TF_CORE_FUNCTION_ENTRY_ONLY) {
        const TfCoreBlock *root=tf_core_block(entry,entry->body);
        for (size_t i=0;i<root->let_count;++i)
            if (root->lets[i].operation.kind==TF_CORE_OP_INPUT_AT) { input=true; break; }
    }
    line(e,"\nstatic void tf_show_origin(tf_span_id span, const char *prefix) {\n"
           "    if (span == 0 || span > tf_origin_count) return;\n"
           "    const struct tf_generated_origin *p = &tf_origins[span - 1];\n"
           "    (void)fputs(prefix, stderr);\n"
           "    (void)fputc('\"', stderr);\n"
           "    for (size_t i = 0; i < p->path_length; ++i) {\n"
           "        const unsigned char c = p->path[i];\n"
           "        if (c >= 32 && c <= 126 && c != '\\\\' && c != '\"') (void)fputc(c, stderr);\n"
           "        else (void)fprintf(stderr, \"\\\\x%%02x\", (unsigned)c);\n"
           "    }\n"
           "    (void)fprintf(stderr, \"\\\":%%zu:%%zu..%%zu:%%zu [bytes %%zu..%%zu)\\n\",\n"
           "                  p->start_line, p->start_column, p->end_line, p->end_column, p->start, p->end);\n"
           "}\n"
           "static void tf_show_fault(const tf_fault *fault) {\n"
           "    static const char *const messages[] = { \"unexpected runtime state\",\n"
           "        \"arithmetic overflow or nonfinite result\", \"division or remainder by zero\",\n"
           "        \"execution resource limit exceeded\", \"unexpected runtime state\",\n"
           "        \"negative exponent for Int power\", \"not enough input lines\" };\n"
           "    const size_t code = (size_t)fault->code;\n"
           "    (void)fprintf(stderr, \"error[R%%03d]: %%s\\n\", (int)fault->code,\n"
           "                  messages[code < sizeof messages / sizeof *messages ? code : 0]);\n"
           "    tf_show_origin(fault->span, \"  --> \");\n"
           "    for (size_t i = fault->frame_count; i != 0; --i) {\n"
           "        const tf_call_frame frame = fault->frames[i - 1];\n"
           "        if (frame.function > %zu || frame.call_span == fault->span) continue;\n"
           "        tf_show_origin(frame.call_span, \"  called here: \");\n"
           "    }\n"
           "}\n",core->function_count);
    line(e,"\nint main(int argc, char **argv) {\n"
           "    (void)tf_builtins; (void)tf_type_count; (void)tf_active_pool;\n"
           "    const char *input_path = NULL, *trace_path = NULL;\n"
           "    for (int i = 1; i < argc; ++i) {\n"
           "        if (i + 1 >= argc || argv[i + 1][0] == '\\0') return 2;\n"
           "        if (strcmp(argv[i], \"--stdin-file\") == 0 && input_path == NULL) input_path = argv[++i];\n"
           "        else if (strcmp(argv[i], \"--trace\") == 0 && trace_path == NULL) trace_path = argv[++i];\n"
           "        else { (void)fputs(\"expected [--stdin-file FILE] [--trace NEW_FILE]\\n\", stderr); return 2; }\n"
           "    }\n"
           "    (void)input_path;\n"
           "    if (tf_body_program.entry == 0) { (void)fputs(\"error[E208]: missing root main\\n\", stderr); return 1; }\n"
           "#ifdef _WIN32\n"
           "    if (_setmode(_fileno(stdout), _O_BINARY) == -1 || _setmode(_fileno(stderr), _O_BINARY) == -1) return 2;\n"
           "#else\n"
           "    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) return 2;\n"
           "#endif\n"
           "    tf_context ctx;\n"
           "    if (tf_context_init(&ctx, &tf_value_program, NULL) != TF_OK) return 2;\n"
           "    tf_generated_stack instance = {0};\n"
           "    TfTraceRecord *records = NULL;\n"
           "    TfTraceRecorder observer = {0};\n"
           "    tf_value input = tf_unit(), output = tf_unit();\n"
           "    int status = 2;\n"
           "    if (trace_path != NULL) {\n"
           "        records = calloc(TF_TRACE_IO_MAX_RECORDS, sizeof *records);\n"
           "        if (records != NULL) (void)tf_trace_recorder_init(&observer, records, TF_TRACE_IO_MAX_RECORDS);\n"
           "    }\n"
           "    if (tf_stack_init(&instance.stack, &ctx, &tf_body_program, ctx.config.call_limit) != TF_OK) goto done;\n"
           "    instance.stack.observer = records == NULL ? NULL : &observer;\n");
    if (entry!=NULL && entry->kind==TF_CORE_FUNCTION_ENTRY_ONLY) {
        if (input) {
            line(e,"    {\n"
                   "        FILE *stream = input_path == NULL ? stdin : fopen(input_path, \"rb\");\n"
                   "#ifdef _WIN32\n"
                   "        if (stream == stdin && _setmode(_fileno(stdin), _O_BINARY) == -1) goto done;\n"
                   "#endif\n"
                   "        if (stream == NULL) { (void)fputs(\"cannot open the input snapshot\\n\", stderr); goto done; }\n"
                   "        const tf_io_status captured = tf_input_capture(&ctx, %zu, stream, &input);\n"
                   "        const bool closed = stream == stdin || fclose(stream) == 0;\n"
                   "        if (captured != TF_IO_OK || !closed) {\n"
                   "            (void)fputs(captured == TF_IO_INVALID_UTF8 ? \"input snapshot is not valid UTF-8\\n\" :\n"
                   "                        \"cannot capture input snapshot\\n\", stderr);\n"
                   "            goto done;\n"
                   "        }\n"
                   "    }\n",entry->parameters[0].type);
        } else {
            line(e,"    if (tf_list(&ctx, 0, %zu, NULL, 0, &input) != TF_OK) goto fault;\n",entry->parameters[0].type);
        }
    }
    line(e,"    if (tf_constants_create(&ctx, tf_literals, tf_literal_count, &instance.pool) != TF_OK) goto fault;\n");
    line(e,"    if (tf_stack_run(&instance.stack, 0, tf_body_program.entry, NULL, 0, %s, %u, &output) != TF_OK) goto fault;\n",
         entry!=NULL && entry->kind==TF_CORE_FUNCTION_ENTRY_ONLY ? "&input" : "NULL",
         entry!=NULL && entry->kind==TF_CORE_FUNCTION_ENTRY_ONLY ? 1u : 0u);
    line(e,"    const tf_io_status published = tf_output_publish(&ctx, stdout, output);\n"
           "    if (published == TF_IO_FAULT) goto fault;\n"
           "    if (published != TF_IO_OK || fflush(stdout) != 0) {\n"
           "        (void)fputs(\"cannot write program output\\n\", stderr); goto done;\n"
           "    }\n"
           "    status = 0;\n"
           "    goto done;\n"
           "fault: ;\n"
           "    const tf_fault *fault = tf_stack_fault(&instance.stack);\n"
           "    if (fault != NULL) { tf_show_fault(fault); status = 3; }\n"
           "    else (void)fputs(\"cannot execute verified Core on this host\\n\", stderr);\n"
           "done:\n"
           "    if (trace_path != NULL && (records == NULL ||\n"
           "        tf_trace_artifact_write(trace_path, &observer, tf_origins, tf_origin_count) != TF_TRACE_IO_OK)) {\n"
           "        (void)fputs(\"cannot write new trace artifact and source map\\n\", stderr); status = 2;\n"
           "    }\n"
           "    free(records);\n"
           "    tf_release(&output); tf_release(&input);\n"
           "    tf_stack_destroy(&instance.stack); tf_constants_destroy(instance.pool);\n"
           "    if (fflush(stderr) != 0) status = 2;\n"
           "    return status;\n"
           "}\n");
}

static TfStatus emit(FILE *out,const TfVerifiedCore *verified,bool shorten) {
    const TfCoreProgramData *core=tf_verified_core_data(verified);
    if (out==NULL || core==NULL || ferror(out) || core->type_count==SIZE_MAX ||
        core->function_count==SIZE_MAX || core->function_count>SIZE_MAX/sizeof(TfCBody)) return TF_HOST_ERROR;
    const size_t functions=core->function_count,helpers=helper_count(core);
    if (helpers>SIZE_MAX-functions) return TF_HOST_ERROR;
    Emitter e={.out=out,.core=core,.next_helper=functions};
    /* Retain a valid array base even for a library with no functions. */
    TfCBody *plans=calloc(functions!=0 ? functions : 1,sizeof *plans);
    if (plans==NULL) return TF_HOST_ERROR;
    if (shorten) {
        e.lifetimes=calloc(functions!=0 ? functions : 1,sizeof *e.lifetimes);
        if (e.lifetimes==NULL) { free(plans); return TF_HOST_ERROR; }
    }
    for (size_t i=0;i<functions;++i) {
        if (tf_c_body(&plans[i],verified,i+1)!=TF_OK || plans[i].function==NULL) { e.failed=true; break; }
        if (shorten && tf_native_lifetimes(&e.lifetimes[i],verified,i+1,NULL)!=TF_OK) { e.failed=true; break; }
    }
    if (!e.failed) {
        line(&e,"/* Ternfold 0.2: generated from verified Core. */\n"
                "#include \"ternfold_rt_stack.h\"\n#include \"ternfold_rt_numeric.h\"\n"
                "#include \"ternfold_rt_value_ops.h\"\n#include \"ternfold_rt_builtins.h\"\n"
                "#include \"ternfold_rt_constants.h\"\n#include \"ternfold_rt_callbacks.h\"\n"
                "#include \"ternfold_rt_io.h\"\n#include <string.h>\n#include <stdlib.h>\n#include <signal.h>\n"
                "#ifdef _WIN32\n#include <fcntl.h>\n#include <io.h>\n#endif\n\n");
        if (tf_c_metadata_write(out,verified)!=TF_OK) e.failed=true;
        line(&e,"typedef struct { tf_stack stack; tf_constant_pool *pool; } tf_generated_stack;\n"
                "static tf_constant_pool *tf_active_pool(tf_stack *s) { return ((tf_generated_stack *)s)->pool; }\n");
        tables(&e,plans,helpers,functions);
        for (size_t i=0;i<functions && !e.failed;++i) body(&e,&plans[i],i+1);
        launcher(&e);
    }
    for (size_t i=0;i<functions;++i) tf_c_body_destroy(&plans[i]);
    if (e.lifetimes!=NULL) {
        for (size_t i=0;i<functions;++i) tf_native_lifetimes_destroy(e.lifetimes[i]);
        free(e.lifetimes);
    }
    free(plans);
    return e.failed || ferror(out) ? TF_HOST_ERROR : TF_OK;
}

TfStatus tf_c_emit_optimized(FILE *out,const TfVerifiedCore *verified,TfCoreOptLevel level) {
    if (level==TF_CORE_OPT_LEVEL_0) return emit(out,verified,false);
    if (level!=TF_CORE_OPT_LEVEL_1) return TF_HOST_ERROR;
    TfCoreOptimization *owner=NULL;
    if (tf_core_optimize(&owner,verified,level,NULL)!=TF_OK) return TF_HOST_ERROR;
    const TfStatus status=emit(out,tf_core_optimization_verified(owner),true);
    tf_core_optimization_destroy(owner);
    return status;
}

TfStatus tf_c_emit(FILE *out,const TfVerifiedCore *verified) {
    return tf_c_emit_optimized(out,verified,TF_CORE_OPT_LEVEL_0);
}
