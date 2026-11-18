#ifndef TERNFOLD_CORE_LOWER_H
#define TERNFOLD_CORE_LOWER_H

#include "ternfold/core.h"
#include "ternfold/machine_plan.h"

typedef enum {
    TF_CORE_LOWER_NONE = 0, TF_CORE_LOWER_UNSUPPORTED_FORM, TF_CORE_LOWER_INVARIANT
} TfCoreLowerCode;
typedef struct { TfCoreLowerCode code; TfSpan span; } TfCoreLowerIssue;

/* Lower a valid machine-program owner into independent, frozen Core.
 * The result borrows only the original source snapshots used by spans.
 * The returned owner is unverified; call tf_core_verify before consuming it.
 * Lowers typed functions, closures, matches, machine plans, and the hidden
 * host entry adapter. A failure aborts lowering and publishes no Core.
 * TF_HOST_ERROR clears out. The optional issue describes an unsupported form
 * or invariant failure; a zero issue denotes invalid API use or resource failure. */
TfStatus tf_core_lower(TfCoreProgram **out, const TfMachineProgram *machine_program,
                       const TfAllocator *allocator, TfCoreLowerIssue *issue);

#endif
