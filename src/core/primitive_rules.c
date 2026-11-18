#include "primitive_rules.h"

static bool number(TfCorePrimitiveClass kind) {
    return kind == TF_CORE_PRIMITIVE_INT || kind == TF_CORE_PRIMITIVE_FLOAT;
}

bool tf_core_primitive_unary(TfCoreUnary op, TfCorePrimitiveClass operand,
                             TfCorePrimitiveClass result) {
    if (result != operand) return false;
    if (op == TF_CORE_UNARY_NEGATE) return number(operand);
    return op == TF_CORE_UNARY_NOT && operand == TF_CORE_PRIMITIVE_BOOL;
}

bool tf_core_primitive_binary(TfCoreBinary op, TfCorePrimitiveClass left,
                              TfCorePrimitiveClass right, TfCorePrimitiveClass result,
                              bool operands_identical, bool result_is_left,
                              bool equality_capable) {
    const bool same = operands_identical && left == right;
    const bool preserves = result_is_left && result == left;
    switch (op) {
    case TF_CORE_BINARY_ADD: case TF_CORE_BINARY_SUBTRACT: case TF_CORE_BINARY_MULTIPLY:
        return same && number(left) && preserves;
    case TF_CORE_BINARY_REAL_DIVIDE:
        return number(left) && number(right) && result == TF_CORE_PRIMITIVE_FLOAT;
    case TF_CORE_BINARY_FLOOR_DIVIDE: case TF_CORE_BINARY_REMAINDER:
        return left == TF_CORE_PRIMITIVE_INT && same && preserves;
    case TF_CORE_BINARY_POWER:
        return number(left) && right == TF_CORE_PRIMITIVE_INT && preserves;
    case TF_CORE_BINARY_LESS: case TF_CORE_BINARY_LESS_EQUAL: case TF_CORE_BINARY_GREATER: case TF_CORE_BINARY_GREATER_EQUAL:
        return same && number(left) && result == TF_CORE_PRIMITIVE_BOOL;
    case TF_CORE_BINARY_EQUAL: case TF_CORE_BINARY_NOT_EQUAL:
        return same && equality_capable && result == TF_CORE_PRIMITIVE_BOOL;
    case TF_CORE_BINARY_CONCAT:
        return same && preserves && (left == TF_CORE_PRIMITIVE_TEXT || left == TF_CORE_PRIMITIVE_LIST);
    default:
        /* && and || must be structured If operations. */
        return false;
    }
}
