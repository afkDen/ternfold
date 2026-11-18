#include "c_nodes.h"

#include <stdlib.h>

static void attach(TfCBody *body,TfCoreBlockId child,TfCExitKind kind,TfCoreValueId parent) {
    if (child!=0) body->exits[child-1]=(TfCBlockExit){.kind=kind,.parent_value=parent};
}

TfStatus tf_c_body(TfCBody *out,const TfVerifiedCore *core,TfCoreFunctionId function) {
    if (out==NULL) return TF_HOST_ERROR;
    *out=(TfCBody){0};
    const TfCoreFunction *fn=tf_core_function(tf_verified_core_data(core),function);
    if (fn==NULL || fn->block_count>SIZE_MAX/sizeof *out->exits || fn->value_count>SIZE_MAX/sizeof *out->types)
        return TF_HOST_ERROR;
    out->function=fn;
    out->exits=calloc(fn->block_count,sizeof *out->exits);
    out->types=calloc(fn->value_count,sizeof *out->types);
    if (out->exits==NULL || out->types==NULL) { tf_c_body_destroy(out); return TF_HOST_ERROR; }
    for (size_t i=0;i<fn->parameter_count;++i) out->types[fn->parameters[i].value-1]=fn->parameters[i].type;
    for (size_t i=0;i<fn->capture_count;++i) out->types[fn->captures[i].value-1]=fn->captures[i].type;
    for (size_t i=0;i<fn->block_count;++i) {
        const TfCoreBlock *block=&fn->blocks[i];
        for (size_t j=0;j<block->let_count;++j) {
            const TfCoreLet *let=&block->lets[j];
            const TfCoreOperation *op=&let->operation;
            out->types[let->value-1]=let->type;
            if (op->operands.count>out->argument_slots) out->argument_slots=op->operands.count;
            if (op->kind==TF_CORE_OP_IF) {
                attach(out,op->as.branch.then_block,TF_C_BRANCH,let->value);
                attach(out,op->as.branch.else_block,TF_C_BRANCH,let->value);
            } else if (op->kind==TF_CORE_OP_SWITCH_TAG) {
                for (size_t k=0;k<op->as.switch_tag.case_count;++k)
                    attach(out,op->as.switch_tag.cases[k].block,TF_C_BRANCH,let->value);
                attach(out,op->as.switch_tag.default_block,TF_C_BRANCH,let->value);
            } else if (op->kind==TF_CORE_OP_LIST_MAP) {
                out->types[op->as.list_map.element.value-1]=op->as.list_map.element.type;
                attach(out,op->as.list_map.body,TF_C_ITERATION,let->value);
                out->exits[op->as.list_map.body-1].element=op->as.list_map.element.value;
            }
        }
    }
    if (out->argument_slots>SIZE_MAX-fn->value_count) { tf_c_body_destroy(out); return TF_HOST_ERROR; }
    return TF_OK;
}

void tf_c_body_destroy(TfCBody *body) {
    if (body==NULL) return;
    free(body->types); free(body->exits); *body=(TfCBody){0};
}
