#include "type_operators.h"

static bool numeric(TfConcreteTypeKind kind) {
    return kind == TF_CONCRETE_INT || kind == TF_CONCRETE_FLOAT;
}

TfStatus tf_type_operator(TfTypedSignatures *signatures, TfTokenKind op,
                           TfConcreteTypeId left, TfConcreteTypeId right,
                           TfConcreteTypeId *out, const char **diagnostic) {
    TfTypeStore *store = tf_signatures_store(signatures);
    const TfConcreteType *a, *b;
    bool same;
    if (diagnostic != NULL) *diagnostic = NULL;
    if (out == NULL) return TF_HOST_ERROR;
    *out = (TfConcreteTypeId){0};
    if (signatures == NULL || tf_diagnostic_bag_has_errors(tf_signatures_diagnostics(signatures)))
        return TF_HOST_ERROR;
    a = tf_type_store_get(store,left);
    if (a == NULL) return TF_HOST_ERROR;
    if (right.index == 0 && right.owner == NULL) {
        if (op == TF_MINUS && numeric(a->kind)) goto preserve;
        if (op == TF_BANG && a->kind == TF_CONCRETE_BOOL) goto preserve;
        goto mismatch;
    }
    b = tf_type_store_get(store,right);
    if (b == NULL) return TF_HOST_ERROR;
    same = tf_concrete_type_id_equal(left,right);
    switch (op) {
    case TF_PLUS: case TF_MINUS: case TF_STAR:
        if (same && numeric(a->kind)) goto preserve;
        break;
    case TF_SLASH:
        if (numeric(a->kind) && numeric(b->kind))
            return tf_type_primitive(store,TF_CONCRETE_FLOAT,out);
        break;
    case TF_DIV: case TF_PERCENT:
        if (a->kind == TF_CONCRETE_INT && b->kind == TF_CONCRETE_INT) goto preserve;
        break;
    case TF_CARET:
        if (numeric(a->kind) && b->kind == TF_CONCRETE_INT) goto preserve;
        break;
    case TF_AND_AND: case TF_OR_OR:
        if (a->kind == TF_CONCRETE_BOOL && b->kind == TF_CONCRETE_BOOL) goto preserve;
        break;
    case TF_LESS: case TF_LESS_EQUAL: case TF_GREATER: case TF_GREATER_EQUAL:
        if (same && numeric(a->kind)) goto boolean;
        break;
    case TF_EQUAL_EQUAL: case TF_BANG_EQUAL: {
        bool contains;
        if (!same) break;
        if (tf_signatures_function_data(signatures,left,&contains) != TF_OK) return TF_HOST_ERROR;
        if (contains) {
            if (diagnostic != NULL) *diagnostic = "E305";
            return TF_SOURCE_ERROR;
        }
        goto boolean;
    }
    case TF_PLUS_PLUS:
        if (same && (a->kind == TF_CONCRETE_TEXT || a->kind == TF_CONCRETE_LIST)) goto preserve;
        break;
    default: return TF_HOST_ERROR;
    }
mismatch:
    if (diagnostic != NULL) *diagnostic = "E302";
    return TF_SOURCE_ERROR;
preserve:
    *out = left;
    return TF_OK;
boolean:
    return tf_type_primitive(store,TF_CONCRETE_BOOL,out);
}
