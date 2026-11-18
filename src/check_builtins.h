#ifndef TERNFOLD_CHECK_BUILTINS_H
#define TERNFOLD_CHECK_BUILTINS_H

#include "check_internal.h"

/* Adapter for TfCheckAdapters.builtin. Schemes are local to this use; both
 * returned IDs (when present) belong to the signatures' type store. */
TfStatus tf_check_builtin(TfCheckContext *, const TfAstNode *use,
                          const TfResolvedSymbol *, TfCheckExpectation,
                          TfConcreteTypeId *result, TfConcreteTypeId *callable);

#endif
