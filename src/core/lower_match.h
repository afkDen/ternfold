#ifndef TERNFOLD_CORE_LOWER_MATCH_H
#define TERNFOLD_CORE_LOWER_MATCH_H

#include "lower_types.h"

typedef struct TfCoreMatchFact TfCoreMatchFact;
struct TfCoreMatchFact {
    const TfCoreMatchFact *prior;
    TfCoreValueId subject;
    size_t exact; /* SIZE_MAX when excluding a default's explicit cases */
    const TfCoreTagCase *excluded;
    size_t excluded_count;
};

/* Operations borrow a single lowering transaction. `allocate` owns all
 * returned scratch arrays until the transaction ends. `emit` may borrow its
 * counted arrays through the transaction. `bind` records the pattern's
 * resolved symbol for the selected arm body; the same arm may be lowered
 * in multiple branches, and its binders are installed again in each. */
typedef struct {
    void *context;
    const TfTypedProgram *typed;
    TfCoreLowerTypes *types;
    size_t *remaining_work; /* shared across this program, including nested matches */
    const TfCoreMatchFact *inherited;
    void *(*allocate)(void *, size_t count, size_t width);
    bool (*failed)(const void *);
    TfCoreValueId (*invariant)(void *, const TfAstNode *);
    TfCoreBlockId (*block)(void *, TfSpan);
    TfCoreValueId (*emit)(void *, TfCoreBlockId, TfCoreTypeId, TfCoreOperation, TfSpan);
    TfCoreValueId (*constant)(void *, TfCoreBlockId, TfCoreConstant, TfSpan);
    TfCoreValueId (*expression)(void *, TfCoreBlockId, TfNodeId, const TfCoreMatchFact *);
    void (*yield)(void *, TfCoreBlockId, TfCoreValueId, TfSpan);
    void (*unreachable)(void *, TfCoreBlockId, TfSpan);
    bool (*bind)(void *, TfNodeId, TfCoreValueId);
    bool (*variant_tag)(void *, TfNodeId, size_t *);
} TfCoreMatchBuilder;

/* Lower an exhaustive checked match. The already evaluated scrutinee is
 * visible in block. Nested blocks are finalized; block remains open. Zero
 * means abort the transaction, including on a local work/size limit where
 * no hook necessarily set the shared failure flag. */
TfCoreValueId tf_core_lower_match(const TfCoreMatchBuilder *builder, TfCoreBlockId block,
                                   const TfAstNode *match, TfCoreTypeId result,
                                   TfCoreValueId scrutinee);

#endif
