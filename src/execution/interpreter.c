#include "builtins.h"
#include "../internal.h"

#include <string.h>

typedef enum { BLOCK, MAP, BUILTIN_MAP, BUILTIN_FOLD } FrameKind;
typedef struct {
    FrameKind kind;
    TfCoreFunctionId function;
    size_t base, destination;
    TfCoreSpanId invocation;
    union {
        struct { const TfCoreBlock *body; size_t next; bool root; } block;
        struct {
            TfExecValues input;
            TfExecValue *output;
            TfCoreTypeId result;
            TfCoreBlockId body;
            TfCoreValueId element;
            size_t next;
            TfCoreSpanId span;
            TfExecValue closure, accumulator;
        } map;
    } as;
} Frame;

struct TfInterpreterResult {
    TfAllocator allocator;
    size_t count;
    TfCoreText lines[];
};

typedef struct {
    TfExecArena arena;
    const TfCoreProgramData *data;
    const TfExecValue *constants;
    Frame *frames;
    TfExecValue *slots;
    size_t frame_count, frame_capacity, slot_count, slot_capacity;
    size_t call_depth, call_limit;
    uint64_t steps;
    TfRuntimeFault *fault;
    TfExecValue result;
    bool complete;
} Vm;

#define HOST_DESTINATION SIZE_MAX
#define MAP_DESTINATION (SIZE_MAX-1)

TfInterpreterLimits tf_interpreter_default_limits(void) {
    return (TfInterpreterLimits){256u*1024u*1024u,1024,UINT64_C(10000000)};
}

static bool budget(Vm *vm, uint64_t work) {
    if (work>vm->steps) return false;
    vm->steps-=work; return true;
}

/* Frame and slot storage can grow and is reused after return. Allocation
 * charges the actual peak while copying the previous contents. */
static bool grow(Vm *vm, void **pointer, size_t *capacity, size_t needed, size_t width) {
    if (needed<=*capacity) return true;
    if (needed>SIZE_MAX/width || needed>vm->arena.remaining/width) return false;
    size_t next=needed;
    if (*capacity<=SIZE_MAX/2 && *capacity*2>needed && *capacity*2<=vm->arena.remaining/width)
        next=*capacity*2;
    const size_t size=next*width, old=*capacity*width;
    void *memory=vm->arena.allocator.allocate(vm->arena.allocator.context,size);
    if (memory==NULL) return false;
    if (*pointer!=NULL) {
        memcpy(memory,*pointer,old);
        vm->arena.allocator.deallocate(vm->arena.allocator.context,*pointer);
    }
    vm->arena.remaining-=size-old;
    *capacity=next; *pointer=memory;
    return true;
}

static TfStatus fault(Vm *vm, TfRuntimeCode code, TfCoreSpanId span) {
    if (vm->fault==NULL) return TF_SOURCE_ERROR;
    TfRuntimeFault *f=vm->fault;
    f->code=code; f->span=span;
    size_t walked=0;
    const size_t head=TF_INTERPRETER_FAULT_FRAMES/2;
    if (vm->call_depth>TF_INTERPRETER_FAULT_FRAMES)
        f->omitted_frames=vm->call_depth-TF_INTERPRETER_FAULT_FRAMES;
    for (size_t i=0; i<vm->frame_count; ++i) {
        const Frame *frame=&vm->frames[i];
        if (frame->kind!=BLOCK || !frame->as.block.root) continue;
        const size_t index=walked++;
        if (index>=head && index<head+f->omitted_frames) continue;
        f->frames[f->frame_count++]=(TfRuntimeFrame){frame->function,frame->invocation};
    }
    return TF_SOURCE_ERROR;
}

static bool push(Vm *vm, Frame frame) {
    if (vm->frame_count==SIZE_MAX) return false;
    void *buffer=vm->frames;
    if (!grow(vm,&buffer,&vm->frame_capacity,vm->frame_count+1,sizeof *vm->frames)) return false;
    vm->frames=buffer;
    vm->frames[vm->frame_count++]=frame;
    return true;
}

static TfExecValue read_slot(const Vm *vm, size_t base, TfCoreValueId id) {
    return vm->slots[base+id-1];
}

/* Either use stable direct values or read IDs from the caller after slot
 * growth. The verifier checked the signature and every lexical use. */
static bool call(Vm *vm, TfCoreFunctionId id, TfExecValues captures, const TfExecValue *direct,
                 const TfCoreValueId *ids, size_t caller, size_t destination, TfCoreSpanId origin) {
    const TfCoreFunction *fn=tf_core_function(vm->data,id);
    const size_t base=vm->slot_count;
    if (fn==NULL || captures.count!=fn->capture_count || (captures.count!=0 && captures.items==NULL) ||
        (fn->parameter_count!=0 && direct==NULL && ids==NULL) ||
        vm->call_depth==vm->call_limit || fn->value_count>SIZE_MAX-base) return false;
    void *buffer=vm->slots;
    if (!grow(vm,&buffer,&vm->slot_capacity,base+fn->value_count,sizeof *vm->slots)) return false;
    vm->slots=buffer;
    for (size_t i=0; i<fn->parameter_count; ++i)
        vm->slots[base+fn->parameters[i].value-1]=direct!=NULL ? direct[i] : read_slot(vm,caller,ids[i]);
    for (size_t i=0; i<captures.count; ++i)
        vm->slots[base+fn->captures[i].value-1]=captures.items[i];
    if (!push(vm,(Frame){.kind=BLOCK,.function=id,.base=base,.destination=destination,.invocation=origin,
                         .as.block={tf_core_block(fn,fn->body),0,true}})) return false;
    vm->slot_count=base+fn->value_count;
    ++vm->call_depth;
    return true;
}

static bool child(Vm *vm, const Frame *frame, TfCoreBlockId block, size_t destination) {
    const TfCoreFunction *fn=tf_core_function(vm->data,frame->function);
    return push(vm,(Frame){.kind=BLOCK,.function=frame->function,.base=frame->base,.destination=destination,
                           .invocation=frame->invocation,.as.block={tf_core_block(fn,block),0,false}});
}

static void deliver(Vm *vm, size_t destination, TfExecValue value) {
    if (destination==HOST_DESTINATION) { vm->complete=true; vm->result=value; }
    else if (destination==MAP_DESTINATION) {
        Frame *map=&vm->frames[vm->frame_count-1];
        if (map->kind==BUILTIN_FOLD) map->as.map.accumulator=value;
        else map->as.map.output[map->as.map.next]=value;
        ++map->as.map.next;
    } else vm->slots[destination]=value;
}

static TfExecStatus aggregate(Vm *vm, const Frame *frame, TfCoreValueIds ids, TfCoreTypeId type,
                               size_t identity, TfExecValue *out, TfRuntimeCode *code) {
    if (!budget(vm,(uint64_t)ids.count)) { *code=TF_RUNTIME_RESOURCE; return TF_EXEC_FAULT; }
    TfExecValue *values=tf_exec_allocate(&vm->arena,ids.count,sizeof *values);
    if (ids.count!=0 && values==NULL) { *code=TF_RUNTIME_RESOURCE; return TF_EXEC_FAULT; }
    for (size_t i=0; i<ids.count; ++i) values[i]=read_slot(vm,frame->base,ids.items[i]);
    *out=(TfExecValue){.type=type,.as.aggregate={identity,{values,ids.count}}};
    return TF_EXEC_OK;
}

static TfStatus evaluate(Vm *vm) {
    while (vm->frame_count!=0) {
        const size_t top=vm->frame_count-1;
        const Frame frame=vm->frames[top]; /* no borrowed pointers into growable stacks */
        if (frame.kind!=BLOCK) {
            const size_t i=frame.as.map.next;
            if (i==frame.as.map.input.count) {
                --vm->frame_count;
                const TfExecValue result=frame.kind==BUILTIN_FOLD ? frame.as.map.accumulator :
                    (TfExecValue){.type=frame.as.map.result,.as.aggregate={0,{frame.as.map.output,i}}};
                deliver(vm,frame.destination,result);
                continue;
            }
            if (!budget(vm,1)) return fault(vm,TF_RUNTIME_RESOURCE,frame.as.map.span);
            if (frame.kind==MAP) {
                vm->slots[frame.base+frame.as.map.element-1]=frame.as.map.input.items[i];
                if (!child(vm,&frame,frame.as.map.body,MAP_DESTINATION))
                    return fault(vm,TF_RUNTIME_RESOURCE,frame.as.map.span);
            } else {
                const TfExecValue closure=frame.as.map.closure;
                const TfExecValue item=frame.as.map.input.items[i];
                const TfExecValue args[]={frame.as.map.accumulator,item};
                if (!call(vm,closure.as.aggregate.identity,closure.as.aggregate.values,
                          frame.kind==BUILTIN_FOLD ? args : &item,NULL,0,MAP_DESTINATION,frame.as.map.span))
                    return fault(vm,TF_RUNTIME_RESOURCE,frame.as.map.span);
            }
            continue;
        }
        const TfCoreBlock *b=frame.as.block.body;
        const bool wrapper=tf_core_function(vm->data,frame.function)->kind==TF_CORE_FUNCTION_PRELUDE_WRAPPER;
        if (frame.as.block.next==b->let_count) {
            const TfCoreTerminator term=b->terminator;
            const TfCoreSpanId span=wrapper ? frame.invocation : term.span;
            if (!budget(vm,1)) return fault(vm,TF_RUNTIME_RESOURCE,span);
            if (term.kind!=TF_CORE_YIELD) return fault(vm,TF_RUNTIME_INVARIANT,span);
            const TfExecValue value=read_slot(vm,frame.base,term.value);
            --vm->frame_count;
            if (frame.as.block.root) { --vm->call_depth; vm->slot_count=frame.base; }
            deliver(vm,frame.destination,value);
            continue;
        }
        const TfCoreLet *let=&b->lets[frame.as.block.next];
        const TfCoreOperation *op=&let->operation;
        const TfCoreSpanId span=wrapper ? frame.invocation : let->span;
        const size_t destination=frame.base+let->value-1;
        const TfExecValue a=op->operands.count!=0 ? read_slot(vm,frame.base,op->operands.items[0]) : (TfExecValue){0};
        TfExecValue value={.type=let->type};
        TfRuntimeCode code=TF_RUNTIME_NONE;
        TfExecStatus status=TF_EXEC_OK;
        if (!budget(vm,1)) return fault(vm,TF_RUNTIME_RESOURCE,span);
        ++vm->frames[top].as.block.next;
        switch (op->kind) {
        case TF_CORE_OP_CONST: value=vm->constants[op->as.constant-1]; break;
        case TF_CORE_OP_UNARY: status=tf_exec_unary(vm->data,op->as.unary,a,let->type,&value,&code); break;
        case TF_CORE_OP_BINARY:
            status=tf_exec_binary(&vm->arena,vm->data,op->as.binary,a,read_slot(vm,frame.base,op->operands.items[1]),
                                   let->type,&value,&code,&vm->steps);
            break;
        case TF_CORE_OP_RECORD: case TF_CORE_OP_MACHINE_VALUE: case TF_CORE_OP_LIST:
            status=aggregate(vm,&frame,op->operands,let->type,0,&value,&code); break;
        case TF_CORE_OP_VARIANT:
            status=aggregate(vm,&frame,op->operands,let->type,op->as.variant.tag,&value,&code); break;
        case TF_CORE_OP_CLOSURE:
            status=aggregate(vm,&frame,op->operands,let->type,op->as.function,&value,&code); break;
        case TF_CORE_OP_PROJECT: value=a.as.aggregate.values.items[op->as.field]; break;
        case TF_CORE_OP_TAG: value.as.tag=a.as.aggregate.identity; break;
        case TF_CORE_OP_PAYLOAD: value=a.as.aggregate.values.items[op->as.payload.index]; break;
        case TF_CORE_OP_IF:
            if (!child(vm,&frame,a.as.bits!=0 ? op->as.branch.then_block : op->as.branch.else_block,destination))
                return fault(vm,TF_RUNTIME_RESOURCE,span);
            continue;
        case TF_CORE_OP_SWITCH_TAG: {
            TfCoreBlockId selected=op->as.switch_tag.default_block;
            for (size_t i=0; i<op->as.switch_tag.case_count; ++i) {
                if (!budget(vm,1)) return fault(vm,TF_RUNTIME_RESOURCE,span);
                if (op->as.switch_tag.cases[i].tag==a.as.aggregate.identity) {
                    selected=op->as.switch_tag.cases[i].block; break;
                }
            }
            if (selected==0) return fault(vm,TF_RUNTIME_INVARIANT,span);
            if (!child(vm,&frame,selected,destination)) return fault(vm,TF_RUNTIME_RESOURCE,span);
            continue;
        }
        case TF_CORE_OP_CALL:
            if (!call(vm,op->as.function,(TfExecValues){0},NULL,op->operands.items,frame.base,destination,span))
                return fault(vm,TF_RUNTIME_RESOURCE,span);
            continue;
        case TF_CORE_OP_CALL_CLOSURE:
            if (!call(vm,a.as.aggregate.identity,a.as.aggregate.values,NULL,op->operands.items+1,frame.base,destination,span))
                return fault(vm,TF_RUNTIME_RESOURCE,span);
            continue;
        case TF_CORE_OP_LIST_MAP: {
            const TfExecValues input=a.as.aggregate.values;
            TfExecValue *output=tf_exec_allocate(&vm->arena,input.count,sizeof *output);
            if ((input.count!=0 && output==NULL) || !push(vm,(Frame){.kind=MAP,.function=frame.function,
                .base=frame.base,.destination=destination,.invocation=frame.invocation,
                .as.map={.input=input,.output=output,.result=let->type,.body=op->as.list_map.body,
                         .element=op->as.list_map.element.value,.span=span}}))
                return fault(vm,TF_RUNTIME_RESOURCE,span);
            continue;
        }
        case TF_CORE_OP_INPUT_AT:
            if (op->as.input_index>=a.as.aggregate.values.count) return fault(vm,TF_RUNTIME_INSUFFICIENT_INPUT,span);
            value=a.as.aggregate.values.items[op->as.input_index]; break;
        case TF_CORE_OP_BUILTIN: {
            const TfCoreBuiltin kind=op->as.builtin.id;
            TfExecValue args[3];
            if (op->operands.count>sizeof args/sizeof *args) return fault(vm,TF_RUNTIME_INVARIANT,span);
            for (size_t i=0; i<op->operands.count; ++i) args[i]=read_slot(vm,frame.base,op->operands.items[i]);
            if (kind==TF_CORE_BUILTIN_MAP || kind==TF_CORE_BUILTIN_FOLD) {
                const bool fold=kind==TF_CORE_BUILTIN_FOLD;
                const TfExecValues input=a.as.aggregate.values;
                TfExecValue *output=fold ? NULL : tf_exec_allocate(&vm->arena,input.count,sizeof *output);
                if ((!fold && input.count!=0 && output==NULL) || !push(vm,(Frame){
                    .kind=fold ? BUILTIN_FOLD : BUILTIN_MAP,.function=frame.function,
                    .base=frame.base,.destination=destination,.invocation=frame.invocation,
                    .as.map={.input=input,.output=output,.result=let->type,.span=span,
                             .closure=args[fold ? 2 : 1],.accumulator=fold ? args[1] : (TfExecValue){0}}
                })) return fault(vm,TF_RUNTIME_RESOURCE,span);
                continue;
            }
            status=tf_exec_builtin(&vm->arena,vm->data,kind,let->type,args,&value,&code,&vm->steps);
            break;
        }
        }
        if (status==TF_EXEC_FAULT) return fault(vm,code,span);
        if (status==TF_EXEC_HOST) return TF_HOST_ERROR;
        vm->slots[destination]=value;
    }
    return vm->complete ? TF_OK : fault(vm,TF_RUNTIME_INVARIANT,0);
}

static bool valid_text(TfCoreText text) {
    if (text.length!=0 && text.bytes==NULL) return false;
    for (size_t i=0,next; i<text.length; i=next) {
        uint32_t scalar;
        if (!tf_decode(text.bytes,text.length,i,&scalar,&next)) return false;
    }
    return true;
}

static bool result_copy(Vm *vm, TfInterpreterResult **out) {
    const TfExecValues values=vm->result.as.aggregate.values;
    if (values.count>(SIZE_MAX-sizeof(TfInterpreterResult))/sizeof(TfCoreText)) return false;
    size_t size=sizeof(TfInterpreterResult)+values.count*sizeof(TfCoreText);
    for (size_t i=0; i<values.count; ++i) {
        const size_t length=values.items[i].as.text.length;
        if (length>SIZE_MAX-size) return false;
        size+=length;
    }
    if (size>vm->arena.remaining) return false;
    TfInterpreterResult *result=vm->arena.allocator.allocate(vm->arena.allocator.context,size);
    if (result==NULL) return false;
    result->allocator=vm->arena.allocator; result->count=values.count;
    unsigned char *bytes=(unsigned char *)(result->lines+values.count);
    for (size_t i=0; i<values.count; ++i) {
        const TfCoreText line=values.items[i].as.text;
        if (line.length!=0) memcpy(bytes,line.bytes,line.length);
        result->lines[i]=(TfCoreText){bytes,line.length};
        bytes+=line.length;
    }
    *out=result;
    return true;
}

TfStatus tf_interpret_entry(TfInterpreterResult **out, const TfVerifiedCore *program,
                           const TfCoreText *lines, size_t line_count,
                           const TfInterpreterLimits *limits, const TfAllocator *allocator,
                           TfRuntimeFault *reported) {
    if (reported!=NULL) *reported=(TfRuntimeFault){0};
    if (out==NULL) return TF_HOST_ERROR;
    *out=NULL;
    const TfInterpreterLimits cap=limits!=NULL ? *limits : tf_interpreter_default_limits();
    Vm vm={.arena={.allocator=tf_allocator(allocator),.remaining=cap.max_bytes},
        .data=tf_verified_core_data(program),.call_limit=cap.max_call_depth,.steps=cap.max_steps,.fault=reported};
    if (vm.data==NULL || vm.data->entry==0 || !tf_allocator_valid(vm.arena.allocator)) return TF_HOST_ERROR;
    const TfCoreFunction *entry=tf_core_function(vm.data,vm.data->entry);
    TfExecValue input={0};
    TfStatus status=TF_HOST_ERROR;
    if (entry->kind==TF_CORE_FUNCTION_ENTRY_ONLY) {
        if (line_count!=0 && lines==NULL) goto done;
        TfExecValue *values=tf_exec_allocate(&vm.arena,line_count,sizeof *values);
        if (line_count!=0 && values==NULL) { status=fault(&vm,TF_RUNTIME_RESOURCE,entry->span); goto done; }
        const TfCoreTypeId list=entry->parameters[0].type;
        const TfCoreTypeId text=tf_core_type(vm.data,list)->as.element;
        for (size_t i=0; i<line_count; ++i) {
            if (!valid_text(lines[i])) goto done;
            values[i]=(TfExecValue){.type=text,.as.text=lines[i]};
        }
        input=(TfExecValue){.type=list,.as.aggregate={0,{values,line_count}}};
    }
    if (!tf_exec_constants(&vm.arena,vm.data,&vm.constants) ||
        !call(&vm,vm.data->entry,(TfExecValues){0},&input,NULL,0,HOST_DESTINATION,0)) {
        status=fault(&vm,TF_RUNTIME_RESOURCE,entry->span); goto done;
    }
    status=evaluate(&vm);
    if (status==TF_OK && !result_copy(&vm,out)) status=fault(&vm,TF_RUNTIME_RESOURCE,entry->span);
done:
    if (vm.frames!=NULL) vm.arena.allocator.deallocate(vm.arena.allocator.context,vm.frames);
    if (vm.slots!=NULL) vm.arena.allocator.deallocate(vm.arena.allocator.context,vm.slots);
    tf_exec_arena_destroy(&vm.arena);
    return status;
}

size_t tf_interpreter_result_count(const TfInterpreterResult *result) { return result!=NULL ? result->count : 0; }

bool tf_interpreter_result_line(const TfInterpreterResult *result, size_t index, TfCoreText *line) {
    if (result==NULL || index>=result->count || line==NULL) return false;
    *line=result->lines[index]; return true;
}

void tf_interpreter_result_destroy(TfInterpreterResult *result) {
    if (result!=NULL) result->allocator.deallocate(result->allocator.context,result);
}
