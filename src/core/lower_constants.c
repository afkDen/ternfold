#include "lower_constants.h"
#include "../check_constants.h"
#include "../internal.h"

#include <string.h>

typedef union Owned Owned;
union Owned { Owned *next; max_align_t alignment; };
struct TfCoreLowerConstants {
    TfCoreLowerTypes *types;
    const TfTypedProgram *typed;
    TfAllocator allocator;
    Owned *owned;
    TfCoreConstant *items;
    size_t count, capacity;
};

static void release_to(TfCoreLowerConstants *c, Owned *mark) {
    while (c->owned != mark) {
        Owned *entry = c->owned;
        c->owned = entry->next;
        c->allocator.deallocate(c->allocator.context,entry);
    }
}

static void *allocate(TfCoreLowerConstants *c, size_t count, size_t width) {
    Owned *entry;
    if (count == 0 || width == 0 || count > (SIZE_MAX-sizeof *entry)/width) return NULL;
    entry = c->allocator.allocate(c->allocator.context,sizeof *entry+count*width);
    if (entry == NULL) return NULL;
    entry->next = c->owned;
    c->owned = entry;
    return entry+1;
}

TfStatus tf_core_lower_constants_create(TfCoreLowerConstants **out, TfCoreLowerTypes *types,
                                        const TfAllocator *allocator) {
    const TfAllocator a = tf_allocator(allocator);
    TfCoreLowerConstants *c;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (types == NULL || !tf_allocator_valid(a)) return TF_HOST_ERROR;
    c = a.allocate(a.context,sizeof *c);
    if (c == NULL) return TF_HOST_ERROR;
    *c = (TfCoreLowerConstants){.types=types,.typed=tf_core_lower_types_program(types),.allocator=a};
    *out = c;
    return TF_OK;
}

void tf_core_lower_constants_destroy(TfCoreLowerConstants *c) {
    if (c == NULL) return;
    const TfAllocator a = c->allocator;
    release_to(c,NULL);
    if (c->items != NULL) a.deallocate(a.context,c->items);
    a.deallocate(a.context,c);
}

static bool copy_ids(TfCoreLowerConstants *c, TfCoreConstantIds *ids) {
    if (ids->count == 0) { ids->items = NULL; return true; }
    if (ids->items == NULL || ids->count > (SIZE_MAX-sizeof(Owned))/sizeof *ids->items) return false;
    for (size_t i=0; i<ids->count; ++i)
        if (ids->items[i] == 0 || ids->items[i] > c->count) return false;
    TfCoreConstantId *copy = allocate(c,ids->count,sizeof *copy);
    if (copy == NULL) return false;
    memcpy(copy,ids->items,ids->count*sizeof *copy);
    ids->items = copy;
    return true;
}

static TfStatus insert(TfCoreLowerConstants *c, TfCoreConstant value, TfCoreConstantId *out) {
    void *items = c->items;
    if (c->count == SIZE_MAX ||
        !tf_grow(&items,&c->capacity,c->count+1,sizeof *c->items,c->allocator)) return TF_HOST_ERROR;
    c->items = items;
    c->items[c->count++] = value;
    *out = c->count;
    return TF_OK;
}

TfStatus tf_core_lower_constant_append(TfCoreLowerConstants *c, TfCoreConstant value,
                                       TfCoreConstantId *out) {
    Owned *mark;
    if (out == NULL) return TF_HOST_ERROR;
    *out = 0;
    if (c == NULL) return TF_HOST_ERROR;
    mark = c->owned;
    switch (value.kind) {
    case TF_CORE_CONSTANT_TEXT:
        if (value.as.text.length == 0) value.as.text.bytes = NULL;
        else {
            if (value.as.text.bytes == NULL) goto fail;
            unsigned char *copy = allocate(c,value.as.text.length,1);
            if (copy == NULL) goto fail;
            memcpy(copy,value.as.text.bytes,value.as.text.length);
            value.as.text.bytes = copy;
        }
        break;
    case TF_CORE_CONSTANT_LIST: case TF_CORE_CONSTANT_RECORD:
        if (!copy_ids(c,&value.as.children)) goto fail;
        break;
    case TF_CORE_CONSTANT_VARIANT:
        if (!copy_ids(c,&value.as.variant.payloads)) goto fail;
        break;
    case TF_CORE_CONSTANT_INT: case TF_CORE_CONSTANT_FLOAT: case TF_CORE_CONSTANT_BOOL:
    case TF_CORE_CONSTANT_CHAR: case TF_CORE_CONSTANT_UNIT: break;
    default: goto fail;
    }
    if (insert(c,value,out) == TF_OK) return TF_OK;
fail:
    release_to(c,mark);
    return TF_HOST_ERROR;
}

static bool spells(TfAstText name, const char *text) {
    const size_t n = strlen(text);
    return name.length == n && memcmp(name.data,text,n) == 0;
}

static bool variant_tag(const TfCoreLowerConstants *c, const TfConstantValue *value, size_t *tag) {
    const TfResolution *r = tf_typed_program_resolution(c->typed);
    const TfResolvedSymbol *v = tf_resolution_symbol(r,value->variant);
    if (v == NULL || v->kind != TF_SYMBOL_VARIANT) return false;
    if (value->kind == TF_CONCRETE_OPTION) {
        if (spells(v->name,"Some")) { *tag=TF_CORE_OPTION_SOME; return true; }
        if (spells(v->name,"None")) { *tag=TF_CORE_OPTION_NONE; return true; }
    } else if (value->kind == TF_CONCRETE_DECISION) {
        if (spells(v->name,"Accept")) { *tag=TF_CORE_DECISION_ACCEPT; return true; }
        if (spells(v->name,"Reject")) { *tag=TF_CORE_DECISION_REJECT; return true; }
    } else if (value->kind == TF_CONCRETE_ENUM) {
        const TfResolvedType *type = tf_resolution_type(r,v->type);
        if (type == NULL) return false;
        const TfAstNode *decl = tf_ast_get(type->declaration.owner,type->declaration);
        if (decl == NULL || decl->kind != TF_AST_ENUM) return false;
        const TfNodeList members = decl->as.aggregate.members;
        for (size_t i=0; i<members.count; ++i) {
            const TfNodeId id = members.items[i];
            if (id.owner == v->declaration.owner && id.index == v->declaration.index) {
                *tag=i; return true;
            }
        }
    }
    return false;
}

typedef struct { TfCoreLowerConstants *owner; TfCoreSpanId span; TfCoreConstantId root; } Materialize;
enum { MATERIALIZE_DEPTH = 256 };
static TfStatus materialize(Materialize *m, const TfConstantValue *value, size_t depth,
                            TfCoreConstantId *out) {
    TfCoreLowerConstants *c = m->owner;
    TfCoreConstant k = {.type=tf_core_lower_type_id(c->types,value->type),.span=m->span};
    if (k.type == 0 || depth == MATERIALIZE_DEPTH) return TF_HOST_ERROR;
    switch (value->kind) {
    case TF_CONCRETE_INT: k.kind=TF_CORE_CONSTANT_INT; k.as.integer=value->as.integer; break;
    case TF_CONCRETE_FLOAT: k.kind=TF_CORE_CONSTANT_FLOAT; k.as.float_bits=value->as.bits; break;
    case TF_CONCRETE_BOOL: k.kind=TF_CORE_CONSTANT_BOOL; k.as.boolean=value->as.boolean ? 1u : 0u; break;
    case TF_CONCRETE_CHAR: k.kind=TF_CORE_CONSTANT_CHAR; k.as.character=value->as.scalar; break;
    case TF_CONCRETE_TEXT:
        k.kind=TF_CORE_CONSTANT_TEXT;
        k.as.text=(TfCoreText){value->as.text.bytes,value->as.text.length};
        break;
    case TF_CONCRETE_UNIT: k.kind=TF_CORE_CONSTANT_UNIT; break;
    case TF_CONCRETE_LIST: case TF_CONCRETE_RECORD:
    case TF_CONCRETE_ENUM: case TF_CONCRETE_OPTION: case TF_CONCRETE_DECISION: {
        const size_t count = value->as.aggregate.count;
        TfCoreConstantId *ids = allocate(c,count,sizeof *ids);
        if (count != 0 && ids == NULL) return TF_HOST_ERROR;
        for (size_t i=0; i<count; ++i)
            if (materialize(m,value->as.aggregate.items[i],depth+1,&ids[i]) != TF_OK) return TF_HOST_ERROR;
        if (value->kind == TF_CONCRETE_LIST || value->kind == TF_CONCRETE_RECORD) {
            k.kind = value->kind == TF_CONCRETE_LIST ? TF_CORE_CONSTANT_LIST : TF_CORE_CONSTANT_RECORD;
            k.as.children = (TfCoreConstantIds){ids,count};
        } else {
            k.kind = TF_CORE_CONSTANT_VARIANT;
            if (!variant_tag(c,value,&k.as.variant.tag)) return TF_HOST_ERROR;
            k.as.variant.payloads = (TfCoreConstantIds){ids,count};
        }
        return insert(c,k,out); /* IDs already owned and children precede this parent. */
    }
    default: return TF_HOST_ERROR;
    }
    return tf_core_lower_constant_append(c,k,out);
}

static TfStatus visit(void *context, const TfConstantValue *value) {
    Materialize *m = context;
    return materialize(m,value,0,&m->root);
}

TfStatus tf_core_lower_initializer_constant(TfCoreLowerConstants *c, TfNodeId state,
                                            TfCoreConstantId *out) {
    const TfAstNode *n, *expr;
    Owned *mark;
    size_t count;
    if (out == NULL) return TF_HOST_ERROR;
    *out = 0;
    if (c == NULL || tf_typed_program_node(c->typed,state) == NULL ||
        (n=tf_ast_get(state.owner,state)) == NULL || n->kind != TF_AST_STATE ||
        (expr=tf_ast_get(state.owner,n->as.state.value)) == NULL) return TF_HOST_ERROR;
    Materialize m = {.owner=c};
    if (tf_core_lower_span(c->types,expr->span,&m.span) != TF_OK) return TF_HOST_ERROR;
    mark = c->owned;
    count = c->count;
    if (tf_visit_checked_initializer(c->typed,state,&c->allocator,visit,&m) != TF_OK) {
        c->count = count;
        release_to(c,mark);
        return TF_HOST_ERROR;
    }
    *out = m.root;
    return TF_OK;
}

void tf_core_lower_constant_view(const TfCoreLowerConstants *c, TfCoreProgramData *data) {
    if (data == NULL) return;
    data->constants = c != NULL ? c->items : NULL;
    data->constant_count = c != NULL ? c->count : 0;
}
