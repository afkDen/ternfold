#include "values.h"

#include <stddef.h>
#include <string.h>

struct TfExecChunk { TfExecChunk *next; max_align_t alignment; };

void *tf_exec_allocate(TfExecArena *arena, size_t count, size_t width) {
    if (count==0 || width==0 || count>(SIZE_MAX-sizeof(TfExecChunk))/width) return NULL;
    const size_t bytes=sizeof(TfExecChunk)+count*width;
    if (bytes>arena->remaining) return NULL;
    TfExecChunk *chunk=arena->allocator.allocate(arena->allocator.context,bytes);
    if (chunk==NULL) return NULL;
    chunk->next=arena->chunks;
    arena->chunks=chunk;
    arena->remaining-=bytes;
    return chunk+1;
}

void tf_exec_arena_destroy(TfExecArena *arena) {
    while (arena->chunks!=NULL) {
        TfExecChunk *chunk=arena->chunks;
        arena->chunks=chunk->next;
        arena->allocator.deallocate(arena->allocator.context,chunk);
    }
}

bool tf_exec_copy_text(TfExecArena *arena, TfCoreText text, TfCoreText *out) {
    unsigned char *copy=tf_exec_allocate(arena,text.length,sizeof *copy);
    if (text.length!=0) {
        if (copy==NULL) return false;
        memcpy(copy,text.bytes,text.length);
    }
    *out=(TfCoreText){copy,text.length};
    return true;
}

bool tf_exec_copy_values(TfExecArena *arena, TfExecValues values, TfExecValues *out) {
    TfExecValue *copy=tf_exec_allocate(arena,values.count,sizeof *copy);
    if (values.count!=0) {
        if (copy==NULL) return false;
        memcpy(copy,values.items,values.count*sizeof *copy);
    }
    *out=(TfExecValues){copy,values.count};
    return true;
}

bool tf_exec_constants(TfExecArena *arena, const TfCoreProgramData *program,
                       const TfExecValue **out) {
    *out=NULL;
    TfExecValue *table=tf_exec_allocate(arena,program->constant_count,sizeof *table);
    if (program->constant_count!=0 && table==NULL) return false;
    for (size_t i=0; i<program->constant_count; ++i) {
        const TfCoreConstant *constant=&program->constants[i];
        TfExecValue value={.type=constant->type};
        TfCoreConstantIds child={0};
        switch (constant->kind) {
        case TF_CORE_CONSTANT_INT: value.as.integer=constant->as.integer; break;
        case TF_CORE_CONSTANT_FLOAT: value.as.bits=constant->as.float_bits; break;
        case TF_CORE_CONSTANT_BOOL: value.as.bits=constant->as.boolean; break;
        case TF_CORE_CONSTANT_CHAR: value.as.bits=constant->as.character; break;
        case TF_CORE_CONSTANT_TEXT: value.as.text=constant->as.text; break;
        case TF_CORE_CONSTANT_UNIT: break;
        case TF_CORE_CONSTANT_VARIANT:
            value.as.aggregate.identity=constant->as.variant.tag;
            value.as.aggregate.values=(TfExecValues){0};
            child=constant->as.variant.payloads;
            break;
        case TF_CORE_CONSTANT_RECORD: case TF_CORE_CONSTANT_LIST:
            value.as.aggregate.identity=0;
            value.as.aggregate.values=(TfExecValues){0};
            child=constant->as.children;
            break;
        }
        if (child.count!=0) {
            TfExecValue *items=tf_exec_allocate(arena,child.count,sizeof *items);
            if (items==NULL) return false;
            for (size_t j=0; j<child.count; ++j) items[j]=table[child.items[j]-1];
            value.as.aggregate.values=(TfExecValues){items,child.count};
        }
        table[i]=value;
    }
    *out=table;
    return true;
}
