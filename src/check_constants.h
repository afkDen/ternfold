#ifndef TERNFOLD_CHECK_CONSTANTS_H
#define TERNFOLD_CHECK_CONSTANTS_H

#include "check_internal.h"

/* Checker adapter for the closed state initializer grammar. It validates
 * original syntax, then checks constant faults in source evaluation order.
 * It is not an executor for general expressions. */
TfStatus tf_check_constant(TfCheckContext *context, const TfAstNode *state);

/* Temporary compile-time value for lowering a previously checked machine
 * initializer. The compiler type and variant IDs are for translation into
 * Core metadata; executors never consume these AST-bound facts. */
typedef struct TfConstantValue TfConstantValue;
struct TfConstantValue {
    TfConcreteTypeId type;
    TfConcreteTypeKind kind;
    TfSymbolId variant; /* present only for enum, Option and Decision values */
    union {
        int64_t integer;
        uint64_t bits;
        bool boolean;
        uint32_t scalar;
        struct { const unsigned char *bytes; size_t length; } text;
        struct { const TfConstantValue *const *items; size_t count; } aggregate;
    } as;
};
typedef TfStatus (*TfConstantVisitor)(void *context, const TfConstantValue *value);

/* Requires a valid typed program and its state declaration node. Evaluates
 * only the checked closed initializer grammar using the same implementation
 * as M06. Calls visit once on success; the tree and bytes are borrowed only
 * during the call and must be copied by the visitor if retained. No visitor
 * call on a host/allocation/invariant failure. A visitor failure is returned
 * as HOST_ERROR. NULL allocator selects malloc/free. No program mutation. */
TfStatus tf_visit_checked_initializer(const TfTypedProgram *program, TfNodeId state,
                                      const TfAllocator *allocator, TfConstantVisitor visit,
                                      void *context);

#endif
