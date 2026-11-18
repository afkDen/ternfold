#include "lifetimes.h"
#include "../internal.h"

#include <string.h>

struct TfNativeLifetimes {
    TfAllocator allocator;
    size_t *offsets;
    TfCoreValueId *values;
    size_t boundaries;
    bool eligible;
};

static void *array(TfAllocator a, size_t count, size_t width) {
    if (count == 0 || count > SIZE_MAX / width) return NULL;
    return a.allocate(a.context, count * width);
}

static bool supported(const TfCoreFunction *fn) {
    if (fn->block_count != 1 || fn->body != 1) return false;
    const TfCoreBlock *block = &fn->blocks[0];
    for (size_t i = 0; i < block->let_count; ++i) {
        const TfCoreOperation *op = &block->lets[i].operation;
        if (op->kind == TF_CORE_OP_IF || op->kind == TF_CORE_OP_SWITCH_TAG ||
            op->kind == TF_CORE_OP_LIST_MAP || (op->kind == TF_CORE_OP_BUILTIN &&
            (op->as.builtin.id == TF_CORE_BUILTIN_MAP || op->as.builtin.id == TF_CORE_BUILTIN_FOLD)))
            return false;
    }
    return true;
}

TfStatus tf_native_lifetimes(TfNativeLifetimes **out, const TfVerifiedCore *verified,
                             TfCoreFunctionId function, const TfAllocator *requested) {
    const TfCoreProgramData *data = tf_verified_core_data(verified);
    const TfCoreFunction *fn = tf_core_function(data, function);
    TfAllocator allocator = tf_allocator(requested);
    TfNativeLifetimes *plan;
    size_t *last = NULL;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (fn == NULL || !tf_allocator_valid(allocator)) return TF_HOST_ERROR;
    plan = allocator.allocate(allocator.context, sizeof(*plan));
    if (plan == NULL) return TF_HOST_ERROR;
    *plan = (TfNativeLifetimes){.allocator = allocator};
    if (!supported(fn)) { *out = plan; return TF_OK; }
    const TfCoreBlock *block = &fn->blocks[0];
    if (fn->value_count == SIZE_MAX || block->let_count > SIZE_MAX - 2) goto fail;
    const size_t slots = fn->value_count + 1;
    plan->boundaries = block->let_count + 1;
    last = array(allocator, slots, sizeof(*last));
    plan->offsets = array(allocator, plan->boundaries + 1, sizeof(*plan->offsets));
    if (last == NULL || plan->offsets == NULL) goto fail;
    for (size_t i = 0; i < slots; ++i) last[i] = SIZE_MAX;
    memset(plan->offsets, 0, (plan->boundaries + 1) * sizeof(*plan->offsets));
    for (size_t i = 0; i < fn->parameter_count; ++i) last[fn->parameters[i].value] = 0;
    for (size_t i = 0; i < fn->capture_count; ++i) last[fn->captures[i].value] = 0;
    for (size_t i = 0; i < block->let_count; ++i) {
        const TfCoreLet *let = &block->lets[i];
        last[let->value] = i + 1; /* dead results still execute, then release */
        for (size_t j = 0; j < let->operation.operands.count; ++j)
            last[let->operation.operands.items[j]] = i + 1;
    }
    size_t point_count = 0;
    const TfCoreTracePoint *points = tf_core_trace_points(data, function, 1, &point_count);
    for (size_t i = 0; i < point_count; ++i)
        if (last[points[i].value] < points[i].after_let) last[points[i].value] = points[i].after_let;
    if (block->terminator.kind == TF_CORE_YIELD) last[block->terminator.value] = SIZE_MAX;
    size_t releases = 0;
    for (size_t v = 1; v < slots; ++v) if (last[v] != SIZE_MAX) {
        ++plan->offsets[last[v] + 1];
        ++releases;
    }
    for (size_t i = 1; i <= plan->boundaries; ++i) plan->offsets[i] += plan->offsets[i - 1];
    if (releases != 0) {
        plan->values = array(allocator, releases, sizeof(*plan->values));
        if (plan->values == NULL) goto fail;
        /* Fill by ascending ID using offsets as cursors; restore them below. */
        for (size_t v = 1; v < slots; ++v)
            if (last[v] != SIZE_MAX) plan->values[plan->offsets[last[v]]++] = v;
        for (size_t i = plan->boundaries; i != 0; --i) plan->offsets[i] = plan->offsets[i - 1];
        plan->offsets[0] = 0;
    }
    plan->eligible = true;
    allocator.deallocate(allocator.context, last);
    *out = plan;
    return TF_OK;
fail:
    if (last != NULL) allocator.deallocate(allocator.context, last);
    tf_native_lifetimes_destroy(plan);
    return TF_HOST_ERROR;
}

bool tf_native_lifetimes_eligible(const TfNativeLifetimes *plan) {
    return plan != NULL && plan->eligible;
}
TfCoreValueIds tf_native_lifetimes_releases(const TfNativeLifetimes *plan, size_t after_let) {
    if (plan == NULL || !plan->eligible || after_let >= plan->boundaries) return (TfCoreValueIds){0};
    const size_t begin = plan->offsets[after_let], end = plan->offsets[after_let + 1];
    return (TfCoreValueIds){end != begin ? plan->values + begin : NULL, end - begin};
}
void tf_native_lifetimes_destroy(TfNativeLifetimes *plan) {
    if (plan == NULL) return;
    const TfAllocator allocator = plan->allocator;
    if (plan->values != NULL) allocator.deallocate(allocator.context, plan->values);
    if (plan->offsets != NULL) allocator.deallocate(allocator.context, plan->offsets);
    allocator.deallocate(allocator.context, plan);
}
