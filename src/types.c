#include "ternfold/types.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    TfConcreteType type;
    uint64_t hash;
    /* Private lazy cache on Machine records. The public nominal record is
     * immutable; these are the types of its declared yields and rejects. */
    TfConcreteTypeId output, rejection;
    bool checking;
    TfConcreteTypeId parameters[];
} TypeEntry;

struct TfTypeStore {
    const TfResolution *resolution;
    TfAllocator allocator;
    TypeEntry **entries;
    size_t count, capacity;
    size_t *slots; /* open-addressed; 0 denotes an empty slot */
    size_t slot_count;
};

static void *system_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void system_deallocate(void *context, void *ptr) {
    (void)context;
    free(ptr);
}

static void release(const TfTypeStore *s, void *ptr) {
    if (ptr != NULL) s->allocator.deallocate(s->allocator.context, ptr);
}

static void *grow(TfTypeStore *s, void *items, size_t used, size_t *capacity,
                  size_t needed, size_t width) {
    size_t n = *capacity;
    void *next;
    if (needed <= n) return items;
    if (needed > SIZE_MAX / width) return NULL;
    if (n == 0) n = 8 <= SIZE_MAX / width ? 8 : needed;
    while (n < needed) {
        if (n > SIZE_MAX / width / 2) { n = needed; break; }
        n *= 2;
    }
    next = s->allocator.allocate(s->allocator.context, n * width);
    if (next == NULL) return NULL;
    if (used != 0) memcpy(next, items, used * width);
    release(s, items);
    *capacity = n;
    return next;
}

static TypeEntry *find_entry(const TfTypeStore *s, TfConcreteTypeId id) {
    return s != NULL && id.owner == s && id.index != 0 && id.index <= s->count
        ? s->entries[id.index - 1] : NULL;
}

bool tf_concrete_type_id_equal(TfConcreteTypeId left, TfConcreteTypeId right) {
    return left.owner != NULL && left.owner == right.owner &&
           left.index != 0 && left.index == right.index;
}

static bool primitive(TfConcreteTypeKind kind) {
    return kind >= TF_CONCRETE_INT && kind <= TF_CONCRETE_UNIT;
}

static uint64_t mix(uint64_t hash, size_t index) {
    uint64_t x = hash ^ (uint64_t)index;
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

/* All child IDs belong to this store, and all nominal IDs to its resolution.
 * Hash stable numeric indexes; resolve collisions with exact comparison. */
static uint64_t type_hash(const TfConcreteType *t) {
    uint64_t h = mix(UINT64_C(0x9e3779b97f4a7c15), (size_t)t->kind);
    switch (t->kind) {
    case TF_CONCRETE_LIST:
    case TF_CONCRETE_OPTION:
        return mix(h, t->as.element.index);
    case TF_CONCRETE_RECORD:
    case TF_CONCRETE_ENUM:
    case TF_CONCRETE_MACHINE:
        return mix(mix(h, t->as.nominal.module_index), t->as.nominal.declaration_index);
    case TF_CONCRETE_DECISION:
        return mix(mix(mix(h, t->as.decision.machine.index), t->as.decision.output.index),
                   t->as.decision.rejection.index);
    case TF_CONCRETE_FUNCTION:
        h = mix(h, t->as.function.arity);
        if (t->as.function.parameters != NULL)
            for (size_t i = 0; i < t->as.function.arity; ++i)
                h = mix(h, t->as.function.parameters[i].index);
        return mix(h, t->as.function.result.index);
    default:
        return h;
    }
}

static bool same_shape(const TfConcreteType *a, const TfConcreteType *b) {
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case TF_CONCRETE_LIST:
    case TF_CONCRETE_OPTION:
        return a->as.element.index == b->as.element.index;
    case TF_CONCRETE_RECORD:
    case TF_CONCRETE_ENUM:
    case TF_CONCRETE_MACHINE:
        return a->as.nominal.module_index == b->as.nominal.module_index &&
               a->as.nominal.declaration_index == b->as.nominal.declaration_index;
    case TF_CONCRETE_DECISION:
        return a->as.decision.machine.index == b->as.decision.machine.index &&
               a->as.decision.output.index == b->as.decision.output.index &&
               a->as.decision.rejection.index == b->as.decision.rejection.index;
    case TF_CONCRETE_FUNCTION:
        if (a->as.function.arity != b->as.function.arity ||
            a->as.function.result.index != b->as.function.result.index) return false;
        for (size_t i = 0; i < a->as.function.arity; ++i)
            if (a->as.function.parameters[i].index != b->as.function.parameters[i].index)
                return false;
        return true;
    default:
        return true;
    }
}

static size_t slot_for(const TfTypeStore *s, uint64_t hash, const TfConcreteType *key) {
    size_t mask = s->slot_count - 1;
    size_t at = (size_t)hash & mask;
    while (s->slots[at] != 0) {
        const TypeEntry *e = s->entries[s->slots[at] - 1];
        if (e->hash == hash && key != NULL && same_shape(&e->type, key)) break;
        at = (at + 1) & mask;
    }
    return at;
}

static bool grow_slots(TfTypeStore *s) {
    size_t n, *slots;
    if (s->slot_count > SIZE_MAX / sizeof(*slots) / 2) return false;
    n = s->slot_count == 0 ? 16 : s->slot_count * 2;
    slots = s->allocator.allocate(s->allocator.context, n * sizeof(*slots));
    if (slots == NULL) return false;
    memset(slots, 0, n * sizeof(*slots));
    release(s, s->slots);
    s->slots = slots;
    s->slot_count = n;
    for (size_t i = 0; i < s->count; ++i)
        slots[slot_for(s, s->entries[i]->hash, NULL)] = i + 1;
    return true;
}

static bool function_fits(size_t arity) {
    return arity <= (SIZE_MAX - sizeof(TypeEntry)) / sizeof(TfConcreteTypeId);
}

/* All structural arguments have already been checked. This operation publishes
 * a record only after every needed allocation succeeds. */
static TfStatus intern(TfTypeStore *s, TfConcreteType key, TfConcreteTypeId *out) {
    uint64_t hash;
    size_t slot, bytes = 0;
    const TfConcreteTypeId *parameters = NULL;
    TypeEntry **entries;
    TypeEntry *entry;
    if (key.kind == TF_CONCRETE_FUNCTION) {
        size_t arity = key.as.function.arity;
        if (arity != 0) {
            parameters = key.as.function.parameters;
            if (parameters == NULL || !function_fits(arity)) return TF_HOST_ERROR;
            bytes = arity * sizeof(*parameters);
        } else key.as.function.parameters = NULL;
    }
    hash = type_hash(&key);
    if (s->slot_count != 0) {
        slot = slot_for(s, hash, &key);
        if (s->slots[slot] != 0) {
            *out = s->entries[s->slots[slot] - 1]->type.id;
            return TF_OK;
        }
    }
    if (s->count == SIZE_MAX) return TF_HOST_ERROR;
    entries = grow(s, s->entries, s->count, &s->capacity, s->count + 1, sizeof(*entries));
    if (entries == NULL) return TF_HOST_ERROR;
    s->entries = entries;
    if (s->count >= s->slot_count / 2 && !grow_slots(s)) return TF_HOST_ERROR;
    entry = s->allocator.allocate(s->allocator.context, sizeof(*entry) + bytes);
    if (entry == NULL) return TF_HOST_ERROR;
    key.id = (TfConcreteTypeId){s, s->count + 1};
    if (parameters != NULL) {
        memcpy(entry->parameters, parameters, bytes);
        key.as.function.parameters = entry->parameters;
    }
    entry->type = key;
    entry->hash = hash;
    entry->output = (TfConcreteTypeId){0};
    entry->rejection = (TfConcreteTypeId){0};
    entry->checking = false;
    slot = slot_for(s, hash, NULL);
    s->entries[s->count++] = entry;
    s->slots[slot] = key.id.index;
    *out = key.id;
    return TF_OK;
}

TfStatus tf_type_store_create(TfTypeStore **out, const TfResolution *resolution,
                              const TfAllocator *allocator) {
    TfAllocator a = {NULL, system_allocate, system_deallocate};
    TfTypeStore *s;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (resolution == NULL || tf_diagnostic_phase_payload(tf_resolution_phase(resolution)) != resolution)
        return TF_HOST_ERROR;
    if (allocator != NULL) a = *allocator;
    if (a.allocate == NULL || a.deallocate == NULL) return TF_HOST_ERROR;
    s = a.allocate(a.context, sizeof(*s));
    if (s == NULL) return TF_HOST_ERROR;
    *s = (TfTypeStore){.resolution = resolution, .allocator = a};
    *out = s;
    return TF_OK;
}

void tf_type_store_destroy(TfTypeStore *s) {
    if (s == NULL) return;
    for (size_t i = 0; i < s->count; ++i) release(s, s->entries[i]);
    release(s, s->entries);
    release(s, s->slots);
    release(s, s);
}

size_t tf_type_store_count(const TfTypeStore *s) { return s != NULL ? s->count : 0; }

const TfConcreteType *tf_type_store_at(const TfTypeStore *s, size_t index) {
    return s != NULL && index < s->count ? &s->entries[index]->type : NULL;
}

const TfConcreteType *tf_type_store_get(const TfTypeStore *s, TfConcreteTypeId id) {
    const TypeEntry *entry = find_entry(s, id);
    return entry != NULL ? &entry->type : NULL;
}

TfStatus tf_type_primitive(TfTypeStore *s, TfConcreteTypeKind kind, TfConcreteTypeId *out) {
    if (out == NULL) return TF_HOST_ERROR;
    *out = (TfConcreteTypeId){0};
    if (s == NULL || !primitive(kind)) return TF_HOST_ERROR;
    return intern(s, (TfConcreteType){.kind = kind}, out);
}

static bool nominal_kind(TfNominalKind resolved, TfConcreteTypeKind *out) {
    switch (resolved) {
    case TF_TYPE_RECORD: *out = TF_CONCRETE_RECORD; return true;
    case TF_TYPE_ENUM: *out = TF_CONCRETE_ENUM; return true;
    case TF_TYPE_MACHINE: *out = TF_CONCRETE_MACHINE; return true;
    default: return false;
    }
}

TfStatus tf_type_nominal(TfTypeStore *s, TfTypeId declaration, TfConcreteTypeId *out) {
    const TfResolvedType *resolved;
    TfConcreteType key = {0};
    if (out == NULL) return TF_HOST_ERROR;
    *out = (TfConcreteTypeId){0};
    if (s == NULL) return TF_HOST_ERROR;
    resolved = tf_resolution_type(s->resolution, declaration);
    if (resolved == NULL || !nominal_kind(resolved->kind, &key.kind)) return TF_HOST_ERROR;
    key.as.nominal = resolved->id;
    return intern(s, key, out);
}

static TfStatus container(TfTypeStore *s, TfConcreteTypeKind kind, TfConcreteTypeId element,
                          TfConcreteTypeId *out) {
    if (out == NULL) return TF_HOST_ERROR;
    *out = (TfConcreteTypeId){0};
    if (find_entry(s, element) == NULL) return TF_HOST_ERROR;
    return intern(s, (TfConcreteType){.kind = kind, .as.element = element}, out);
}

TfStatus tf_type_list(TfTypeStore *s, TfConcreteTypeId element, TfConcreteTypeId *out) {
    return container(s, TF_CONCRETE_LIST, element, out);
}

TfStatus tf_type_option(TfTypeStore *s, TfConcreteTypeId element, TfConcreteTypeId *out) {
    return container(s, TF_CONCRETE_OPTION, element, out);
}

TfStatus tf_type_function(TfTypeStore *s, const TfConcreteTypeId *parameters, size_t arity,
                          TfConcreteTypeId result, TfConcreteTypeId *out) {
    TfConcreteTypeId id = {0};
    TfStatus status = TF_HOST_ERROR;
    if (out == NULL) return TF_HOST_ERROR;
    if (find_entry(s, result) != NULL && function_fits(arity) &&
        (arity == 0 || parameters != NULL)) {
        size_t i = 0;
        while (i < arity && find_entry(s, parameters[i]) != NULL) ++i;
        if (i == arity) {
            TfConcreteType key = {.kind = TF_CONCRETE_FUNCTION,
                .as.function = {arity != 0 ? parameters : NULL, arity, result}};
            status = intern(s, key, &id);
        }
    }
    *out = id;
    return status;
}

/* Decode only canonical M05 prelude declarations, never a use-site spelling
 * or a fixed declaration index. User nominals use their M05 identity. */
static bool resolved_kind(const TfResolvedType *r, TfConcreteTypeKind *kind, size_t *arity) {
    static const struct { const char *name; TfConcreteTypeKind kind; size_t arity; } prelude[] = {
        {"Int", TF_CONCRETE_INT, 0}, {"Float", TF_CONCRETE_FLOAT, 0},
        {"Bool", TF_CONCRETE_BOOL, 0}, {"Text", TF_CONCRETE_TEXT, 0},
        {"Char", TF_CONCRETE_CHAR, 0}, {"Unit", TF_CONCRETE_UNIT, 0},
        {"List", TF_CONCRETE_LIST, 1}, {"Option", TF_CONCRETE_OPTION, 1},
        {"Decision", TF_CONCRETE_DECISION, 3}
    };
    *arity = 0;
    if (r->kind != TF_TYPE_BUILTIN) return nominal_kind(r->kind, kind);
    for (size_t i = 0; i < sizeof prelude / sizeof *prelude; ++i) {
        if (r->name.length == strlen(prelude[i].name) &&
            memcmp(r->name.data, prelude[i].name, r->name.length) == 0) {
            *kind = prelude[i].kind;
            *arity = prelude[i].arity;
            return true;
        }
    }
    return false;
}

typedef struct {
    const TfAstNode *syntax; /* NULL for a direct Decision request */
    TfConcreteType key;
    size_t base, next, arity;
    TypeEntry *checking; /* only the frame that owns the cycle marker */
    const TfAstNode *declaration; /* for Decision */
} TypeFrame;

typedef struct {
    TypeFrame *frames;
    size_t depth, frame_capacity;
    TfConcreteTypeId *values;
    size_t count, value_capacity;
} Conversion;

static TfStatus source_issue(TfTypeIssue *issue, const char *code, const char *message,
                             const TfAstNode *primary, TfSpan related) {
    if (issue != NULL)
        *issue = (TfTypeIssue){code, message, primary != NULL ? primary->span : (TfSpan){0}, related};
    return TF_SOURCE_ERROR;
}

static TfStatus push_type(TfTypeStore *s, Conversion *c, const TfAstNode *syntax,
                           TfTypeIssue *issue) {
    TypeFrame frame = {.syntax = syntax, .base = c->count};
    TypeFrame *frames;
    if (syntax == NULL) {
        frame.key.kind = TF_CONCRETE_DECISION; /* caller supplies the three values */
        frame.arity = frame.next = 3;
    } else if (syntax->kind == TF_AST_TYPE_FUNCTION) {
        size_t arity = syntax->as.type_function.parameters.count;
        if (!function_fits(arity) || arity == SIZE_MAX) return TF_HOST_ERROR;
        frame.key.kind = TF_CONCRETE_FUNCTION;
        frame.arity = arity + 1; /* includes result */
    } else if (syntax->kind == TF_AST_TYPE_PATH) {
        const TfResolvedReference *ref = tf_resolution_reference(s->resolution, syntax->id);
        const TfResolvedType *r;
        if (ref == NULL || ref->kind != TF_REFERENCE_TYPE) return TF_HOST_ERROR;
        r = tf_resolution_type(s->resolution, ref->type);
        if (r == NULL || !resolved_kind(r, &frame.key.kind, &frame.arity)) return TF_HOST_ERROR;
        if (syntax->as.type_path.arguments.count != frame.arity)
            return source_issue(issue, "E303", "wrong number of type arguments", syntax, r->name.span);
        if (r->kind != TF_TYPE_BUILTIN) frame.key.as.nominal = r->id;
    } else return TF_HOST_ERROR;
    if (c->depth == SIZE_MAX) return TF_HOST_ERROR;
    frames = grow(s, c->frames, c->depth, &c->frame_capacity, c->depth + 1, sizeof(*frames));
    if (frames == NULL) return TF_HOST_ERROR;
    c->frames = frames;
    c->frames[c->depth++] = frame;
    return TF_OK;
}

static bool push_value(TfTypeStore *s, Conversion *c, TfConcreteTypeId value) {
    TfConcreteTypeId *values;
    if (c->count == SIZE_MAX) return false;
    values = grow(s, c->values, c->count, &c->value_capacity, c->count + 1, sizeof(*values));
    if (values == NULL) return false;
    c->values = values;
    c->values[c->count++] = value;
    return true;
}

static const TfAstNode *type_child(const TypeFrame *frame, size_t i) {
    const TfAstNode *n = frame->syntax;
    TfNodeId id;
    if (n == NULL) return NULL;
    if (n->kind == TF_AST_TYPE_FUNCTION)
        id = i == n->as.type_function.parameters.count ? n->as.type_function.result
                                                       : n->as.type_function.parameters.items[i];
    else id = n->as.type_path.arguments.items[i];
    return tf_ast_get(n->id.owner, id);
}

static const TfAstNode *machine_declaration(const TfTypeStore *s, TfTypeId id) {
    const TfResolvedType *r = tf_resolution_type(s->resolution, id);
    const TfResolvedModule *m = tf_resolution_module(s->resolution,
                                                    (TfModuleId){s->resolution, id.module_index});
    const TfAstNode *n = r != NULL && m != NULL ? tf_ast_get(m->ast, r->declaration) : NULL;
    return n != NULL && n->kind == TF_AST_MACHINE ? n : NULL;
}

static const TfAstNode *machine_annotation(const TypeFrame *f, size_t i) {
    const TfAstNode *d = f->declaration;
    return tf_ast_get(d->id.owner, i == 0 ? d->as.machine.yields : d->as.machine.rejects);
}

/* Shared by syntax conversion and the direct Decision constructor. A Decision
 * frame may suspend to convert two annotations. Successful pairs are cached,
 * so a diamond of signature references does not re-expand the same machine. */
static TfStatus finish_decision(TfTypeStore *s, Conversion *c, TypeFrame *f,
                                TfTypeIssue *issue, bool *ready) {
    const TfConcreteTypeId *args = c->values + f->base;
    TypeEntry *machine = find_entry(s, args[0]);
    *ready = false;
    if (machine == NULL || machine->type.kind != TF_CONCRETE_MACHINE)
        return source_issue(issue, "E307", "first Decision argument must be a machine type",
                            type_child(f, 0), (TfSpan){0});
    if (f->declaration == NULL)
        f->declaration = machine_declaration(s, machine->type.as.nominal);
    if (f->declaration == NULL) return TF_HOST_ERROR;
    if (machine->output.index == 0) {
        size_t converted = c->count - f->base - 3;
        if (f->checking == NULL) {
            if (machine->checking)
                return source_issue(issue, "E307", "cycle in Decision machine signatures",
                                    f->syntax, f->declaration->span);
            f->checking = machine;
            machine->checking = true;
        }
        if (converted < 2) {
            const TfAstNode *annotation = machine_annotation(f, converted);
            if (annotation == NULL) return TF_HOST_ERROR;
            return push_type(s, c, annotation, issue);
        }
        machine->output = args[3];
        machine->rejection = args[4];
        machine->checking = false;
        f->checking = NULL;
    }
    if (!tf_concrete_type_id_equal(args[1], machine->output) ||
        !tf_concrete_type_id_equal(args[2], machine->rejection)) {
        size_t i = tf_concrete_type_id_equal(args[1], machine->output) ? 1 : 0;
        const TfAstNode *annotation = machine_annotation(f, i);
        if (annotation == NULL) return TF_HOST_ERROR;
        return source_issue(issue, "E307", i == 0 ? "Decision output differs from machine yields type"
                                                 : "Decision rejection differs from machine rejects type",
                            type_child(f, i + 1), annotation->span);
    }
    f->key.as.decision.machine = args[0];
    f->key.as.decision.output = args[1];
    f->key.as.decision.rejection = args[2];
    *ready = true;
    return TF_OK;
}

static TfStatus convert(TfTypeStore *s, const TfAstNode *syntax, const TfConcreteTypeId *decision,
                        TfConcreteTypeId *out, TfTypeIssue *issue) {
    Conversion c = {0};
    TfStatus status = push_type(s, &c, syntax, issue);
    if (status == TF_OK && decision != NULL)
        for (size_t i = 0; i < 3 && status == TF_OK; ++i)
            if (!push_value(s, &c, decision[i])) status = TF_HOST_ERROR;
    while (status == TF_OK && c.depth != 0) {
        TypeFrame *f = &c.frames[c.depth - 1];
        TfConcreteTypeId value;
        if (f->next < f->arity) {
            const TfAstNode *child = type_child(f, f->next++);
            status = child != NULL ? push_type(s, &c, child, issue) : TF_HOST_ERROR;
            continue;
        }
        switch (f->key.kind) {
        case TF_CONCRETE_LIST:
        case TF_CONCRETE_OPTION:
            f->key.as.element = c.values[f->base];
            break;
        case TF_CONCRETE_FUNCTION:
            f->key.as.function.arity = f->arity - 1;
            f->key.as.function.parameters = f->arity > 1 ? c.values + f->base : NULL;
            f->key.as.function.result = c.values[f->base + f->arity - 1];
            break;
        case TF_CONCRETE_DECISION: {
            bool ready;
            status = finish_decision(s, &c, f, issue, &ready);
            if (status != TF_OK || !ready) continue;
            break;
        }
        default:
            break;
        }
        status = intern(s, f->key, &value);
        if (status == TF_OK) {
            c.count = f->base;
            --c.depth;
            if (c.depth == 0) *out = value;
            else if (!push_value(s, &c, value)) status = TF_HOST_ERROR;
        }
    }
    for (size_t i = 0; i < c.depth; ++i)
        if (c.frames[i].checking != NULL) c.frames[i].checking->checking = false;
    release(s, c.frames);
    release(s, c.values);
    return status;
}

TfStatus tf_type_decision(TfTypeStore *s, TfConcreteTypeId machine, TfConcreteTypeId output,
                          TfConcreteTypeId rejection, TfConcreteTypeId *out, TfTypeIssue *issue) {
    TfConcreteTypeId args[] = {machine, output, rejection};
    if (issue != NULL) *issue = (TfTypeIssue){0};
    if (out == NULL) return TF_HOST_ERROR;
    *out = (TfConcreteTypeId){0};
    for (size_t i = 0; i < sizeof args / sizeof *args; ++i)
        if (find_entry(s, args[i]) == NULL) return TF_HOST_ERROR;
    return convert(s, NULL, args, out, issue);
}

TfStatus tf_type_from_syntax(TfTypeStore *s, TfNodeId syntax, TfConcreteTypeId *out,
                             TfTypeIssue *issue) {
    if (issue != NULL) *issue = (TfTypeIssue){0};
    if (out == NULL) return TF_HOST_ERROR;
    *out = (TfConcreteTypeId){0};
    if (s == NULL || syntax.owner == NULL || syntax.index == 0) return TF_HOST_ERROR;
    for (size_t i = 0; i < tf_resolution_module_count(s->resolution); ++i) {
        const TfResolvedModule *m = tf_resolution_module_at(s->resolution, i);
        if (m->ast == syntax.owner) {
            const TfAstNode *n = tf_ast_get(m->ast, syntax);
            return n != NULL ? convert(s, n, NULL, out, issue) : TF_HOST_ERROR;
        }
    }
    return TF_HOST_ERROR;
}
