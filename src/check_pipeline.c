#include "check_builtins.h"
#include "check_constants.h"
#include "typed_patterns.h"

static bool pattern_node(const TfTypedPatterns *patterns, TfNodeId node,
                         TfTypedNode *out, TfLiteralValue *value, bool *has_literal) {
    const TfTypedPatternNode *record = tf_typed_pattern_node(patterns,node);
    if (record == NULL) return false;
    *out = (TfTypedNode){.node=record->node,.type=record->type,.target=record->target};
    *has_literal = record->has_literal;
    if (*has_literal) *value = record->literal;
    return true;
}

TfStatus tf_check(TfTypedProgram **out, const TfResolution *resolution,
                  TfTypedSignatures *signatures, size_t diagnostic_cap,
                  const TfAllocator *allocator) {
    static const TfCheckAdapters adapters = {
        .builtin = tf_check_builtin,
        .patterns = tf_typed_patterns,
        .pattern_binding = tf_typed_pattern_binding,
        .patterns_destroy = tf_typed_patterns_destroy,
        .pattern_node = pattern_node,
        .constant = tf_check_constant
    };
    return tf_check_with_adapters(out,resolution,signatures,diagnostic_cap,allocator,&adapters);
}
