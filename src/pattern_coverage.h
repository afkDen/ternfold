#ifndef TERNFOLD_PATTERN_COVERAGE_H
#define TERNFOLD_PATTERN_COVERAGE_H

#include "ternfold/lexer.h"

typedef struct TfCoverageDomain TfCoverageDomain;
typedef struct TfCoveragePattern TfCoveragePattern;

/* The caller supplies canonical constructor identities. A finite domain lists
 * all its constructors in deterministic witness order. An open domain has no
 * list; its literal constructors are nullary and interned by typed value, not
 * spelling. Opaque domains are open but have no literal constructors. */
typedef struct {
    const TfCoverageDomain *domain;
    const TfCoverageDomain *const *payload;
    size_t arity;
} TfCoverageConstructor;
struct TfCoverageDomain {
    const TfCoverageConstructor *const *constructors;
    size_t count;
};
struct TfCoveragePattern {
    const TfCoverageConstructor *constructor;
    const TfCoveragePattern *const *arguments;
};
typedef struct TfCoverageWitness TfCoverageWitness;

/* NULL patterns mean wildcard or binder. All supplied trees are already
 * type-checked, acyclic, and borrowed (including constructors/payload domains).
 * A returned witness owns its tree and borrows the constructor descriptors.
 * TF_OK with *out=NULL means covered; a non-NULL witness means useful.
 * The witness root may itself be NULL for a symbolic residual wildcard.
 * HOST_ERROR publishes nothing, including on work or stack limits.
 * Search caps charged cells and allocations at 8 Mi units and recursive
 * specializations at 192 (distinct from parser syntax nesting). */
TfStatus tf_coverage_witness(TfCoverageWitness **out, const TfCoverageDomain *domain,
                             const TfCoveragePattern *const *previous, size_t count,
                             const TfCoveragePattern *query, const TfAllocator *allocator);
enum { TF_COVERAGE_WORK = 8*1024*1024 };
/* As above, drawing from a budget shared by all queries in one match. */
TfStatus tf_coverage_witness_budget(TfCoverageWitness **out, const TfCoverageDomain *domain,
                                    const TfCoveragePattern *const *previous, size_t count,
                                    const TfCoveragePattern *query, const TfAllocator *allocator,
                                    size_t *remaining);
const TfCoveragePattern *tf_coverage_root(const TfCoverageWitness *witness);
void tf_coverage_destroy(TfCoverageWitness *witness);

#endif
