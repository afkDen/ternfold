#include "core_internal.h"
#include "../internal.h"

#include <string.h>

union TfCoreArenaChunk {
    max_align_t alignment;
    struct {
        TfCoreArenaChunk *next;
        size_t used, capacity;
    } arena;
};

enum { CORE_CHUNK_BYTES = 4096 };

/* Arena allocations have fixed-depth copying: semantic edges are IDs and are
 * never followed here. Even a cyclic malformed program can be freed by walking
 * only the allocation chain. */
static void *copy_array(TfCoreProgram *program, const void *input,
                        size_t count, size_t width) {
    TfCoreArenaChunk *chunk = program->chunks;
    const size_t alignment = _Alignof(TfCoreArenaChunk);
    size_t bytes, offset = 0;
    void *output;
    if (count == 0 || input == NULL || width == 0 || count > SIZE_MAX / width)
        return NULL;
    bytes = count * width;
    if (bytes > SIZE_MAX - sizeof(*chunk)) return NULL;
    if (chunk != NULL) {
        const size_t used = chunk->arena.used;
        const size_t rest = chunk->arena.capacity - used;
        const size_t padding = (alignment - used % alignment) % alignment;
        if (padding <= rest && bytes <= rest - padding) {
            offset = used + padding;
        } else {
            chunk = NULL;
        }
    }
    if (chunk == NULL) {
        const size_t capacity = bytes > CORE_CHUNK_BYTES ? bytes : CORE_CHUNK_BYTES;
        if (capacity > SIZE_MAX - sizeof(*chunk)) return NULL;
        chunk = program->allocator.allocate(program->allocator.context,
                                            sizeof(*chunk) + capacity);
        if (chunk == NULL) return NULL;
        chunk->arena.next = program->chunks;
        chunk->arena.capacity = capacity;
        program->chunks = chunk;
    }
    output = (unsigned char *)(chunk + 1) + offset;
    memcpy(output, input, bytes);
    chunk->arena.used = offset + bytes; /* fits by the capacity checks above */
    return output;
}

static bool copy_text(TfCoreProgram *program, TfCoreText *text) {
    text->bytes = copy_array(program, text->bytes, text->length, 1);
    return text->length == 0 || text->bytes != NULL;
}

static bool copy_type_ids(TfCoreProgram *program, TfCoreTypeIds *ids) {
    ids->items = copy_array(program, ids->items, ids->count, sizeof(*ids->items));
    return ids->count == 0 || ids->items != NULL;
}

static bool copy_constant_ids(TfCoreProgram *program, TfCoreConstantIds *ids) {
    ids->items = copy_array(program, ids->items, ids->count, sizeof(*ids->items));
    return ids->count == 0 || ids->items != NULL;
}

static bool copy_fields(TfCoreProgram *program, const TfCoreField **fields, size_t count) {
    TfCoreField *copy = copy_array(program, *fields, count, sizeof(*copy));
    if (count != 0 && copy == NULL) return false;
    for (size_t i = 0; i < count; ++i)
        if (!copy_text(program, &copy[i].name)) return false;
    *fields = copy;
    return true;
}

static bool copy_variants(TfCoreProgram *program, TfCoreType *type) {
    const size_t count = type->as.enumeration.variant_count;
    TfCoreVariant *copy = copy_array(program, type->as.enumeration.variants,
                                    count, sizeof(*copy));
    if (count != 0 && copy == NULL) return false;
    for (size_t i = 0; i < count; ++i)
        if (!copy_text(program, &copy[i].name) ||
            !copy_type_ids(program, &copy[i].payloads)) return false;
    type->as.enumeration.variants = copy;
    return true;
}

static bool copy_types(TfCoreProgram *program) {
    TfCoreProgramData *data = &program->data;
    TfCoreType *types = copy_array(program, data->types, data->type_count, sizeof(*types));
    if (data->type_count != 0 && types == NULL) return false;
    for (size_t i = 0; i < data->type_count; ++i) {
        TfCoreType *type = &types[i];
        if (!copy_text(program, &type->name) || !copy_text(program, &type->module))
            return false;
        switch (type->kind) {
        case TF_CORE_TYPE_RECORD:
            if (!copy_fields(program, &type->as.record.fields, type->as.record.field_count))
                return false;
            break;
        case TF_CORE_TYPE_ENUM:
            if (!copy_variants(program, type)) return false;
            break;
        case TF_CORE_TYPE_MACHINE:
            if (!copy_fields(program, &type->as.machine.cells, type->as.machine.cell_count))
                return false;
            break;
        case TF_CORE_TYPE_FUNCTION:
            if (!copy_type_ids(program, &type->as.function.parameters)) return false;
            break;
        case TF_CORE_TYPE_INT: case TF_CORE_TYPE_FLOAT: case TF_CORE_TYPE_BOOL:
        case TF_CORE_TYPE_TEXT: case TF_CORE_TYPE_CHAR: case TF_CORE_TYPE_UNIT:
        case TF_CORE_TYPE_LIST: case TF_CORE_TYPE_OPTION: case TF_CORE_TYPE_DECISION:
        case TF_CORE_TYPE_TAG:
            break;
        default:
            memset(&type->as, 0, sizeof(type->as));
            break;
        }
    }
    data->types = types;
    return true;
}

static bool copy_constants(TfCoreProgram *program) {
    TfCoreProgramData *data = &program->data;
    TfCoreConstant *constants = copy_array(program, data->constants, data->constant_count,
                                          sizeof(*constants));
    if (data->constant_count != 0 && constants == NULL) return false;
    for (size_t i = 0; i < data->constant_count; ++i) {
        TfCoreConstant *constant = &constants[i];
        switch (constant->kind) {
        case TF_CORE_CONSTANT_TEXT:
            if (!copy_text(program, &constant->as.text)) return false;
            break;
        case TF_CORE_CONSTANT_LIST: case TF_CORE_CONSTANT_RECORD:
            if (!copy_constant_ids(program, &constant->as.children)) return false;
            break;
        case TF_CORE_CONSTANT_VARIANT:
            if (!copy_constant_ids(program, &constant->as.variant.payloads)) return false;
            break;
        case TF_CORE_CONSTANT_INT: case TF_CORE_CONSTANT_FLOAT:
        case TF_CORE_CONSTANT_BOOL: case TF_CORE_CONSTANT_CHAR: case TF_CORE_CONSTANT_UNIT:
            break;
        default:
            memset(&constant->as, 0, sizeof(constant->as));
            break;
        }
    }
    data->constants = constants;
    return true;
}

static bool copy_parameters(TfCoreProgram *program, const TfCoreParameter **parameters,
                            size_t count) {
    TfCoreParameter *copy = copy_array(program, *parameters, count, sizeof(*copy));
    if (count != 0 && copy == NULL) return false;
    for (size_t i = 0; i < count; ++i)
        if (!copy_text(program, &copy[i].name)) return false;
    *parameters = copy;
    return true;
}

static bool copy_operation(TfCoreProgram *program, TfCoreOperation *operation) {
    TfCoreValueIds *ids = &operation->operands;
    ids->items = copy_array(program, ids->items, ids->count, sizeof(*ids->items));
    if (ids->count != 0 && ids->items == NULL) return false;
    switch (operation->kind) {
    case TF_CORE_OP_SWITCH_TAG: {
        const size_t count = operation->as.switch_tag.case_count;
        const TfCoreTagCase *cases = copy_array(program, operation->as.switch_tag.cases,
                                               count, sizeof(*cases));
        operation->as.switch_tag.cases = cases;
        return count == 0 || cases != NULL;
    }
    case TF_CORE_OP_BUILTIN:
        return copy_type_ids(program, &operation->as.builtin.type_arguments);
    case TF_CORE_OP_LIST_MAP:
        return copy_text(program, &operation->as.list_map.element.name);
    case TF_CORE_OP_CONST: case TF_CORE_OP_UNARY: case TF_CORE_OP_BINARY:
    case TF_CORE_OP_RECORD: case TF_CORE_OP_VARIANT: case TF_CORE_OP_LIST:
    case TF_CORE_OP_PROJECT: case TF_CORE_OP_TAG: case TF_CORE_OP_PAYLOAD:
    case TF_CORE_OP_IF: case TF_CORE_OP_CALL: case TF_CORE_OP_CLOSURE:
    case TF_CORE_OP_CALL_CLOSURE: case TF_CORE_OP_MACHINE_VALUE: case TF_CORE_OP_INPUT_AT:
        return true;
    default:
        memset(&operation->as, 0, sizeof(operation->as));
        return true;
    }
}

static bool copy_blocks(TfCoreProgram *program, TfCoreFunction *function) {
    const size_t count = function->block_count;
    TfCoreBlock *blocks = copy_array(program, function->blocks, count, sizeof(*blocks));
    if (count != 0 && blocks == NULL) return false;
    for (size_t i = 0; i < count; ++i) {
        const size_t let_count = blocks[i].let_count;
        TfCoreLet *lets = copy_array(program, blocks[i].lets, let_count, sizeof(*lets));
        if (let_count != 0 && lets == NULL) return false;
        for (size_t j = 0; j < let_count; ++j)
            if (!copy_operation(program, &lets[j].operation)) return false;
        blocks[i].lets = lets;
    }
    function->blocks = blocks;
    return true;
}

static bool copy_functions(TfCoreProgram *program) {
    TfCoreProgramData *data = &program->data;
    TfCoreFunction *functions = copy_array(program, data->functions, data->function_count,
                                          sizeof(*functions));
    if (data->function_count != 0 && functions == NULL) return false;
    for (size_t i = 0; i < data->function_count; ++i) {
        TfCoreFunction *fn = &functions[i];
        if (!copy_text(program, &fn->name) || !copy_text(program, &fn->module) ||
            !copy_parameters(program, &fn->parameters, fn->parameter_count) ||
            !copy_parameters(program, &fn->captures, fn->capture_count) ||
            !copy_blocks(program, fn)) return false;
    }
    data->functions = functions;
    return true;
}

TfStatus tf_core_program_copy(TfCoreProgram **out, const TfCoreProgramData *data,
                              const TfAllocator *allocator) {
    TfAllocator selected;
    TfCoreProgram *program;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (data == NULL) return TF_HOST_ERROR;
    selected = tf_allocator(allocator);
    if (!tf_allocator_valid(selected)) return TF_HOST_ERROR;
    program = selected.allocate(selected.context, sizeof(*program));
    if (program == NULL) return TF_HOST_ERROR;
    *program = (TfCoreProgram){.allocator = selected, .data = *data};
    program->data.trace_machines = copy_array(program, data->trace_machines,
        data->trace_machine_count, sizeof *data->trace_machines);
    program->data.trace_points = copy_array(program, data->trace_points,
        data->trace_point_count, sizeof *data->trace_points);
    if ((data->trace_machine_count && !program->data.trace_machines) ||
        (data->trace_point_count && !program->data.trace_points)) {
        tf_core_program_destroy(program);
        return TF_HOST_ERROR;
    }
    program->data.spans = copy_array(program, data->spans, data->span_count,
                                     sizeof(*data->spans));
    if ((data->span_count != 0 && program->data.spans == NULL) ||
        !copy_types(program) || !copy_constants(program) || !copy_functions(program)) {
        tf_core_program_destroy(program);
        return TF_HOST_ERROR;
    }
    *out = program;
    return TF_OK;
}

void tf_core_program_destroy(TfCoreProgram *program) {
    TfCoreArenaChunk *chunk;
    TfAllocator allocator;
    if (program == NULL) return;
    allocator = program->allocator;
    while ((chunk = program->chunks) != NULL) {
        program->chunks = chunk->arena.next;
        allocator.deallocate(allocator.context, chunk);
    }
    allocator.deallocate(allocator.context, program);
}

const TfCoreProgramData *tf_core_program_data(const TfCoreProgram *program) {
    return program != NULL ? &program->data : NULL;
}

const TfCoreType *tf_core_type(const TfCoreProgramData *data, TfCoreTypeId id) {
    return data != NULL && data->types != NULL && id != 0 && id <= data->type_count ?
           &data->types[id - 1] : NULL;
}

const TfCoreConstant *tf_core_constant(const TfCoreProgramData *data, TfCoreConstantId id) {
    return data != NULL && data->constants != NULL && id != 0 && id <= data->constant_count ?
           &data->constants[id - 1] : NULL;
}

const TfCoreFunction *tf_core_function(const TfCoreProgramData *data, TfCoreFunctionId id) {
    return data != NULL && data->functions != NULL && id != 0 && id <= data->function_count ?
           &data->functions[id - 1] : NULL;
}

const TfSpan *tf_core_span(const TfCoreProgramData *data, TfCoreSpanId id) {
    return data != NULL && data->spans != NULL && id != 0 && id <= data->span_count ?
           &data->spans[id - 1] : NULL;
}

const TfCoreBlock *tf_core_block(const TfCoreFunction *function, TfCoreBlockId id) {
    return function != NULL && function->blocks != NULL && id != 0 && id <= function->block_count ?
           &function->blocks[id - 1] : NULL;
}

const TfCoreTraceMachine *tf_core_trace_machine(const TfCoreProgramData *data,
                                               TfCoreFunctionId function) {
    if (data == NULL || data->trace_machines == NULL) return NULL;
    size_t lo = 0, hi = data->trace_machine_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (data->trace_machines[mid].function < function) lo = mid + 1;
        else hi = mid;
    }
    return lo < data->trace_machine_count && data->trace_machines[lo].function == function ?
           &data->trace_machines[lo] : NULL;
}

const TfCoreTracePoint *tf_core_trace_points(const TfCoreProgramData *data,
                                            TfCoreFunctionId function,
                                            TfCoreBlockId block, size_t *count) {
    if (count == NULL) return NULL;
    *count = 0;
    if (data == NULL || data->trace_points == NULL) return NULL;
    size_t lo = 0, hi = data->trace_point_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const TfCoreTracePoint *p = &data->trace_points[mid];
        if (p->function < function || (p->function == function && p->block < block)) lo = mid + 1;
        else hi = mid;
    }
    const size_t start = lo;
    while (lo < data->trace_point_count && data->trace_points[lo].function == function &&
           data->trace_points[lo].block == block) ++lo;
    *count = lo - start;
    return *count ? data->trace_points + start : NULL;
}
