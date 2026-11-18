#include "ternfold_rt_builtins.h"
#include "scalars.h"
#include "ternfold/checked_numeric.h"

#include <stdlib.h>

static const tf_rt_type *entry(const tf_builtin_env *env, tf_type_id id) {
    if (id == 0 || id > env->program->type_count) return NULL;
    return &env->program->types[id - 1];
}

static bool kind(const tf_builtin_env *env, tf_type_id id, tf_rt_type_kind expected) {
    const tf_rt_type *type = entry(env, id);
    return type && type->kind == expected;
}

static tf_type_id option_element(const tf_builtin_env *env, tf_type_id id) {
    const tf_rt_type *type = entry(env, id);
    if (!type || type->kind != TF_RT_TYPE_VARIANT ||
        type->as.variants.count != 2 || !type->as.variants.tags) return 0;
    const tf_rt_shape *tags = type->as.variants.tags;
    if (tags[0].count != 1 || !tags[0].types || tags[1].count != 0) return 0;
    return entry(env, tags[0].types[0]) ? tags[0].types[0] : 0;
}

static tf_status option(tf_context *ctx, tf_span_id span, tf_type_id type,
                         const tf_value *some, tf_value *out) {
    return tf_variant(ctx, span, type, some ? 0u : 1u, some, some ? 1u : 0u, out);
}

static bool is_index(int64_t index, size_t length) {
    return index >= 0 && (uint64_t)index < (uintmax_t)length;
}

static void *scratch(tf_context *ctx, const tf_builtin_env *env,
                     tf_span_id span, size_t count) {
    if ((uintmax_t)count > INT64_MAX || count > SIZE_MAX / sizeof(tf_value) ||
        count * sizeof(tf_value) > env->scratch_byte_limit) {
        (void)tf_context_fail(ctx, TF_R003, span);
        return NULL;
    }
    const size_t bytes = count * sizeof(tf_value);
    if (ctx->live_bytes > ctx->config.byte_budget ||
        bytes > ctx->config.byte_budget - ctx->live_bytes) {
        (void)tf_context_fail(ctx, TF_R003, span);
        return NULL;
    }
    void *p = env->scratch_allocator.allocate
        ? env->scratch_allocator.allocate(env->scratch_allocator.user, bytes)
        : malloc(bytes);
    if (p) ctx->live_bytes += bytes; /* checked against the remaining budget */
    else (void)tf_context_fail(ctx, TF_R003, span);
    return p;
}

static void scratch_free(tf_context *ctx, const tf_builtin_env *env, void *p, size_t count) {
    if (!p) return;
    /* count is the same checked extent that scratch successfully allocated. */
    ctx->live_bytes -= count * sizeof(tf_value);
    if (env->scratch_allocator.deallocate)
        env->scratch_allocator.deallocate(env->scratch_allocator.user, p);
    else free(p);
}

static tf_status append(tf_context *ctx, const tf_builtin_env *env,
                         tf_span_id span, tf_type_id type,
                         const tf_value *args, tf_value *out) {
    size_t count;
    if (tf_list_length(ctx, span, args[0], &count) != TF_OK) return TF_FAULT;
    if ((uintmax_t)count >= INT64_MAX || count == SIZE_MAX)
        return tf_context_fail(ctx, TF_R003, span);
    tf_value *items = scratch(ctx, env, span, count + 1);
    if (!items) return TF_FAULT;
    size_t acquired = 0;
    tf_status status = TF_FAULT;
    for (; acquired < count; ++acquired) {
        if (tf_list_at(ctx, span, args[0], acquired, &items[acquired]) != TF_OK)
            goto cleanup;
    }
    if (tf_retain(ctx, span, args[1], &items[acquired]) != TF_OK) goto cleanup;
    ++acquired;
    status = tf_list(ctx, span, type, items, acquired, out);
cleanup:
    while (acquired != 0) tf_release(&items[--acquired]);
    scratch_free(ctx, env, items, count + 1);
    return status;
}

static tf_status range(tf_context *ctx, const tf_builtin_env *env,
                        tf_span_id span, tf_type_id type,
                        const tf_value *args, tf_value *out) {
    const int64_t start = args[0].as.integer, end = args[1].as.integer;
    if (start >= end) return tf_list(ctx, span, type, NULL, 0, out);
    /* Unsigned subtraction gives the exact positive difference even across
     * zero, including IntMin..IntMax; no signed subtraction occurs. */
    const uint64_t distance = (uint64_t)end - (uint64_t)start;
    if (distance > INT64_MAX || distance > SIZE_MAX)
        return tf_context_fail(ctx, TF_R003, span);
    const size_t count = (size_t)distance;
    tf_value *items = scratch(ctx, env, span, count);
    if (!items) return TF_FAULT;
    int64_t value = start;
    for (size_t i = 0; i < count; ++i) {
        items[i] = tf_int(value);
        /* value < end on every iteration, so this cannot overflow. */
        if (i + 1 < count) ++value;
    }
    const tf_status status = tf_list(ctx, span, type, items, count, out);
    scratch_free(ctx, env, items, count);
    return status;
}

/* Text views are already validated UTF-8 by tf_text. Scan scalar boundaries
 * only, preserving all bytes including NUL and combining characters. */
static size_t next_scalar(const unsigned char *bytes, size_t at) {
    const unsigned char lead = bytes[at];
    return at + (lead < 0x80 ? 1u : lead < 0xe0 ? 2u : lead < 0xf0 ? 3u : 4u);
}

static tf_status text_operation(tf_context *ctx, tf_span_id span,
                                 tf_builtin builtin, tf_type_id type,
                                 tf_type_id payload_type, const tf_value *args,
                                 tf_value *out) {
    const unsigned char *bytes;
    size_t length;
    if (tf_text_view(ctx, span, args[0], &bytes, &length) != TF_OK) return TF_FAULT;
    size_t scalars = 0;
    for (size_t at = 0; at < length; ++scalars) at = next_scalar(bytes, at);
    if (builtin == TF_BUILTIN_TEXT_LEN) {
        if ((uintmax_t)scalars > INT64_MAX) return tf_context_fail(ctx, TF_R003, span);
        *out = tf_int((int64_t)scalars);
        return TF_OK;
    }
    const int64_t first = args[1].as.integer;
    size_t start, end;
    if (builtin == TF_BUILTIN_TEXT_AT) {
        if (!is_index(first, scalars)) return option(ctx, span, type, NULL, out);
        start = (size_t)first;
        end = start + 1;
    } else {
        const int64_t last = args[2].as.integer;
        if (first < 0 || last < first || (uint64_t)last > (uintmax_t)scalars)
            return option(ctx, span, type, NULL, out);
        start = (size_t)first;
        end = (size_t)last;
    }
    size_t begin_byte = 0;
    for (size_t i = 0; i < start; ++i) begin_byte = next_scalar(bytes, begin_byte);
    size_t end_byte = begin_byte;
    for (size_t i = start; i < end; ++i) end_byte = next_scalar(bytes, end_byte);
    tf_value text = tf_unit();
    if (tf_text(ctx, span, payload_type, end_byte != begin_byte ? bytes + begin_byte : NULL,
                end_byte - begin_byte, &text) != TF_OK) return TF_FAULT;
    const tf_status status = option(ctx, span, type, &text, out);
    tf_release(&text);
    return status;
}

tf_status tf_builtin_call(tf_context *ctx, const tf_builtin_env *env,
                          tf_span_id span, tf_builtin builtin,
                          tf_type_id result_type, const tf_value *args,
                          size_t argc, tf_value *out) {
    *out = tf_unit();
    if (tf_context_fault(ctx)) return TF_FAULT;
    if (!env || !env->program ||
        (env->program->type_count != 0 && !env->program->types) ||
        env->program->type_count > SIZE_MAX / sizeof(tf_rt_type) ||
        (!!env->scratch_allocator.allocate != !!env->scratch_allocator.deallocate))
        return tf_context_fail(ctx, TF_R004, span);
    size_t arity;
    switch (builtin) {
        case TF_BUILTIN_TEXT_SLICE: arity = 3; break;
        case TF_BUILTIN_AT: case TF_BUILTIN_APPEND: case TF_BUILTIN_RANGE:
        case TF_BUILTIN_TEXT_AT: arity = 2; break;
        case TF_BUILTIN_LEN: case TF_BUILTIN_INT_TEXT: case TF_BUILTIN_INT_FLOAT:
        case TF_BUILTIN_FLOAT_INT: case TF_BUILTIN_FLOAT_TEXT: case TF_BUILTIN_BOOL_TEXT:
        case TF_BUILTIN_PARSE_INT: case TF_BUILTIN_TEXT_LEN: arity = 1; break;
        default: return tf_context_fail(ctx, TF_R004, span);
    }
    if (argc != arity || !args) return tf_context_fail(ctx, TF_R004, span);
    tf_type_id types[3] = {0};
    for (size_t i = 0; i < argc; ++i) {
        types[i] = tf_value_type(ctx, args[i]);
        if (types[i] == 0) return tf_context_fail(ctx, TF_R004, span);
    }
    const tf_type_id payload = option_element(env, result_type);
    bool valid = false;
    switch (builtin) {
        case TF_BUILTIN_LEN:
            valid = kind(env, types[0], TF_RT_TYPE_LIST) && kind(env, result_type, TF_RT_TYPE_INT);
            break;
        case TF_BUILTIN_AT:
            valid = kind(env, types[0], TF_RT_TYPE_LIST) && kind(env, types[1], TF_RT_TYPE_INT) &&
                payload != 0 && payload == entry(env, types[0])->as.element;
            break;
        case TF_BUILTIN_APPEND:
            valid = kind(env, types[0], TF_RT_TYPE_LIST) && result_type == types[0] &&
                types[1] == entry(env, types[0])->as.element;
            break;
        case TF_BUILTIN_RANGE:
            valid = kind(env, types[0], TF_RT_TYPE_INT) && types[1] == types[0] &&
                kind(env, result_type, TF_RT_TYPE_LIST) &&
                entry(env, result_type)->as.element == types[0];
            break;
        case TF_BUILTIN_INT_TEXT: case TF_BUILTIN_INT_FLOAT:
            valid = kind(env, types[0], TF_RT_TYPE_INT) && kind(env, result_type,
                builtin == TF_BUILTIN_INT_TEXT ? TF_RT_TYPE_TEXT : TF_RT_TYPE_FLOAT);
            break;
        case TF_BUILTIN_FLOAT_INT:
            valid = kind(env, types[0], TF_RT_TYPE_FLOAT) && kind(env, payload, TF_RT_TYPE_INT);
            break;
        case TF_BUILTIN_FLOAT_TEXT:
            valid = kind(env, types[0], TF_RT_TYPE_FLOAT) && kind(env, result_type, TF_RT_TYPE_TEXT);
            break;
        case TF_BUILTIN_BOOL_TEXT:
            valid = kind(env, types[0], TF_RT_TYPE_BOOL) && kind(env, result_type, TF_RT_TYPE_TEXT);
            break;
        case TF_BUILTIN_PARSE_INT:
            valid = kind(env, types[0], TF_RT_TYPE_TEXT) && kind(env, payload, TF_RT_TYPE_INT);
            break;
        case TF_BUILTIN_TEXT_LEN:
            valid = kind(env, types[0], TF_RT_TYPE_TEXT) && kind(env, result_type, TF_RT_TYPE_INT);
            break;
        case TF_BUILTIN_TEXT_AT: case TF_BUILTIN_TEXT_SLICE:
            valid = kind(env, types[0], TF_RT_TYPE_TEXT) && kind(env, types[1], TF_RT_TYPE_INT) &&
                kind(env, payload, TF_RT_TYPE_TEXT) &&
                (arity == 2 || types[2] == types[1]);
            break;
    }
    if (!valid) return tf_context_fail(ctx, TF_R004, span);
    switch (builtin) {
        case TF_BUILTIN_LEN: {
            size_t length;
            if (tf_list_length(ctx, span, args[0], &length) != TF_OK) return TF_FAULT;
            if ((uintmax_t)length > INT64_MAX) return tf_context_fail(ctx, TF_R003, span);
            *out = tf_int((int64_t)length);
            return TF_OK;
        }
        case TF_BUILTIN_AT: {
            size_t length;
            if (tf_list_length(ctx, span, args[0], &length) != TF_OK) return TF_FAULT;
            if (!is_index(args[1].as.integer, length)) return option(ctx, span, result_type, NULL, out);
            tf_value item = tf_unit();
            if (tf_list_at(ctx, span, args[0], (size_t)args[1].as.integer, &item) != TF_OK)
                return TF_FAULT;
            const tf_status status = option(ctx, span, result_type, &item, out);
            tf_release(&item);
            return status;
        }
        case TF_BUILTIN_APPEND: return append(ctx, env, span, result_type, args, out);
        case TF_BUILTIN_RANGE: return range(ctx, env, span, result_type, args, out);
        case TF_BUILTIN_INT_FLOAT:
            return tf_float(ctx, span, tf_checked_int_to_float_bits(args[0].as.integer), out);
        case TF_BUILTIN_INT_TEXT: case TF_BUILTIN_FLOAT_TEXT: {
            char bytes[TF_EXEC_FLOAT_TEXT_CAPACITY];
            const size_t length = builtin == TF_BUILTIN_INT_TEXT
                ? tf_exec_int_text(args[0].as.integer, bytes)
                : tf_exec_float_text(args[0].as.float_bits, bytes);
            return tf_text(ctx, span, result_type, (const unsigned char *)bytes, length, out);
        }
        case TF_BUILTIN_BOOL_TEXT: {
            static const unsigned char yes[] = {0x74, 0x72, 0x75, 0x65};
            static const unsigned char no[] = {0x66, 0x61, 0x6c, 0x73, 0x65};
            return tf_text(ctx, span, result_type, args[0].as.boolean ? yes : no,
                           args[0].as.boolean ? sizeof(yes) : sizeof(no), out);
        }
        case TF_BUILTIN_FLOAT_INT: case TF_BUILTIN_PARSE_INT: {
            int64_t value;
            bool some;
            if (builtin == TF_BUILTIN_FLOAT_INT) some = tf_exec_float_int(args[0].as.float_bits, &value);
            else {
                const unsigned char *bytes;
                size_t length;
                if (tf_text_view(ctx, span, args[0], &bytes, &length) != TF_OK) return TF_FAULT;
                some = tf_exec_parse_int(bytes, length, &value);
            }
            const tf_value item = some ? tf_int(value) : tf_unit();
            return option(ctx, span, result_type, some ? &item : NULL, out);
        }
        case TF_BUILTIN_TEXT_LEN: case TF_BUILTIN_TEXT_AT: case TF_BUILTIN_TEXT_SLICE:
            return text_operation(ctx, span, builtin, result_type, payload, args, out);
    }
    return tf_context_fail(ctx, TF_R004, span);
}
