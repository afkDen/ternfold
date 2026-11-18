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
    size_t max_tags;
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

/* Structural constructors must be interned. Nominal identity is the table
 * ID, never a display name or an accidentally identical layout. */
static bool same_structure(const TfCoreType *a, const TfCoreType *b) {
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case TF_CORE_TYPE_INT: case TF_CORE_TYPE_FLOAT: case TF_CORE_TYPE_BOOL:
    case TF_CORE_TYPE_TEXT: case TF_CORE_TYPE_CHAR: case TF_CORE_TYPE_UNIT:
        return true;
    case TF_CORE_TYPE_LIST: case TF_CORE_TYPE_OPTION:
        return a->as.element == b->as.element;
    case TF_CORE_TYPE_TAG: return a->as.tagged == b->as.tagged;
    case TF_CORE_TYPE_DECISION:
        return a->as.decision.machine == b->as.decision.machine &&
               a->as.decision.output == b->as.decision.output &&
               a->as.decision.rejection == b->as.decision.rejection;
    case TF_CORE_TYPE_FUNCTION:
        if (a->as.function.result != b->as.function.result ||
            a->as.function.parameters.count != b->as.function.parameters.count) return false;
        for (size_t i = 0; i < a->as.function.parameters.count; ++i)
            if (a->as.function.parameters.items[i] != b->as.function.parameters.items[i])
                return false;
        return true;
    default: return false;
    }
}

static size_t mix_type(size_t hash, size_t word) {
    return (size_t)(((uintmax_t)hash ^ (uintmax_t)word) * UINTMAX_C(16777619));
}

/* Zero marks nominal types, whose IDs intentionally distinguish them. A
 * collision is always resolved by comparing the full structural key. */
static size_t structural_hash(const TfCoreType *t) {
    size_t h = mix_type(0, (size_t)t->kind);
    switch (t->kind) {
    case TF_CORE_TYPE_LIST: case TF_CORE_TYPE_OPTION:
        h = mix_type(h, t->as.element);
        break;
    case TF_CORE_TYPE_TAG:
        h = mix_type(h, t->as.tagged);
        break;
    case TF_CORE_TYPE_DECISION:
        h = mix_type(h, t->as.decision.machine);
        h = mix_type(h, t->as.decision.output);
        h = mix_type(h, t->as.decision.rejection);
        break;
    case TF_CORE_TYPE_FUNCTION:
        h = mix_type(h, t->as.function.result);
        for (size_t i = 0; i < t->as.function.parameters.count; ++i)
            h = mix_type(h, t->as.function.parameters.items[i]);
        break;
    case TF_CORE_TYPE_RECORD: case TF_CORE_TYPE_ENUM: case TF_CORE_TYPE_MACHINE:
        return 0;
    default:
        break;
    }
    return h == 0 ? 1 : h;
}

static bool canonical_types(Verify *v) {
    const size_t count = v->data->type_count;
    TfCoreTypeId *ids;
    size_t capacity;
    bool ok = true;
    if (count > SIZE_MAX / 2) {
        v->resource_failure = true;
        return false;
    }
    capacity = count * 2;
    ids = workspace(v, capacity, sizeof(*ids));
    if (ids == NULL) return false;
    memset(ids, 0, capacity * sizeof(*ids));
    for (size_t i = 0; i < count; ++i) {
        const TfCoreType *t = &v->data->types[i];
        const size_t hash = structural_hash(t);
        size_t slot;
        if (hash == 0) continue;
        slot = hash % capacity;
        while (ids[slot] != 0 && !same_structure(t, type(v, ids[slot]))) {
            if (++slot == capacity) slot = 0;
        }
        if (ids[slot] != 0) {
            v->at = (TfCoreVerifyIssue){.type = i + 1, .span = t->span};
            ok = reject(v, TF_CORE_VERIFY_TYPE);
            break;
        }
        ids[slot] = i + 1;
    }
    release(v, ids);
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
        if (tag_count(t) > v->max_tags) v->max_tags = tag_count(t);
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

typedef struct Fact Fact;
struct Fact {
    const Fact *prior;
    const TfCoreTagCase *excluded;
    size_t excluded_count, remaining;
    size_t exact;
    bool is_exact;
};

typedef struct {
    TfCoreTypeId type; /* retained after exit to enforce global uniqueness */
    const Fact *fact;
    bool visible;
} Slot;

typedef struct {
    TfCoreBlockId id;
    TfCoreTypeId result;
    size_t next_let, next_child, residual;
    TfCoreValueId element, subject;
    Fact fact;
    bool started, impossible;
} BlockFrame;

typedef struct {
    Verify *verify;
    const TfCoreFunction *function;
    Slot *slots;
    BlockFrame *stack;
    unsigned char *seen, *marks;
    size_t depth, visited, defined;
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
    ++body->defined;
    return true;
}

static bool allows(const Fact *fact, size_t tag) {
    for (; fact != NULL; fact = fact->prior) {
        if (fact->remaining == 0) return false;
        if (fact->is_exact) return fact->exact == tag;
        for (size_t i = 0; i < fact->excluded_count; ++i)
            if (fact->excluded[i].tag == tag) return false;
    }
    return true;
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
    const Fact *prior = b->slots[op->operands.items[0] - 1].fact;
    frame->residual = prior == NULL ? count : prior->remaining;
    if (op->as.switch_tag.case_count > count) return reject(v, TF_CORE_VERIFY_TAG_PROOF);
    memset(b->marks, 0, count);
    for (size_t i = 0; i < op->as.switch_tag.case_count; ++i) {
        const size_t tag = op->as.switch_tag.cases[i].tag;
        if (tag >= count || b->marks[tag] != 0) return reject(v, TF_CORE_VERIFY_TAG_PROOF);
        b->marks[tag] = 1;
        if (allows(prior, tag)) --frame->residual;
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
        proof = b->slots[args.items[0] - 1].fact;
        if (proof != NULL && proof->remaining == 1 && allows(proof, tag)) return true;
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
        if (fn == NULL || fn->kind != TF_CORE_FUNCTION_ORDINARY ||
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
        if (b->function->kind == TF_CORE_FUNCTION_ENTRY_ONLY && frame->id == b->function->body &&
            args.count == 1 && args.items[0] == b->function->parameters[0].value &&
            r->kind == TF_CORE_TYPE_TEXT) return true;
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
            child->fact.excluded = op->as.switch_tag.cases;
            child->fact.excluded_count = op->as.switch_tag.case_count;
            child->fact.remaining = parent->residual;
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
    child->fact.prior = b->slots[child->subject - 1].fact;
    if (child->fact.is_exact)
        child->fact.remaining = allows(child->fact.prior, child->fact.exact) ? 1u : 0u;
    child->impossible = child->impossible || child->fact.remaining == 0;
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
                         .subject = child->subject, .fact = child->fact,
                         .impossible = child->impossible};
    if (frame->subject != 0) b->slots[frame->subject - 1].fact = &frame->fact;
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
    if (frame->subject != 0) b->slots[frame->subject - 1].fact = frame->fact.prior;
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
    if (b->defined != fn->value_count) {
        for (size_t i = 0; i < fn->value_count; ++i)
            if (b->slots[i].type == 0) {
                v->at.block = 0;
                v->at.value = i + 1;
                v->at.span = fn->span;
                return reject(v, TF_CORE_VERIFY_VALUE);
            }
    }
    return true;
}

static bool verify_body(Verify *v, const TfCoreFunction *fn) {
    Body b = {.verify = v, .function = fn};
    bool ok = false;
    b.slots = workspace(v, fn->value_count, sizeof(*b.slots));
    if (fn->value_count != 0 && b.slots == NULL) goto cleanup;
    for (size_t i = 0; i < fn->value_count; ++i) b.slots[i] = (Slot){0};
    b.seen = workspace(v, fn->block_count, sizeof(*b.seen));
    if (b.seen == NULL) goto cleanup;
    memset(b.seen, 0, fn->block_count);
    b.stack = workspace(v, fn->block_count, sizeof(*b.stack));
    if (b.stack == NULL) goto cleanup;
    b.marks = workspace(v, v->max_tags, sizeof(*b.marks));
    if (v->max_tags != 0 && b.marks == NULL) goto cleanup;
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
    release(v, b.marks);
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
