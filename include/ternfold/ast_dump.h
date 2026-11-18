#ifndef TERNFOLD_AST_DUMP_H
#define TERNFOLD_AST_DUMP_H
#include "ternfold/ast.h"
/* Allocation-free flat inspection; AST/source are borrowed and must outlive the
 * call. All nodes, including recovery/missing nodes, are emitted in stable ID
 * order. Absent references are null. No host addresses or semantic resolution.
 * Caller owns binary stream/flush/close. A failed stream may contain a prefix.
 * JSON schema1 has root, has_errors and nodes; each node has id,kind, original
 * byte span and all syntactic fields. Text presents the same fields per line. */
TfStatus tf_ast_dump_write(FILE *stream,const TfAst *ast,bool has_errors,bool json);
#endif
