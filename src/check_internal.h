#ifndef TERNFOLD_CHECK_INTERNAL_H
#define TERNFOLD_CHECK_INTERNAL_H

#include "ternfold/check.h"
#include "literal_value.h"
#include "typed_signatures.h"

typedef struct TfCheckContext TfCheckContext;
typedef struct TfTypedPatterns TfTypedPatterns;
typedef enum {
    TF_CHECK_PENDING_BUILTINS = 1,
    TF_CHECK_PENDING_PATTERNS = 2,
    TF_CHECK_PENDING_CONSTANTS = 4
} TfCheckPending;
unsigned tf_typed_program_pending(const TfTypedProgram *program);
typedef struct {
    TfConcreteTypeId type;
    TfSpan origin;
    bool unavailable; /* recovery: an expected type could not be computed */
} TfCheckExpectation;

typedef struct {
    /* use is a bare path or a direct call. The adapter checks the call's
     * arguments using tf_check_argument and returns an instantiated callable
     * (for direct calls) and the use's type. No metavariables escape. Return
     * TF_SOURCE_ERROR only after recording a diagnostic; TF_OK with absent
     * result means recovery from an already diagnosed or pending child. */
    TfStatus (*builtin)(TfCheckContext *, const TfAstNode *use,
                        const TfResolvedSymbol *, TfCheckExpectation,
                        TfConcreteTypeId *result, TfConcreteTypeId *callable);
    TfStatus (*patterns)(TfTypedPatterns **, const TfResolution *, TfTypedSignatures *,
                         const TfAstNode *match, TfConcreteTypeId scrutinee,
                         TfDiagnosticBag *, const TfAllocator *);
    TfConcreteTypeId (*pattern_binding)(const TfTypedPatterns *, TfSymbolId);
    void (*patterns_destroy)(TfTypedPatterns *);
    /* Optional while the pattern bridge exposes only bindings. A concrete
     * record is required for each successful pattern (including wrappers).
     * Return false when no record is available for the requested node. */
    bool (*pattern_node)(const TfTypedPatterns *, TfNodeId, TfTypedNode *,
                         TfLiteralValue *, bool *has_literal);
    /* Called after expression typing for a state initializer. Owns syntax
     * validation and constant fault diagnostics. No source execution here. */
    TfStatus (*constant)(TfCheckContext *, const TfAstNode *state);
} TfCheckAdapters;

TfStatus tf_check_with_adapters(TfTypedProgram **out, const TfResolution *resolution,
                                TfTypedSignatures *signatures, size_t diagnostic_cap,
                                const TfAllocator *allocator, const TfCheckAdapters *adapters);
TfConcreteTypeId tf_check_expression(TfCheckContext *, TfNodeId, TfCheckExpectation);
TfConcreteTypeId tf_check_argument(TfCheckContext *, TfNodeId, TfCheckExpectation);
/* Trial synthesis for an unresolved builtin parameter. Does not publish node,
 * symbol or diagnostic changes. TF_OK with an absent type requires context;
 * TF_SOURCE_ERROR means real checking must diagnose the argument. */
TfStatus tf_check_probe_argument(TfCheckContext *, TfNodeId argument, TfConcreteTypeId *);
TfStatus tf_check_diagnostic(TfCheckContext *, const char *code, const char *message,
                             TfSpan primary, TfSpan related);
TfTypedSignatures *tf_check_signatures(TfCheckContext *);
const TfTypedProgram *tf_check_program(const TfCheckContext *);
const TfAllocator *tf_check_allocator(const TfCheckContext *);
const TfLiteralValue *tf_typed_program_literal(const TfTypedProgram *, TfNodeId);

#endif
