#ifndef TERNFOLD_REPL_INPUT_H
#define TERNFOLD_REPL_INPUT_H

#include "ternfold/lexer.h"

typedef enum {
    TF_REPL_UNIT_EMPTY,
    TF_REPL_UNIT_DECLARATION,
    TF_REPL_UNIT_BINDING,
    TF_REPL_UNIT_EXPRESSION,
    TF_REPL_UNIT_INCOMPLETE,
    TF_REPL_UNIT_HOST_ERROR
} TfReplUnitKind;

/* Borrows the immutable source and uses its allocator for scratch storage.
 * A complete classification does not imply validity. Send complete units to
 * the ordinary frontend, including EXPRESSION for a source-encoding error.
 * Incomplete syntax is determined by the ordinary grammar's EOF diagnostics;
 * a later checked AST shape check still guards assembly of a session turn.
 * Include the physical line ending, if any, in the source snapshot. An open
 * quote at byte EOF may accept more bytes; one cut off by a newline cannot.
 */
TfReplUnitKind tf_repl_classify(const TfSource *source);

#endif
