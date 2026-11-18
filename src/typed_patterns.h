#ifndef TERNFOLD_TYPED_PATTERNS_H
#define TERNFOLD_TYPED_PATTERNS_H

#include "typed_signatures.h"
#include "literal_value.h"

typedef struct TfTypedPatterns TfTypedPatterns;
typedef struct {
    TfNodeId node;
    TfConcreteTypeId type;
    TfSymbolId target; /* variant or binder, if applicable */
    bool has_literal;
    TfLiteralValue literal;
} TfTypedPatternNode;

/* Check the patterns of a resolved match against a concrete scrutinee type.
 * Adds E301/E312 for ill-typed patterns, or E310/E311 for coverage errors.
 * TF_OK returns binder information even with diagnosed errors; HOST_ERROR
 * clears out and the caller must discard its incomplete diagnostic phase.
 * Result borrows the resolution, type store and allocator context. */
TfStatus tf_typed_patterns(TfTypedPatterns **out, const TfResolution *resolution,
                           TfTypedSignatures *signatures, const TfAstNode *match,
                           TfConcreteTypeId scrutinee, TfDiagnosticBag *destination,
                           const TfAllocator *allocator);
TfConcreteTypeId tf_typed_pattern_binding(const TfTypedPatterns *patterns, TfSymbolId symbol);
/* Read-only pattern facts for lowering; NULL for foreign/invalid pattern IDs. */
const TfTypedPatternNode *tf_typed_pattern_node(const TfTypedPatterns *patterns, TfNodeId node);
void tf_typed_patterns_destroy(TfTypedPatterns *patterns);

#endif
