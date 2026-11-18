#ifndef TERNFOLD_EXEC_BUILTINS_H
#define TERNFOLD_EXEC_BUILTINS_H

#include "operations.h"

/* Non-callback prelude. The VM handles map/fold as iterative control
 * frames; the lowerer has already translated step to an ordinary call. */
TfExecStatus tf_exec_builtin(TfExecArena *arena, const TfCoreProgramData *program,
                             TfCoreBuiltin builtin, TfCoreTypeId result, const TfExecValue *args,
                             TfExecValue *out, TfRuntimeCode *fault, uint64_t *steps);

#endif
