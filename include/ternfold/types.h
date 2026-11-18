#ifndef TERNFOLD_TYPES_H
#define TERNFOLD_TYPES_H
#include "ternfold/resolve.h"

typedef struct TfTypeStore TfTypeStore;
/* Store-scoped, one-based; {0} is absent. TfTypeId identifies a declaration
 * in M05, whereas TfConcreteTypeId identifies an interned concrete type. */
typedef struct { const TfTypeStore *owner; size_t index; } TfConcreteTypeId;
typedef enum {
    TF_CONCRETE_INT, TF_CONCRETE_FLOAT, TF_CONCRETE_BOOL, TF_CONCRETE_TEXT,
    TF_CONCRETE_CHAR, TF_CONCRETE_UNIT, TF_CONCRETE_LIST, TF_CONCRETE_OPTION,
    TF_CONCRETE_DECISION, TF_CONCRETE_RECORD, TF_CONCRETE_ENUM,
    TF_CONCRETE_MACHINE, TF_CONCRETE_FUNCTION
} TfConcreteTypeKind;

typedef struct {
    TfConcreteTypeId id;
    TfConcreteTypeKind kind;
    union {
        TfConcreteTypeId element; /* List, Option */
        struct { TfConcreteTypeId machine, output, rejection; } decision;
        TfTypeId nominal; /* Record, Enum, Machine */
        struct {
            const TfConcreteTypeId *parameters; /* NULL for zero arity */
            size_t arity;
            TfConcreteTypeId result;
        } function;
    } as;
} TfConcreteType;

/* Optional description of TF_SOURCE_ERROR. Text is static; spans borrow the
 * resolution's sources. A span with source==NULL is absent. For a direct
 * Decision request, supply a use-site span in the checker if primary is absent. */
typedef struct {
    const char *code, *message;
    TfSpan primary, related;
} TfTypeIssue;

/* The store owns all type records and copies function parameter arrays. It
 * borrows a valid, frozen M05 resolution and allocator context, which must
 * outlive it and all borrowers. NULL allocator selects malloc/free. Destroy
 * once (NULL permitted); do not copy the owner or reenter/concurrently mutate
 * it. All returned records and arrays remain immutable at stable addresses
 * until destruction, even through insertions and failed operations.
 *
 * Create requires a fresh output and clears it on failure. Constructors and
 * conversion clear *out on failure and *issue on entry (if supplied). Bad
 * handles/arguments, overflow and allocation failure return TF_HOST_ERROR;
 * semantic failures return TF_SOURCE_ERROR. Complete subtypes interned during
 * conversion may remain on failure. There is no concrete ErrorType or type
 * variable; checker recovery must track failure separately. IDs are comparable
 * by owner and index, and never valid beyond their owner's lifetime. */
TfStatus tf_type_store_create(TfTypeStore **out, const TfResolution *resolution,
                              const TfAllocator *allocator);
void tf_type_store_destroy(TfTypeStore *store);
size_t tf_type_store_count(const TfTypeStore *store);
const TfConcreteType *tf_type_store_at(const TfTypeStore *store, size_t index); /* zero-based */
const TfConcreteType *tf_type_store_get(const TfTypeStore *store, TfConcreteTypeId id);
bool tf_concrete_type_id_equal(TfConcreteTypeId left, TfConcreteTypeId right);

TfStatus tf_type_primitive(TfTypeStore *store, TfConcreteTypeKind kind, TfConcreteTypeId *out);
TfStatus tf_type_nominal(TfTypeStore *store, TfTypeId declaration, TfConcreteTypeId *out);
TfStatus tf_type_list(TfTypeStore *store, TfConcreteTypeId element, TfConcreteTypeId *out);
TfStatus tf_type_option(TfTypeStore *store, TfConcreteTypeId element, TfConcreteTypeId *out);
TfStatus tf_type_function(TfTypeStore *store, const TfConcreteTypeId *parameters, size_t arity,
                          TfConcreteTypeId result, TfConcreteTypeId *out);
/* Checks that machine is nominal and output/rejection match its declarations
 * exactly. These annotations are read from the frozen AST, not caller metadata. */
TfStatus tf_type_decision(TfTypeStore *store, TfConcreteTypeId machine, TfConcreteTypeId output,
                          TfConcreteTypeId rejection, TfConcreteTypeId *out, TfTypeIssue *issue);

/* Accepts a type-path or function-type node owned by the bound resolution.
 * Reports E303 for type arity and E307 for invalid Decision
 * parameters, including cycles while expanding declared yields/rejects.
 * This does not validate the full nominal/signature dependency graph, privacy,
 * data-only state, or expressions; those are checker responsibilities. */
TfStatus tf_type_from_syntax(TfTypeStore *store, TfNodeId syntax, TfConcreteTypeId *out,
                             TfTypeIssue *issue);

#endif
