#ifndef TERNFOLD_TYPED_SIGNATURES_H
#define TERNFOLD_TYPED_SIGNATURES_H

#include "ternfold/types.h"

typedef struct TfTypedSignatures TfTypedSignatures;
typedef struct {
    TfConcreteTypeId input, output, rejection;
} TfMachineSignature;

/* Convert declarations and validate nominal dependencies and cell data types
 * before visiting expression bodies. This is an internal checker step, not a
 * fully checked program: constants and expression semantics are separate.
 * The result owns the store, diagnostics and tables; it borrows the resolution
 * and allocator context. HOST_ERROR clears out; TF_OK may carry diagnostics.
 * After a diagnosed annotation failure only diagnostics may be consumed. */
TfStatus tf_signatures_build(TfTypedSignatures **out, const TfResolution *resolution,
                             size_t diagnostic_cap, const TfAllocator *allocator);
void tf_signatures_destroy(TfTypedSignatures *signatures);
const TfDiagnosticBag *tf_signatures_diagnostics(const TfTypedSignatures *signatures);
const TfResolution *tf_signatures_resolution(const TfTypedSignatures *signatures);
TfTypeStore *tf_signatures_store(TfTypedSignatures *signatures);
/* Returns absent for local binders, generic prelude callables, and foreign IDs.
 * Constructor signatures describe direct construction; they do not permit
 * constructors to become first-class function values. */
TfConcreteTypeId tf_signatures_symbol(const TfTypedSignatures *signatures, TfSymbolId symbol);
bool tf_signatures_machine(const TfTypedSignatures *signatures, TfTypeId machine,
                            TfMachineSignature *out);
/* Capability used for equality and data-only values. After the signature
 * graph succeeds, newly interned structural types may also be queried. */
TfStatus tf_signatures_function_data(TfTypedSignatures *signatures, TfConcreteTypeId type,
                                      bool *out);

#endif
