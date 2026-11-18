#ifndef TERNFOLD_CORE_LOWER_CONSTANTS_H
#define TERNFOLD_CORE_LOWER_CONSTANTS_H

#include "lower_types.h"

/* Owns a lowering scratch constant pool; borrows the layout tables. A final
 * tf_core_program_copy must copy the records before releasing this owner. */
typedef struct TfCoreLowerConstants TfCoreLowerConstants;
TfStatus tf_core_lower_constants_create(TfCoreLowerConstants **out, TfCoreLowerTypes *types,
                                        const TfAllocator *allocator);
void tf_core_lower_constants_destroy(TfCoreLowerConstants *constants);
/* Copy a literal or aggregate descriptor, whose child IDs (if any) already
 * exist. Does not verify the descriptor. On failure publish no new constant. */
TfStatus tf_core_lower_constant_append(TfCoreLowerConstants *constants, TfCoreConstant constant,
                                       TfCoreConstantId *out);
/* Copy the checked closed initializer and its children into Core metadata.
 * On failure roll back the constant pool. An interned span may remain. */
TfStatus tf_core_lower_initializer_constant(TfCoreLowerConstants *constants, TfNodeId state,
                                            TfCoreConstantId *out);
void tf_core_lower_constant_view(const TfCoreLowerConstants *constants, TfCoreProgramData *data);

#endif
