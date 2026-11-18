#include "ternfold/core_dump.h"

#include <inttypes.h>
#include <string.h>

static bool quoted(FILE *out, TfCoreText text) {
    if (fputc('"', out) == EOF) return false;
    for (size_t i = 0; i < text.length; ++i) {
        const unsigned char byte = text.bytes[i];
        const char *escape = NULL;
        switch (byte) {
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        }
        if (escape != NULL) {
            if (fputs(escape, out) == EOF) return false;
        } else if (byte < 0x20 || byte > 0x7e) {
            if (fprintf(out, "\\x%02x", (unsigned)byte) < 0) return false;
        } else if (fputc(byte, out) == EOF) {
            return false;
        }
    }
    return fputc('"', out) != EOF;
}

/* Core ID typedefs share the size_t representation. */
static bool ids(FILE *out, char prefix, const size_t *items, size_t count) {
    if (fputc('[', out) == EOF) return false;
    for (size_t i = 0; i < count; ++i) {
        if ((i != 0 && fputs(", ", out) == EOF) ||
            fprintf(out, "%c%zu", prefix, items[i]) < 0) return false;
    }
    return fputc(']', out) != EOF;
}

static const char *type_kind(TfCoreTypeKind kind) {
    switch (kind) {
    case TF_CORE_TYPE_INT: return "Int";
    case TF_CORE_TYPE_FLOAT: return "Float";
    case TF_CORE_TYPE_BOOL: return "Bool";
    case TF_CORE_TYPE_TEXT: return "Text";
    case TF_CORE_TYPE_CHAR: return "Char";
    case TF_CORE_TYPE_UNIT: return "Unit";
    case TF_CORE_TYPE_LIST: return "List";
    case TF_CORE_TYPE_OPTION: return "Option";
    case TF_CORE_TYPE_DECISION: return "Decision";
    case TF_CORE_TYPE_RECORD: return "Record";
    case TF_CORE_TYPE_ENUM: return "Enum";
    case TF_CORE_TYPE_MACHINE: return "Machine";
    case TF_CORE_TYPE_FUNCTION: return "Function";
    case TF_CORE_TYPE_TAG: return "Tag";
    }
    return NULL;
}

static bool fields(FILE *out, const char *label, const TfCoreField *items, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (fprintf(out, "  %s %zu name=", label, i) < 0 || !quoted(out, items[i].name) ||
            fprintf(out, " type=t%zu span=s%zu\n", items[i].type, items[i].span) < 0)
            return false;
    }
    return true;
}

static bool type_line(FILE *out, TfCoreTypeId id, const TfCoreType *type) {
    const char *kind = type_kind(type->kind);
    if (kind == NULL || fprintf(out, "type t%zu %s name=", id, kind) < 0 ||
        !quoted(out, type->name) || fputs(" module=", out) == EOF ||
        !quoted(out, type->module) || fprintf(out, " span=s%zu", type->span) < 0)
        return false;
    switch (type->kind) {
    case TF_CORE_TYPE_INT: case TF_CORE_TYPE_FLOAT: case TF_CORE_TYPE_BOOL:
    case TF_CORE_TYPE_TEXT: case TF_CORE_TYPE_CHAR: case TF_CORE_TYPE_UNIT:
        break;
    case TF_CORE_TYPE_LIST: case TF_CORE_TYPE_OPTION:
        if (fprintf(out, " element=t%zu", type->as.element) < 0) return false;
        break;
    case TF_CORE_TYPE_DECISION:
        if (fprintf(out, " machine=t%zu output=t%zu rejection=t%zu emissions=t%zu",
                    type->as.decision.machine, type->as.decision.output,
                    type->as.decision.rejection, type->as.decision.emissions) < 0)
            return false;
        break;
    case TF_CORE_TYPE_RECORD:
        return fputc('\n', out) != EOF &&
            fields(out, "field", type->as.record.fields, type->as.record.field_count);
    case TF_CORE_TYPE_ENUM:
        if (fputc('\n', out) == EOF) return false;
        for (size_t i = 0; i < type->as.enumeration.variant_count; ++i) {
            const TfCoreVariant *variant = &type->as.enumeration.variants[i];
            if (fprintf(out, "  tag %zu name=", i) < 0 || !quoted(out, variant->name) ||
                fprintf(out, " span=s%zu payloads=", variant->span) < 0 ||
                !ids(out, 't', variant->payloads.items, variant->payloads.count) ||
                fputc('\n', out) == EOF) return false;
        }
        return true;
    case TF_CORE_TYPE_MACHINE:
        return fprintf(out, " input=t%zu output=t%zu rejection=t%zu\n",
                       type->as.machine.input, type->as.machine.output,
                       type->as.machine.rejection) >= 0 &&
            fields(out, "cell", type->as.machine.cells, type->as.machine.cell_count);
    case TF_CORE_TYPE_FUNCTION:
        if (fputs(" parameters=", out) == EOF ||
            !ids(out, 't', type->as.function.parameters.items, type->as.function.parameters.count) ||
            fprintf(out, " result=t%zu", type->as.function.result) < 0) return false;
        break;
    case TF_CORE_TYPE_TAG:
        if (fprintf(out, " tagged=t%zu", type->as.tagged) < 0) return false;
        break;
    }
    return fputc('\n', out) != EOF;
}

static bool constant_value(FILE *out, const TfCoreConstant *constant) {
    switch (constant->kind) {
    case TF_CORE_CONSTANT_INT:
        return fprintf(out, "Int %" PRId64, constant->as.integer) >= 0;
    case TF_CORE_CONSTANT_FLOAT:
        return fprintf(out, "Float bits=0x%016" PRIx64, constant->as.float_bits) >= 0;
    case TF_CORE_CONSTANT_BOOL:
        return fputs(constant->as.boolean ? "Bool true" : "Bool false", out) != EOF;
    case TF_CORE_CONSTANT_TEXT:
        return fputs("Text ", out) != EOF && quoted(out, constant->as.text);
    case TF_CORE_CONSTANT_CHAR:
        return fprintf(out, "Char U+%04" PRIX32, constant->as.character) >= 0;
    case TF_CORE_CONSTANT_UNIT:
        return fputs("Unit", out) != EOF;
    case TF_CORE_CONSTANT_LIST: case TF_CORE_CONSTANT_RECORD:
        return fputs(constant->kind == TF_CORE_CONSTANT_LIST ? "List " : "Record ", out) != EOF &&
            ids(out, 'c', constant->as.children.items, constant->as.children.count);
    case TF_CORE_CONSTANT_VARIANT:
        return fprintf(out, "Variant tag=%zu payloads=", constant->as.variant.tag) >= 0 &&
            ids(out, 'c', constant->as.variant.payloads.items, constant->as.variant.payloads.count);
    }
    return false;
}

static const char *unary_name(TfCoreUnary unary) {
    switch (unary) {
    case TF_CORE_UNARY_NEGATE: return "negate";
    case TF_CORE_UNARY_NOT: return "not";
    }
    return NULL;
}

static const char *binary_name(TfCoreBinary binary) {
    switch (binary) {
    case TF_CORE_BINARY_ADD: return "+";
    case TF_CORE_BINARY_SUBTRACT: return "-";
    case TF_CORE_BINARY_MULTIPLY: return "*";
    case TF_CORE_BINARY_REAL_DIVIDE: return "/";
    case TF_CORE_BINARY_FLOOR_DIVIDE: return "div";
    case TF_CORE_BINARY_REMAINDER: return "%";
    case TF_CORE_BINARY_POWER: return "^";
    case TF_CORE_BINARY_LESS: return "<";
    case TF_CORE_BINARY_LESS_EQUAL: return "<=";
    case TF_CORE_BINARY_GREATER: return ">";
    case TF_CORE_BINARY_GREATER_EQUAL: return ">=";
    case TF_CORE_BINARY_EQUAL: return "==";
    case TF_CORE_BINARY_NOT_EQUAL: return "!=";
    case TF_CORE_BINARY_CONCAT: return "concat";
    }
    return NULL;
}

static const char *builtin_name(TfCoreBuiltin builtin) {
    switch (builtin) {
    case TF_CORE_BUILTIN_LEN: return "len";
    case TF_CORE_BUILTIN_AT: return "at";
    case TF_CORE_BUILTIN_APPEND: return "append";
    case TF_CORE_BUILTIN_MAP: return "map";
    case TF_CORE_BUILTIN_FOLD: return "fold";
    case TF_CORE_BUILTIN_RANGE: return "range";
    case TF_CORE_BUILTIN_INT_TEXT: return "int_text";
    case TF_CORE_BUILTIN_INT_FLOAT: return "int_float";
    case TF_CORE_BUILTIN_FLOAT_INT: return "float_int";
    case TF_CORE_BUILTIN_FLOAT_TEXT: return "float_text";
    case TF_CORE_BUILTIN_BOOL_TEXT: return "bool_text";
    case TF_CORE_BUILTIN_PARSE_INT: return "parse_int";
    case TF_CORE_BUILTIN_TEXT_LEN: return "text_len";
    case TF_CORE_BUILTIN_TEXT_AT: return "text_at";
    case TF_CORE_BUILTIN_TEXT_SLICE: return "text_slice";
    }
    return NULL;
}

static bool parameter(FILE *out, const TfCoreParameter *param) {
    return fprintf(out, "%%%zu:t%zu name=", param->value, param->type) >= 0 &&
        quoted(out, param->name) && fprintf(out, " span=s%zu", param->span) >= 0;
}

static bool switch_tag(FILE *out, const TfCoreOperation *op) {
    if (fputs("SwitchTag cases=[", out) == EOF) return false;
    for (size_t i = 0; i < op->as.switch_tag.case_count; ++i) {
        const TfCoreTagCase *item = &op->as.switch_tag.cases[i];
        if ((i != 0 && fputs(", ", out) == EOF) ||
            fprintf(out, "%zu:b%zu", item->tag, item->block) < 0) return false;
    }
    return fprintf(out, "] default=b%zu", op->as.switch_tag.default_block) >= 0;
}

static bool operation(FILE *out, const TfCoreOperation *op) {
    const char *name;
    switch (op->kind) {
    case TF_CORE_OP_CONST:
        return fprintf(out, "Const c%zu", op->as.constant) >= 0;
    case TF_CORE_OP_UNARY:
        name = unary_name(op->as.unary);
        return name != NULL && fprintf(out, "Unary %s", name) >= 0;
    case TF_CORE_OP_BINARY:
        name = binary_name(op->as.binary);
        return name != NULL && fprintf(out, "Binary %s", name) >= 0;
    case TF_CORE_OP_RECORD:
        return fprintf(out, "Record type=t%zu", op->as.constructed_type) >= 0;
    case TF_CORE_OP_VARIANT:
        return fprintf(out, "Variant type=t%zu tag=%zu", op->as.variant.type,
                       op->as.variant.tag) >= 0;
    case TF_CORE_OP_LIST:
        return fprintf(out, "List element=t%zu", op->as.element_type) >= 0;
    case TF_CORE_OP_PROJECT:
        return fprintf(out, "Project field=%zu", op->as.field) >= 0;
    case TF_CORE_OP_TAG:
        return fputs("Tag", out) != EOF;
    case TF_CORE_OP_PAYLOAD:
        return fprintf(out, "Payload tag=%zu index=%zu", op->as.payload.tag,
                       op->as.payload.index) >= 0;
    case TF_CORE_OP_IF:
        return fprintf(out, "If then=b%zu else=b%zu", op->as.branch.then_block,
                       op->as.branch.else_block) >= 0;
    case TF_CORE_OP_SWITCH_TAG:
        return switch_tag(out, op);
    case TF_CORE_OP_CALL:
        return fprintf(out, "Call f%zu", op->as.function) >= 0;
    case TF_CORE_OP_CLOSURE:
        return fprintf(out, "Closure f%zu", op->as.function) >= 0;
    case TF_CORE_OP_CALL_CLOSURE:
        return fputs("CallClosure", out) != EOF;
    case TF_CORE_OP_BUILTIN:
        name = builtin_name(op->as.builtin.id);
        return name != NULL && fprintf(out, "Builtin %s types=", name) >= 0 &&
            ids(out, 't', op->as.builtin.type_arguments.items, op->as.builtin.type_arguments.count);
    case TF_CORE_OP_MACHINE_VALUE:
        return fprintf(out, "MachineValue type=t%zu", op->as.constructed_type) >= 0;
    case TF_CORE_OP_LIST_MAP:
        return fputs("ListMap element=", out) != EOF && parameter(out, &op->as.list_map.element) &&
            fprintf(out, " body=b%zu", op->as.list_map.body) >= 0;
    case TF_CORE_OP_INPUT_AT:
        return fprintf(out, "InputAt index=%zu", op->as.input_index) >= 0;
    }
    return false;
}

static bool block_lines(FILE *out, TfCoreBlockId id, const TfCoreBlock *block) {
    if (fprintf(out, "  block b%zu span=s%zu\n", id, block->span) < 0) return false;
    for (size_t i = 0; i < block->let_count; ++i) {
        const TfCoreLet *let = &block->lets[i];
        if (fprintf(out, "    %%%zu:t%zu = ", let->value, let->type) < 0 ||
            !operation(out, &let->operation) || fputs(" operands=", out) == EOF ||
            !ids(out, '%', let->operation.operands.items, let->operation.operands.count) ||
            fprintf(out, " span=s%zu\n", let->span) < 0) return false;
    }
    switch (block->terminator.kind) {
    case TF_CORE_YIELD:
        return fprintf(out, "    Yield %%%zu span=s%zu\n", block->terminator.value,
                       block->terminator.span) >= 0;
    case TF_CORE_UNREACHABLE:
        return fprintf(out, "    Unreachable span=s%zu\n", block->terminator.span) >= 0;
    }
    return false;
}

static bool function_lines(FILE *out, TfCoreFunctionId id, const TfCoreFunction *fn) {
    const char *kind;
    switch (fn->kind) {
    case TF_CORE_FUNCTION_ORDINARY: kind = "ordinary"; break;
    case TF_CORE_FUNCTION_ENTRY_ONLY: kind = "entry-only"; break;
    case TF_CORE_FUNCTION_PRELUDE_WRAPPER: kind = "prelude-wrapper"; break;
    default: return false;
    }
    if (fprintf(out, "function f%zu %s name=", id, kind) < 0 || !quoted(out, fn->name) ||
        fputs(" module=", out) == EOF || !quoted(out, fn->module) ||
        fprintf(out, " result=t%zu body=b%zu values=%zu span=s%zu\n",
                fn->result, fn->body, fn->value_count, fn->span) < 0) return false;
    for (size_t i = 0; i < fn->parameter_count; ++i)
        if (fputs("  param ", out) == EOF || !parameter(out, &fn->parameters[i]) ||
            fputc('\n', out) == EOF) return false;
    for (size_t i = 0; i < fn->capture_count; ++i)
        if (fputs("  capture ", out) == EOF || !parameter(out, &fn->captures[i]) ||
            fputc('\n', out) == EOF) return false;
    for (size_t i = 0; i < fn->block_count; ++i)
        if (!block_lines(out, i + 1, &fn->blocks[i])) return false;
    return true;
}

static bool span_line(FILE *out, TfCoreSpanId id, const TfSpan *span) {
    const char *name = span->source != NULL ? span->source->name : NULL;
    /* TfSource owns a terminated name; all Core text itself is counted. */
    if (fprintf(out, "span s%zu source=", id) < 0 ||
        !(name == NULL ? fputs("none", out) != EOF :
          quoted(out, (TfCoreText){(const unsigned char *)name, strlen(name)})))
        return false;
    return fprintf(out, " bytes=%zu..%zu\n", span->start, span->end) >= 0;
}

TfStatus tf_core_dump_write(FILE *stream, const TfVerifiedCore *verified) {
    const TfCoreProgramData *data = tf_verified_core_data(verified);
    if (stream == NULL || data == NULL || ferror(stream)) return TF_HOST_ERROR;
    if (fprintf(stream, "Core entry=f%zu\n", data->entry) < 0) return TF_HOST_ERROR;
    for (size_t i = 0; i < data->span_count; ++i)
        if (!span_line(stream, i + 1, &data->spans[i])) return TF_HOST_ERROR;
    for (size_t i = 0; i < data->type_count; ++i)
        if (!type_line(stream, i + 1, &data->types[i])) return TF_HOST_ERROR;
    for (size_t i = 0; i < data->constant_count; ++i) {
        const TfCoreConstant *constant = &data->constants[i];
        if (fprintf(stream, "const c%zu:t%zu ", i + 1, constant->type) < 0 ||
            !constant_value(stream, constant) ||
            fprintf(stream, " span=s%zu\n", constant->span) < 0) return TF_HOST_ERROR;
    }
    for (size_t i = 0; i < data->function_count; ++i)
        if (!function_lines(stream, i + 1, &data->functions[i])) return TF_HOST_ERROR;
    return ferror(stream) ? TF_HOST_ERROR : TF_OK;
}
