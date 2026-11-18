#ifndef TERNFOLD_EXEC_OPERATIONS_H
#define TERNFOLD_EXEC_OPERATIONS_H

#include "values.h"

typedef enum { TF_EXEC_OK, TF_EXEC_FAULT, TF_EXEC_HOST } TfExecStatus;

/* Inputs are borrowed, outputs refer to the run's value arena. A fault
 * writes its code without publishing an output. Origins belong to the VM. */
TfExecStatus tf_exec_unary(const TfCoreProgramData *program, TfCoreUnary op,
                           TfExecValue value, TfCoreTypeId result, TfExecValue *out,
                           TfRuntimeCode *fault);
TfExecStatus tf_exec_binary(TfExecArena *arena, const TfCoreProgramData *program,
                            TfCoreBinary op, TfExecValue left, TfExecValue right,
                            TfCoreTypeId result, TfExecValue *out, TfRuntimeCode *fault,
                            uint64_t *steps);

#endif
