#ifndef TERNFOLD_CORE_VERIFY_H
#define TERNFOLD_CORE_VERIFY_H

#include "ternfold/core.h"

typedef enum {
    TF_CORE_VERIFY_NONE = 0,
    TF_CORE_VERIFY_SPAN,
    TF_CORE_VERIFY_TYPE,
    TF_CORE_VERIFY_TYPE_CYCLE,
    TF_CORE_VERIFY_CONSTANT,
    TF_CORE_VERIFY_ENTRY,
    TF_CORE_VERIFY_FUNCTION,
    TF_CORE_VERIFY_BLOCK,
    TF_CORE_VERIFY_VALUE,
    TF_CORE_VERIFY_OPERATION,
    TF_CORE_VERIFY_TAG_PROOF,
    TF_CORE_VERIFY_TERMINATOR,
    TF_CORE_VERIFY_OBSERVATION
} TfCoreVerifyCode;

/* First malformed-IR finding. Unused IDs are zero. The offending span ID
 * itself may be invalid for TF_CORE_VERIFY_SPAN. This is an internal compiler
 * finding, not a source diagnostic. */
typedef struct {
    TfCoreVerifyCode code;
    TfCoreSpanId span;
    TfCoreTypeId type;
    TfCoreConstantId constant;
    TfCoreFunctionId function;
    TfCoreBlockId block;
    TfCoreValueId value;
} TfCoreVerifyIssue;

/* Borrows the frozen owner and its source objects. They must outlive this
 * handle and any data read through it. The handle owns only its own storage;
 * destroy it before the program. The allocator context must outlive it.
 *
 * Requires fresh out. Returns TF_SOURCE_ERROR for malformed Core and
 * TF_HOST_ERROR for API misuse or resource failure; either clears *out.
 * The optional issue is zeroed on success and on host error. */
TfStatus tf_core_verify(TfVerifiedCore **out, const TfCoreProgram *program,
                        const TfAllocator *allocator, TfCoreVerifyIssue *issue);
const TfCoreProgramData *tf_verified_core_data(const TfVerifiedCore *verified);
void tf_verified_core_destroy(TfVerifiedCore *verified); /* NULL permitted */

#endif
