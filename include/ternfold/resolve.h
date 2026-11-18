#ifndef TERNFOLD_RESOLVE_H
#define TERNFOLD_RESOLVE_H
#include "ternfold/parser.h"
#include "ternfold/source_manager.h"

typedef struct TfResolution TfResolution;
typedef struct { const TfResolution *owner; size_t index; } TfModuleId;
typedef struct { const TfResolution *owner; size_t index; } TfSymbolId;
typedef struct { const TfResolution *owner; size_t index; } TfScopeId;
/* Nominal identity is (canonical root-relative module, declaration index).
 * module_index 0 denotes the compiler prelude; declaration_index 0 is absent.
 * User declaration indexes are one-based and exclude import statements.
 * These are declaration identities, not M06's structural/inferred types. */
typedef struct { const TfResolution *owner; size_t module_index, declaration_index; } TfTypeId;
typedef enum {
    TF_SYMBOL_BUILTIN, TF_SYMBOL_FUNCTION, TF_SYMBOL_CONSTRUCTOR,
    TF_SYMBOL_VARIANT, TF_SYMBOL_ALIAS, TF_SYMBOL_PARAMETER, TF_SYMBOL_LOCAL,
    TF_SYMBOL_PATTERN, TF_SYMBOL_LOOP, TF_SYMBOL_ENTRY_INPUT,
    TF_SYMBOL_MACHINE_INPUT, TF_SYMBOL_CELL, TF_SYMBOL_FIELD
} TfSymbolKind;
typedef enum { TF_TYPE_BUILTIN, TF_TYPE_RECORD, TF_TYPE_ENUM, TF_TYPE_MACHINE } TfNominalKind;
typedef enum {
    TF_REFERENCE_VALUE, TF_REFERENCE_TYPE, TF_REFERENCE_MODULE,
    TF_REFERENCE_OLD, TF_REFERENCE_INPUT, TF_REFERENCE_CANDIDATE,
    TF_REFERENCE_WRITER, TF_REFERENCE_DECLARATION
} TfReferenceKind;
typedef struct {
    TfModuleId id;
    const TfManagedSource *source;
    const TfAst *ast; /* NULL for an invalid source snapshot */
    const char *relative_path; /* canonical root-relative path; '/' separators,
                                UTF-8 on Windows and native bytes on POSIX */
    bool is_root;
} TfResolvedModule;
typedef struct {
    TfSymbolId id;
    TfSymbolKind kind;
    TfAstText name;
    TfModuleId module, target_module; /* target_module only for aliases */
    TfNodeId declaration, closure; /* closure zero for non-closure binders */
    TfTypeId type; /* owning nominal declaration for constructor/variant/member
                     and machine input; otherwise absent (not inferred type) */
    TfScopeId scope;
    bool public_decl, entry_only;
} TfResolvedSymbol;
typedef struct {
    TfTypeId id;
    TfNominalKind kind;
    TfAstText name;
    TfNodeId declaration;
    bool public_decl;
} TfResolvedType;
typedef struct {
    TfScopeId id, parent;
    TfModuleId module;
    TfNodeId node, closure;
} TfResolvedScope;
typedef struct {
    TfNodeId node;
    TfReferenceKind kind;
    TfSymbolId symbol;
    TfTypeId type;
} TfResolvedReference;
typedef struct {
    TfNodeId closure;
    TfSymbolId symbol;
    TfReferenceKind kind;
} TfResolvedCapture;
typedef struct {
    const char *root_directory; /* NULL = canonical entry parent */
    size_t diagnostic_cap; /* 0..100; NULL options selects 100 */
} TfResolveOptions;

/* Frozen ownership contract: manager and allocator context are BORROWED and
 * must outlive the result, every span and downstream borrower. The result owns
 * all loaded parse results, diagnostics, graph and immutable resolution tables;
 * callers never destroy its ast borrows. Manager snapshots are reused, never
 * reread, including cached aliases to deleted or retargeted links. A failed
 * resolution can leave COMPLETE snapshots cached in manager.
 * No supplied parser is borrowed. Root is parsed with is_root=true; imports
 * always false. Native paths follow SourceManager's contract. Imports enforce
 * relative POSIX .tfd grammar and canonical root containment before reading
 * source bytes; probes may inspect local metadata outside the root. Windows
 * requires queryable case-insensitive ancestors and local drive paths, refuses
 * remote/device reparse targets, and converts UTF-8 import text to the native
 * code page losslessly. Unknown Windows reparse types are unsupported.
 * POSIX uses the native filesystem namespace; it does not inspect mount types.
 * Missing/forbidden imports are E204; unsupported host capabilities and OOM
 * are HOST_ERROR. Concurrent hostile filesystem changes are out of contract.
 *
 * TF_OK owns a complete result, possibly diagnosed errors. VALID carries this
 * resolution; ERROR has no payload. Recovery ASTs/tables remain inspection-only.
 * HOST_ERROR sets *out=NULL and releases all resolver ownership. No printing,
 * typechecking, constant evaluation, dependency scheduling or execution.
 * All accessors return borrows stable until destroy; zero/foreign IDs reject.
 * Module indexes follow canonical root-relative byte order. Signatures and
 * bodies are visited dependency-first, with all signatures before any body.
 * A field projection/argument label requires M06 type information and is not a
 * lexical reference. Every successfully resolved lexical path (and qualifier),
 * declaration binder, writer and candidate has an explicit table identity.
 */
TfStatus tf_resolve(TfResolution **out, TfSourceManager *manager,
                    const char *entry_path, const TfResolveOptions *options,
                    const TfAllocator *allocator);
void tf_resolution_destroy(TfResolution *resolution);
const TfDiagnosticBag *tf_resolution_diagnostics(const TfResolution *resolution);
const TfDiagnosticPhaseResult *tf_resolution_phase(const TfResolution *resolution);
TfModuleId tf_resolution_root(const TfResolution *resolution);
const char *tf_resolution_root_directory(const TfResolution *resolution);
size_t tf_resolution_module_count(const TfResolution *resolution);
const TfResolvedModule *tf_resolution_module_at(const TfResolution *resolution, size_t index);
const TfResolvedModule *tf_resolution_module(const TfResolution *resolution, TfModuleId id);
size_t tf_resolution_symbol_count(const TfResolution *resolution);
const TfResolvedSymbol *tf_resolution_symbol_at(const TfResolution *resolution, size_t index);
const TfResolvedSymbol *tf_resolution_symbol(const TfResolution *resolution, TfSymbolId id);
size_t tf_resolution_type_count(const TfResolution *resolution);
const TfResolvedType *tf_resolution_type_at(const TfResolution *resolution, size_t index);
const TfResolvedType *tf_resolution_type(const TfResolution *resolution, TfTypeId id);
size_t tf_resolution_scope_count(const TfResolution *resolution);
const TfResolvedScope *tf_resolution_scope_at(const TfResolution *resolution, size_t index);
const TfResolvedScope *tf_resolution_scope(const TfResolution *resolution, TfScopeId id);
size_t tf_resolution_reference_count(const TfResolution *resolution);
const TfResolvedReference *tf_resolution_reference_at(const TfResolution *resolution, size_t index);
const TfResolvedReference *tf_resolution_reference(const TfResolution *resolution, TfNodeId node);
size_t tf_resolution_capture_count(const TfResolution *resolution);
const TfResolvedCapture *tf_resolution_capture_at(const TfResolution *resolution, size_t index);
#endif
