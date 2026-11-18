#ifndef TERNFOLD_REPL_PARSE_H
#define TERNFOLD_REPL_PARSE_H

#include "ternfold/parser.h"

typedef enum { TF_REPL_PARSE_MODULE, TF_REPL_PARSE_LET, TF_REPL_PARSE_EXPRESSION } TfReplParseKind;
/* Runs the ordinary grammar on the token prefix without publishing an AST.
 * A prefix is incomplete only if it has an error and every error is at EOF.
 * The caller supplies an otherwise lexically valid stream. */
TfStatus tf_parse_repl_prefix(const TfLexResult *lex,TfReplParseKind kind,bool *incomplete);

#endif
