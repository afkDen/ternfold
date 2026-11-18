#ifndef TERNFOLD_PARSER_H
#define TERNFOLD_PARSER_H
#include "ternfold/ast.h"
#include "ternfold/diagnostics.h"

typedef struct TfParseResult TfParseResult;
typedef struct {
    bool is_root; /* only root pub fn main() outer body admits I/O */
    size_t nesting_limit; /* 1..128, default 64; excess is HOST_ERROR */
    size_t diagnostic_cap; /* 0..100, including lexer diagnostics, default 100 */
} TfParseOptions;

/* NULL options = {true,64,100}. Input is an unmodified tf_lex result, retaining
 * comments and EOF. Source must be valid. Invalid lexical tokens are recoverable
 * error nodes; lexical diagnostics are adapted into the same owned bag. Source
 * errors must be handled by the caller before parsing.
 * TF_OK means a complete owning result (possibly diagnosed syntax errors).
 * Inspect tf_parse_phase(): VALID carries borrowed TfAst*, ERROR no payload.
 * tf_parse_ast() also exposes the immutable recovery tree for inspection ONLY.
 * Neither API confers resolution/type/Core/execution validity.
 * HOST_ERROR (OOM, invalid arguments, nesting budget) sets *out=NULL and frees
 * all temporary ownership; no partial valid result escapes. No printing.
 * Result owns AST arena and bag; source/allocator context must outlive it. Input
 * lexer may be destroyed immediately after return; decoded payloads are copied.
 * All returned borrows expire at tf_parse_destroy; do not mutate their bags or
 * copy an owning result. Destroy downstream borrowers before the result.
 */
TfStatus tf_parse(TfParseResult **out, const TfLexResult *lex,
                  const TfParseOptions *options, const TfAllocator *allocator);
void tf_parse_destroy(TfParseResult *result);
const TfAst *tf_parse_ast(const TfParseResult *result);
const TfDiagnosticBag *tf_parse_diagnostics(const TfParseResult *result);
const TfDiagnosticPhaseResult *tf_parse_phase(const TfParseResult *result);
#endif
