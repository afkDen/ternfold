#ifndef TERNFOLD_TOKEN_DUMP_H
#define TERNFOLD_TOKEN_DUMP_H

#include "ternfold/lexer.h"

typedef enum { TF_TOKEN_DUMP_TEXT, TF_TOKEN_DUMP_JSON } TfTokenDumpFormat;

/* Allocation-free inspection of an immutable accepted source/lexical result.
 * stream, source, result and every payload are borrowed. They must stay alive
 * and unchanged during this call. Valid sources require a result from tf_lex
 * (OK or LEX_ERROR); invalid sources require result=NULL and emit no tokens.
 * Includes ordered comments and exactly one EOF. Never converts numeric ranges
 * or changes lexical token classification; Unit remains LPAREN RPAREN.
 * Invalid arguments or observed write errors return HOST_ERROR; output can be
 * partial on error. Caller selects binary mode, flushes and closes. Buffered
 * failures observed only at caller flush remain caller-owned. No ANSI color.
 *
 * JSON v1: {schema_version:1,source_valid:bool,has_errors:bool,tokens:[...]}, LF.
 * Each token: kind(string),reserved(bool),lexeme(exact source UTF-8 slice),
 * payload(string for IDENT/INT/FLOAT original spelling and decoded TEXT/CHAR;
 * boolean for TRUE/FALSE; null otherwise),payload_bytes(string byte length,
 * zero for bool/null),start_byte,end_byte,start,end,error(code string or null).
 * start/end: scalar(zero-based index),line,column,display_column(one-based).
 * Spans are original half-open bytes; strings escape controls/NUL as \u00XX.
 * Invalid source JSON: source_valid=false,has_errors=true,tokens=[]; source
 * errors/coordinates are carried separately by the M02 diagnostic stream.
 * Text starts source_valid=... has_errors=..., then one record per token with
 * the same information; coordinates use line:scalar-column and tab display.
 */
TfStatus tf_token_dump_write(FILE *stream, const TfSource *source,
                              const TfLexResult *result, TfTokenDumpFormat format);

/* Compiler inspection CLI (implemented by compiler_main.c, separate from the
 * accepted lexical submission main):
 * ternfold tokens source.tfd [--json] [--diagnostics-json] [--symbol-table DIR]
 * Options may appear in any order AFTER the source, once each. DIR must exist.
 * No report is published unless --symbol-table is explicitly supplied. Text
 * diagnostics are default; --diagnostics-json emits M02 v2 JSON on stderr,
 * including an empty bag on lexical success. Usage/extension/host messages are
 * driver messages, not fabricated language diagnostics; these remain text.
 * Exit: 0 lexical success, 1 extension/source/lexical error, 2 usage/host/output
 * failure. --help describes only tokens; check/run/compile are unavailable.
 */
#endif
