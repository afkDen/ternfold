#include "ternfold_rt.h"

#include <stdlib.h>
#include <string.h>

struct tf_object {
    tf_rt_type_kind kind;
    uint64_t refcount;
    tf_type_id type;
    size_t identity; /* variant tag or function ID */
    size_t count; /* bytes or owned children */
    size_t size;
    tf_context *owner;
    tf_object *pending;
};

typedef struct { tf_object header; unsigned char bytes[]; } text_object;
typedef struct { tf_object header; tf_value items[]; } children_object;

static void *default_allocate(void *user, size_t bytes) {
    (void)user;
    return malloc(bytes);
}

static void default_deallocate(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static const tf_rt_type *type_entry(const tf_context *ctx, tf_type_id type) {
    if (type == 0 || type > ctx->program.type_count) return NULL;
    const tf_rt_type *entry = &ctx->program.types[type - 1];
    return entry->kind > TF_RT_TYPE_UNUSED && entry->kind <= TF_RT_TYPE_FUNCTION
        ? entry : NULL;
}

static const tf_rt_function *function_entry(const tf_context *ctx, tf_function_id id) {
    if (id == 0 || id > ctx->program.function_count) return NULL;
    const tf_rt_function *fn = &ctx->program.functions[id - 1];
    const tf_rt_type *type = type_entry(ctx, fn->type);
    return type && type->kind == TF_RT_TYPE_FUNCTION ? fn : NULL;
}

tf_status tf_context_fail(tf_context *ctx, tf_fault_code code, tf_span_id span) {
    if (ctx->fault.code == 0) {
        if (code < TF_R001 || code > TF_R006) code = TF_R004;
        ctx->fault = (tf_fault){code, span, ctx->frames, ctx->depth};
    }
    return TF_FAULT;
}

tf_status tf_context_init(tf_context *ctx, const tf_rt_program *program,
                          const tf_rt_config *config) {
    *ctx = (tf_context){0};
    if (config) {
        ctx->config = *config;
    } else {
        ctx->config.byte_budget = (uintmax_t)SIZE_MAX < TF_RT_DEFAULT_BYTE_BUDGET
            ? SIZE_MAX : (size_t)TF_RT_DEFAULT_BYTE_BUDGET;
        ctx->config.call_limit = TF_RT_MAX_CALL_DEPTH;
    }
    if (!ctx->config.allocator.allocate && !ctx->config.allocator.deallocate) {
        ctx->config.allocator.allocate = default_allocate;
        ctx->config.allocator.deallocate = default_deallocate;
    }
    if (!ctx->config.allocator.allocate || !ctx->config.allocator.deallocate ||
        ctx->config.call_limit > TF_RT_MAX_CALL_DEPTH || !program ||
        (program->type_count != 0 && !program->types) ||
        program->type_count > SIZE_MAX / sizeof(*program->types) ||
        (program->function_count != 0 && !program->functions) ||
        program->function_count > SIZE_MAX / sizeof(*program->functions)) {
        return tf_context_fail(ctx, TF_R004, 0);
    }

    for (size_t i = 0; i < program->type_count; ++i) {
        tf_value_tag tag;
        switch (program->types[i].kind) {
            case TF_RT_TYPE_UNIT: tag = TF_VALUE_UNIT; break;
            case TF_RT_TYPE_INT: tag = TF_VALUE_INT; break;
            case TF_RT_TYPE_FLOAT: tag = TF_VALUE_FLOAT; break;
            case TF_RT_TYPE_BOOL: tag = TF_VALUE_BOOL; break;
            case TF_RT_TYPE_CHAR: tag = TF_VALUE_CHAR; break;
            default: continue;
        }
        if (ctx->scalar_types[tag] != 0) return tf_context_fail(ctx, TF_R004, 0);
        ctx->scalar_types[tag] = i + 1;
    }
    ctx->program = *program;
    return TF_OK;
}

const tf_fault *tf_context_fault(const tf_context *ctx) {
    return ctx->fault.code != 0 ? &ctx->fault : NULL;
}

size_t tf_context_live_bytes(const tf_context *ctx) { return ctx->live_bytes; }

tf_status tf_context_enter(tf_context *ctx, tf_function_id function, tf_span_id span) {
    if (ctx->fault.code != 0) return TF_FAULT;
    if (!function_entry(ctx, function)) return tf_context_fail(ctx, TF_R004, span);
    if (ctx->depth >= ctx->config.call_limit) return tf_context_fail(ctx, TF_R003, span);
    ctx->frames[ctx->depth++] = (tf_call_frame){function, span};
    return TF_OK;
}

void tf_context_leave(tf_context *ctx) {
    /* Do not clear the backing entries: first fault borrows the active prefix,
     * and a faulted context never enters another frame. */
    if (ctx->depth != 0) --ctx->depth;
}

tf_value tf_unit(void) { return (tf_value){0}; }
tf_value tf_int(int64_t value) { return (tf_value){TF_VALUE_INT, {.integer = value}}; }
tf_value tf_bool(bool value) {
    return (tf_value){TF_VALUE_BOOL, {.boolean = (uint8_t)value}};
}

static bool unicode_scalar(uint32_t value) {
    return value <= UINT32_C(0x10ffff) &&
        (value < UINT32_C(0xd800) || value > UINT32_C(0xdfff));
}

static bool finite_float(uint64_t bits) {
    return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
}

static bool start_value(const tf_context *ctx, tf_value *out) {
    *out = tf_unit();
    return ctx->fault.code == 0;
}

tf_status tf_float(tf_context *ctx, tf_span_id span, uint64_t bits, tf_value *out) {
    if (!start_value(ctx, out)) return TF_FAULT;
    if (!finite_float(bits)) return tf_context_fail(ctx, TF_R001, span);
    if (bits == UINT64_C(0x8000000000000000)) bits = 0;
    *out = (tf_value){TF_VALUE_FLOAT, {.float_bits = bits}};
    return TF_OK;
}

tf_status tf_char(tf_context *ctx, tf_span_id span, uint32_t scalar, tf_value *out) {
    if (!start_value(ctx, out)) return TF_FAULT;
    if (!unicode_scalar(scalar)) return tf_context_fail(ctx, TF_R004, span);
    *out = (tf_value){TF_VALUE_CHAR, {.character = scalar}};
    return TF_OK;
}

static tf_object *box(const tf_context *ctx, tf_value value) {
    if (value.tag != TF_VALUE_BOXED || !value.as.object) return NULL;
    return value.as.object->owner == ctx ? value.as.object : NULL;
}

/* Representation validation and scalar type lookup are separate: unused
 * scalar types may be absent from a verified program's table. */
static bool valid_inline(tf_value value) {
    switch (value.tag) {
        case TF_VALUE_UNIT:
        case TF_VALUE_INT: return true;
        case TF_VALUE_BOOL: return value.as.boolean <= 1;
        case TF_VALUE_CHAR: return unicode_scalar(value.as.character);
        case TF_VALUE_FLOAT:
            return finite_float(value.as.float_bits) &&
                value.as.float_bits != UINT64_C(0x8000000000000000);
        default: return false;
    }
}

tf_type_id tf_value_type(const tf_context *ctx, tf_value value) {
    tf_object *object = box(ctx, value);
    if (object) return object->type;
    return valid_inline(value) ? ctx->scalar_types[value.tag] : 0;
}

static tf_status retain_valid(tf_context *ctx, tf_span_id span, tf_value value, tf_value *out) {
    if (value.tag == TF_VALUE_BOXED) {
        tf_object *object = value.as.object;
        if (object->refcount == UINT64_MAX) return tf_context_fail(ctx, TF_R003, span);
        ++object->refcount;
    }
    *out = value;
    return TF_OK;
}

tf_status tf_retain(tf_context *ctx, tf_span_id span, tf_value value, tf_value *out) {
    if (!start_value(ctx, out)) return TF_FAULT;
    if (!valid_inline(value) && !box(ctx, value)) return tf_context_fail(ctx, TF_R004, span);
    return retain_valid(ctx, span, value, out);
}

static void drop(tf_value value, tf_object **pending) {
    if (value.tag == TF_VALUE_BOXED) {
        tf_object *object = value.as.object;
        if (--object->refcount == 0) {
            object->pending = *pending;
            *pending = object;
        }
    }
}

void tf_release(tf_value *owned) {
    tf_object *pending = NULL;
    tf_value value = *owned;
    *owned = tf_unit();
    drop(value, &pending);
    while (pending) {
        tf_object *object = pending;
        pending = object->pending;
        void *allocation;
        if (object->kind == TF_RT_TYPE_TEXT) {
            allocation = (text_object *)object;
        } else {
            children_object *children = (children_object *)object;
            for (size_t i = 0; i < object->count; ++i) drop(children->items[i], &pending);
            allocation = children;
        }
        tf_context *ctx = object->owner;
        ctx->live_bytes -= object->size;
        ctx->config.allocator.deallocate(ctx->config.allocator.user, allocation);
    }
}

static bool box_size(size_t count, size_t base, size_t stride, size_t *size) {
    if ((uintmax_t)count > INT64_MAX || count > (SIZE_MAX - base) / stride) return false;
    *size = base + count * stride;
    return true;
}

static void *allocate_box(tf_context *ctx, tf_span_id span, size_t size) {
    if (size <= ctx->config.byte_budget - ctx->live_bytes) {
        void *memory = ctx->config.allocator.allocate(ctx->config.allocator.user, size);
        if (memory) {
            ctx->live_bytes += size;
            return memory;
        }
    }
    (void)tf_context_fail(ctx, TF_R003, span);
    return NULL;
}

static tf_value boxed(tf_object *object) {
    return (tf_value){TF_VALUE_BOXED, {.object = object}};
}

static bool valid_utf8(const unsigned char *bytes, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned char lead = bytes[i++];
        if (lead < 0x80) continue;
        uint32_t scalar, minimum;
        size_t tail;
        if (lead >= 0xc2 && lead <= 0xdf) {
            scalar = lead & 0x1fu; minimum = 0x80; tail = 1;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            scalar = lead & 0x0fu; minimum = 0x800; tail = 2;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            scalar = lead & 0x07u; minimum = 0x10000; tail = 3;
        } else {
            return false;
        }
        if (tail > length - i) return false;
        while (tail != 0) {
            unsigned char next = bytes[i++];
            if ((next & 0xc0u) != 0x80u) return false;
            scalar = (scalar << 6) | (next & 0x3fu);
            --tail;
        }
        if (scalar < minimum || !unicode_scalar(scalar)) return false;
    }
    return true;
}

tf_status tf_text(tf_context *ctx, tf_span_id span, tf_type_id type,
                  const unsigned char *bytes, size_t length, tf_value *out) {
    if (!start_value(ctx, out)) return TF_FAULT;
    const tf_rt_type *entry = type_entry(ctx, type);
    if (!entry || entry->kind != TF_RT_TYPE_TEXT || (length != 0 && !bytes)) {
        return tf_context_fail(ctx, TF_R004, span);
    }
    size_t size;
    if (!box_size(length, sizeof(text_object), sizeof(unsigned char), &size)) {
        return tf_context_fail(ctx, TF_R003, span);
    }
    if (!valid_utf8(bytes, length)) return tf_context_fail(ctx, TF_R004, span);
    text_object *object = allocate_box(ctx, span, size);
    if (!object) return TF_FAULT;
    object->header = (tf_object){TF_RT_TYPE_TEXT, 1, type, 0, length, size, ctx, NULL};
    if (length != 0) memcpy(object->bytes, bytes, length);
    *out = boxed(&object->header);
    return TF_OK;
}

/* Nonzero repeated_type selects a vector; otherwise use the exact tuple
 * supplied by the selected record, machine, variant or function layout. */
static tf_status make_children(tf_context *ctx, tf_span_id span, tf_rt_type_kind kind,
                               tf_type_id type, size_t identity, tf_type_id repeated_type,
                               tf_rt_shape shape, const tf_value *items, size_t count,
                               tf_value *out) {
    if ((count != 0 && !items) ||
        (repeated_type == 0 && (shape.count != count || (count != 0 && !shape.types)))) {
        return tf_context_fail(ctx, TF_R004, span);
    }
    size_t size;
    if (!box_size(count, sizeof(children_object), sizeof(tf_value), &size)) {
        return tf_context_fail(ctx, TF_R003, span);
    }
    for (size_t i = 0; i < count; ++i) {
        tf_type_id expected = repeated_type != 0 ? repeated_type : shape.types[i];
        if (expected == 0 || tf_value_type(ctx, items[i]) != expected) {
            return tf_context_fail(ctx, TF_R004, span);
        }
    }

    children_object *object = allocate_box(ctx, span, size);
    if (!object) return TF_FAULT;
    object->header = (tf_object){kind, 1, type, identity, 0, size, ctx, NULL};
    tf_value owner = boxed(&object->header);
    for (size_t i = 0; i < count; ++i) {
        if (retain_valid(ctx, span, items[i], &object->items[i]) != TF_OK) {
            /* Count is the acquired prefix, excluding the failed retain. */
            tf_release(&owner);
            return TF_FAULT;
        }
        ++object->header.count;
    }
    *out = owner;
    return TF_OK;
}

tf_status tf_list(tf_context *ctx, tf_span_id span, tf_type_id type,
                  const tf_value *items, size_t count, tf_value *out) {
    if (!start_value(ctx, out)) return TF_FAULT;
    const tf_rt_type *entry = type_entry(ctx, type);
    if (!entry || entry->kind != TF_RT_TYPE_LIST || !type_entry(ctx, entry->as.element)) {
        return tf_context_fail(ctx, TF_R004, span);
    }
    return make_children(ctx, span, TF_RT_TYPE_LIST, type, 0, entry->as.element,
                         (tf_rt_shape){0}, items, count, out);
}

static tf_status make_fields(tf_context *ctx, tf_span_id span, tf_rt_type_kind kind,
                             tf_type_id type, const tf_value *items, size_t count,
                             tf_value *out) {
    if (!start_value(ctx, out)) return TF_FAULT;
    const tf_rt_type *entry = type_entry(ctx, type);
    if (!entry || entry->kind != kind) return tf_context_fail(ctx, TF_R004, span);
    return make_children(ctx, span, kind, type, 0, 0, entry->as.fields, items, count, out);
}

tf_status tf_record(tf_context *ctx, tf_span_id span, tf_type_id type,
                    const tf_value *fields, size_t count, tf_value *out) {
    return make_fields(ctx, span, TF_RT_TYPE_RECORD, type, fields, count, out);
}

tf_status tf_machine(tf_context *ctx, tf_span_id span, tf_type_id type,
                     const tf_value *cells, size_t count, tf_value *out) {
    return make_fields(ctx, span, TF_RT_TYPE_MACHINE, type, cells, count, out);
}

tf_status tf_variant(tf_context *ctx, tf_span_id span, tf_type_id type,
                     size_t tag, const tf_value *payloads, size_t count, tf_value *out) {
    if (!start_value(ctx, out)) return TF_FAULT;
    const tf_rt_type *entry = type_entry(ctx, type);
    if (!entry || entry->kind != TF_RT_TYPE_VARIANT ||
        tag >= entry->as.variants.count || !entry->as.variants.tags ||
        entry->as.variants.count > SIZE_MAX / sizeof(*entry->as.variants.tags)) {
        return tf_context_fail(ctx, TF_R004, span);
    }
    return make_children(ctx, span, TF_RT_TYPE_VARIANT, type, tag, 0,
                         entry->as.variants.tags[tag], payloads, count, out);
}

tf_status tf_closure(tf_context *ctx, tf_span_id span, tf_function_id function,
                     const tf_value *captures, size_t count, tf_value *out) {
    if (!start_value(ctx, out)) return TF_FAULT;
    const tf_rt_function *fn = function_entry(ctx, function);
    if (!fn) return tf_context_fail(ctx, TF_R004, span);
    return make_children(ctx, span, TF_RT_TYPE_FUNCTION, fn->type, function, 0,
                         fn->captures, captures, count, out);
}

tf_status tf_text_view(tf_context *ctx, tf_span_id span, tf_value text,
                       const unsigned char **bytes, size_t *length) {
    *bytes = NULL;
    *length = 0;
    if (ctx->fault.code != 0) return TF_FAULT;
    const tf_object *object = box(ctx, text);
    if (!object || object->kind != TF_RT_TYPE_TEXT) return tf_context_fail(ctx, TF_R004, span);
    *length = object->count;
    if (*length != 0) *bytes = ((const text_object *)object)->bytes;
    return TF_OK;
}

tf_status tf_list_length(tf_context *ctx, tf_span_id span, tf_value list, size_t *length) {
    *length = 0;
    if (ctx->fault.code != 0) return TF_FAULT;
    const tf_object *object = box(ctx, list);
    if (!object || object->kind != TF_RT_TYPE_LIST) return tf_context_fail(ctx, TF_R004, span);
    *length = object->count;
    return TF_OK;
}

tf_status tf_variant_tag(tf_context *ctx, tf_span_id span, tf_value variant, size_t *tag) {
    *tag = 0;
    if (ctx->fault.code != 0) return TF_FAULT;
    const tf_object *object = box(ctx, variant);
    if (!object || object->kind != TF_RT_TYPE_VARIANT) return tf_context_fail(ctx, TF_R004, span);
    *tag = object->identity;
    return TF_OK;
}

tf_status tf_closure_view(tf_context *ctx, tf_span_id span, tf_value closure,
                          tf_function_id *function, const tf_value **captures, size_t *count) {
    *function = 0;
    *captures = NULL;
    *count = 0;
    if (ctx->fault.code != 0) return TF_FAULT;
    const tf_object *object = box(ctx, closure);
    if (!object || object->kind != TF_RT_TYPE_FUNCTION) return tf_context_fail(ctx, TF_R004, span);
    *function = object->identity;
    *count = object->count;
    if (*count != 0) *captures = ((const children_object *)object)->items;
    return TF_OK;
}

static tf_status project(tf_context *ctx, tf_span_id span, tf_value value, tf_rt_type_kind kind,
                         size_t tag, size_t index, tf_value *out) {
    if (!start_value(ctx, out)) return TF_FAULT;
    const tf_object *object = box(ctx, value);
    if (!object || index >= object->count ||
        (kind == TF_RT_TYPE_UNUSED
            ? object->kind != TF_RT_TYPE_RECORD && object->kind != TF_RT_TYPE_MACHINE
            : object->kind != kind) ||
        (kind == TF_RT_TYPE_VARIANT && tag != object->identity)) {
        return tf_context_fail(ctx, TF_R004, span);
    }
    return retain_valid(ctx, span, ((const children_object *)object)->items[index], out);
}

tf_status tf_list_at(tf_context *ctx, tf_span_id span, tf_value list, size_t index,
                      tf_value *out) {
    return project(ctx, span, list, TF_RT_TYPE_LIST, 0, index, out);
}

tf_status tf_project(tf_context *ctx, tf_span_id span, tf_value aggregate, size_t index,
                     tf_value *out) {
    return project(ctx, span, aggregate, TF_RT_TYPE_UNUSED, 0, index, out);
}

tf_status tf_payload(tf_context *ctx, tf_span_id span, tf_value variant, size_t tag,
                     size_t index, tf_value *out) {
    return project(ctx, span, variant, TF_RT_TYPE_VARIANT, tag, index, out);
}

#ifdef TF_RT_TESTING
uint64_t tf_test_swap_refcount(tf_context *ctx, tf_value value, uint64_t count) {
    tf_object *object = box(ctx, value);
    if (!object || count == 0) return 0;
    uint64_t previous = object->refcount;
    object->refcount = count;
    return previous;
}
#endif

static TfTraceValueView trace_view(void *context, const void *input) {
    const tf_context *ctx=context;
    const tf_value *v=input;
    TfTraceValueView view={0};
    switch (v->tag) {
    case TF_VALUE_UNIT: view.kind=TF_TRACE_VALUE_UNIT; break;
    case TF_VALUE_INT: view.kind=TF_TRACE_VALUE_INT; memcpy(&view.bits,&v->as.integer,8); break;
    case TF_VALUE_FLOAT: view.kind=TF_TRACE_VALUE_FLOAT; view.bits=v->as.float_bits; break;
    case TF_VALUE_BOOL: view.kind=TF_TRACE_VALUE_BOOL; view.bits=v->as.boolean; break;
    case TF_VALUE_CHAR: view.kind=TF_TRACE_VALUE_CHAR; view.bits=v->as.character; break;
    case TF_VALUE_BOXED: {
        const tf_object *o=box(ctx,*v);
        if (!o) break; /* Fabricated values are outside the adapter contract. */
        if (o->kind==TF_RT_TYPE_TEXT) {
            view.kind=TF_TRACE_VALUE_TEXT; view.bytes=((const text_object *)o)->bytes; view.count=o->count;
        } else {
            view.kind=o->kind==TF_RT_TYPE_LIST ? TF_TRACE_VALUE_LIST :
                      o->kind==TF_RT_TYPE_RECORD ? TF_TRACE_VALUE_RECORD :
                      o->kind==TF_RT_TYPE_MACHINE ? TF_TRACE_VALUE_MACHINE :
                      o->kind==TF_RT_TYPE_FUNCTION ? TF_TRACE_VALUE_FUNCTION : TF_TRACE_VALUE_VARIANT;
            view.tag=o->identity; view.count=o->count;
        }
        break;
    }
    }
    return view;
}
static const void *trace_child(void *context, const void *input, size_t index) {
    const tf_value *v=input;
    const tf_object *o=box(context,*v);
    return &((const children_object *)o)->items[index];
}
TfTraceValueAdapter tf_runtime_trace_adapter(tf_context *ctx) {
    return (TfTraceValueAdapter){ctx,trace_view,trace_child};
}
