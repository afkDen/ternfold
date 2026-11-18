#include "ternfold_rt_value_ops.h"

#include <string.h>

/* The public context carries the configured allocator/budget. Scratch is
 * charged exactly like boxes; allocation callbacks cannot reenter the runtime.
 * No object representation or unchecked child access is used here. */
static void *scratch(tf_context *ctx, tf_span_id span, size_t bytes) {
    if (ctx->live_bytes <= ctx->config.byte_budget &&
        bytes <= ctx->config.byte_budget - ctx->live_bytes) {
        void *memory = ctx->config.allocator.allocate(ctx->config.allocator.user, bytes);
        if (memory) {
            ctx->live_bytes += bytes;
            return memory;
        }
    }
    (void)tf_context_fail(ctx, TF_R003, span);
    return NULL;
}

static void unscratch(tf_context *ctx, void *memory, size_t bytes) {
    if (memory) {
        ctx->live_bytes -= bytes;
        ctx->config.allocator.deallocate(ctx->config.allocator.user, memory);
    }
}

static bool work(tf_context *ctx, tf_span_id span, size_t *remaining) {
    if (*remaining == 0) {
        (void)tf_context_fail(ctx, TF_R003, span);
        return false;
    }
    --*remaining;
    return true;
}

static bool start(const tf_context *ctx, tf_value *out) {
    *out = tf_unit();
    return tf_context_fault(ctx) == NULL;
}

static const tf_rt_type *entry(const tf_context *ctx, tf_type_id id) {
    return id != 0 && id <= ctx->program.type_count ? &ctx->program.types[id - 1] : NULL;
}

static tf_type_id matching(tf_context *ctx, tf_span_id span, tf_value a, tf_value b) {
    tf_type_id type = tf_value_type(ctx, a);
    if (type == 0 || type != tf_value_type(ctx, b)) {
        (void)tf_context_fail(ctx, TF_R004, span);
        return 0;
    }
    return type;
}

typedef struct { tf_type_id queued; bool seen; } type_visit;

static bool enqueue(tf_context *ctx, tf_span_id span, tf_type_id id,
                    type_visit *visits, size_t *tail, size_t *remaining) {
    if (!work(ctx, span, remaining)) return false;
    const tf_rt_type *type = entry(ctx, id);
    if (!type || type->kind <= TF_RT_TYPE_UNUSED || type->kind >= TF_RT_TYPE_FUNCTION) {
        (void)tf_context_fail(ctx, TF_R004, span);
        return false;
    }
    if (!visits[id - 1].seen) {
        visits[id - 1].seen = true;
        visits[(*tail)++].queued = id;
    }
    return true;
}

static bool shape_enqueue(tf_context *ctx, tf_span_id span, tf_rt_shape shape,
                          type_visit *visits, size_t *tail, size_t *remaining) {
    if ((shape.count != 0 && !shape.types) || shape.count > SIZE_MAX / sizeof(*shape.types)) {
        (void)tf_context_fail(ctx, TF_R004, span);
        return false;
    }
    for (size_t i = 0; i < shape.count; ++i)
        if (!enqueue(ctx, span, shape.types[i], visits, tail, remaining)) return false;
    return true;
}

/* Capability is a property of the entire reachable type graph, not just
 * populated children. Mark on enqueue so even shared/cyclic metadata is
 * visited once. Verified programs have acyclic nominal type graphs. */
static bool equality_type(tf_context *ctx, tf_span_id span, tf_type_id root,
                          size_t *remaining) {
    const tf_rt_type *root_type = entry(ctx, root);
    if (!root_type) return false; /* matching already diagnosed invalid IDs */
    if (root_type->kind >= TF_RT_TYPE_UNIT && root_type->kind <= TF_RT_TYPE_TEXT) return true;
    size_t count = ctx->program.type_count;
    if (count > SIZE_MAX / sizeof(type_visit)) {
        (void)tf_context_fail(ctx, TF_R003, span);
        return false;
    }
    size_t bytes = count * sizeof(type_visit);
    type_visit *visits = scratch(ctx, span, bytes);
    if (!visits) return false;
    memset(visits, 0, bytes);
    size_t head = 0, tail = 0;
    bool okay = enqueue(ctx, span, root, visits, &tail, remaining);
    while (okay && head < tail) {
        const tf_rt_type *type = entry(ctx, visits[head++].queued);
        switch (type->kind) {
            case TF_RT_TYPE_LIST:
                okay = enqueue(ctx, span, type->as.element, visits, &tail, remaining);
                break;
            case TF_RT_TYPE_RECORD:
            case TF_RT_TYPE_MACHINE:
                okay = shape_enqueue(ctx, span, type->as.fields, visits, &tail, remaining);
                break;
            case TF_RT_TYPE_VARIANT:
                if ((type->as.variants.count != 0 && !type->as.variants.tags) ||
                    type->as.variants.count > SIZE_MAX / sizeof(*type->as.variants.tags)) {
                    (void)tf_context_fail(ctx, TF_R004, span);
                    okay = false;
                    break;
                }
                for (size_t i = 0; okay && i < type->as.variants.count; ++i) {
                    okay = work(ctx, span, remaining) &&
                        shape_enqueue(ctx, span, type->as.variants.tags[i], visits, &tail, remaining);
                }
                break;
            default: break; /* primitive, checked by enqueue */
        }
    }
    unscratch(ctx, visits, bytes);
    return okay;
}

typedef struct { tf_object *a, *b; } pair;
typedef struct {
    tf_value a, b;
    size_t index, count, tag;
    tf_rt_type_kind kind;
    bool owned, opened;
} frame;
typedef struct {
    frame *frames;
    size_t depth, capacity;
    pair *pairs;
    size_t used, slots;
} traversal;

static size_t pair_hash(pair key, size_t slots) {
    /* uintptr_t conversion does not dereference or order unrelated pointers. */
    uintptr_t a = (uintptr_t)key.a, b = (uintptr_t)key.b;
    return (size_t)((a >> 3) ^ (b >> 4) ^ (a << 7)) & (slots - 1);
}

static bool pair_slot(tf_context *ctx, tf_span_id span, pair *pairs, size_t slots,
                      pair key, size_t *index, size_t *remaining) {
    *index = pair_hash(key, slots);
    while (pairs[*index].a) {
        if (!work(ctx, span, remaining)) return false;
        if (pairs[*index].a == key.a && pairs[*index].b == key.b) return true;
        *index = (*index + 1) & (slots - 1);
    }
    return work(ctx, span, remaining);
}

/* At most half full, doubling with checked peak allocation. Every probe is
 * metered, bounding even adversarial hash collisions. Pairs borrow roots'
 * immutable descendants for the duration of the operation. */
static bool remember(tf_context *ctx, tf_span_id span, traversal *walk,
                     pair key, bool *seen, size_t *remaining) {
    if (walk->used >= walk->slots / 2) {
        if (walk->slots > SIZE_MAX / 2 / sizeof(pair)) {
            (void)tf_context_fail(ctx, TF_R003, span);
            return false;
        }
        size_t slots = walk->slots == 0 ? 16 : walk->slots * 2;
        pair *pairs = scratch(ctx, span, slots * sizeof(pair));
        if (!pairs) return false;
        /* Pointer null is not required to be all-bits-zero in ISO C. */
        for (size_t i = 0; i < slots; ++i) pairs[i] = (pair){NULL, NULL};
        for (size_t i = 0; i < walk->slots; ++i) {
            if (walk->pairs[i].a) {
                size_t index;
                if (!pair_slot(ctx, span, pairs, slots, walk->pairs[i], &index, remaining)) {
                    unscratch(ctx, pairs, slots * sizeof(pair));
                    return false;
                }
                pairs[index] = walk->pairs[i];
            }
        }
        unscratch(ctx, walk->pairs, walk->slots * sizeof(pair));
        walk->pairs = pairs;
        walk->slots = slots;
    }
    size_t index;
    if (!pair_slot(ctx, span, walk->pairs, walk->slots, key, &index, remaining)) return false;
    *seen = walk->pairs[index].a != NULL;
    if (!*seen) { walk->pairs[index] = key; ++walk->used; }
    return true;
}

/* On success consumes owned a/b into the frame; failure leaves them with
 * the caller. The initial root frame only borrows the external owners. */
static bool push(tf_context *ctx, tf_span_id span, traversal *walk,
                 tf_value a, tf_value b, bool owned) {
    if (walk->depth == walk->capacity) {
        if (walk->capacity > SIZE_MAX / 2 / sizeof(frame)) {
            (void)tf_context_fail(ctx, TF_R003, span);
            return false;
        }
        size_t capacity = walk->capacity == 0 ? 16 : walk->capacity * 2;
        frame *frames = scratch(ctx, span, capacity * sizeof(frame));
        if (!frames) return false;
        if (walk->depth != 0) memcpy(frames, walk->frames, walk->depth * sizeof(frame));
        /* Move frame owners; old storage is not released as values. */
        unscratch(ctx, walk->frames, walk->capacity * sizeof(frame));
        walk->frames = frames;
        walk->capacity = capacity;
    }
    walk->frames[walk->depth++] = (frame){a, b, 0, 0, 0, TF_RT_TYPE_UNUSED, owned, false};
    return true;
}

static void pop(traversal *walk) {
    frame *top = &walk->frames[--walk->depth];
    if (top->owned) { tf_release(&top->a); tf_release(&top->b); }
}

static tf_status child(tf_context *ctx, tf_span_id span, const frame *top,
                       tf_value parent, tf_value *out) {
    if (top->kind == TF_RT_TYPE_LIST) return tf_list_at(ctx, span, parent, top->index, out);
    if (top->kind == TF_RT_TYPE_VARIANT)
        return tf_payload(ctx, span, parent, top->tag, top->index, out);
    return tf_project(ctx, span, parent, top->index, out);
}

static bool inline_equal(tf_value a, tf_value b) {
    switch (a.tag) {
        case TF_VALUE_UNIT: return true;
        case TF_VALUE_INT: return a.as.integer == b.as.integer;
        case TF_VALUE_FLOAT: return a.as.float_bits == b.as.float_bits;
        case TF_VALUE_BOOL: return a.as.boolean == b.as.boolean;
        case TF_VALUE_CHAR: return a.as.character == b.as.character;
        case TF_VALUE_BOXED: return false; /* handled by traversal */
    }
    return false;
}

tf_status tf_value_equal(tf_context *ctx, tf_span_id span,
                          tf_value left, tf_value right, tf_value *out) {
    if (!start(ctx, out)) return TF_FAULT;
    size_t remaining = TF_VALUE_OPS_WORK_LIMIT;
    tf_type_id type = matching(ctx, span, left, right);
    if (type == 0) return TF_FAULT;
    if (left.tag != TF_VALUE_BOXED) {
        *out = tf_bool(inline_equal(left, right));
        return TF_OK;
    }
    if (!equality_type(ctx, span, type, &remaining)) return TF_FAULT;
    traversal walk = {0};
    bool equal = true;
    if (!push(ctx, span, &walk, left, right, false)) return TF_FAULT;
    while (walk.depth != 0 && equal && !tf_context_fault(ctx)) {
        if (!work(ctx, span, &remaining)) break;
        frame *top = &walk.frames[walk.depth - 1];
        if (!top->opened) {
            type = matching(ctx, span, top->a, top->b);
            if (type == 0) break;
            top->kind = entry(ctx, type)->kind;
            top->opened = true;
            switch (top->a.tag) {
                case TF_VALUE_UNIT:
                case TF_VALUE_INT:
                case TF_VALUE_FLOAT:
                case TF_VALUE_BOOL:
                case TF_VALUE_CHAR: equal = inline_equal(top->a, top->b); break;
                case TF_VALUE_BOXED: {
                    bool seen;
                    if (!remember(ctx, span, &walk, (pair){top->a.as.object, top->b.as.object},
                                  &seen, &remaining)) break;
                    if (seen || top->a.as.object == top->b.as.object) { pop(&walk); continue; }
                    if (top->kind == TF_RT_TYPE_TEXT) {
                        const unsigned char *a, *b;
                        size_t na, nb;
                        if (tf_text_view(ctx, span, top->a, &a, &na) != TF_OK ||
                            tf_text_view(ctx, span, top->b, &b, &nb) != TF_OK) break;
                        equal = na == nb && (na == 0 || memcmp(a, b, na) == 0);
                    } else if (top->kind == TF_RT_TYPE_LIST) {
                        size_t other;
                        if (tf_list_length(ctx, span, top->a, &top->count) != TF_OK ||
                            tf_list_length(ctx, span, top->b, &other) != TF_OK) break;
                        equal = top->count == other;
                    } else if (top->kind == TF_RT_TYPE_VARIANT) {
                        size_t other;
                        if (tf_variant_tag(ctx, span, top->a, &top->tag) != TF_OK ||
                            tf_variant_tag(ctx, span, top->b, &other) != TF_OK) break;
                        equal = top->tag == other;
                        top->count = entry(ctx, type)->as.variants.tags[top->tag].count;
                    } else {
                        top->count = entry(ctx, type)->as.fields.count;
                    }
                    break;
                }
            }
            if (tf_context_fault(ctx) || !equal) break;
        }
        if (top->index == top->count) { pop(&walk); continue; }
        tf_value a = tf_unit(), b = tf_unit();
        if (child(ctx, span, top, top->a, &a) != TF_OK ||
            child(ctx, span, top, top->b, &b) != TF_OK) {
            tf_release(&a); tf_release(&b); break;
        }
        ++top->index;
        if (!push(ctx, span, &walk, a, b, true)) {
            tf_release(&a); tf_release(&b); break;
        }
    }
    while (walk.depth != 0) pop(&walk);
    unscratch(ctx, walk.frames, walk.capacity * sizeof(frame));
    unscratch(ctx, walk.pairs, walk.slots * sizeof(pair));
    if (tf_context_fault(ctx)) return TF_FAULT;
    *out = tf_bool(equal);
    return TF_OK;
}

static bool total_size(tf_context *ctx, tf_span_id span, size_t a, size_t b,
                       size_t stride, size_t *total) {
    if (b > SIZE_MAX - a || (uintmax_t)(a + b) > INT64_MAX ||
        a + b > SIZE_MAX / stride) {
        (void)tf_context_fail(ctx, TF_R003, span);
        return false;
    }
    *total = a + b;
    return true;
}

tf_status tf_text_concat(tf_context *ctx, tf_span_id span,
                          tf_value left, tf_value right, tf_value *out) {
    if (!start(ctx, out)) return TF_FAULT;
    tf_type_id type = matching(ctx, span, left, right);
    if (type == 0) return TF_FAULT;
    const unsigned char *a, *b;
    size_t na, nb, count;
    if (tf_text_view(ctx, span, left, &a, &na) != TF_OK ||
        tf_text_view(ctx, span, right, &b, &nb) != TF_OK ||
        !total_size(ctx, span, na, nb, 1, &count)) return TF_FAULT;
    if (count == 0) return tf_text(ctx, span, type, NULL, 0, out);
    unsigned char *bytes = scratch(ctx, span, count);
    if (!bytes) return TF_FAULT;
    if (na != 0) memcpy(bytes, a, na);
    if (nb != 0) memcpy(bytes + na, b, nb);
    tf_status status = tf_text(ctx, span, type, bytes, count, out);
    unscratch(ctx, bytes, count);
    return status;
}

tf_status tf_list_concat(tf_context *ctx, tf_span_id span,
                          tf_value left, tf_value right, tf_value *out) {
    if (!start(ctx, out)) return TF_FAULT;
    tf_type_id type = matching(ctx, span, left, right);
    if (type == 0) return TF_FAULT;
    size_t na, nb, count;
    if (tf_list_length(ctx, span, left, &na) != TF_OK ||
        tf_list_length(ctx, span, right, &nb) != TF_OK ||
        !total_size(ctx, span, na, nb, sizeof(tf_value), &count)) return TF_FAULT;
    if (count == 0) return tf_list(ctx, span, type, NULL, 0, out);
    size_t bytes = count * sizeof(tf_value);
    tf_value *items = scratch(ctx, span, bytes);
    if (!items) return TF_FAULT;
    size_t acquired = 0;
    while (acquired < count) {
        tf_value source = acquired < na ? left : right;
        size_t index = acquired < na ? acquired : acquired - na;
        if (tf_list_at(ctx, span, source, index, &items[acquired]) != TF_OK) break;
        ++acquired;
    }
    tf_status status = acquired == count ? tf_list(ctx, span, type, items, count, out) : TF_FAULT;
    while (acquired != 0) tf_release(&items[--acquired]);
    unscratch(ctx, items, bytes);
    return status;
}
