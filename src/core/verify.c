#include "ternfold/core_verify.h"

#include "../internal.h"
#include "primitive_rules.h"

#include <string.h>

struct TfVerifiedCore {
    const TfCoreProgram *program;
    TfAllocator memory;
};

typedef struct {
    const TfCoreProgramData *data;
    TfAllocator memory;
    TfCoreVerifyIssue at;
    bool resource_failure;
    unsigned char *type_flags;
} Verify;

enum { VISITING = 1, COMPLETE = 2, DATA = 4, EQUALITY = 8 };

static bool reject(Verify *v, TfCoreVerifyCode code) {
    v->at.code = code;
    return false;
}

static void *workspace(Verify *v, size_t count, size_t width) {
    void *p;
    if (count == 0) return NULL;
    if (count <= SIZE_MAX / width) {
        p = v->memory.allocate(v->memory.context, count * width);
        if (p != NULL) return p;
    }
    v->resource_failure = true;
    return NULL;
}

static void release(const Verify *v, void *p) {
    if (p != NULL) v->memory.deallocate(v->memory.context, p);
}

static bool span_id(Verify *v, TfCoreSpanId id) {
    v->at.span = id;
    return id <= v->data->span_count || reject(v, TF_CORE_VERIFY_SPAN);
}

static bool utf8(TfCoreText text) {
    size_t end;
    uint32_t scalar;
    for (size_t at = 0; at < text.length; at = end)
        if (!tf_decode(text.bytes, text.length, at, &scalar, &end)) return false;
    return true;
}

static const TfCoreType *type(const Verify *v, TfCoreTypeId id) {
    return tf_core_type(v->data, id);
}

static bool kind(const Verify *v, TfCoreTypeId id, TfCoreTypeKind wanted) {
    const TfCoreType *t = type(v, id);
    return t != NULL && t->kind == wanted;
}

static bool value_type(const Verify *v, TfCoreTypeId id) {
    const TfCoreType *t = type(v, id);
    return t != NULL && t->kind != TF_CORE_TYPE_TAG;
}

static bool collection(const Verify *v, TfCoreTypeId id, TfCoreTypeKind wanted,
                       TfCoreTypeId element) {
    const TfCoreType *t = type(v, id);
    return t != NULL && t->kind == wanted && t->as.element == element;
}

static bool collection_kind(const Verify *v, TfCoreTypeId id, TfCoreTypeKind wanted,
                            TfCoreTypeKind element) {
    const TfCoreType *t = type(v, id);
    return t != NULL && t->kind == wanted && kind(v, t->as.element, element);
}

static size_t tag_count(const TfCoreType *t) {
    switch (t->kind) {
    case TF_CORE_TYPE_OPTION: case TF_CORE_TYPE_DECISION: return 2;
    case TF_CORE_TYPE_ENUM: return t->as.enumeration.variant_count;
    default: return 0;
    }
}

/* The caller checks tag < tag_count before using these layout helpers. */
static size_t payload_count(const TfCoreType *t, size_t tag) {
    if (t->kind == TF_CORE_TYPE_OPTION) return tag == TF_CORE_OPTION_SOME ? 1u : 0u;
    if (t->kind == TF_CORE_TYPE_DECISION) return 2;
    return t->as.enumeration.variants[tag].payloads.count;
}

static TfCoreTypeId payload_type(const TfCoreType *t, size_t tag, size_t index) {
    if (t->kind == TF_CORE_TYPE_OPTION) return t->as.element;
    if (t->kind == TF_CORE_TYPE_DECISION) {
        if (index == 0) return t->as.decision.machine;
        return tag == TF_CORE_DECISION_ACCEPT ? t->as.decision.emissions
                                               : t->as.decision.rejection;
    }
    return t->as.enumeration.variants[tag].payloads.items[index];
}

static const TfCoreField *fields(const TfCoreType *t, size_t *count) {
    *count = 0;
    if (t->kind == TF_CORE_TYPE_RECORD) {
        *count = t->as.record.field_count;
        return t->as.record.fields;
    }
    if (t->kind == TF_CORE_TYPE_MACHINE) {
        *count = t->as.machine.cell_count;
        return t->as.machine.cells;
    }
    return NULL;
}

static bool type_shape(Verify *v, const TfCoreType *t) {
    size_t count;
    const TfCoreField *layout;
    if (!span_id(v, t->span)) return false;
    if (!utf8(t->name) || !utf8(t->module)) return reject(v, TF_CORE_VERIFY_TYPE);
    switch (t->kind) {
    case TF_CORE_TYPE_INT: case TF_CORE_TYPE_FLOAT: case TF_CORE_TYPE_BOOL:
    case TF_CORE_TYPE_TEXT: case TF_CORE_TYPE_CHAR: case TF_CORE_TYPE_UNIT:
    case TF_CORE_TYPE_LIST: case TF_CORE_TYPE_OPTION: case TF_CORE_TYPE_FUNCTION:
        return true;
    case TF_CORE_TYPE_TAG: {
        const TfCoreType *subject = type(v, t->as.tagged);
        return (subject != NULL && tag_count(subject) != 0) ||
               reject(v, TF_CORE_VERIFY_TYPE);
    }
    case TF_CORE_TYPE_DECISION: {
        const TfCoreType *m = type(v, t->as.decision.machine);
        return (m != NULL && m->kind == TF_CORE_TYPE_MACHINE &&
                m->as.machine.output == t->as.decision.output &&
                m->as.machine.rejection == t->as.decision.rejection &&
                collection(v, t->as.decision.emissions, TF_CORE_TYPE_LIST,
                           t->as.decision.output)) || reject(v, TF_CORE_VERIFY_TYPE);
    }
    case TF_CORE_TYPE_RECORD: case TF_CORE_TYPE_MACHINE:
        layout = fields(t, &count);
        if (t->kind == TF_CORE_TYPE_MACHINE && count == 0)
            return reject(v, TF_CORE_VERIFY_TYPE);
        for (size_t i = 0; i < count; ++i) {
            if (!span_id(v, layout[i].span)) return false;
            if (!utf8(layout[i].name)) return reject(v, TF_CORE_VERIFY_TYPE);
        }
        return true;
    case TF_CORE_TYPE_ENUM:
        count = tag_count(t);
        if (count == 0) return reject(v, TF_CORE_VERIFY_TYPE);
        for (size_t i = 0; i < count; ++i) {
            const TfCoreVariant *variant = &t->as.enumeration.variants[i];
            if (!span_id(v, variant->span)) return false;
            if (!utf8(variant->name)) return reject(v, TF_CORE_VERIFY_TYPE);
        }
        return true;
    default:
        return reject(v, TF_CORE_VERIFY_TYPE);
    }
}

typedef struct {
    TfCoreTypeId id;
    size_t index, payload;
    unsigned char capabilities;
    bool waiting, content;
    TfCoreTypeId child;
} TypeFrame;

/* All edges affect acyclicity. Only actual contents (not machine signature
 * types, for example) affect data/equality capability. */
static bool type_edge(const TfCoreType *t, TypeFrame *f) {
    size_t count;
    const TfCoreField *layout;
    const size_t i = f->index;
    f->content = false;
    switch (t->kind) {
    case TF_CORE_TYPE_LIST: case TF_CORE_TYPE_OPTION:
        if (i != 0) return false;
        f->child = t->as.element;
        f->content = true;
        break;
    case TF_CORE_TYPE_TAG:
        if (i != 0) return false;
        f->child = t->as.tagged;
        break;
    case TF_CORE_TYPE_DECISION:
        switch (i) {
        case 0: f->child = t->as.decision.machine; f->content = true; break;
        case 1: f->child = t->as.decision.output; break;
        case 2: f->child = t->as.decision.rejection; f->content = true; break;
        case 3: f->child = t->as.decision.emissions; f->content = true; break;
        default: return false;
        }
        break;
    case TF_CORE_TYPE_RECORD: case TF_CORE_TYPE_MACHINE:
        layout = fields(t, &count);
        if (i < count) {
            f->child = layout[i].type;
            f->content = true;
        } else {
            if (t->kind != TF_CORE_TYPE_MACHINE) return false;
            switch (i - count) {
            case 0: f->child = t->as.machine.input; break;
            case 1: f->child = t->as.machine.output; break;
            case 2: f->child = t->as.machine.rejection; break;
            default: return false;
            }
        }
        break;
    case TF_CORE_TYPE_FUNCTION:
        count = t->as.function.parameters.count;
        if (i > count) return false;
        f->child = i == count ? t->as.function.result : t->as.function.parameters.items[i];
        break;
    case TF_CORE_TYPE_ENUM:
        while (f->index < tag_count(t)) {
            if (f->payload < payload_count(t, f->index)) {
                f->child = payload_type(t, f->index, f->payload++);
                f->content = true;
                return true;
            }
            ++f->index;
            f->payload = 0;
        }
        return false;
    default:
        return false;
    }
    ++f->index;
    return true;
}

static void type_push(Verify *v, TypeFrame *frame, TfCoreTypeId id) {
    const TfCoreTypeKind k = type(v, id)->kind;
    v->type_flags[id - 1] = VISITING;
    *frame = (TypeFrame){.id = id,
        .capabilities = (k == TF_CORE_TYPE_FUNCTION || k == TF_CORE_TYPE_TAG)
                        ? 0 : DATA | EQUALITY};
}

static bool type_graph(Verify *v, TypeFrame *stack) {
    const size_t count = v->data->type_count;
    for (size_t i = 0; i < count; ++i) {
        size_t depth = 0;
        if (v->type_flags[i] != 0) continue;
        type_push(v, &stack[depth++], i + 1);
        while (depth != 0) {
            TypeFrame *f = &stack[depth - 1];
            const TfCoreType *t = type(v, f->id);
            v->at = (TfCoreVerifyIssue){.type = f->id, .span = t->span};
            if (f->waiting) {
                const unsigned char child = v->type_flags[f->child - 1];
                if (f->content) {
                    if (t->kind == TF_CORE_TYPE_MACHINE && (child & DATA) == 0)
                        return reject(v, TF_CORE_VERIFY_TYPE);
                    f->capabilities &= child;
                }
                f->waiting = false;
            }
            if (!type_edge(t, f)) {
                v->type_flags[f->id - 1] = (unsigned char)(COMPLETE | f->capabilities);
                --depth;
                continue;
            }
            if (!value_type(v, f->child)) return reject(v, TF_CORE_VERIFY_TYPE);
            f->waiting = true;
            if (v->type_flags[f->child - 1] == VISITING)
                return reject(v, TF_CORE_VERIFY_TYPE_CYCLE);
            if (v->type_flags[f->child - 1] == 0)
                type_push(v, &stack[depth++], f->child);
        }
    }
    return true;
}

typedef struct {
    size_t first, second;
    size_t exclusions;
    const TfCoreOperation *marked_by;
} Row;

typedef int (*CompareRows)(const Verify *, const Row *, const Row *);

static int order(size_t a, size_t b) {
    return (a > b) - (a < b);
}

/* Bottom-up merges give a worst-case bound even for adversarial keys. The
 * function signature comparator compares arity before examining parameters,
 * so a long comparison can be charged to either row emitted by the merge. */
static bool sort_rows(Verify *v, Row *rows, size_t count, CompareRows compare) {
    Row *scratch, *from = rows, *to;
    if (count < 2) return true;
    scratch = workspace(v, count, sizeof(*scratch));
    if (scratch == NULL) return false;
    to = scratch;
    for (size_t width = 1; width < count;) {
        size_t start = 0;
        while (start < count) {
            const size_t middle = count - start < width ? count : start + width;
            const size_t end = count - middle < width ? count : middle + width;
            size_t left = start, right = middle;
            for (size_t i = start; i < end; ++i) {
                if (right == end || (left < middle && compare(v, &from[left], &from[right]) <= 0))
                    to[i] = from[left++];
                else
                    to[i] = from[right++];
            }
            start = end;
        }
        Row *swap = from;
        from = to;
        to = swap;
        if (width > count / 2) break;
        width *= 2;
    }
    if (from != rows) memcpy(rows, from, count * sizeof(*rows));
    release(v, scratch);
    return true;
}

static bool nominal(TfCoreTypeKind k) {
    return k == TF_CORE_TYPE_RECORD || k == TF_CORE_TYPE_ENUM || k == TF_CORE_TYPE_MACHINE;
}

static int compare_types(const Verify *v, const Row *left, const Row *right) {
    const TfCoreType *a = type(v, left->first), *b = type(v, right->first);
    int cmp = order((size_t)a->kind, (size_t)b->kind);
    if (cmp != 0) return cmp;
    switch (a->kind) {
    case TF_CORE_TYPE_LIST: case TF_CORE_TYPE_OPTION:
        return order(a->as.element, b->as.element);
    case TF_CORE_TYPE_TAG: return order(a->as.tagged, b->as.tagged);
    case TF_CORE_TYPE_DECISION:
        /* Checked layout already fixes output/rejection/emissions for M. */
        return order(a->as.decision.machine, b->as.decision.machine);
    case TF_CORE_TYPE_FUNCTION:
        cmp = order(a->as.function.parameters.count, b->as.function.parameters.count);
        if (cmp != 0) return cmp;
        cmp = order(a->as.function.result, b->as.function.result);
        if (cmp != 0) return cmp;
        for (size_t i = 0; i < a->as.function.parameters.count; ++i) {
            cmp = order(a->as.function.parameters.items[i], b->as.function.parameters.items[i]);
            if (cmp != 0) return cmp;
        }
        return 0;
    default:
        return 0;
    }
}

/* Nominal IDs are distinct even if all their display data matches. */
static bool canonical_types(Verify *v) {
    const size_t count = v->data->type_count;
    size_t n = 0;
    TfCoreTypeId duplicate = 0;
    Row *rows = workspace(v, count, sizeof(*rows));
    bool ok;
    if (rows == NULL) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!nominal(v->data->types[i].kind)) rows[n++] = (Row){.first = i + 1};
    }
    ok = sort_rows(v, rows, n, compare_types);
    if (ok) {
        for (size_t i = 1; i < n; ++i)
            if ((duplicate == 0 || rows[i].first < duplicate) &&
                compare_types(v, &rows[i - 1], &rows[i]) == 0)
                duplicate = rows[i].first;
    }
    release(v, rows);
    if (duplicate != 0) {
        v->at = (TfCoreVerifyIssue){.type = duplicate, .span = type(v, duplicate)->span};
        return reject(v, TF_CORE_VERIFY_TYPE);
    }
    return ok;
}

static bool verify_types(Verify *v) {
    const size_t count = v->data->type_count;
    TypeFrame *stack;
    bool ok;
    for (size_t i = 0; i < count; ++i) {
        const TfCoreType *t = &v->data->types[i];
        v->at = (TfCoreVerifyIssue){.type = i + 1};
        if (!type_shape(v, t)) return false;
    }
    if (count == 0) return true;
    if (!canonical_types(v)) return false;
    v->type_flags = workspace(v, count, sizeof(*v->type_flags));
    if (v->type_flags == NULL) return false;
    memset(v->type_flags, 0, count);
    stack = workspace(v, count, sizeof(*stack));
    if (stack == NULL) return false;
    ok = type_graph(v, stack);
    release(v, stack);
    return ok;
}

static bool constant_child(const Verify *v, TfCoreConstantId id, TfCoreTypeId expected) {
    const TfCoreConstant *c = tf_core_constant(v->data, id);
    return c != NULL && c->type == expected;
}

static bool constant_layout(const Verify *v, const TfCoreConstant *c, const TfCoreType *t) {
    TfCoreConstantIds children;
    const TfCoreField *layout;
    size_t count;
    switch (c->kind) {
    case TF_CORE_CONSTANT_INT: return t->kind == TF_CORE_TYPE_INT;
    case TF_CORE_CONSTANT_FLOAT:
        return t->kind == TF_CORE_TYPE_FLOAT &&
               (c->as.float_bits & UINT64_C(0x7ff0000000000000)) !=
                                  UINT64_C(0x7ff0000000000000) &&
               c->as.float_bits != UINT64_C(0x8000000000000000);
    case TF_CORE_CONSTANT_BOOL: return t->kind == TF_CORE_TYPE_BOOL && c->as.boolean <= 1;
    case TF_CORE_CONSTANT_TEXT: return t->kind == TF_CORE_TYPE_TEXT && utf8(c->as.text);
    case TF_CORE_CONSTANT_CHAR:
        return t->kind == TF_CORE_TYPE_CHAR && c->as.character <= UINT32_C(0x10ffff) &&
               (c->as.character < UINT32_C(0xd800) || c->as.character > UINT32_C(0xdfff));
    case TF_CORE_CONSTANT_UNIT: return t->kind == TF_CORE_TYPE_UNIT;
    case TF_CORE_CONSTANT_LIST:
        if (t->kind != TF_CORE_TYPE_LIST) return false;
        children = c->as.children;
        for (size_t i = 0; i < children.count; ++i)
            if (!constant_child(v, children.items[i], t->as.element)) return false;
        return true;
    case TF_CORE_CONSTANT_RECORD:
        if (t->kind != TF_CORE_TYPE_RECORD) return false;
        layout = fields(t, &count);
        children = c->as.children;
        if (children.count != count) return false;
        for (size_t i = 0; i < count; ++i)
            if (!constant_child(v, children.items[i], layout[i].type)) return false;
        return true;
    case TF_CORE_CONSTANT_VARIANT: {
        const size_t tag = c->as.variant.tag;
        children = c->as.variant.payloads;
        if (tag >= tag_count(t) || children.count != payload_count(t, tag)) return false;
        for (size_t i = 0; i < children.count; ++i)
            if (!constant_child(v, children.items[i], payload_type(t, tag, i))) return false;
        return true;
    }
    default:
        return false;
    }
}

static bool verify_constants(Verify *v) {
    /* Each accepted child follows a checked edge of the acyclic type graph,
     * so the entire constant pool is acyclic, including forward references. */
    for (size_t i = 0; i < v->data->constant_count; ++i) {
        const TfCoreConstant *c = &v->data->constants[i];
        const TfCoreType *t = type(v, c->type);
        v->at = (TfCoreVerifyIssue){.constant = i + 1};
        if (!span_id(v, c->span)) return false;
        if (t == NULL || !constant_layout(v, c, t)) return reject(v, TF_CORE_VERIFY_CONSTANT);
    }
    return true;
}

static bool parameter(Verify *v, const TfCoreParameter *p) {
    if (!span_id(v, p->span)) return false;
    return (value_type(v, p->type) && utf8(p->name)) || reject(v, TF_CORE_VERIFY_VALUE);
}

static bool signatures(Verify *v) {
    const TfCoreFunctionId entry = v->data->entry;
    v->at = (TfCoreVerifyIssue){.function = entry};
    if (entry > v->data->function_count) return reject(v, TF_CORE_VERIFY_ENTRY);
    for (size_t i = 0; i < v->data->function_count; ++i) {
        const TfCoreFunction *fn = &v->data->functions[i];
        v->at = (TfCoreVerifyIssue){.function = i + 1};
        if (!span_id(v, fn->span)) return false;
        if (!utf8(fn->name) || !utf8(fn->module) || !value_type(v, fn->result))
            return reject(v, TF_CORE_VERIFY_FUNCTION);
        if (tf_core_block(fn, fn->body) == NULL) return reject(v, TF_CORE_VERIFY_BLOCK);
        switch (fn->kind) {
        case TF_CORE_FUNCTION_ENTRY_ONLY:
            if (entry != i + 1 || fn->parameter_count != 1 ||
                !collection_kind(v, fn->parameters[0].type, TF_CORE_TYPE_LIST, TF_CORE_TYPE_TEXT))
                return reject(v, TF_CORE_VERIFY_ENTRY);
            break;
        case TF_CORE_FUNCTION_ORDINARY:
            if (entry == i + 1 && fn->parameter_count != 0)
                return reject(v, TF_CORE_VERIFY_ENTRY);
            break;
        case TF_CORE_FUNCTION_PRELUDE_WRAPPER: {
            const TfCoreBlock *block = tf_core_block(fn, fn->body);
            if (entry == i + 1 || fn->capture_count != 0 || fn->span != 0 ||
                fn->block_count != 1 || block->span != 0 || block->let_count != 1 ||
                block->lets[0].span != 0 || block->terminator.span != 0 ||
                (block->lets[0].operation.kind != TF_CORE_OP_BUILTIN &&
                 block->lets[0].operation.kind != TF_CORE_OP_CALL) ||
                block->terminator.kind != TF_CORE_YIELD || block->terminator.value != block->lets[0].value)
                return reject(v, TF_CORE_VERIFY_FUNCTION);
            const TfCoreValueIds operands = block->lets[0].operation.operands;
            if (operands.count != fn->parameter_count) return reject(v, TF_CORE_VERIFY_FUNCTION);
            for (size_t j = 0; j < fn->parameter_count; ++j)
                if (fn->parameters[j].span != 0 || operands.items[j] != fn->parameters[j].value)
                    return reject(v, TF_CORE_VERIFY_FUNCTION);
            break;
        }
        default: return reject(v, TF_CORE_VERIFY_FUNCTION);
        }
        if (entry == i + 1 && (fn->capture_count != 0 ||
            !collection_kind(v, fn->result, TF_CORE_TYPE_LIST, TF_CORE_TYPE_TEXT)))
            return reject(v, TF_CORE_VERIFY_ENTRY);
        for (size_t j = 0; j < fn->parameter_count; ++j) {
            v->at.value = fn->parameters[j].value;
            if (!parameter(v, &fn->parameters[j])) return false;
        }
        for (size_t j = 0; j < fn->capture_count; ++j) {
            v->at.value = fn->captures[j].value;
            if (!parameter(v, &fn->captures[j])) return false;
        }
    }
    return true;
}

typedef struct {
    size_t remaining, exact;
    bool active, is_exact;
} Fact;

typedef struct {
    TfCoreTypeId type; /* retained after exit to enforce global uniqueness */
    Fact fact;
    bool visible;
} Slot;

typedef struct {
    TfCoreBlockId id;
    TfCoreTypeId result;
    size_t next_let, next_child, residual;
    TfCoreValueId element, subject;
    Fact prior;
    const TfCoreOperation *excludes;
    bool started, impossible;
} BlockFrame;

typedef struct {
    Verify *verify;
    const TfCoreFunction *function;
    Slot *slots;
    BlockFrame *stack;
    unsigned char *seen;
    Row *keys;
    size_t key_count, depth, visited, input_index;
    TfCoreValueId snapshot;
} Body;

static TfCoreTypeId visible(const Body *body, TfCoreValueId id) {
    if (id == 0 || id > body->function->value_count || !body->slots[id - 1].visible)
        return 0;
    return body->slots[id - 1].type;
}

static bool define(Body *body, TfCoreValueId id, TfCoreTypeId type_id) {
    Verify *v = body->verify;
    v->at.value = id;
    if (id == 0 || id > body->function->value_count || body->slots[id - 1].type != 0)
        return reject(v, TF_CORE_VERIFY_VALUE);
    body->slots[id - 1] = (Slot){.type = type_id, .visible = true};
    return true;
}

static int compare_keys(const Verify *v, const Row *a, const Row *b) {
    const int cmp = order(a->first, b->first);
    (void)v;
    return cmp == 0 ? order(a->second, b->second) : cmp;
}

/* The immutable part of the index contains one key per distinct pair named
 * by a switch case. A tag absent from this index was never excluded. */
static Row *tag_key(const Body *b, TfCoreValueId id, size_t tag) {
    const Row wanted = {.first = id, .second = tag};
    size_t first = 0, end = b->key_count;
    while (first < end) {
        const size_t mid = first + (end - first) / 2;
        const int cmp = compare_keys(NULL, &b->keys[mid], &wanted);
        if (cmp == 0) return &b->keys[mid];
        if (cmp < 0) first = mid + 1;
        else end = mid;
    }
    return NULL;
}

static bool allows(const Fact *fact, const Row *key, size_t tag) {
    if (!fact->active) return true;
    if (fact->remaining == 0) return false;
    if (fact->is_exact) return fact->exact == tag;
    return key == NULL || key->exclusions == 0;
}

static bool typed_args(const Body *b, TfCoreValueIds args, size_t skip, TfCoreTypeIds wanted) {
    if (args.count < skip || args.count - skip != wanted.count) return false;
    for (size_t i = 0; i < wanted.count; ++i)
        if (visible(b, args.items[i + skip]) != wanted.items[i]) return false;
    return true;
}

static bool parameter_args(const Body *b, TfCoreValueIds args,
                           const TfCoreParameter *wanted, size_t count) {
    if (args.count != count) return false;
    for (size_t i = 0; i < count; ++i)
        if (visible(b, args.items[i]) != wanted[i].type) return false;
    return true;
}

static bool callback(const Verify *v, TfCoreTypeId id, TfCoreTypeId result, TfCoreTypeIds args) {
    const TfCoreType *fn = type(v, id);
    if (fn == NULL || fn->kind != TF_CORE_TYPE_FUNCTION || fn->as.function.result != result ||
        fn->as.function.parameters.count != args.count) return false;
    for (size_t i = 0; i < args.count; ++i)
        if (fn->as.function.parameters.items[i] != args.items[i]) return false;
    return true;
}

static TfCorePrimitiveClass primitive(TfCoreTypeKind k) {
    switch (k) {
    case TF_CORE_TYPE_INT: return TF_CORE_PRIMITIVE_INT;
    case TF_CORE_TYPE_FLOAT: return TF_CORE_PRIMITIVE_FLOAT;
    case TF_CORE_TYPE_BOOL: return TF_CORE_PRIMITIVE_BOOL;
    case TF_CORE_TYPE_TEXT: return TF_CORE_PRIMITIVE_TEXT;
    case TF_CORE_TYPE_LIST: return TF_CORE_PRIMITIVE_LIST;
    default: return TF_CORE_PRIMITIVE_OTHER;
    }
}

/* Independently check the finite Core prelude without checker schemes. */
static bool builtin(const Body *body, const TfCoreOperation *op, TfCoreTypeId result) {
    const Verify *v = body->verify;
    const TfCoreTypeIds types = op->as.builtin.type_arguments;
    const TfCoreValueIds args = op->operands;
    const TfCoreTypeId a = args.count > 0 ? visible(body, args.items[0]) : 0;
    const TfCoreTypeId b = args.count > 1 ? visible(body, args.items[1]) : 0;
    const TfCoreTypeId c = args.count > 2 ? visible(body, args.items[2]) : 0;
    size_t type_count, arg_count;
    switch (op->as.builtin.id) {
    case TF_CORE_BUILTIN_LEN: type_count = 1; arg_count = 1; break;
    case TF_CORE_BUILTIN_AT: case TF_CORE_BUILTIN_APPEND:
        type_count = 1; arg_count = 2; break;
    case TF_CORE_BUILTIN_MAP: type_count = 2; arg_count = 2; break;
    case TF_CORE_BUILTIN_FOLD: type_count = 2; arg_count = 3; break;
    case TF_CORE_BUILTIN_RANGE: case TF_CORE_BUILTIN_TEXT_AT:
        type_count = 0; arg_count = 2; break;
    case TF_CORE_BUILTIN_TEXT_SLICE: type_count = 0; arg_count = 3; break;
    case TF_CORE_BUILTIN_INT_TEXT: case TF_CORE_BUILTIN_INT_FLOAT:
    case TF_CORE_BUILTIN_FLOAT_INT: case TF_CORE_BUILTIN_FLOAT_TEXT:
    case TF_CORE_BUILTIN_BOOL_TEXT: case TF_CORE_BUILTIN_PARSE_INT:
    case TF_CORE_BUILTIN_TEXT_LEN:
        type_count = 0; arg_count = 1; break;
    default: return false;
    }
    if (types.count != type_count || args.count != arg_count) return false;
    for (size_t i = 0; i < type_count; ++i)
        if (!value_type(v, types.items[i])) return false;
    if (type_count != 0 && !collection(v, a, TF_CORE_TYPE_LIST, types.items[0])) return false;
    switch (op->as.builtin.id) {
    case TF_CORE_BUILTIN_LEN: return kind(v, result, TF_CORE_TYPE_INT);
    case TF_CORE_BUILTIN_AT:
        return kind(v, b, TF_CORE_TYPE_INT) && collection(v, result, TF_CORE_TYPE_OPTION, types.items[0]);
    case TF_CORE_BUILTIN_APPEND: return b == types.items[0] && result == a;
    case TF_CORE_BUILTIN_MAP:
        return callback(v, b, types.items[1], (TfCoreTypeIds){types.items, 1}) &&
               collection(v, result, TF_CORE_TYPE_LIST, types.items[1]);
    case TF_CORE_BUILTIN_FOLD: {
        const TfCoreTypeId parameters[] = {types.items[1], types.items[0]};
        return b == types.items[1] && result == b &&
               callback(v, c, b, (TfCoreTypeIds){parameters, 2});
    }
    case TF_CORE_BUILTIN_RANGE:
        return kind(v, a, TF_CORE_TYPE_INT) && kind(v, b, TF_CORE_TYPE_INT) &&
               collection_kind(v, result, TF_CORE_TYPE_LIST, TF_CORE_TYPE_INT);
    case TF_CORE_BUILTIN_INT_TEXT:
        return kind(v, a, TF_CORE_TYPE_INT) && kind(v, result, TF_CORE_TYPE_TEXT);
    case TF_CORE_BUILTIN_INT_FLOAT:
        return kind(v, a, TF_CORE_TYPE_INT) && kind(v, result, TF_CORE_TYPE_FLOAT);
    case TF_CORE_BUILTIN_FLOAT_INT:
        return kind(v, a, TF_CORE_TYPE_FLOAT) &&
               collection_kind(v, result, TF_CORE_TYPE_OPTION, TF_CORE_TYPE_INT);
    case TF_CORE_BUILTIN_FLOAT_TEXT:
        return kind(v, a, TF_CORE_TYPE_FLOAT) && kind(v, result, TF_CORE_TYPE_TEXT);
    case TF_CORE_BUILTIN_BOOL_TEXT:
        return kind(v, a, TF_CORE_TYPE_BOOL) && kind(v, result, TF_CORE_TYPE_TEXT);
    case TF_CORE_BUILTIN_PARSE_INT:
        return kind(v, a, TF_CORE_TYPE_TEXT) &&
               collection_kind(v, result, TF_CORE_TYPE_OPTION, TF_CORE_TYPE_INT);
    case TF_CORE_BUILTIN_TEXT_LEN:
        return kind(v, a, TF_CORE_TYPE_TEXT) && kind(v, result, TF_CORE_TYPE_INT);
    case TF_CORE_BUILTIN_TEXT_SLICE:
        if (!kind(v, c, TF_CORE_TYPE_INT)) return false;
        /* fall through */
    case TF_CORE_BUILTIN_TEXT_AT:
        return kind(v, a, TF_CORE_TYPE_TEXT) && kind(v, b, TF_CORE_TYPE_INT) &&
               collection_kind(v, result, TF_CORE_TYPE_OPTION, TF_CORE_TYPE_TEXT);
    default: return false;
    }
}

static bool switch_shape(Body *b, BlockFrame *frame, const TfCoreOperation *op,
                         size_t count) {
    Verify *v = b->verify;
    const TfCoreValueId id = op->operands.items[0];
    const Fact *prior = &b->slots[id - 1].fact;
    frame->residual = prior->active ? prior->remaining : count;
    if (op->as.switch_tag.case_count > count) return reject(v, TF_CORE_VERIFY_TAG_PROOF);
    for (size_t i = 0; i < op->as.switch_tag.case_count; ++i) {
        const size_t tag = op->as.switch_tag.cases[i].tag;
        Row *key = tag_key(b, id, tag);
        if (tag >= count || key == NULL || key->marked_by == op)
            return reject(v, TF_CORE_VERIFY_TAG_PROOF);
        key->marked_by = op;
        if (allows(prior, key, tag)) --frame->residual;
    }
    return (frame->residual == 0 || op->as.switch_tag.default_block != 0) ||
           reject(v, TF_CORE_VERIFY_TAG_PROOF);
}

/* Operand IDs are checked before interpreting any operation-specific layout. */
static bool operation(Body *b, BlockFrame *frame, const TfCoreLet *let) {
    Verify *v = b->verify;
    const TfCoreOperation *op = &let->operation;
    const TfCoreValueIds args = op->operands;
    const TfCoreType *r = type(v, let->type);
    TfCoreTypeId a = 0, c = 0;
    const TfCoreType *subject;
    const TfCoreFunction *fn;
    const TfCoreField *layout;
    size_t count;
    if (r == NULL) return reject(v, TF_CORE_VERIFY_TYPE);
    if (r->kind == TF_CORE_TYPE_TAG && op->kind != TF_CORE_OP_TAG)
        return reject(v, TF_CORE_VERIFY_OPERATION);
    for (size_t i = 0; i < args.count; ++i) {
        const TfCoreTypeId id = visible(b, args.items[i]);
        if (id == 0) return reject(v, TF_CORE_VERIFY_VALUE);
        if (args.items[i] == b->snapshot && op->kind != TF_CORE_OP_INPUT_AT)
            return reject(v, TF_CORE_VERIFY_ENTRY);
        if (i == 0) a = id;
        if (i == 1) c = id;
    }
    subject = type(v, a);
    switch (op->kind) {
    case TF_CORE_OP_CONST: {
        const TfCoreConstant *constant = tf_core_constant(v->data, op->as.constant);
        return (args.count == 0 && constant != NULL && constant->type == let->type) ||
               reject(v, TF_CORE_VERIFY_OPERATION);
    }
    case TF_CORE_OP_UNARY:
        if (args.count == 1 && a == let->type &&
            tf_core_primitive_unary(op->as.unary, primitive(subject->kind), primitive(r->kind)))
            return true;
        break;
    case TF_CORE_OP_BINARY:
        if (args.count == 2 && tf_core_primitive_binary(op->as.binary, primitive(subject->kind),
            primitive(type(v, c)->kind), primitive(r->kind), a == c, let->type == a,
            (v->type_flags[a - 1] & EQUALITY) != 0)) return true;
        break;
    case TF_CORE_OP_RECORD: case TF_CORE_OP_MACHINE_VALUE:
        if (op->as.constructed_type != let->type ||
            r->kind != (op->kind == TF_CORE_OP_RECORD ? TF_CORE_TYPE_RECORD : TF_CORE_TYPE_MACHINE))
            break;
        layout = fields(r, &count);
        if (args.count != count) break;
        for (size_t i = 0; i < count; ++i)
            if (visible(b, args.items[i]) != layout[i].type)
                return reject(v, TF_CORE_VERIFY_OPERATION);
        return true;
    case TF_CORE_OP_VARIANT: {
        const size_t tag = op->as.variant.tag;
        if (op->as.variant.type != let->type || tag >= tag_count(r) ||
            args.count != payload_count(r, tag)) break;
        for (size_t i = 0; i < args.count; ++i)
            if (visible(b, args.items[i]) != payload_type(r, tag, i))
                return reject(v, TF_CORE_VERIFY_OPERATION);
        return true;
    }
    case TF_CORE_OP_LIST:
        if (r->kind != TF_CORE_TYPE_LIST || op->as.element_type != r->as.element) break;
        for (size_t i = 0; i < args.count; ++i)
            if (visible(b, args.items[i]) != r->as.element)
                return reject(v, TF_CORE_VERIFY_OPERATION);
        return true;
    case TF_CORE_OP_PROJECT:
        if (args.count != 1) break;
        layout = fields(subject, &count);
        if (op->as.field < count && layout[op->as.field].type == let->type) return true;
        break;
    case TF_CORE_OP_TAG:
        if (args.count == 1 && r->kind == TF_CORE_TYPE_TAG && r->as.tagged == a) return true;
        break;
    case TF_CORE_OP_PAYLOAD: {
        const size_t tag = op->as.payload.tag;
        const size_t index = op->as.payload.index;
        const Fact *proof;
        if (args.count != 1 || tag >= tag_count(subject) || index >= payload_count(subject, tag) ||
            let->type != payload_type(subject, tag, index)) break;
        proof = &b->slots[args.items[0] - 1].fact;
        if (proof->active && proof->remaining == 1 &&
            allows(proof, tag_key(b, args.items[0], tag), tag)) return true;
        return reject(v, TF_CORE_VERIFY_TAG_PROOF);
    }
    case TF_CORE_OP_IF:
        if (args.count == 1 && subject->kind == TF_CORE_TYPE_BOOL) return true;
        break;
    case TF_CORE_OP_SWITCH_TAG:
        if (args.count == 1 && tag_count(subject) != 0)
            return switch_shape(b, frame, op, tag_count(subject));
        break;
    case TF_CORE_OP_CALL:
        fn = tf_core_function(v->data, op->as.function);
        if (fn != NULL && fn->kind == TF_CORE_FUNCTION_ORDINARY && fn->capture_count == 0 &&
            fn->result == let->type && parameter_args(b, args, fn->parameters, fn->parameter_count))
            return true;
        break;
    case TF_CORE_OP_CLOSURE:
        fn = tf_core_function(v->data, op->as.function);
        if (fn == NULL || (fn->kind != TF_CORE_FUNCTION_ORDINARY &&
                           fn->kind != TF_CORE_FUNCTION_PRELUDE_WRAPPER) ||
            r->kind != TF_CORE_TYPE_FUNCTION || fn->result != r->as.function.result ||
            fn->parameter_count != r->as.function.parameters.count ||
            !parameter_args(b, args, fn->captures, fn->capture_count)) break;
        for (size_t i = 0; i < fn->parameter_count; ++i)
            if (fn->parameters[i].type != r->as.function.parameters.items[i])
                return reject(v, TF_CORE_VERIFY_OPERATION);
        return true;
    case TF_CORE_OP_CALL_CLOSURE:
        if (args.count != 0 && subject->kind == TF_CORE_TYPE_FUNCTION &&
            subject->as.function.result == let->type &&
            typed_args(b, args, 1, subject->as.function.parameters)) return true;
        break;
    case TF_CORE_OP_BUILTIN:
        if (builtin(b, op, let->type)) return true;
        break;
    case TF_CORE_OP_LIST_MAP:
        if (args.count == 1 && subject->kind == TF_CORE_TYPE_LIST && r->kind == TF_CORE_TYPE_LIST &&
            op->as.list_map.element.type == subject->as.element)
            return parameter(v, &op->as.list_map.element);
        break;
    case TF_CORE_OP_INPUT_AT:
        if (b->snapshot != 0 && frame->id == b->function->body && args.count == 1 &&
            args.items[0] == b->snapshot && r->kind == TF_CORE_TYPE_TEXT &&
            op->as.input_index == b->input_index) {
            /* Each increment corresponds to one counted root binding. */
            ++b->input_index;
            return true;
        }
        return reject(v, TF_CORE_VERIFY_ENTRY);
    default:
        break;
    }
    return reject(v, TF_CORE_VERIFY_OPERATION);
}

typedef struct {
    TfCoreBlockId block;
    TfCoreTypeId result;
    const TfCoreParameter *element;
    TfCoreValueId subject;
    Fact fact;
    const TfCoreOperation *excludes;
    bool impossible;
} Child;

static bool child_block(const Body *b, const BlockFrame *parent, const TfCoreLet *let,
                        Child *child) {
    const TfCoreOperation *op = &let->operation;
    const size_t i = parent->next_child;
    *child = (Child){.result = let->type, .impossible = parent->impossible};
    switch (op->kind) {
    case TF_CORE_OP_IF:
        if (i > 1) return false;
        child->block = i == 0 ? op->as.branch.then_block : op->as.branch.else_block;
        child->fact.is_exact = true;
        child->fact.exact = i == 0 ? 1u : 0u; /* Bool true/false */
        break;
    case TF_CORE_OP_SWITCH_TAG:
        if (i < op->as.switch_tag.case_count) {
            const TfCoreTagCase *c = &op->as.switch_tag.cases[i];
            child->block = c->block;
            child->fact.is_exact = true;
            child->fact.exact = c->tag;
        } else {
            if (i != op->as.switch_tag.case_count || op->as.switch_tag.default_block == 0)
                return false;
            child->block = op->as.switch_tag.default_block;
            child->excludes = op;
        }
        break;
    case TF_CORE_OP_LIST_MAP:
        if (i != 0) return false;
        child->block = op->as.list_map.body;
        child->result = type(b->verify, let->type)->as.element;
        child->element = &op->as.list_map.element;
        return true;
    default:
        return false;
    }
    child->subject = op->operands.items[0];
    const Fact *prior = &b->slots[child->subject - 1].fact;
    if (child->excludes != NULL) {
        child->fact = *prior;
        child->fact.remaining = parent->residual;
    } else {
        const Row *key = tag_key(b, child->subject, child->fact.exact);
        child->fact.remaining = allows(prior, key, child->fact.exact) ? 1u : 0u;
    }
    child->fact.active = true;
    child->impossible = child->impossible || child->fact.remaining == 0;
    return true;
}

/* Exclusions are scoped to defaults. Repeated exclusions of the same tag
 * accumulate depth, so leaving a nested default restores the outer proof. */
static bool adjust_exclusions(Body *b, const BlockFrame *frame, bool entering) {
    if (frame->excludes == NULL) return true;
    for (size_t i = 0; i < frame->excludes->as.switch_tag.case_count; ++i) {
        const size_t tag = frame->excludes->as.switch_tag.cases[i].tag;
        Row *key = tag_key(b, frame->subject, tag);
        if (key == NULL || (!entering && key->exclusions == 0))
            return reject(b->verify, TF_CORE_VERIFY_TAG_PROOF);
        /* At most one exclusion per key per active block. */
        if (entering) ++key->exclusions;
        else --key->exclusions;
    }
    return true;
}

static bool enter(Body *b, const Child *child) {
    Verify *v = b->verify;
    const TfCoreBlock *block = tf_core_block(b->function, child->block);
    BlockFrame *frame;
    v->at.block = child->block;
    v->at.value = 0;
    if (block == NULL) return reject(v, TF_CORE_VERIFY_BLOCK);
    if (!span_id(v, block->span)) return false;
    if (b->seen[child->block - 1] != 0) return reject(v, TF_CORE_VERIFY_BLOCK);
    b->seen[child->block - 1] = 1;
    ++b->visited;
    frame = &b->stack[b->depth++];
    *frame = (BlockFrame){.id = child->block, .result = child->result,
                         .subject = child->subject, .excludes = child->excludes,
                         .impossible = child->impossible};
    if (frame->subject != 0) {
        frame->prior = b->slots[frame->subject - 1].fact;
        b->slots[frame->subject - 1].fact = child->fact;
        if (!adjust_exclusions(b, frame, true)) return false;
    }
    if (child->element != NULL) {
        frame->element = child->element->value;
        v->at.span = child->element->span; /* checked before entry */
        if (!define(b, frame->element, child->element->type)) return false;
    }
    return true;
}

static bool leave(Body *b, const BlockFrame *frame, const TfCoreBlock *block) {
    Verify *v = b->verify;
    const TfCoreTerminator *term = &block->terminator;
    v->at.value = term->value;
    if (!span_id(v, term->span)) return false;
    switch (term->kind) {
    case TF_CORE_YIELD:
        if (b->snapshot != 0 && term->value == b->snapshot)
            return reject(v, TF_CORE_VERIFY_ENTRY);
        if (visible(b, term->value) != frame->result) return reject(v, TF_CORE_VERIFY_TERMINATOR);
        break;
    case TF_CORE_UNREACHABLE:
        if (term->value != 0 || !frame->impossible) return reject(v, TF_CORE_VERIFY_TERMINATOR);
        break;
    default:
        return reject(v, TF_CORE_VERIFY_TERMINATOR);
    }
    for (size_t i = 0; i < block->let_count; ++i)
        b->slots[block->lets[i].value - 1].visible = false;
    if (frame->element != 0) b->slots[frame->element - 1].visible = false;
    if (frame->subject != 0) {
        if (!adjust_exclusions(b, frame, false)) return false;
        b->slots[frame->subject - 1].fact = frame->prior;
    }
    --b->depth;
    return true;
}

static bool walk_blocks(Body *b) {
    Verify *v = b->verify;
    const TfCoreFunction *fn = b->function;
    const Child root = {.block = fn->body, .result = fn->result};
    if (!enter(b, &root)) return false;
    while (b->depth != 0) {
        BlockFrame *frame = &b->stack[b->depth - 1];
        const TfCoreBlock *block = tf_core_block(fn, frame->id);
        const TfCoreLet *let;
        Child child;
        v->at.block = frame->id;
        if (frame->next_let == block->let_count) {
            if (!leave(b, frame, block)) return false;
            continue;
        }
        let = &block->lets[frame->next_let];
        v->at.value = let->value;
        if (!span_id(v, let->span)) return false;
        if (!frame->started) {
            if (let->value == 0 || let->value > fn->value_count ||
                b->slots[let->value - 1].type != 0) return reject(v, TF_CORE_VERIFY_VALUE);
            if (!operation(b, frame, let)) return false;
            frame->started = true;
            frame->next_child = 0;
        }
        if (child_block(b, frame, let, &child)) {
            ++frame->next_child;
            if (!enter(b, &child)) return false;
            continue;
        }
        if (!define(b, let->value, let->type)) return false;
        frame->started = false;
        ++frame->next_let;
    }
    if (b->visited != fn->block_count) {
        for (size_t i = 0; i < fn->block_count; ++i)
            if (b->seen[i] == 0) {
                v->at.block = i + 1;
                v->at.value = 0;
                v->at.span = fn->blocks[i].span;
                return reject(v, TF_CORE_VERIFY_BLOCK);
            }
    }
    return true;
}

/* The declaration is a scalar in untrusted IR. Size slot storage only after
 * deriving the count from copied arrays, including one binder per ListMap.
 * All of those definitions must later be visited and have unique in-range IDs. */
static bool census(Body *b) {
    const TfCoreFunction *fn = b->function;
    size_t actual = fn->parameter_count;
    if (actual > fn->value_count || fn->capture_count > fn->value_count - actual)
        return reject(b->verify, TF_CORE_VERIFY_VALUE);
    actual += fn->capture_count;
    for (size_t i = 0; i < fn->block_count; ++i) {
        const TfCoreBlock *block = &fn->blocks[i];
        if (block->let_count > fn->value_count - actual)
            return reject(b->verify, TF_CORE_VERIFY_VALUE);
        actual += block->let_count;
        for (size_t j = 0; j < block->let_count; ++j) {
            const TfCoreOperation *op = &block->lets[j].operation;
            if (op->kind == TF_CORE_OP_LIST_MAP) {
                if (actual == fn->value_count) return reject(b->verify, TF_CORE_VERIFY_VALUE);
                ++actual;
            }
            if (op->kind == TF_CORE_OP_SWITCH_TAG && op->operands.count == 1) {
                const size_t count = op->as.switch_tag.case_count;
                if (count > SIZE_MAX - b->key_count) {
                    b->verify->resource_failure = true;
                    return false;
                }
                b->key_count += count;
            }
        }
    }
    if (actual != fn->value_count) {
        b->verify->at.value = actual + 1;
        return reject(b->verify, TF_CORE_VERIFY_VALUE);
    }
    return true;
}

static bool build_keys(Body *b) {
    const TfCoreFunction *fn = b->function;
    size_t n = 0, unique = 0;
    if (b->key_count == 0) return true;
    b->keys = workspace(b->verify, b->key_count, sizeof(*b->keys));
    if (b->keys == NULL) return false;
    for (size_t i = 0; i < fn->block_count; ++i) {
        const TfCoreBlock *block = &fn->blocks[i];
        for (size_t j = 0; j < block->let_count; ++j) {
            const TfCoreOperation *op = &block->lets[j].operation;
            if (op->kind != TF_CORE_OP_SWITCH_TAG || op->operands.count != 1) continue;
            for (size_t k = 0; k < op->as.switch_tag.case_count; ++k)
                b->keys[n++] = (Row){.first = op->operands.items[0],
                                    .second = op->as.switch_tag.cases[k].tag};
        }
    }
    if (!sort_rows(b->verify, b->keys, n, compare_keys)) return false;
    for (size_t i = 0; i < n; ++i) {
        if (unique == 0 || compare_keys(NULL, &b->keys[unique - 1], &b->keys[i]) != 0)
            b->keys[unique++] = b->keys[i];
    }
    b->key_count = unique;
    return true;
}

static bool verify_body(Verify *v, const TfCoreFunction *fn) {
    Body b = {.verify = v, .function = fn};
    bool ok = false;
    if (!census(&b)) return false;
    if (fn->kind == TF_CORE_FUNCTION_ENTRY_ONLY) b.snapshot = fn->parameters[0].value;
    b.slots = workspace(v, fn->value_count, sizeof(*b.slots));
    if (fn->value_count != 0 && b.slots == NULL) goto cleanup;
    for (size_t i = 0; i < fn->value_count; ++i) b.slots[i] = (Slot){0};
    b.seen = workspace(v, fn->block_count, sizeof(*b.seen));
    if (b.seen == NULL) goto cleanup;
    memset(b.seen, 0, fn->block_count);
    b.stack = workspace(v, fn->block_count, sizeof(*b.stack));
    if (b.stack == NULL) goto cleanup;
    if (!build_keys(&b)) goto cleanup;
    for (size_t i = 0; i < fn->parameter_count; ++i) {
        const TfCoreParameter *p = &fn->parameters[i];
        v->at.span = p->span;
        if (!define(&b, p->value, p->type)) goto cleanup;
    }
    for (size_t i = 0; i < fn->capture_count; ++i) {
        const TfCoreParameter *p = &fn->captures[i];
        v->at.span = p->span;
        if (!define(&b, p->value, p->type)) goto cleanup;
    }
    ok = walk_blocks(&b);
cleanup:
    release(v, b.keys);
    release(v, b.stack);
    release(v, b.seen);
    release(v, b.slots);
    return ok;
}

static bool verify_program(Verify *v) {
    for (size_t i = 0; i < v->data->span_count; ++i) {
        const TfSpan *span = &v->data->spans[i];
        TfSpan checked;
        if (!tf_source_span(span->source, span->start, span->end, &checked)) {
            v->at = (TfCoreVerifyIssue){.span = i + 1};
            return reject(v, TF_CORE_VERIFY_SPAN);
        }
    }
    if (!verify_types(v) || !verify_constants(v) || !signatures(v)) return false;
    for (size_t i = 0; i < v->data->function_count; ++i) {
        const TfCoreFunction *fn = &v->data->functions[i];
        v->at = (TfCoreVerifyIssue){.function = i + 1, .span = fn->span};
        if (!verify_body(v, fn)) return false;
    }
    return true;
}

TfStatus tf_core_verify(TfVerifiedCore **out, const TfCoreProgram *program,
                        const TfAllocator *allocator, TfCoreVerifyIssue *issue) {
    Verify v = {.data = tf_core_program_data(program), .memory = tf_allocator(allocator)};
    TfVerifiedCore *verified;
    bool ok;
    if (issue != NULL) *issue = (TfCoreVerifyIssue){0};
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (v.data == NULL || !tf_allocator_valid(v.memory)) return TF_HOST_ERROR;
    ok = verify_program(&v);
    release(&v, v.type_flags);
    if (!ok) {
        if (v.resource_failure) return TF_HOST_ERROR;
        if (issue != NULL) *issue = v.at;
        return TF_SOURCE_ERROR;
    }
    verified = workspace(&v, 1, sizeof(*verified));
    if (verified == NULL) return TF_HOST_ERROR;
    *verified = (TfVerifiedCore){program, v.memory};
    *out = verified;
    return TF_OK;
}

const TfCoreProgramData *tf_verified_core_data(const TfVerifiedCore *verified) {
    return verified != NULL ? tf_core_program_data(verified->program) : NULL;
}

void tf_verified_core_destroy(TfVerifiedCore *verified) {
    if (verified != NULL) verified->memory.deallocate(verified->memory.context, verified);
}
