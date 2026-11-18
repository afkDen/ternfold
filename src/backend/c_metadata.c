#include "c_metadata.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* File-scope const compound literals have static storage duration. Every
 * emitted pointer targets one of them, a named static array, or NULL. */
static bool byte_initializer(FILE *out, const unsigned char *data, size_t count) {
    if (fputc('{', out) == EOF) return false;
    for (size_t i = 0; i < count; ++i) {
        if (fputs(i % 12 == 0 ? "\n        " : " ", out) == EOF ||
            fprintf(out, "0x%02x,", (unsigned)data[i]) < 0) return false;
    }
    return fputs("\n    }", out) != EOF;
}

static bool bytes(FILE *out, const unsigned char *data, size_t count) {
    if (count == 0) return fputs("NULL", out) != EOF;
    return fputs("(const unsigned char[])", out) != EOF && byte_initializer(out, data, count);
}

static bool type_ids(FILE *out, const TfCoreTypeId *ids, const TfCoreField *fields,
                     size_t count) {
    if (count == 0) return fputs("NULL", out) != EOF;
    if (ids == NULL && fields == NULL) return false;
    if (fputs("(const tf_type_id[]){", out) == EOF) return false;
    for (size_t i = 0; i < count; ++i) {
        if (fputs(i % 12 == 0 ? "\n        " : " ", out) == EOF ||
            fprintf(out, "%zuu,", fields != NULL ? fields[i].type : ids[i]) < 0)
            return false;
    }
    return fputs("\n    }", out) != EOF;
}

static const char *type_kind(TfCoreTypeKind kind) {
    switch (kind) {
    case TF_CORE_TYPE_INT: return "TF_RT_TYPE_INT";
    case TF_CORE_TYPE_FLOAT: return "TF_RT_TYPE_FLOAT";
    case TF_CORE_TYPE_BOOL: return "TF_RT_TYPE_BOOL";
    case TF_CORE_TYPE_TEXT: return "TF_RT_TYPE_TEXT";
    case TF_CORE_TYPE_CHAR: return "TF_RT_TYPE_CHAR";
    case TF_CORE_TYPE_UNIT: return "TF_RT_TYPE_UNIT";
    case TF_CORE_TYPE_LIST: return "TF_RT_TYPE_LIST";
    case TF_CORE_TYPE_OPTION: case TF_CORE_TYPE_DECISION: case TF_CORE_TYPE_ENUM:
        return "TF_RT_TYPE_VARIANT";
    case TF_CORE_TYPE_RECORD: return "TF_RT_TYPE_RECORD";
    case TF_CORE_TYPE_MACHINE: return "TF_RT_TYPE_MACHINE";
    case TF_CORE_TYPE_FUNCTION: return "TF_RT_TYPE_FUNCTION";
    case TF_CORE_TYPE_TAG: return "TF_RT_TYPE_UNUSED";
    }
    return NULL;
}

static bool fields_shape(FILE *out, const TfCoreField *fields, size_t count) {
    return fputs(", .as.fields = {", out) != EOF &&
           type_ids(out, NULL, fields, count) && fprintf(out, ", %zuu}", count) >= 0;
}

static bool variant_shapes(FILE *out, const TfCoreType *type) {
    const TfCoreTypeId some[] = {type->kind == TF_CORE_TYPE_OPTION ? type->as.element : 0};
    TfCoreTypeId accept[2], reject[2];
    const TfCoreTypeIds option[] = {[TF_CORE_OPTION_SOME] = {some, 1},
                                    [TF_CORE_OPTION_NONE] = {NULL, 0}};
    const TfCoreTypeIds decision[] = {[TF_CORE_DECISION_ACCEPT] = {accept, 2},
                                      [TF_CORE_DECISION_REJECT] = {reject, 2}};
    if (type->kind == TF_CORE_TYPE_DECISION) {
        accept[0] = reject[0] = type->as.decision.machine;
        accept[1] = type->as.decision.emissions;
        reject[1] = type->as.decision.rejection;
    }
    const size_t count = type->kind == TF_CORE_TYPE_ENUM ? type->as.enumeration.variant_count : 2;
    if (fputs(", .as.variants = {(const tf_rt_shape[]){\n", out) == EOF) return false;
    for (size_t tag = 0; tag < count; ++tag) {
        const TfCoreTypeIds payloads = type->kind == TF_CORE_TYPE_OPTION ? option[tag] :
            type->kind == TF_CORE_TYPE_DECISION ? decision[tag] :
            type->as.enumeration.variants[tag].payloads;
        if (fputs("        {", out) == EOF || !type_ids(out, payloads.items, NULL, payloads.count) ||
            fprintf(out, ", %zuu},\n", payloads.count) < 0) return false;
    }
    return fprintf(out, "    }, %zuu}", count) >= 0;
}

static bool write_types(FILE *out, const TfCoreProgramData *core) {
    if (fputs("static const tf_rt_type tf_types[] = {\n", out) == EOF) return false;
    for (size_t i = 0; i < core->type_count; ++i) {
        const TfCoreType *type = &core->types[i];
        const char *kind = type_kind(type->kind);
        if (kind == NULL || fprintf(out, "    {.kind = %s", kind) < 0) return false;
        switch (type->kind) {
        case TF_CORE_TYPE_LIST:
            if (fprintf(out, ", .as.element = %zuu", type->as.element) < 0) return false;
            break;
        case TF_CORE_TYPE_RECORD:
            if (!fields_shape(out, type->as.record.fields, type->as.record.field_count)) return false;
            break;
        case TF_CORE_TYPE_MACHINE:
            if (!fields_shape(out, type->as.machine.cells, type->as.machine.cell_count)) return false;
            break;
        case TF_CORE_TYPE_OPTION: case TF_CORE_TYPE_DECISION: case TF_CORE_TYPE_ENUM:
            if (!variant_shapes(out, type)) return false;
            break;
        default:
            break;
        }
        if (fputs("},\n", out) == EOF) return false;
    }
    return fputs("    {.kind = TF_RT_TYPE_FUNCTION}, /* native call marker */\n};\n"
                 "static const size_t tf_type_count = sizeof tf_types / sizeof tf_types[0];\n\n",
                 out) != EOF;
}

typedef struct { size_t pool_id, core_id; } LiteralOrder;
typedef struct { size_t id, next; } LiteralFrame;

static TfCoreConstantIds children(const TfCoreConstant *constant) {
    switch (constant->kind) {
    case TF_CORE_CONSTANT_LIST: case TF_CORE_CONSTANT_RECORD: return constant->as.children;
    case TF_CORE_CONSTANT_VARIANT: return constant->as.variant.payloads;
    default: return (TfCoreConstantIds){NULL, 0};
    }
}

static bool child_first(const TfCoreProgramData *core) {
    for (size_t i = 0; i < core->constant_count; ++i) {
        const TfCoreConstantIds refs = children(&core->constants[i]);
        for (size_t j = 0; j < refs.count; ++j)
            if (refs.items[j] > i) return false;
    }
    return true;
}

/* Core permits forward edges, but verification proves that following them
 * cannot cycle. Walk roots by Core ID and edges in layout order. A row's
 * pool_id maps from Core to pool; core_id at that same index maps backwards.
 * SIZE_MAX marks an active frame and cannot be a valid constant ID. */
static bool order_literals(const TfCoreProgramData *core, LiteralOrder *order, LiteralFrame *stack) {
    size_t emitted = 0;
    for (size_t i = 0; i < core->constant_count; ++i) {
        if (order[i].pool_id != 0) continue;
        size_t depth = 1;
        stack[0] = (LiteralFrame){i + 1, 0};
        order[i].pool_id = SIZE_MAX;
        while (depth != 0) {
            LiteralFrame *frame = &stack[depth - 1];
            const TfCoreConstantIds refs = children(&core->constants[frame->id - 1]);
            if (frame->next < refs.count) {
                const size_t child = refs.items[frame->next++];
                if (order[child - 1].pool_id == SIZE_MAX) return false;
                if (order[child - 1].pool_id == 0) {
                    order[child - 1].pool_id = SIZE_MAX;
                    stack[depth++] = (LiteralFrame){child, 0};
                }
            } else {
                order[emitted++].core_id = frame->id;
                order[frame->id - 1].pool_id = emitted;
                --depth;
            }
        }
    }
    return true;
}

static size_t literal_id(const LiteralOrder *order, size_t core_id) {
    return order == NULL ? core_id : order[core_id - 1].pool_id;
}

static const char *literal_kind(TfCoreConstantKind kind) {
    switch (kind) {
    case TF_CORE_CONSTANT_INT: return "TF_RT_CONST_INT";
    case TF_CORE_CONSTANT_FLOAT: return "TF_RT_CONST_FLOAT";
    case TF_CORE_CONSTANT_BOOL: return "TF_RT_CONST_BOOL";
    case TF_CORE_CONSTANT_TEXT: return "TF_RT_CONST_TEXT";
    case TF_CORE_CONSTANT_CHAR: return "TF_RT_CONST_CHAR";
    case TF_CORE_CONSTANT_UNIT: return "TF_RT_CONST_UNIT";
    case TF_CORE_CONSTANT_LIST: return "TF_RT_CONST_LIST";
    case TF_CORE_CONSTANT_RECORD: return "TF_RT_CONST_RECORD";
    case TF_CORE_CONSTANT_VARIANT: return "TF_RT_CONST_VARIANT";
    }
    return NULL;
}

static bool integer(FILE *out, int64_t value) {
    if (value == INT64_MIN)
        return fputs("(-INT64_C(9223372036854775807) - INT64_C(1))", out) != EOF;
    if (value < 0) return fprintf(out, "-INT64_C(%" PRId64 ")", -value) >= 0;
    return fprintf(out, "INT64_C(%" PRId64 ")", value) >= 0;
}

static bool literal_children(FILE *out, TfCoreConstantIds refs, size_t tag,
                             const LiteralOrder *order) {
    if (fputs(", .as.children = {", out) == EOF) return false;
    if (refs.count == 0) {
        if (fputs("NULL", out) == EOF) return false;
    } else {
        if (fputs("(const size_t[]){", out) == EOF) return false;
        for (size_t i = 0; i < refs.count; ++i)
            if (fputs(i % 12 == 0 ? "\n        " : " ", out) == EOF ||
                fprintf(out, "%zuu,", literal_id(order, refs.items[i])) < 0) return false;
        if (fputs("\n    }", out) == EOF) return false;
    }
    return fprintf(out, ", %zuu, %zuu}", refs.count, tag) >= 0;
}

static bool literal_row(FILE *out, const TfCoreConstant *c, const LiteralOrder *order) {
    const char *kind = literal_kind(c->kind);
    if (kind == NULL || fprintf(out, "    {.kind = %s, .type = %zuu, .span = %zuu",
                                kind, c->type, c->span) < 0) return false;
    switch (c->kind) {
    case TF_CORE_CONSTANT_INT:
        if (fputs(", .as.integer = ", out) == EOF || !integer(out, c->as.integer)) return false;
        break;
    case TF_CORE_CONSTANT_FLOAT:
        if (fprintf(out, ", .as.bits = UINT64_C(0x%016" PRIx64 ")", c->as.float_bits) < 0)
            return false;
        break;
    case TF_CORE_CONSTANT_BOOL:
        if (fprintf(out, ", .as.bits = %uu", (unsigned)c->as.boolean) < 0) return false;
        break;
    case TF_CORE_CONSTANT_CHAR:
        if (fprintf(out, ", .as.bits = UINT32_C(0x%" PRIx32 ")", c->as.character) < 0) return false;
        break;
    case TF_CORE_CONSTANT_TEXT:
        if (fputs(", .as.text = {", out) == EOF ||
            !bytes(out, c->as.text.bytes, c->as.text.length) ||
            fprintf(out, ", %zuu}", c->as.text.length) < 0) return false;
        break;
    case TF_CORE_CONSTANT_LIST: case TF_CORE_CONSTANT_RECORD: case TF_CORE_CONSTANT_VARIANT:
        if (!literal_children(out, children(c),
                c->kind == TF_CORE_CONSTANT_VARIANT ? c->as.variant.tag : 0, order)) return false;
        break;
    case TF_CORE_CONSTANT_UNIT:
        break;
    }
    return fputs("},\n", out) != EOF;
}

static bool write_literals(FILE *out, const TfCoreProgramData *core, const LiteralOrder *order) {
    if (fputs("static const tf_rt_constant tf_literals[] = {\n", out) == EOF) return false;
    /* C17 has no empty arrays. The sentinel is outside the counted table. */
    if (core->constant_count == 0 && fputs("    {0},\n", out) == EOF) return false;
    for (size_t i = 0; i < core->constant_count; ++i) {
        const size_t id = order == NULL ? i + 1 : order[i].core_id;
        if (!literal_row(out, &core->constants[id - 1], order)) return false;
    }
    if (fprintf(out, "};\nstatic const size_t tf_literal_count = %zuu;\n",
                core->constant_count) < 0) return false;
    for (size_t i = 0; i < core->constant_count; ++i)
        if (fprintf(out, "#define tf_literal_id_%zu ((size_t)%zuu)\n",
                    i + 1, literal_id(order, i + 1)) < 0) return false;
    return fputc('\n', out) != EOF;
}

/* Endpoints are verified scalar boundaries. Search the same source indexes as
 * tf_source_location, without walking the line to expand display-only tabs. */
static void coordinates(const TfSource *source, size_t byte, size_t *line, size_t *column) {
    size_t lo = 0, hi = source->scalar_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (source->scalar_offsets[mid] < byte) lo = mid + 1;
        else hi = mid;
    }
    const size_t scalar = lo;
    lo = 0;
    hi = source->line_count;
    while (hi - lo > 1) {
        const size_t mid = lo + (hi - lo) / 2;
        if (source->line_scalars[mid] <= scalar) lo = mid;
        else hi = mid;
    }
    *line = lo + 1;
    *column = scalar - source->line_scalars[lo] + 1;
}

static bool write_origins(FILE *out, const TfCoreProgramData *core) {
    /* Consecutive spans from one source share a path without allocating a
     * lookup map. IDs come from first span ordinals, never pointer bits. */
    const TfSource *previous = NULL;
    for (size_t i = 0; i < core->span_count; ++i) {
        const TfSource *source = core->spans[i].source;
        if (source == previous) continue;
        previous = source;
        const char *name = source->name;
        if (name != NULL && *name != '\0' &&
            (fprintf(out, "static const unsigned char tf_origin_path_%zu[] = ", i + 1) < 0 ||
             !byte_initializer(out, (const unsigned char *)name, strlen(name)) ||
             fputs(";\n", out) == EOF)) return false;
    }
    if (fputs("#include \"ternfold/trace_artifact.h\"\n"
              "static const struct tf_generated_origin tf_origins[] = {\n", out) == EOF) return false;
    if (core->span_count == 0 && fputs("    {0},\n", out) == EOF) return false;
    previous = NULL;
    size_t length = 0, path_id = 0;
    for (size_t i = 0; i < core->span_count; ++i) {
        const TfSpan *span = &core->spans[i];
        if (span->source != previous) {
            previous = span->source;
            const char *name = previous->name;
            length = name != NULL ? strlen(name) : 0;
            path_id = i + 1;
        }
        size_t sl, sc, el, ec;
        coordinates(span->source, span->start, &sl, &sc);
        coordinates(span->source, span->end, &el, &ec);
        if (fputs("    {", out) == EOF ||
            (length == 0 ? fputs("NULL", out) == EOF :
             fprintf(out, "tf_origin_path_%zu", path_id) < 0) ||
            fprintf(out, ", %zuu, %zuu, %zuu, %zuu, %zuu, %zuu, %zuu},\n",
                    length, span->start, span->end, sl, sc, el, ec) < 0) return false;
    }
    return fprintf(out, "};\nstatic const size_t tf_origin_count = %zuu;\n\n", core->span_count) >= 0;
}

TfStatus tf_c_metadata_write(FILE *out, const TfVerifiedCore *program) {
    const TfCoreProgramData *core = tf_verified_core_data(program);
    LiteralOrder *order = NULL;
    if (out == NULL || core == NULL || ferror(out) || core->type_count == SIZE_MAX)
        return TF_HOST_ERROR;
    if (!child_first(core)) {
        const size_t count = core->constant_count;
        if (count > SIZE_MAX / sizeof(*order) || count > SIZE_MAX / sizeof(LiteralFrame))
            return TF_HOST_ERROR;
        order = calloc(count, sizeof(*order));
        if (order == NULL) return TF_HOST_ERROR;
        LiteralFrame *stack = malloc(count * sizeof(*stack));
        const bool ordered = stack != NULL && order_literals(core, order, stack);
        free(stack);
        if (!ordered) {
            free(order);
            return TF_HOST_ERROR;
        }
    }
    const bool ok = write_types(out, core) && write_literals(out, core, order) &&
                    write_origins(out, core) && !ferror(out);
    free(order);
    return ok ? TF_OK : TF_HOST_ERROR;
}
