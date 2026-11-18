#ifndef TERNFOLD_CHECK_H
#define TERNFOLD_CHECK_H

#include "ternfold/types.h"

typedef struct TfTypedSignatures TfTypedSignatures;
typedef struct TfTypedProgram TfTypedProgram;

typedef struct {
    TfNodeId node;
    TfConcreteTypeId type;
    TfSymbolId target; /* lexical reference/binder or resolved member, if any */
    size_t member_index; /* zero-based for projections and named record arguments */
} TfTypedNode;

/* Requires a valid resolution and successful signatures built from that same
 * resolution. The result borrows both and the allocator context; it owns its
 * immutable node/symbol tables and diagnostic bag. The signatures' type store
 * may intern new types, including before a failed check; prior IDs are stable.
 * Do not reenter or mutate the same store concurrently.
 *
 * TF_OK owns an inspectable result: VALID for a fully typed program or
 * ERROR for diagnosed errors. Machine scheduling is a separate phase.
 * HOST_ERROR (including resource limits) clears the fresh output. The cap is
 * 0..100; zero suppresses records but still records the error phase.
 */
TfStatus tf_check(TfTypedProgram **out, const TfResolution *resolution,
                  TfTypedSignatures *signatures, size_t diagnostic_cap,
                  const TfAllocator *allocator);
void tf_typed_program_destroy(TfTypedProgram *program);
const TfDiagnosticBag *tf_typed_program_diagnostics(const TfTypedProgram *program);
const TfDiagnosticPhaseResult *tf_typed_program_phase(const TfTypedProgram *program);
const TfResolution *tf_typed_program_resolution(const TfTypedProgram *program);
const TfTypeStore *tf_typed_program_store(const TfTypedProgram *program);

/* Borrows by stable resolution/AST ID. Absent/foreign IDs and nodes without a
 * concrete result have no record. Qualifier fragments and other syntax-only
 * nodes are not expressions and need no record. On ERROR/INVALID these tables
 * are for inspection only. */
const TfTypedNode *tf_typed_program_node(const TfTypedProgram *program, TfNodeId node);
TfConcreteTypeId tf_typed_program_symbol(const TfTypedProgram *program, TfSymbolId symbol);

#endif
