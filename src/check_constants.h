#ifndef TERNFOLD_CHECK_CONSTANTS_H
#define TERNFOLD_CHECK_CONSTANTS_H

#include "check_internal.h"

/* Checker adapter for the closed state initializer grammar. It validates
 * original syntax, then checks constant faults in source evaluation order.
 * It is not an executor for general expressions. */
TfStatus tf_check_constant(TfCheckContext *context, const TfAstNode *state);

#endif
