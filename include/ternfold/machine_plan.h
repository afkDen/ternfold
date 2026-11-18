#ifndef TERNFOLD_MACHINE_PLAN_H
#define TERNFOLD_MACHINE_PLAN_H
#include "ternfold/check.h"

typedef struct TfMachineProgram TfMachineProgram;
/* Machine-scoped zero-based indexes into cells; SIZE_MAX denotes no cell. */
typedef size_t TfMachineCellId;
typedef enum {
    TF_MACHINE_INITIALIZER, TF_MACHINE_WRITER, TF_MACHINE_GUARD,
    TF_MACHINE_REJECTION, TF_MACHINE_EMISSION
} TfMachineReadContext;
typedef struct {
    TfNodeId node, expression;
    TfSpan span;
} TfMachineClause;
typedef struct {
    TfMachineCellId id;
    TfSymbolId symbol;
    TfAstText name;
    TfConcreteTypeId type;
    TfMachineClause declaration;
    /* Indexes into writers in source order, including duplicates on ERROR. */
    const size_t *writers;
    size_t writer_count;
} TfMachineCell;
typedef struct {
    TfMachineClause clause;
    TfMachineCellId cell;
} TfMachineWriter;
typedef struct {
    TfMachineClause clause;
    TfNodeId rejection;
} TfMachineGuard;
typedef struct {
    TfReferenceKind kind; /* OLD, INPUT or CANDIDATE */
    TfSymbolId symbol;
    TfNodeId node;
    TfSpan span;
    TfMachineReadContext context;
    size_t clause_index; /* index into cells/writers/guards/emissions by context */
    TfMachineCellId cell; /* SIZE_MAX for input */
} TfMachineRead;
typedef struct {
    TfMachineCellId prerequisite, dependent; /* a -> b for writer b reading next.a */
    const size_t *occurrences; /* indexes into reads; every lexical projection */
    size_t occurrence_count;
} TfMachineDependency;
typedef struct {
    TfTypeId type;
    TfNodeId declaration;
    TfSpan span;
    TfAstText name;
    TfSymbolId input;
    const TfMachineCell *cells; /* state declaration order */
    size_t cell_count;
    const TfMachineWriter *writers; /* source order */
    size_t writer_count;
    const TfMachineGuard *guards; /* source order; rejection evaluated on false only */
    size_t guard_count;
    const TfMachineClause *emissions; /* source order; private until success */
    size_t emission_count;
    const TfMachineRead *reads; /* lexical byte order, including dead branches */
    size_t read_count;
    const TfMachineDependency *dependencies; /* unique pairs, sorted by cell IDs; absent on coverage errors */
    size_t dependency_count;
    const TfMachineCellId *schedule; /* Kahn: smallest ready ASCII name each time */
    size_t schedule_count; /* zero on writer/cycle errors; never a partial schedule */
} TfMachinePlan;

/* Requires a VALID borrowed TfTypedProgram; it, its resolution, store, sources
 * and allocator context must outlive this owner. Owns a frozen phase arena and
 * diagnostic bag, never executes/folds ASTs or mutates its borrowed inputs.
 * NULL allocator uses malloc/free. cap 0..100, including suppressed errors.
 * TF_OK returns an inspectable owner, VALID payload==owner or ERROR without
 * payload. ERROR records are inspection-only, never lowering/execution input.
 * Invalid arguments/overflow/OOM: HOST_ERROR and *out=NULL, complete cleanup.
 * Fresh output required; destroy once, NULL allowed. All accessor borrows and
 * arrays remain stable until destruction. No mutable global compiler state. */
TfStatus tf_machine_program_build(TfMachineProgram **out, const TfTypedProgram *typed,
                                  size_t diagnostic_cap, const TfAllocator *allocator);
void tf_machine_program_destroy(TfMachineProgram *program);
const TfTypedProgram *tf_machine_program_typed(const TfMachineProgram *program);
const TfDiagnosticBag *tf_machine_program_diagnostics(const TfMachineProgram *program);
const TfDiagnosticPhaseResult *tf_machine_program_phase(const TfMachineProgram *program);
size_t tf_machine_program_count(const TfMachineProgram *program);
const TfMachinePlan *tf_machine_program_at(const TfMachineProgram *program, size_t index);
const TfMachinePlan *tf_machine_program_get(const TfMachineProgram *program, TfTypeId type);
#endif
