#ifndef TERNFOLD_CORE_H
#define TERNFOLD_CORE_H

#include "ternfold/lexer.h" /* TfAllocator, TfStatus, TfSpan; no frontend IR. */

/* Type, constant, function and span IDs index program tables from one. Block
 * IDs index a function's block table from one. Value IDs are function-local
 * slots 1..value_count, uniquely defined by a parameter, capture, Let, or
 * ListMap element. Zero means absent; tags and field/payload indexes are
 * zero-based declaration ordinals, not IDs. */
typedef size_t TfCoreTypeId;
typedef size_t TfCoreConstantId;
typedef size_t TfCoreFunctionId;
typedef size_t TfCoreSpanId;
typedef size_t TfCoreBlockId;
typedef size_t TfCoreValueId;

typedef struct TfCoreProgram TfCoreProgram; /* frozen, unverified owner */
typedef struct TfVerifiedCore TfVerifiedCore; /* constructed only by verification */

typedef struct {
    const unsigned char *bytes;
    size_t length;
} TfCoreText;

typedef struct {
    const TfCoreTypeId *items;
    size_t count;
} TfCoreTypeIds;

typedef struct {
    const TfCoreConstantId *items;
    size_t count;
} TfCoreConstantIds;

typedef struct {
    const TfCoreValueId *items;
    size_t count;
} TfCoreValueIds;

typedef enum {
    TF_CORE_TYPE_INT = 1, TF_CORE_TYPE_FLOAT, TF_CORE_TYPE_BOOL,
    TF_CORE_TYPE_TEXT, TF_CORE_TYPE_CHAR, TF_CORE_TYPE_UNIT,
    TF_CORE_TYPE_LIST, TF_CORE_TYPE_OPTION, TF_CORE_TYPE_DECISION,
    TF_CORE_TYPE_RECORD, TF_CORE_TYPE_ENUM, TF_CORE_TYPE_MACHINE,
    TF_CORE_TYPE_FUNCTION, TF_CORE_TYPE_TAG
} TfCoreTypeKind;

enum {
    TF_CORE_OPTION_SOME = 0, TF_CORE_OPTION_NONE = 1,
    TF_CORE_DECISION_ACCEPT = 0, TF_CORE_DECISION_REJECT = 1
};

typedef struct {
    TfCoreText name;
    TfCoreTypeId type;
    TfCoreSpanId span;
} TfCoreField;

typedef struct {
    TfCoreText name;
    TfCoreTypeIds payloads; /* declaration order, including zero arity */
    TfCoreSpanId span;
} TfCoreVariant;

typedef struct {
    TfCoreTypeKind kind;
    TfCoreText name, module; /* optional display metadata, never identity */
    TfCoreSpanId span;
    union {
        TfCoreTypeId element; /* List[T] and Option[T]; Some(T), None() */
        struct {
            /* Accept(machine, emissions), Reject(machine, rejection).
             * emissions must be List[output]; O/E match machine metadata. */
            TfCoreTypeId machine, output, rejection, emissions;
        } decision;
        struct { const TfCoreField *fields; size_t field_count; } record;
        struct { const TfCoreVariant *variants; size_t variant_count; } enumeration;
        struct {
            const TfCoreField *cells;
            size_t cell_count;
            TfCoreTypeId input, output, rejection;
        } machine;
        struct { TfCoreTypeIds parameters; TfCoreTypeId result; } function;
        TfCoreTypeId tagged; /* internal Tag type, qualified by enum-like type */
    } as;
} TfCoreType;

typedef enum {
    TF_CORE_CONSTANT_INT = 1, TF_CORE_CONSTANT_FLOAT, TF_CORE_CONSTANT_BOOL,
    TF_CORE_CONSTANT_TEXT, TF_CORE_CONSTANT_CHAR, TF_CORE_CONSTANT_UNIT,
    TF_CORE_CONSTANT_LIST, TF_CORE_CONSTANT_RECORD, TF_CORE_CONSTANT_VARIANT
} TfCoreConstantKind;

typedef struct {
    TfCoreConstantKind kind;
    TfCoreTypeId type;
    TfCoreSpanId span;
    union {
        int64_t integer;
        uint64_t float_bits; /* finite binary64, with positive zero only in valid IR */
        uint8_t boolean; /* valid values 0 and 1 */
        TfCoreText text; /* UTF-8 bytes, including embedded NUL */
        uint32_t character; /* Unicode scalar */
        TfCoreConstantIds children; /* List elements or Record fields */
        struct { size_t tag; TfCoreConstantIds payloads; } variant;
    } as;
} TfCoreConstant;

typedef struct {
    TfCoreValueId value;
    TfCoreTypeId type;
    TfCoreText name;
    TfCoreSpanId span;
} TfCoreParameter;

typedef enum {
    TF_CORE_UNARY_NEGATE = 1, TF_CORE_UNARY_NOT
} TfCoreUnary;

typedef enum {
    TF_CORE_BINARY_ADD = 1, TF_CORE_BINARY_SUBTRACT, TF_CORE_BINARY_MULTIPLY,
    TF_CORE_BINARY_REAL_DIVIDE, TF_CORE_BINARY_FLOOR_DIVIDE,
    TF_CORE_BINARY_REMAINDER, TF_CORE_BINARY_POWER,
    TF_CORE_BINARY_LESS, TF_CORE_BINARY_LESS_EQUAL, TF_CORE_BINARY_GREATER,
    TF_CORE_BINARY_GREATER_EQUAL, TF_CORE_BINARY_EQUAL, TF_CORE_BINARY_NOT_EQUAL,
    TF_CORE_BINARY_CONCAT
} TfCoreBinary;

typedef enum {
    TF_CORE_BUILTIN_LEN = 1, TF_CORE_BUILTIN_AT, TF_CORE_BUILTIN_APPEND,
    TF_CORE_BUILTIN_MAP, TF_CORE_BUILTIN_FOLD, TF_CORE_BUILTIN_RANGE,
    TF_CORE_BUILTIN_INT_TEXT, TF_CORE_BUILTIN_INT_FLOAT, TF_CORE_BUILTIN_FLOAT_INT,
    TF_CORE_BUILTIN_FLOAT_TEXT, TF_CORE_BUILTIN_BOOL_TEXT, TF_CORE_BUILTIN_PARSE_INT,
    TF_CORE_BUILTIN_TEXT_LEN, TF_CORE_BUILTIN_TEXT_AT, TF_CORE_BUILTIN_TEXT_SLICE
} TfCoreBuiltin;

typedef enum {
    TF_CORE_OP_CONST = 1, TF_CORE_OP_UNARY, TF_CORE_OP_BINARY,
    TF_CORE_OP_RECORD, TF_CORE_OP_VARIANT, TF_CORE_OP_LIST, TF_CORE_OP_PROJECT,
    TF_CORE_OP_TAG, TF_CORE_OP_PAYLOAD, TF_CORE_OP_IF, TF_CORE_OP_SWITCH_TAG,
    TF_CORE_OP_CALL, TF_CORE_OP_CLOSURE, TF_CORE_OP_CALL_CLOSURE,
    TF_CORE_OP_BUILTIN, TF_CORE_OP_MACHINE_VALUE, TF_CORE_OP_LIST_MAP,
    TF_CORE_OP_INPUT_AT
} TfCoreOperationKind;

typedef struct { size_t tag; TfCoreBlockId block; } TfCoreTagCase;

typedef struct {
    TfCoreOperationKind kind;
    /* Const: none. Unary/Project/Tag/Payload/If/SwitchTag/ListMap/InputAt:
     * one subject. Binary: left, right. CallClosure: callee then arguments.
     * Call/Builtin: arguments; Closure: captures. List/Record/Variant/
     * MachineValue: elements/fields/payloads/cells in layout order. */
    TfCoreValueIds operands;
    union {
        TfCoreConstantId constant;
        TfCoreUnary unary;
        TfCoreBinary binary;
        TfCoreTypeId constructed_type; /* Record, MachineValue */
        struct { TfCoreTypeId type; size_t tag; } variant;
        TfCoreTypeId element_type; /* List */
        size_t field; /* Project */
        struct { size_t tag, index; } payload;
        struct { TfCoreBlockId then_block, else_block; } branch;
        struct {
            const TfCoreTagCase *cases;
            size_t case_count;
            TfCoreBlockId default_block; /* zero only if exhaustively covered */
        } switch_tag;
        TfCoreFunctionId function; /* Call, Closure; no step builtin */
        struct {
            TfCoreBuiltin id;
            TfCoreTypeIds type_arguments; /* A then B per prelude; empty if monomorphic */
        } builtin;
        struct {
            TfCoreParameter element; /* definition visible in body only */
            TfCoreBlockId body;
        } list_map;
        size_t input_index; /* InputAt: index in entry snapshot, R006 if missing */
    } as;
} TfCoreOperation;

typedef struct {
    TfCoreValueId value;
    TfCoreTypeId type;
    TfCoreOperation operation;
    TfCoreSpanId span;
} TfCoreLet;

typedef enum { TF_CORE_YIELD = 1, TF_CORE_UNREACHABLE } TfCoreTerminatorKind;

typedef struct {
    TfCoreTerminatorKind kind;
    TfCoreValueId value; /* zero for Unreachable */
    TfCoreSpanId span;
} TfCoreTerminator;

typedef struct {
    const TfCoreLet *lets;
    size_t let_count;
    TfCoreTerminator terminator;
    TfCoreSpanId span;
} TfCoreBlock;

typedef enum {
    TF_CORE_FUNCTION_ORDINARY = 1, TF_CORE_FUNCTION_ENTRY_ONLY
} TfCoreFunctionKind;

typedef struct {
    TfCoreFunctionKind kind;
    TfCoreText name, module;
    TfCoreSpanId span;
    const TfCoreParameter *parameters, *captures;
    size_t parameter_count, capture_count;
    TfCoreTypeId result;
    const TfCoreBlock *blocks;
    size_t block_count;
    TfCoreBlockId body;
    size_t value_count;
} TfCoreFunction;

/* Blocks form a lexical tree within each function. Parameters and captures
 * dominate its root; only earlier Lets and enclosing definitions are visible.
 * The operation result is defined after its child blocks. A ListMap evaluates
 * its input list once and runs its body in a fresh element scope, left to right.
 * Each body Yield supplies one element of the result list.
 *
 * entry identifies the root main or hidden adapter (zero for a library).
 * ENTRY_ONLY marks the hidden adapter: one List[Text] snapshot parameter,
 * no captures, List[Text] result. No Call or Closure may target it; InputAt
 * is confined to its root block and reads that parameter. Staged output uses
 * ordinary List/Concat and is returned before the final source result lines.
 * A pure root main is ORDINARY, callable, and takes no arguments. */
typedef struct {
    const TfCoreType *types;
    size_t type_count;
    const TfCoreConstant *constants;
    size_t constant_count;
    const TfCoreFunction *functions;
    size_t function_count;
    const TfSpan *spans;
    size_t span_count;
    TfCoreFunctionId entry;
} TfCoreProgramData;

/* Deep copy the descriptor and all active, counted arrays and bytes; the
 * caller can discard them immediately on return. Only TfSpan.source pointers
 * are borrowed: their stable TfSource objects must outlive the program and
 * all verified views. The allocator context must outlive the owner.
 *
 * This enforces C storage safety only. Counts must describe readable arrays
 * when nonzero; NULL/nonzero or size overflow is TF_HOST_ERROR. Unknown tags,
 * invalid IDs, malformed lexical trees, noncanonical values etc. remain
 * constructible for independent verification. Unknown union discriminants
 * have no active payload. A fresh *out is cleared on failure. */
TfStatus tf_core_program_copy(TfCoreProgram **out, const TfCoreProgramData *data,
                              const TfAllocator *allocator);
void tf_core_program_destroy(TfCoreProgram *program); /* NULL permitted */
const TfCoreProgramData *tf_core_program_data(const TfCoreProgram *program);

/* Read-only ID lookup; NULL for absent or out-of-range IDs. */
const TfCoreType *tf_core_type(const TfCoreProgramData *data, TfCoreTypeId id);
const TfCoreConstant *tf_core_constant(const TfCoreProgramData *data, TfCoreConstantId id);
const TfCoreFunction *tf_core_function(const TfCoreProgramData *data, TfCoreFunctionId id);
const TfSpan *tf_core_span(const TfCoreProgramData *data, TfCoreSpanId id);
const TfCoreBlock *tf_core_block(const TfCoreFunction *function, TfCoreBlockId id);

#endif
