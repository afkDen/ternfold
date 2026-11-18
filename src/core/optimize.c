#include "ternfold/core_optimize.h"
#include "ternfold/checked_numeric.h"
#include "../internal.h"

#include <string.h>

struct TfCoreOptimization {
    TfAllocator allocator;
    TfCoreProgram *program;
    TfVerifiedCore *verified;
    size_t folds;
};

static void *array(TfAllocator a, size_t count, size_t width) {
    if (count == 0 || count > SIZE_MAX / width) return NULL;
    return a.allocate(a.context, count * width);
}

static bool scalar(const TfCoreConstant *c) {
    return c->kind == TF_CORE_CONSTANT_INT || c->kind == TF_CORE_CONSTANT_FLOAT ||
           c->kind == TF_CORE_CONSTANT_BOOL || c->kind == TF_CORE_CONSTANT_CHAR ||
           c->kind == TF_CORE_CONSTANT_UNIT;
}

/* Finite canonical binary64 has a total numerical order directly in its bits.
 * Comparing it this way cannot raise an FP exception or depend on host mode. */
static int float_order(uint64_t a, uint64_t b) {
    const uint64_t sign = UINT64_C(0x8000000000000000);
    if (a == b) return 0;
    if ((a & sign) != (b & sign)) return (a & sign) != 0 ? -1 : 1;
    if ((a & sign) != 0) return a > b ? -1 : 1;
    return a < b ? -1 : 1;
}

static bool unary(TfCoreUnary op, const TfCoreConstant *a, TfCoreConstant *r) {
    r->kind = a->kind;
    if (op == TF_CORE_UNARY_NOT && a->kind == TF_CORE_CONSTANT_BOOL) {
        r->as.boolean = (uint8_t)!a->as.boolean;
        return true;
    }
    if (op != TF_CORE_UNARY_NEGATE) return false;
    if (a->kind == TF_CORE_CONSTANT_INT)
        return tf_checked_int_neg(a->as.integer, &r->as.integer) == TF_CHECKED_NUMERIC_OK;
    if (a->kind == TF_CORE_CONSTANT_FLOAT)
        return tf_checked_float_neg(a->as.float_bits, &r->as.float_bits) == TF_CHECKED_NUMERIC_OK;
    return false;
}

static bool binary(TfCoreBinary op, const TfCoreConstant *a,
                   const TfCoreConstant *b, TfCoreConstant *r) {
    TfCheckedNumericStatus status = TF_CHECKED_NUMERIC_UNSUPPORTED;
    const bool ints = a->kind == TF_CORE_CONSTANT_INT && b->kind == TF_CORE_CONSTANT_INT;
    const bool floats = a->kind == TF_CORE_CONSTANT_FLOAT && b->kind == TF_CORE_CONSTANT_FLOAT;
    if (op >= TF_CORE_BINARY_LESS && op <= TF_CORE_BINARY_NOT_EQUAL && a->kind == b->kind) {
        int order;
        switch (a->kind) {
        case TF_CORE_CONSTANT_INT:
            order = (a->as.integer > b->as.integer) - (a->as.integer < b->as.integer); break;
        case TF_CORE_CONSTANT_FLOAT: order = float_order(a->as.float_bits, b->as.float_bits); break;
        case TF_CORE_CONSTANT_BOOL: order = (int)a->as.boolean - (int)b->as.boolean; break;
        case TF_CORE_CONSTANT_CHAR:
            order = (a->as.character > b->as.character) - (a->as.character < b->as.character); break;
        case TF_CORE_CONSTANT_UNIT: order = 0; break;
        default: return false;
        }
        r->kind = TF_CORE_CONSTANT_BOOL;
        switch (op) {
        case TF_CORE_BINARY_LESS: r->as.boolean = (uint8_t)(order < 0); break;
        case TF_CORE_BINARY_LESS_EQUAL: r->as.boolean = (uint8_t)(order <= 0); break;
        case TF_CORE_BINARY_GREATER: r->as.boolean = (uint8_t)(order > 0); break;
        case TF_CORE_BINARY_GREATER_EQUAL: r->as.boolean = (uint8_t)(order >= 0); break;
        case TF_CORE_BINARY_EQUAL: r->as.boolean = (uint8_t)(order == 0); break;
        case TF_CORE_BINARY_NOT_EQUAL: r->as.boolean = (uint8_t)(order != 0); break;
        default: return false;
        }
        return true;
    }
    if (op == TF_CORE_BINARY_REAL_DIVIDE &&
        (a->kind == TF_CORE_CONSTANT_INT || a->kind == TF_CORE_CONSTANT_FLOAT) &&
        (b->kind == TF_CORE_CONSTANT_INT || b->kind == TF_CORE_CONSTANT_FLOAT)) {
        const uint64_t left = a->kind == TF_CORE_CONSTANT_INT ?
            tf_checked_int_to_float_bits(a->as.integer) : a->as.float_bits;
        const uint64_t right = b->kind == TF_CORE_CONSTANT_INT ?
            tf_checked_int_to_float_bits(b->as.integer) : b->as.float_bits;
        r->kind = TF_CORE_CONSTANT_FLOAT;
        status = tf_checked_float_div(left, right, &r->as.float_bits);
    } else if (ints) {
        r->kind = TF_CORE_CONSTANT_INT;
        const int64_t x = a->as.integer, y = b->as.integer;
        switch (op) {
        case TF_CORE_BINARY_ADD: status = tf_checked_int_add(x, y, &r->as.integer); break;
        case TF_CORE_BINARY_SUBTRACT: status = tf_checked_int_sub(x, y, &r->as.integer); break;
        case TF_CORE_BINARY_MULTIPLY: status = tf_checked_int_mul(x, y, &r->as.integer); break;
        case TF_CORE_BINARY_FLOOR_DIVIDE: status = tf_checked_int_div(x, y, &r->as.integer); break;
        case TF_CORE_BINARY_REMAINDER: status = tf_checked_int_rem(x, y, &r->as.integer); break;
        case TF_CORE_BINARY_POWER: status = tf_checked_int_pow(x, y, &r->as.integer); break;
        default: break;
        }
    } else if (a->kind == TF_CORE_CONSTANT_FLOAT && b->kind == TF_CORE_CONSTANT_INT &&
               op == TF_CORE_BINARY_POWER) {
        r->kind = TF_CORE_CONSTANT_FLOAT;
        status = tf_checked_float_pow(a->as.float_bits, b->as.integer, &r->as.float_bits);
    } else if (floats) {
        r->kind = TF_CORE_CONSTANT_FLOAT;
        const uint64_t x = a->as.float_bits;
        switch (op) {
        case TF_CORE_BINARY_ADD: status = tf_checked_float_add(x, b->as.float_bits, &r->as.float_bits); break;
        case TF_CORE_BINARY_SUBTRACT: status = tf_checked_float_sub(x, b->as.float_bits, &r->as.float_bits); break;
        case TF_CORE_BINARY_MULTIPLY: status = tf_checked_float_mul(x, b->as.float_bits, &r->as.float_bits); break;
        default: break;
        }
    }
    return status == TF_CHECKED_NUMERIC_OK;
}

static bool fold(TfCoreProgramData *data, TfCoreConstant *constants,
                 TfAllocator allocator, size_t *folds) {
    /* Only this unpublished deep copy is mutable. Its underlying objects were
     * allocated without const qualification by tf_core_program_copy. Public
     * const views do not authorize modifying the caller's verified input. */
    for (size_t f = 0; f < data->function_count; ++f) {
        const TfCoreFunction *fn = &data->functions[f];
        if (fn->value_count == SIZE_MAX) return false;
        const size_t count = fn->value_count + 1;
        TfCoreConstantId *known = array(allocator, count, sizeof(*known));
        if (known == NULL) return false;
        memset(known, 0, count * sizeof(*known));
        for (size_t b = 0; b < fn->block_count; ++b) {
            for (size_t i = 0; i < fn->blocks[b].let_count; ++i) {
                TfCoreLet *let = &((TfCoreLet *)fn->blocks[b].lets)[i];
                TfCoreOperation *op = &let->operation;
                if (op->kind == TF_CORE_OP_CONST) {
                    if (scalar(&constants[op->as.constant - 1])) known[let->value] = op->as.constant;
                    continue;
                }
                if (op->kind != TF_CORE_OP_UNARY && op->kind != TF_CORE_OP_BINARY) continue;
                const TfCoreConstantId left = known[op->operands.items[0]];
                if (left == 0) continue;
                TfCoreConstant result = {.type = let->type, .span = let->span};
                bool safe;
                if (op->kind == TF_CORE_OP_UNARY) safe = unary(op->as.unary, &constants[left - 1], &result);
                else {
                    const TfCoreConstantId right = known[op->operands.items[1]];
                    safe = right != 0 && binary(op->as.binary, &constants[left - 1], &constants[right - 1], &result);
                }
                if (!safe) continue; /* The original fault and span remain executable. */
                constants[data->constant_count++] = result;
                *op = (TfCoreOperation){.kind = TF_CORE_OP_CONST, .as.constant = data->constant_count};
                known[let->value] = data->constant_count;
                ++*folds;
            }
            /* Only definitions in this block set entries. Clear those rather
             * than scanning every function slot for each (possibly empty) block. */
            for (size_t i = 0; i < fn->blocks[b].let_count; ++i)
                known[fn->blocks[b].lets[i].value] = 0;
        }
        allocator.deallocate(allocator.context, known);
    }
    return true;
}

TfStatus tf_core_optimize(TfCoreOptimization **out, const TfVerifiedCore *input,
                          TfCoreOptLevel level, const TfAllocator *requested) {
    TfAllocator allocator = tf_allocator(requested);
    TfCoreProgram *scratch = NULL;
    TfCoreConstant *constants = NULL;
    TfCoreOptimization *result = NULL;
    const TfCoreProgramData *source = tf_verified_core_data(input);
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (source == NULL || !tf_allocator_valid(allocator) ||
        (level != TF_CORE_OPT_LEVEL_0 && level != TF_CORE_OPT_LEVEL_1)) return TF_HOST_ERROR;
    result = allocator.allocate(allocator.context, sizeof(*result));
    if (result == NULL) return TF_HOST_ERROR;
    *result = (TfCoreOptimization){.allocator = allocator};
    if (tf_core_program_copy(&scratch, source, &allocator) != TF_OK) goto fail;
    if (level == TF_CORE_OPT_LEVEL_0) {
        result->program = scratch;
        scratch = NULL;
    } else {
        TfCoreProgramData data = *tf_core_program_data(scratch);
        size_t capacity = data.constant_count;
        for (size_t f = 0; f < data.function_count; ++f)
            for (size_t b = 0; b < data.functions[f].block_count; ++b) {
                const size_t n = data.functions[f].blocks[b].let_count;
                if (n > SIZE_MAX - capacity) goto fail;
                capacity += n;
            }
        if (capacity != 0) {
            constants = array(allocator, capacity, sizeof(*constants));
            if (constants == NULL) goto fail;
            if (data.constant_count != 0)
                memcpy(constants, data.constants, data.constant_count * sizeof(*constants));
        }
        data.constants = constants;
        if (!fold(&data, constants, allocator, &result->folds) ||
            tf_core_program_copy(&result->program, &data, &allocator) != TF_OK) goto fail;
    }
    if (tf_core_verify(&result->verified, result->program, &allocator, NULL) != TF_OK) goto fail;
    tf_core_program_destroy(scratch);
    if (constants != NULL) allocator.deallocate(allocator.context, constants);
    *out = result;
    return TF_OK;
fail:
    tf_core_program_destroy(scratch);
    if (constants != NULL) allocator.deallocate(allocator.context, constants);
    tf_core_optimization_destroy(result);
    return TF_HOST_ERROR;
}

const TfVerifiedCore *tf_core_optimization_verified(const TfCoreOptimization *out) {
    return out != NULL ? out->verified : NULL;
}
size_t tf_core_optimization_fold_count(const TfCoreOptimization *out) {
    return out != NULL ? out->folds : 0;
}
void tf_core_optimization_destroy(TfCoreOptimization *out) {
    if (out == NULL) return;
    const TfAllocator allocator = out->allocator;
    tf_verified_core_destroy(out->verified);
    tf_core_program_destroy(out->program);
    allocator.deallocate(allocator.context, out);
}
