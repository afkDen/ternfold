#ifndef TERNFOLD_C_METADATA_H
#define TERNFOLD_C_METADATA_H

#include "ternfold/core_verify.h"

/* Append immutable metadata to the caller's byte-oriented binary C stream.
 * The caller supplies ternfold_rt_constants.h (includes ternfold_rt.h) first.
 * tf_types, tf_literals and tf_origins are static arrays with corresponding
 * size_t counts. Look up nonzero IDs at id - 1; origin ID zero means absent.
 * Types preserve Core IDs and append a function marker at core.type_count + 1.
 * Emit tf_literal_id_<CoreId> wherever a Core Const needs a runtime pool ID.
 * tf_generated_origin has counted unsigned path bytes (not a C string), byte
 * offsets and one-based line/scalar columns for each half-open span endpoint.
 * The program and stream are borrowed. On error discard the private output;
 * the caller owns flushing, closing and publishing the complete TU. */
TfStatus tf_c_metadata_write(FILE *out, const TfVerifiedCore *program);

#endif
