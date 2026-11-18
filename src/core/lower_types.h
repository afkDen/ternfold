#ifndef TERNFOLD_CORE_LOWER_TYPES_H
#define TERNFOLD_CORE_LOWER_TYPES_H

#include "ternfold/core.h"
#include "ternfold/machine_plan.h"

/* Lowering scratch tables, borrowing a valid machine program. Source concrete
 * IDs keep their one-based ordinal. Synthesized List, Decision and Tag types append.
 * No semantic handle borrows this owner after tf_core_program_copy. */
typedef struct TfCoreLowerTypes TfCoreLowerTypes;
TfStatus tf_core_lower_types_create(TfCoreLowerTypes **out, const TfMachineProgram *program,
                                    const TfAllocator *allocator);
void tf_core_lower_types_destroy(TfCoreLowerTypes *types);
const TfTypedProgram *tf_core_lower_types_program(const TfCoreLowerTypes *types);
TfCoreTypeId tf_core_lower_type_id(const TfCoreLowerTypes *types, TfConcreteTypeId type);
/* For generated functions, resolve a plan's typed constructor result to the
 * Core snapshot ID. For an already known concrete machine ID use
 * tf_core_lower_type_id directly. Returns 0 for a missing or mismatched plan. */
TfCoreTypeId tf_core_lower_machine_id(const TfCoreLowerTypes *types, const TfMachinePlan *plan);
/* Reuse the source Decision or append the machine's unique Decision and, if
 * needed, List[output]. On failure clears *out and adds no type entries. */
TfStatus tf_core_lower_decision(TfCoreLowerTypes *types, TfCoreTypeId machine, TfCoreTypeId *out);
TfStatus tf_core_lower_list(TfCoreLowerTypes *types, TfCoreTypeId element, TfCoreTypeId *out);
TfStatus tf_core_lower_tag(TfCoreLowerTypes *types, TfCoreTypeId tagged, TfCoreTypeId *out);
/* Bool is needed for synthesized literal-pattern tests even when no source
 * expression has Bool type. Reuse the original ID if present. */
TfStatus tf_core_lower_bool(TfCoreLowerTypes *types, TfCoreTypeId *out);
/* Borrow validated display metadata or omit an invalid native path. Names
 * do not participate in nominal type identity. */
TfCoreText tf_core_lower_module_name(const TfResolvedModule *module);
TfStatus tf_core_lower_span(TfCoreLowerTypes *types, TfSpan span, TfCoreSpanId *out);
/* Fill only type and span arrays. Borrows remain valid until a table grows or
 * the owner is destroyed. Take the final view after all lowering additions. */
void tf_core_lower_type_view(const TfCoreLowerTypes *types, TfCoreProgramData *data);

#endif
