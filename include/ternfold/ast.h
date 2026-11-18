#ifndef TERNFOLD_AST_H
#define TERNFOLD_AST_H
#include "ternfold/lexer.h"

typedef struct TfAst TfAst;
typedef struct { const TfAst *owner; size_t index; } TfNodeId;
typedef struct { const TfNodeId *items; size_t count; } TfNodeList;
typedef struct { const unsigned char *data; size_t length; TfSpan span; } TfAstText;
typedef enum {
    TF_AST_ERROR, TF_AST_MISSING, TF_AST_PROGRAM, TF_AST_IMPORT,
    TF_AST_FUNCTION, TF_AST_RECORD, TF_AST_ENUM, TF_AST_MACHINE,
    TF_AST_PARAMETER, TF_AST_VARIANT, TF_AST_TYPE_PATH, TF_AST_TYPE_FUNCTION,
    TF_AST_PATH, TF_AST_LITERAL, TF_AST_UNIT, TF_AST_GROUP, TF_AST_LIST,
    TF_AST_UNARY, TF_AST_BINARY, TF_AST_CALL, TF_AST_ARGUMENT, TF_AST_PROJECT,
    TF_AST_NEXT, TF_AST_BLOCK, TF_AST_LET, TF_AST_IF, TF_AST_MATCH, TF_AST_ARM,
    TF_AST_CLOSURE, TF_AST_FOR, TF_AST_INPUT, TF_AST_OUTPUT,
    TF_AST_STATE, TF_AST_WRITER, TF_AST_GUARD, TF_AST_EMIT,
    TF_AST_PATTERN_WILDCARD, TF_AST_PATTERN_BINDER, TF_AST_PATTERN_LITERAL,
    TF_AST_PATTERN_VARIANT, TF_AST_PATTERN_UNIT
} TfAstKind;

/* All fields are immutable after parse. Every ID is context-scoped, index 0 is
 * absent; lookup rejects foreign IDs. Nodes, lists, names and decoded literals
 * are arena-owned at stable addresses, independent of TfLexResult lifetime.
 * Original TfSource is borrowed and must outlive AST and all span consumers.
 * Numeric literal data is exact unsigned spelling; minus has its own node/span.
 * No resolution, type arity validation, execution or constant folding occurs.
 * Missing tokens are zero-width MISSING nodes with the expected token kind;
 * recovery ERROR nodes retain skipped spans. Neither makes an executable AST.
 */
typedef struct {
    TfNodeId id;
    TfAstKind kind;
    TfSpan span;
    union {
        struct { TfNodeList declarations; } program;
        struct { TfAstText path, alias; } import_decl;
        struct { TfAstText name; bool public_decl; TfNodeList parameters; TfNodeId result_type, body; } function;
        struct { TfAstText name; bool public_decl; TfNodeList members; } aggregate;
        struct { TfAstText name; bool public_decl; TfNodeId parameter, yields, rejects; TfNodeList sections; } machine;
        struct { TfAstText name; TfNodeId type; } parameter;
        struct { TfAstText name; TfNodeList types; bool parentheses; } variant;
        struct { TfNodeList segments, arguments; } type_path;
        struct { TfNodeList parameters; TfNodeId result; } type_function;
        struct { TfAstText name; TfNodeList segments; } path;
        struct { TfTokenKind token; TfAstText value; TfSpan minus; bool negative; } literal;
        struct { TfNodeId expression; } group;
        struct { TfNodeList elements; } list;
        struct { TfTokenKind op; TfSpan operator_span; TfNodeId operand; } unary;
        struct { TfTokenKind op; TfSpan operator_span; TfNodeId left, right; } binary;
        struct { TfNodeId callee; TfNodeList arguments; } call;
        struct { TfAstText name; bool named; TfNodeId value; } argument;
        struct { TfNodeId base; TfAstText name; } project;
        struct { TfAstText name; } next;
        struct { TfNodeList statements; TfNodeId final_expression; } block;
        struct { TfAstText name; bool discard; TfNodeId type, value; } binding;
        struct { TfNodeId condition, then_block, else_block; } conditional;
        struct { TfNodeId value; TfNodeList arms; } matching;
        struct { TfNodeId pattern, body; } arm;
        struct { TfNodeList parameters; TfNodeId result_type, body; } closure;
        struct { TfAstText name; TfNodeId iterable, body; } loop;
        struct { TfAstText name; TfNodeId type; bool from_stdin; } input;
        struct { TfNodeId value; bool to_stdout; } output;
        struct { TfAstText name; TfNodeId type, value; } state;
        struct { TfAstText name; TfNodeId value; } writer;
        struct { TfNodeId condition, rejection; } guard;
        struct { TfNodeId value; } emission;
        struct { TfAstText name; } binder;
        struct { TfNodeId path; TfNodeList arguments; bool parentheses; } pattern_variant;
        struct { TfTokenKind expected; } missing;
    } as;
} TfAstNode;

const TfAstNode *tf_ast_get(const TfAst *ast, TfNodeId id);
const TfAstNode *tf_ast_at(const TfAst *ast, size_t zero_based_index);
size_t tf_ast_count(const TfAst *ast);
TfNodeId tf_ast_root(const TfAst *ast);
const TfSource *tf_ast_source(const TfAst *ast);
const char *tf_ast_kind_name(TfAstKind kind);

/* Construction interface for the sole parser implementation. Borrowers should
 * use only const accessors above. Arena allocations are aligned/zero-initialized;
 * failure never invalidates prior allocations. Destroy once, NULL permitted. */
#ifdef TERNFOLD_AST_BUILD
TfAst *tf_ast_create(const TfSource *source, TfAllocator allocator);
void tf_ast_destroy(TfAst *ast);
void *tf_ast_allocate(TfAst *ast, size_t count, size_t width);
TfAstNode *tf_ast_node(TfAst *ast, TfAstKind kind, TfSpan span);
bool tf_ast_append(TfAst *ast, TfNodeList *list, TfNodeId id);
bool tf_ast_copy_text(TfAst *ast, TfAstText *out, const void *bytes, size_t length, TfSpan span);
void tf_ast_set_root(TfAst *ast, TfNodeId root);
#endif
#endif
