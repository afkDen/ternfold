#ifndef TERNFOLD_CORE_PRIMITIVE_RULES_H
#define TERNFOLD_CORE_PRIMITIVE_RULES_H

#include "ternfold/core.h"

/* The verifier supplies the classes from validated Core type metadata.
 * This layer never consults source AST, resolution or the type checker. */
typedef enum {
    TF_CORE_PRIMITIVE_INT, TF_CORE_PRIMITIVE_FLOAT, TF_CORE_PRIMITIVE_BOOL,
    TF_CORE_PRIMITIVE_TEXT, TF_CORE_PRIMITIVE_LIST, TF_CORE_PRIMITIVE_OTHER
} TfCorePrimitiveClass;

bool tf_core_primitive_unary(TfCoreUnary op, TfCorePrimitiveClass operand,
                             TfCorePrimitiveClass result);
bool tf_core_primitive_binary(TfCoreBinary op, TfCorePrimitiveClass left,
                              TfCorePrimitiveClass right, TfCorePrimitiveClass result,
                              bool operands_identical, bool result_is_left,
                              bool equality_capable);

#endif
