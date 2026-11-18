#ifndef TERNFOLD_C_EMIT_H
#define TERNFOLD_C_EMIT_H

#include "ternfold/core_verify.h"
#include "ternfold/core_optimize.h"

/* Writes C17 from verified Core only. The caller owns the binary byte stream
 * and publishes it only after success and flush/close. */
TfStatus tf_c_emit(FILE *out,const TfVerifiedCore *program);
/* Level one folds checked scalar operations and releases eligible slots only
 * after their final execution/observation use. Level zero is the reference. */
TfStatus tf_c_emit_optimized(FILE *out,const TfVerifiedCore *program,TfCoreOptLevel level);

#endif
