#ifndef TERNFOLD_REPL_LOWER_H
#define TERNFOLD_REPL_LOWER_H

#include "ternfold/core_lower.h"

/* Lower a checked session root. The caller supplies the symbol of a private
 * root fn () -> Unit and the AST ID of a direct let in its body. Its synthetic
 * name must be absent from user identifiers: source cannot use the wrapper's
 * original signature. Evaluate the checked body in order, yielding the
 * selected initializer's actual value instead of the final Unit. The wrapper
 * remains ordinary and the program's designated entry is unchanged.
 * Both outputs clear on failure; verification is required before execution. */
TfStatus tf_core_lower_repl(TfCoreProgram **out, TfCoreFunctionId *function,
                           const TfMachineProgram *machine_program, TfSymbolId wrapper,
                           TfNodeId result_let, const TfAllocator *allocator,
                           TfCoreLowerIssue *issue);

#endif
