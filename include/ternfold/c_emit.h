#ifndef TERNFOLD_C_EMIT_H
#define TERNFOLD_C_EMIT_H

#include "ternfold/core_verify.h"

/* Writes C17 from verified Core only. The caller owns the binary byte stream
 * and publishes it only after success and flush/close. */
TfStatus tf_c_emit(FILE *out,const TfVerifiedCore *program);

#endif
