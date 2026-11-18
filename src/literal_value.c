#include "literal_value.h"
#include "ternfold/numeric.h"
#include "internal.h"

static bool adjacent(TfSpan minus, TfSpan digits) {
    return minus.source != NULL && minus.source == digits.source && minus.end == digits.start;
}

static bool direct_minus(const TfAstNode *parent, const TfAstNode *literal) {
    return parent != NULL && parent->kind == TF_AST_UNARY && parent->as.unary.op == TF_MINUS &&
           parent->as.unary.operand.owner == literal->id.owner &&
           parent->as.unary.operand.index == literal->id.index &&
           adjacent(parent->as.unary.operator_span,literal->as.literal.value.span);
}

TfStatus tf_literal_value(const TfAstNode *literal, const TfAstNode *expression_parent,
                          TfLiteralValue *out) {
    TfLiteralValue value = {0};
    TfAstText text;
    bool pattern, negative;
    TfNumericStatus parsed;
    uint64_t number;
    if (literal == NULL || out == NULL) return TF_HOST_ERROR;
    if (literal->kind == TF_AST_UNIT || literal->kind == TF_AST_PATTERN_UNIT) {
        *out = (TfLiteralValue){.kind = TF_LITERAL_UNIT};
        return TF_OK;
    }
    pattern = literal->kind == TF_AST_PATTERN_LITERAL;
    if (!pattern && literal->kind != TF_AST_LITERAL) return TF_HOST_ERROR;
    negative = pattern && literal->as.literal.negative;
    text = literal->as.literal.value;
    switch (literal->as.literal.token) {
    case TF_INT:
        parsed = tf_parse_int_magnitude(text.data,text.length,&number);
        if (parsed == TF_NUMERIC_INVALID) return TF_HOST_ERROR;
        if (parsed == TF_NUMERIC_RANGE) return TF_SOURCE_ERROR;
        value.kind = TF_LITERAL_INT;
        if (number > (uint64_t)INT64_MAX) {
            if (pattern ? !(negative && adjacent(literal->as.literal.minus,text.span)) :
                          !direct_minus(expression_parent,literal)) return TF_SOURCE_ERROR;
            value.minimum_magnitude = true;
        }
        if (!pattern) value.as.int_magnitude = number;
        else if (value.minimum_magnitude) value.as.signed_pattern = INT64_MIN;
        else value.as.signed_pattern = negative ? -(int64_t)number : (int64_t)number;
        break;
    case TF_FLOAT:
        parsed = tf_parse_float_bits(text.data,text.length,&number);
        if (parsed == TF_NUMERIC_INVALID) return TF_HOST_ERROR;
        if (parsed == TF_NUMERIC_RANGE) return TF_SOURCE_ERROR;
        value.kind = TF_LITERAL_FLOAT;
        value.as.float_bits = negative && number != 0 ? number | (UINT64_C(1)<<63) : number;
        break;
    case TF_TRUE: case TF_FALSE:
        value.kind = TF_LITERAL_BOOL; value.as.boolean = literal->as.literal.token == TF_TRUE; break;
    case TF_TEXT:
        value.kind = TF_LITERAL_TEXT; value.as.text = text; break;
    case TF_CHAR: {
        size_t end;
        value.kind = TF_LITERAL_CHAR;
        if (!tf_decode(text.data,text.length,0,&value.as.scalar,&end) || end != text.length)
            return TF_HOST_ERROR;
        break;
    }
    default: return TF_HOST_ERROR;
    }
    *out = value;
    return TF_OK;
}
