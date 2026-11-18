#ifndef TERNFOLD_BACKEND_C_NODES_H
#define TERNFOLD_BACKEND_C_NODES_H

#include "ternfold/core_verify.h"

/* Structured C control shape for one verified function. Block references are
 * local; a nested block has one parent operation and one completion edge.
 * Source ValueIds stay stable in native slots. Borrowed argument staging
 * occupies a disjoint tail of the frame and must be cleared before return. */
typedef enum { TF_C_ROOT, TF_C_BRANCH, TF_C_ITERATION } TfCExitKind;
typedef struct { TfCExitKind kind; TfCoreValueId parent_value, element; } TfCBlockExit;
typedef struct {
    const TfCoreFunction *function;
    TfCBlockExit *exits;
    TfCoreTypeId *types;
    size_t argument_slots;
} TfCBody;

/* Requires verified metadata. Owns only exits/types, using malloc/free. */
TfStatus tf_c_body(TfCBody *out,const TfVerifiedCore *core,TfCoreFunctionId function);
void tf_c_body_destroy(TfCBody *body);

#endif
