#ifndef TERNFOLD_LITERAL_VALUE_H
#define TERNFOLD_LITERAL_VALUE_H

#include "ternfold/ast.h"

typedef enum {
    TF_LITERAL_INT, TF_LITERAL_FLOAT, TF_LITERAL_BOOL, TF_LITERAL_TEXT,
    TF_LITERAL_CHAR, TF_LITERAL_UNIT
} TfLiteralKind;
typedef struct {
    TfLiteralKind kind;
    /* Pattern minus is applied here. Expression minus remains an AST op.
     * The sole magnitude exceeding INT64_MAX requires contiguous unary minus
     * (or a signed pattern) and the parent must form INT64_MIN directly. */
    bool minimum_magnitude;
    union {
        uint64_t int_magnitude;
        int64_t signed_pattern;
        uint64_t float_bits;
        bool boolean;
        TfAstText text;
        uint32_t scalar;
    } as;
} TfLiteralValue;

/* Input must be an AST node from a successful parse. Expression_parent, if
 * supplied, is the literal's direct parent and permits only the minimum-Int
 * exception. Borrowed Text bytes expire with the AST. TF_SOURCE_ERROR is the
 * E301 range case; HOST_ERROR means wrong node or broken parser invariant.
 * The output is written only on success. */
TfStatus tf_literal_value(const TfAstNode *literal, const TfAstNode *expression_parent,
                          TfLiteralValue *out);

#endif
