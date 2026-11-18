#ifndef TERNFOLD_TYPE_OPERATORS_H
#define TERNFOLD_TYPE_OPERATORS_H

#include "typed_signatures.h"

/* Check resolved operand types, not values or evaluation. With an absent
 * right operand this checks unary '-' or '!'. TF_SOURCE_ERROR provides the
 * applicable E302/E305 code; invalid handles are TF_HOST_ERROR. */
TfStatus tf_type_operator(TfTypedSignatures *signatures, TfTokenKind op,
                           TfConcreteTypeId left, TfConcreteTypeId right,
                           TfConcreteTypeId *out, const char **diagnostic);

#endif
