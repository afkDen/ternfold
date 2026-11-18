#ifndef TERNFOLD_CORE_OPTIMIZE_H
#define TERNFOLD_CORE_OPTIMIZE_H

#include "ternfold/core_verify.h"

typedef enum { TF_CORE_OPT_LEVEL_0 = 0, TF_CORE_OPT_LEVEL_1 = 1 } TfCoreOptLevel;
typedef struct TfCoreOptimization TfCoreOptimization;

/* Borrow verified input for this call only. Both levels return an independent
 * deep copy, verified again before publication. Level 0 is the default/reference.
 * Level 1 replaces only proven nonfaulting scalar Unary/Binary operations with
 * Const, retaining every Let, value ID, span and observation boundary. Existing
 * constant IDs are preserved; folded constants are appended. No AST is read.
 * Only TfSpan.source and allocator context remain borrowed and must outlive out.
 * Fresh out is cleared on any failure; no partial Core is published. */
TfStatus tf_core_optimize(TfCoreOptimization **out, const TfVerifiedCore *input,
                          TfCoreOptLevel level, const TfAllocator *allocator);
const TfVerifiedCore *tf_core_optimization_verified(const TfCoreOptimization *out);
size_t tf_core_optimization_fold_count(const TfCoreOptimization *out);
void tf_core_optimization_destroy(TfCoreOptimization *out);

#endif
