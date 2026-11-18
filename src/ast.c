#define TERNFOLD_AST_BUILD
#include "ternfold/ast.h"
#include "internal.h"
#include <string.h>

typedef union ArenaBlock ArenaBlock;
union ArenaBlock { max_align_t alignment; struct { ArenaBlock *next; } link; };
struct TfAst {
    TfAllocator allocator;
    const TfSource *source;
    ArenaBlock *blocks;
    TfAstNode **nodes;
    size_t count, capacity;
    TfNodeId root;
};
TfAst *tf_ast_create(const TfSource *source, TfAllocator allocator) {
    TfAst *ast = allocator.allocate(allocator.context,sizeof(*ast));
    if (ast != NULL) { *ast = (TfAst){0}; ast->allocator = allocator; ast->source = source; }
    return ast;
}
void tf_ast_destroy(TfAst *ast) {
    ArenaBlock *block;
    TfAllocator a;
    if (ast == NULL) return;
    a = ast->allocator;
    while ((block = ast->blocks) != NULL) { ast->blocks = block->link.next; a.deallocate(a.context,block); }
    if (ast->nodes != NULL) a.deallocate(a.context,ast->nodes);
    a.deallocate(a.context,ast);
}
void *tf_ast_allocate(TfAst *ast, size_t count, size_t width) {
    ArenaBlock *block; size_t bytes;
    if (count == 0 || width == 0 || count > (SIZE_MAX-sizeof(*block))/width) return NULL;
    bytes = count*width;
    block = ast->allocator.allocate(ast->allocator.context,sizeof(*block)+bytes);
    if (block == NULL) return NULL;
    block->link.next = ast->blocks; ast->blocks = block;
    memset(block+1,0,bytes); return block+1;
}
TfAstNode *tf_ast_node(TfAst *ast, TfAstKind kind, TfSpan span) {
    TfAstNode *node; void *storage = ast->nodes;
    if (ast->count == SIZE_MAX || !tf_grow(&storage,&ast->capacity,ast->count+1,sizeof(*ast->nodes),ast->allocator)) return NULL;
    ast->nodes = storage;
    node = tf_ast_allocate(ast,1,sizeof(*node));
    if (node == NULL) return NULL;
    node->id = (TfNodeId){ast,ast->count+1}; node->kind = kind; node->span = span;
    ast->nodes[ast->count++] = node; return node;
}
bool tf_ast_append(TfAst *ast, TfNodeList *list, TfNodeId id) {
    /* Power-of-two arena vectors retain previous backing allocations; total
     * list storage is bounded by twice the final power-of-two capacity. */
    TfNodeId *items; size_t capacity;
    if (list->count == SIZE_MAX) return false;
    if (list->count == 0 || (list->count & (list->count-1)) == 0) {
        if (list->count > SIZE_MAX/2) return false;
        capacity = list->count == 0 ? 1 : list->count*2;
        items = tf_ast_allocate(ast,capacity,sizeof(*items)); if (items == NULL) return false;
        if (list->count != 0) memcpy(items,list->items,list->count*sizeof(*items));
        list->items = items;
    }
    items = (TfNodeId *)list->items; items[list->count++] = id; return true;
}
bool tf_ast_copy_text(TfAst *ast, TfAstText *out, const void *bytes, size_t length, TfSpan span) {
    unsigned char *text;
    if (length == SIZE_MAX) return false;
    text = tf_ast_allocate(ast,length+1,1); if (text == NULL) return false;
    if (length != 0) memcpy(text,bytes,length);
    *out = (TfAstText){text,length,span}; return true;
}
const TfAstNode *tf_ast_get(const TfAst *ast, TfNodeId id) {
    return ast != NULL && id.owner == ast && id.index != 0 && id.index <= ast->count ? ast->nodes[id.index-1] : NULL;
}
const TfAstNode *tf_ast_at(const TfAst *ast, size_t index) { return ast != NULL && index < ast->count ? ast->nodes[index] : NULL; }
size_t tf_ast_count(const TfAst *ast) { return ast != NULL ? ast->count : 0; }
TfNodeId tf_ast_root(const TfAst *ast) { return ast != NULL ? ast->root : (TfNodeId){0}; }
const TfSource *tf_ast_source(const TfAst *ast) { return ast != NULL ? ast->source : NULL; }
void tf_ast_set_root(TfAst *ast, TfNodeId root) { ast->root = root; }
const char *tf_ast_kind_name(TfAstKind kind) {
    static const char *const names[] = {"ERROR","MISSING","PROGRAM","IMPORT","FUNCTION","RECORD","ENUM","MACHINE",
        "PARAMETER","VARIANT","TYPE_PATH","TYPE_FUNCTION","PATH","LITERAL","UNIT","GROUP","LIST","UNARY","BINARY",
        "CALL","ARGUMENT","PROJECT","NEXT","BLOCK","LET","IF","MATCH","ARM","CLOSURE","FOR","INPUT","OUTPUT",
        "STATE","WRITER","GUARD","EMIT","PATTERN_WILDCARD","PATTERN_BINDER","PATTERN_LITERAL","PATTERN_VARIANT","PATTERN_UNIT"};
    size_t index = (size_t)kind;
    return index < sizeof(names)/sizeof(names[0]) ? names[index] : "UNKNOWN";
}
