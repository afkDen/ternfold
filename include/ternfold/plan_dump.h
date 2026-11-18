#ifndef TERNFOLD_PLAN_DUMP_H
#define TERNFOLD_PLAN_DUMP_H

#include "ternfold/machine_plan.h"

/* Allocation-free inspection of a VALID borrowed machine program. All IDs
 * are stable source identities, not host addresses. Names are ASCII source
 * identifiers. JSON v1 emits machines in program order; cell IDs are
 * zero-based and node IDs are module-local. Byte spans are half-open.
 * Includes modules' canonical source paths, cell and writer identities,
 * schedules, dependency occurrence indexes, lexical read contexts and
 * source-ordered guard/rejection/emission expressions.
 * Refuses an erroneous program before writing. Caller owns the binary
 * stream, flush and close; an observed stream failure returns HOST_ERROR
 * and may leave a prefix. Does not execute candidates or prove guards. */
TfStatus tf_plan_dump_write(FILE *stream, const TfMachineProgram *program, bool json);

#endif
