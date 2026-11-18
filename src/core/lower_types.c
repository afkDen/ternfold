#include "lower_types.h"
#include "../internal.h"

#include <string.h>

typedef union LowerChunk LowerChunk;
union LowerChunk { LowerChunk *next; max_align_t alignment; };
typedef struct { TfCoreTypeId list, tag, decision; } TypeIndex;
struct TfCoreLowerTypes {
    const TfTypedProgram *typed;
    const TfTypeStore *source;
    TfAllocator allocator;
    LowerChunk *chunks;
    TfCoreType *types;
    TfSpan *spans;
    TypeIndex *indexes;
    size_t source_count, type_count, type_capacity, span_count, span_capacity;
    size_t indexed_count, index_capacity;
};

static void *reserve(TfCoreLowerTypes *t, size_t count, size_t width) {
    LowerChunk *chunk;
    if (count == 0 || width == 0 || count > (SIZE_MAX-sizeof *chunk)/width) return NULL;
    chunk = t->allocator.allocate(t->allocator.context,sizeof *chunk+count*width);
    if (chunk == NULL) return NULL;
    chunk->next = t->chunks;
    t->chunks = chunk;
    return chunk+1;
}

static TfCoreText name(TfAstText text) { return (TfCoreText){text.data,text.length}; }

TfCoreText tf_core_lower_module_name(const TfResolvedModule *module) {
    if (module == NULL || module->relative_path == NULL) return (TfCoreText){0};
    const unsigned char *bytes = (const unsigned char *)module->relative_path;
    const size_t length = strlen(module->relative_path);
    for (size_t at=0, next; at<length; at=next) {
        uint32_t scalar;
        if (!tf_decode(bytes,length,at,&scalar,&next)) return (TfCoreText){0};
    }
    return (TfCoreText){bytes,length};
}

TfStatus tf_core_lower_span(TfCoreLowerTypes *t, TfSpan span, TfCoreSpanId *out) {
    void *buffer;
    if (out == NULL) return TF_HOST_ERROR;
    *out = 0;
    if (t == NULL) return TF_HOST_ERROR;
    if (span.source == NULL) return TF_OK;
    buffer = t->spans;
    if (t->span_count == SIZE_MAX ||
        !tf_grow(&buffer,&t->span_capacity,t->span_count+1,sizeof *t->spans,t->allocator))
        return TF_HOST_ERROR;
    t->spans = buffer;
    t->spans[t->span_count++] = span;
    *out = t->span_count;
    return TF_OK;
}

TfCoreTypeId tf_core_lower_type_id(const TfCoreLowerTypes *t, TfConcreteTypeId id) {
    return t != NULL && id.owner == t->source && id.index <= t->source_count ? id.index : 0;
}

TfCoreTypeId tf_core_lower_machine_id(const TfCoreLowerTypes *t, const TfMachinePlan *plan) {
    if (t == NULL || plan == NULL) return 0;
    const TfTypedNode *decl = tf_typed_program_node(t->typed,plan->declaration);
    if (decl == NULL) return 0;
    /* The declaration's type is its constructor signature, () -> Machine. */
    const TfConcreteType *ctor = tf_type_store_get(t->source,decl->type);
    if (ctor == NULL || ctor->kind != TF_CONCRETE_FUNCTION) return 0;
    const TfConcreteTypeId result = ctor->as.function.result;
    const TfConcreteType *machine = tf_type_store_get(t->source,result);
    if (machine == NULL || machine->kind != TF_CONCRETE_MACHINE ||
        machine->as.nominal.owner != plan->type.owner ||
        machine->as.nominal.module_index != plan->type.module_index ||
        machine->as.nominal.declaration_index != plan->type.declaration_index) return 0;
    return tf_core_lower_type_id(t,result);
}

static TfCoreTypeKind kind_of(TfConcreteTypeKind kind) {
    switch (kind) {
    case TF_CONCRETE_INT: return TF_CORE_TYPE_INT;
    case TF_CONCRETE_FLOAT: return TF_CORE_TYPE_FLOAT;
    case TF_CONCRETE_BOOL: return TF_CORE_TYPE_BOOL;
    case TF_CONCRETE_TEXT: return TF_CORE_TYPE_TEXT;
    case TF_CONCRETE_CHAR: return TF_CORE_TYPE_CHAR;
    case TF_CONCRETE_UNIT: return TF_CORE_TYPE_UNIT;
    case TF_CONCRETE_LIST: return TF_CORE_TYPE_LIST;
    case TF_CONCRETE_OPTION: return TF_CORE_TYPE_OPTION;
    case TF_CONCRETE_DECISION: return TF_CORE_TYPE_DECISION;
    case TF_CONCRETE_RECORD: return TF_CORE_TYPE_RECORD;
    case TF_CONCRETE_ENUM: return TF_CORE_TYPE_ENUM;
    case TF_CONCRETE_MACHINE: return TF_CORE_TYPE_MACHINE;
    case TF_CONCRETE_FUNCTION: return TF_CORE_TYPE_FUNCTION;
    }
    return 0;
}

static bool ensure_types(TfCoreLowerTypes *t, size_t extra) {
    void *buffer = t->types;
    if (extra > SIZE_MAX-t->type_count ||
        !tf_grow(&buffer,&t->type_capacity,t->type_count+extra,sizeof *t->types,t->allocator))
        return false;
    t->types = buffer;
    return true;
}

static TfCoreTypeId push_type(TfCoreLowerTypes *t, TfCoreType type) {
    t->types[t->type_count++] = type;
    return t->type_count;
}

static TfCoreTypeId push_list(TfCoreLowerTypes *t, TfCoreTypeId element) {
    const TfCoreTypeId id = push_type(t,(TfCoreType){.kind=TF_CORE_TYPE_LIST,.as.element=element});
    t->indexes[element-1].list = id;
    return id;
}

static bool ensure_index(TfCoreLowerTypes *t, size_t count) {
    void *buffer = t->indexes;
    if (count <= t->indexed_count) return true;
    if (!tf_grow(&buffer,&t->index_capacity,count,sizeof *t->indexes,t->allocator)) return false;
    t->indexes = buffer;
    for (size_t i=t->indexed_count; i<count; ++i) t->indexes[i] = (TypeIndex){0};
    t->indexed_count = count;
    return true;
}

TfStatus tf_core_lower_list(TfCoreLowerTypes *t, TfCoreTypeId element, TfCoreTypeId *out) {
    if (out == NULL) return TF_HOST_ERROR;
    *out = 0;
    if (t == NULL || element == 0 || element > t->type_count ||
        t->types[element-1].kind == TF_CORE_TYPE_TAG || !ensure_index(t,element)) return TF_HOST_ERROR;
    *out = t->indexes[element-1].list;
    if (*out != 0) return TF_OK;
    if (!ensure_types(t,1)) return TF_HOST_ERROR;
    *out = push_list(t,element);
    return TF_OK;
}

TfStatus tf_core_lower_tag(TfCoreLowerTypes *t, TfCoreTypeId tagged, TfCoreTypeId *out) {
    TfCoreTypeKind kind;
    if (out == NULL) return TF_HOST_ERROR;
    *out = 0;
    if (t == NULL || tagged == 0 || tagged > t->type_count) return TF_HOST_ERROR;
    kind = t->types[tagged-1].kind;
    if (kind != TF_CORE_TYPE_ENUM && kind != TF_CORE_TYPE_OPTION && kind != TF_CORE_TYPE_DECISION)
        return TF_HOST_ERROR;
    if (!ensure_index(t,tagged)) return TF_HOST_ERROR;
    *out = t->indexes[tagged-1].tag;
    if (*out != 0) return TF_OK;
    if (!ensure_types(t,1)) return TF_HOST_ERROR;
    *out = push_type(t,(TfCoreType){.kind=TF_CORE_TYPE_TAG,.as.tagged=tagged});
    t->indexes[tagged-1].tag = *out;
    return TF_OK;
}

TfStatus tf_core_lower_decision(TfCoreLowerTypes *t, TfCoreTypeId machine, TfCoreTypeId *out) {
    if (out == NULL) return TF_HOST_ERROR;
    *out = 0;
    if (t == NULL || machine == 0 || machine > t->source_count ||
        t->types[machine-1].kind != TF_CORE_TYPE_MACHINE) return TF_HOST_ERROR;
    *out = t->indexes[machine-1].decision;
    if (*out != 0) return TF_OK;

    /* A valid machine fixes O and E. Both are source IDs, so their indexes
     * already exist. Copy IDs before growth can move the type table. */
    const TfCoreTypeId output = t->types[machine-1].as.machine.output;
    const TfCoreTypeId rejection = t->types[machine-1].as.machine.rejection;
    TfCoreTypeId emissions = t->indexes[output-1].list;
    if (!ensure_types(t,emissions == 0 ? 2 : 1)) return TF_HOST_ERROR;
    if (emissions == 0) emissions = push_list(t,output);
    *out = push_type(t,(TfCoreType){.kind=TF_CORE_TYPE_DECISION,
                                  .as.decision={machine,output,rejection,emissions}});
    t->indexes[machine-1].decision = *out;
    return TF_OK;
}

static TfCoreTypeId node_type(const TfCoreLowerTypes *t, TfNodeId node) {
    const TfTypedNode *typed = tf_typed_program_node(t->typed,node);
    return typed != NULL ? tf_core_lower_type_id(t,typed->type) : 0;
}

static bool field(TfCoreLowerTypes *t, TfCoreField *out, TfAstText label, TfCoreTypeId id) {
    if (id == 0) return false;
    *out = (TfCoreField){.name=name(label),.type=id};
    return tf_core_lower_span(t,label.span,&out->span) == TF_OK;
}

static bool nominal(TfCoreLowerTypes *t, const TfMachineProgram *p, const TfConcreteType *source,
                    TfCoreType *out) {
    const TfResolution *r = tf_typed_program_resolution(t->typed);
    const TfTypeId id = source->as.nominal;
    const TfResolvedType *decl = tf_resolution_type(r,id);
    const TfAstNode *syntax;
    if (decl == NULL || (syntax=tf_ast_get(decl->declaration.owner,decl->declaration)) == NULL ||
        (out->kind == TF_CORE_TYPE_MACHINE && syntax->kind != TF_AST_MACHINE) ||
        (out->kind == TF_CORE_TYPE_RECORD && syntax->kind != TF_AST_RECORD) ||
        (out->kind == TF_CORE_TYPE_ENUM && syntax->kind != TF_AST_ENUM))
        return false;
    out->name = name(decl->name);
    out->module = tf_core_lower_module_name(tf_resolution_module(r,(TfModuleId){r,id.module_index}));
    if (tf_core_lower_span(t,syntax->span,&out->span) != TF_OK) return false;
    if (out->kind == TF_CORE_TYPE_MACHINE) {
        const TfMachinePlan *plan = tf_machine_program_get(p,id);
        if (plan == NULL) return false;
        TfCoreField *cells = reserve(t,plan->cell_count,sizeof *cells);
        if (cells == NULL) return false;
        for (size_t i=0; i<plan->cell_count; ++i)
            if (!field(t,&cells[i],plan->cells[i].name,tf_core_lower_type_id(t,plan->cells[i].type)))
                return false;
        out->as.machine.cells = cells;
        out->as.machine.cell_count = plan->cell_count;
        out->as.machine.input = node_type(t,syntax->as.machine.parameter);
        out->as.machine.output = node_type(t,syntax->as.machine.yields);
        out->as.machine.rejection = node_type(t,syntax->as.machine.rejects);
        return out->as.machine.input && out->as.machine.output && out->as.machine.rejection;
    }
    const TfNodeList members = syntax->as.aggregate.members;
    if (out->kind == TF_CORE_TYPE_RECORD) {
        TfCoreField *fields = reserve(t,members.count,sizeof *fields);
        if (members.count != 0 && fields == NULL) return false;
        for (size_t i=0; i<members.count; ++i) {
            const TfAstNode *member = tf_ast_get(syntax->id.owner,members.items[i]);
            if (member == NULL || !field(t,&fields[i],member->as.parameter.name,node_type(t,member->id)))
                return false;
        }
        out->as.record.fields = fields;
        out->as.record.field_count = members.count;
        return true;
    }
    TfCoreVariant *variants = reserve(t,members.count,sizeof *variants);
    if (variants == NULL) return false;
    for (size_t i=0; i<members.count; ++i) {
        const TfAstNode *member = tf_ast_get(syntax->id.owner,members.items[i]);
        if (member == NULL) return false;
        const TfNodeList payload = member->as.variant.types;
        TfCoreTypeId *ids = reserve(t,payload.count,sizeof *ids);
        if (payload.count != 0 && ids == NULL) return false;
        for (size_t j=0; j<payload.count; ++j)
            if ((ids[j]=node_type(t,payload.items[j])) == 0) return false;
        variants[i] = (TfCoreVariant){.name=name(member->as.variant.name),.payloads={ids,payload.count}};
        if (tf_core_lower_span(t,member->span,&variants[i].span) != TF_OK) return false;
    }
    out->as.enumeration.variants = variants;
    out->as.enumeration.variant_count = members.count;
    return true;
}

static bool translate(TfCoreLowerTypes *t, const TfMachineProgram *p, size_t i) {
    const TfConcreteType *source = tf_type_store_at(t->source,i);
    TfCoreType out = t->types[i];
    switch (out.kind) {
    case TF_CORE_TYPE_RECORD: case TF_CORE_TYPE_ENUM: case TF_CORE_TYPE_MACHINE:
        if (!nominal(t,p,source,&out)) return false;
        break;
    case TF_CORE_TYPE_DECISION:
        out.as.decision.machine = tf_core_lower_type_id(t,source->as.decision.machine);
        out.as.decision.output = tf_core_lower_type_id(t,source->as.decision.output);
        out.as.decision.rejection = tf_core_lower_type_id(t,source->as.decision.rejection);
        if (out.as.decision.machine == 0 || out.as.decision.rejection == 0 ||
            t->types[out.as.decision.machine-1].kind != TF_CORE_TYPE_MACHINE ||
            t->indexes[out.as.decision.machine-1].decision != 0 ||
            tf_core_lower_list(t,out.as.decision.output,&out.as.decision.emissions) != TF_OK) return false;
        t->indexes[out.as.decision.machine-1].decision = i+1;
        break;
    case TF_CORE_TYPE_FUNCTION: {
        const size_t count = source->as.function.arity;
        TfCoreTypeId *ids = reserve(t,count,sizeof *ids);
        if (count != 0 && ids == NULL) return false;
        for (size_t j=0; j<count; ++j)
            if ((ids[j]=tf_core_lower_type_id(t,source->as.function.parameters[j])) == 0) return false;
        out.as.function.parameters = (TfCoreTypeIds){ids,count};
        out.as.function.result = tf_core_lower_type_id(t,source->as.function.result);
        if (out.as.function.result == 0) return false;
        break;
    }
    default: break;
    }
    t->types[i] = out;
    return true;
}

TfStatus tf_core_lower_types_create(TfCoreLowerTypes **out, const TfMachineProgram *p,
                                    const TfAllocator *allocator) {
    const TfAllocator a = tf_allocator(allocator);
    TfCoreLowerTypes *t;
    void *buffer = NULL;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (!tf_allocator_valid(a) || tf_diagnostic_phase_kind(tf_machine_program_phase(p)) != TF_DIAG_PHASE_VALID ||
        tf_diagnostic_phase_payload(tf_machine_program_phase(p)) != p) return TF_HOST_ERROR;
    t = a.allocate(a.context,sizeof *t);
    if (t == NULL) return TF_HOST_ERROR;
    *t = (TfCoreLowerTypes){.allocator=a,.typed=tf_machine_program_typed(p)};
    t->source = tf_typed_program_store(t->typed);
    t->type_count = t->source_count = tf_type_store_count(t->source);
    if (t->source_count != 0 &&
        !tf_grow(&buffer,&t->type_capacity,t->source_count,sizeof *t->types,t->allocator)) goto fail;
    t->types = buffer;
    if (!ensure_index(t,t->source_count)) goto fail;
    /* Seed every original ID and List element before a Decision looks for
     * an existing List[O]. Synthesized IDs never shift source ordinals. */
    for (size_t i=0; i<t->source_count; ++i) {
        const TfConcreteType *type = tf_type_store_at(t->source,i);
        t->types[i] = (TfCoreType){.kind=kind_of(type->kind)};
        if (t->types[i].kind == 0) goto fail;
        if (type->kind == TF_CONCRETE_LIST || type->kind == TF_CONCRETE_OPTION) {
            t->types[i].as.element = tf_core_lower_type_id(t,type->as.element);
            if (t->types[i].as.element == 0) goto fail;
            if (type->kind == TF_CONCRETE_LIST)
                t->indexes[t->types[i].as.element-1].list = i+1;
        }
    }
    for (size_t i=0; i<t->source_count; ++i) if (!translate(t,p,i)) goto fail;
    *out = t;
    return TF_OK;
fail:
    tf_core_lower_types_destroy(t);
    return TF_HOST_ERROR;
}

void tf_core_lower_types_destroy(TfCoreLowerTypes *t) {
    if (t == NULL) return;
    const TfAllocator a = t->allocator;
    while (t->chunks != NULL) {
        LowerChunk *next = t->chunks->next;
        a.deallocate(a.context,t->chunks);
        t->chunks = next;
    }
    if (t->types != NULL) a.deallocate(a.context,t->types);
    if (t->spans != NULL) a.deallocate(a.context,t->spans);
    if (t->indexes != NULL) a.deallocate(a.context,t->indexes);
    a.deallocate(a.context,t);
}

const TfTypedProgram *tf_core_lower_types_program(const TfCoreLowerTypes *t) {
    return t != NULL ? t->typed : NULL;
}

void tf_core_lower_type_view(const TfCoreLowerTypes *t, TfCoreProgramData *data) {
    if (data == NULL) return;
    data->types = t != NULL ? t->types : NULL;
    data->type_count = t != NULL ? t->type_count : 0;
    data->spans = t != NULL ? t->spans : NULL;
    data->span_count = t != NULL ? t->span_count : 0;
}
