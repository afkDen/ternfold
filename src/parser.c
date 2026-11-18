#define TERNFOLD_AST_BUILD
#include "ternfold/parser.h"
#include "internal.h"
#include "repl_parse.h"
#include <string.h>

struct TfParseResult {
    TfAllocator allocator;
    TfAst *ast;
    TfDiagnosticBag *diagnostics;
    TfDiagnosticPhaseResult phase;
};
typedef struct {
    const TfLexResult *lex;
    TfParseResult *result;
    TfParseOptions options;
    size_t at, end, depth;
    bool failed, saw_error, interior_error;
    TfAstNode dummy;
} Parser;

static bool trivia(TfTokenKind k) { return k == TF_COMMENT_LINE || k == TF_COMMENT_BLOCK; }
static const TfToken *peek(Parser *p) {
    while (p->at+1 < p->lex->token_count && trivia(p->lex->tokens[p->at].kind)) ++p->at;
    return &p->lex->tokens[p->at];
}
static TfTokenKind kind(Parser *p) { return p->failed ? TF_EOF : peek(p)->kind; }
static TfToken take(Parser *p) {
    TfToken t = *peek(p);
    if (t.kind != TF_EOF) ++p->at;
    p->end = t.span.end; return t;
}
static bool eat(Parser *p, TfTokenKind k) { if (kind(p) != k) return false; (void)take(p); return true; }
static TfSpan span(Parser *p, size_t start, size_t end) { return (TfSpan){p->lex->source,start,end}; }
static TfAstNode *node(Parser *p, TfAstKind k, size_t start) {
    TfAstNode *n;
    if (p->failed) return &p->dummy;
    n = tf_ast_node(p->result->ast,k,span(p,start,start));
    if (n == NULL) { p->failed = true; return &p->dummy; }
    return n;
}
static TfAstNode *finish(Parser *p, TfAstNode *n) {
    n->span.end = p->end >= n->span.start ? p->end : n->span.start; return n;
}
static void append(Parser *p, TfNodeList *list, TfNodeId id) {
    if (!p->failed && !tf_ast_append(p->result->ast,list,id)) p->failed = true;
}
static void diagnose(Parser *p, const char *code, TfSpan where, const char *message) {
    TfDiagnosticLabel label = {where,true,{NULL,0}};
    TfDiagnosticRecord r = {0};
    if (p->failed) return;
    p->saw_error = true;
    if (where.start != p->lex->source->length || where.end != where.start) p->interior_error = true;
    r.code = code; r.severity = TF_DIAG_ERROR; r.message = (TfDiagnosticText){message,strlen(message)};
    r.labels = &label; r.label_count = 1;
    if (tf_diagnostic_bag_add(p->result->diagnostics,&r) != TF_OK) p->failed = true;
}
static TfToken expect(Parser *p, TfTokenKind expected) {
    TfToken t;
    if (kind(p) == expected) return take(p);
    t = *peek(p); t.kind = expected; t.span.end = t.span.start; t.value = NULL; t.value_length = 0;
    diagnose(p,"E110",t.span,"expected missing syntax token");
    node(p,TF_AST_MISSING,t.span.start)->as.missing.expected = expected;
    return t;
}
static TfAstText token_text(Parser *p, TfToken t, bool decoded) {
    TfAstText text = {0};
    const void *bytes = decoded ? t.value : t.span.source->bytes+t.span.start;
    size_t length = decoded ? t.value_length : t.span.end-t.span.start;
    if (!p->failed && !tf_ast_copy_text(p->result->ast,&text,bytes,length,t.span)) p->failed = true;
    return text;
}
static TfAstText name(Parser *p) { return token_text(p,expect(p,TF_IDENT),false); }
static bool text_is(TfAstText text, const char *value) {
    size_t length = strlen(value); return text.length == length && (length == 0 || memcmp(text.data,value,length) == 0);
}
static bool enter(Parser *p) {
    if (p->failed || p->depth >= p->options.nesting_limit) { p->failed = true; return false; }
    ++p->depth; return true;
}
static bool top_boundary(Parser *p) {
    TfTokenKind k = kind(p);
    if (k == TF_PUB || k == TF_RECORD || k == TF_ENUM || k == TF_MACHINE || k == TF_IMPORT) return true;
    if (k == TF_FN) {
        size_t at = p->at+1;
        while (at < p->lex->token_count && trivia(p->lex->tokens[at].kind)) ++at;
        return at < p->lex->token_count && p->lex->tokens[at].kind == TF_IDENT;
    }
    return false;
}
static bool expression_stop(TfTokenKind k) {
    return k == TF_EOF || k == TF_RBRACE || k == TF_RPAREN || k == TF_RBRACKET || k == TF_COMMA ||
        k == TF_SEMICOLON || k == TF_ELSE || k == TF_FAT_ARROW || k == TF_TO || k == TF_FROM;
}
static TfAstNode *recover(Parser *p, bool top) {
    size_t start = peek(p)->span.start, depth = 0;
    TfTokenKind closers[128];
    TfAstNode *error = node(p,TF_AST_ERROR,start);
    while (kind(p) != TF_EOF) {
        TfTokenKind k = kind(p);
        /* Declaration keywords cannot occur inside an expression. Even when
         * malformed skipped text left an opener unmatched, this declaration
         * belongs to the caller, not the discarded recovery range. */
        if (top_boundary(p)) break;
        if (depth == 0) {
            if (k == TF_RBRACE || (!top && (k == TF_LET || k == TF_INPUT || k == TF_OUTPUT))) break;
            if (k == TF_SEMICOLON) { (void)take(p); break; }
        }
        if (k == TF_LBRACE || k == TF_LPAREN || k == TF_LBRACKET) {
            if (depth == p->options.nesting_limit) { p->failed = true; break; }
            closers[depth++] = k == TF_LBRACE ? TF_RBRACE : k == TF_LPAREN ? TF_RPAREN : TF_RBRACKET;
        } else if (depth != 0 && k == closers[depth-1]) --depth;
        (void)take(p);
    }
    /* Insertion records retain zero-width spans at the next declaration/EOF;
     * expect never consumes that boundary, and all work shares the bag cap. */
    while (depth != 0 && !p->failed) (void)expect(p,closers[--depth]);
    return finish(p,error);
}
static TfAstNode *expression(Parser *p, unsigned minimum);
static TfAstNode *type(Parser *p);
static TfAstNode *pattern(Parser *p);
static TfAstNode *block(Parser *p, bool io);

static TfNodeList segments(Parser *p) {
    TfNodeList list = {0};
    do {
        TfAstNode *segment = node(p,TF_AST_PATH,peek(p)->span.start);
        segment->as.path.name = name(p); finish(p,segment); append(p,&list,segment->id);
    } while (eat(p,TF_DOUBLE_COLON));
    return list;
}
static TfNodeList parameters(Parser *p) {
    TfNodeList list = {0};
    (void)expect(p,TF_LPAREN);
    while (kind(p) != TF_RPAREN && kind(p) != TF_EOF && !top_boundary(p)) {
        size_t before = p->at;
        TfAstNode *param = node(p,TF_AST_PARAMETER,peek(p)->span.start);
        param->as.parameter.name = name(p); (void)expect(p,TF_COLON);
        param->as.parameter.type = type(p)->id; finish(p,param); append(p,&list,param->id);
        if (!eat(p,TF_COMMA)) break;
        if (p->at == before) break;
    }
    (void)expect(p,TF_RPAREN); return list;
}
static TfAstNode *type(Parser *p) {
    TfAstNode *n;
    size_t start = peek(p)->span.start;
    if (!enter(p)) return &p->dummy;
    if (top_boundary(p)) {
        diagnose(p,"E110",span(p,start,start),"expected type before following declaration"); n = node(p,TF_AST_ERROR,start);
    } else if (eat(p,TF_FN)) {
        n = node(p,TF_AST_TYPE_FUNCTION,start); (void)expect(p,TF_LPAREN);
        if (kind(p) != TF_RPAREN) do { append(p,&n->as.type_function.parameters,type(p)->id); } while (eat(p,TF_COMMA));
        (void)expect(p,TF_RPAREN); (void)expect(p,TF_ARROW); n->as.type_function.result = type(p)->id;
    } else if (kind(p) == TF_IDENT) {
        n = node(p,TF_AST_TYPE_PATH,start); n->as.type_path.segments = segments(p);
        if (eat(p,TF_LBRACKET)) {
            do { append(p,&n->as.type_path.arguments,type(p)->id); } while (eat(p,TF_COMMA));
            (void)expect(p,TF_RBRACKET);
        }
    } else {
        diagnose(p,"E110",peek(p)->span,"expected a type"); n = node(p,TF_AST_ERROR,start);
        if (!expression_stop(kind(p)) && kind(p) != TF_EQUAL && kind(p) != TF_LBRACE && !top_boundary(p)) (void)take(p);
    }
    --p->depth; return finish(p,n);
}
static TfAstNode *literal(Parser *p, bool pat, bool negative, TfSpan minus) {
    TfToken t = take(p);
    TfAstNode *n = node(p,pat ? TF_AST_PATTERN_LITERAL : TF_AST_LITERAL,negative ? minus.start : t.span.start);
    n->as.literal.token = t.kind; n->as.literal.negative = negative; n->as.literal.minus = minus;
    n->as.literal.value = token_text(p,t,t.kind == TF_TEXT || t.kind == TF_CHAR);
    return finish(p,n);
}
static TfAstNode *pattern(Parser *p) {
    TfAstNode *n; size_t start = peek(p)->span.start;
    if (!enter(p)) return &p->dummy;
    if (eat(p,TF_UNDERSCORE)) n = node(p,TF_AST_PATTERN_WILDCARD,start);
    else if (kind(p) == TF_MINUS) {
        TfToken minus = take(p);
        if (kind(p) == TF_INT || kind(p) == TF_FLOAT) n = literal(p,true,true,minus.span);
        else { diagnose(p,"E110",peek(p)->span,"expected numeric pattern after minus"); n = node(p,TF_AST_ERROR,start); }
    } else if (kind(p) == TF_INT || kind(p) == TF_FLOAT || kind(p) == TF_TEXT || kind(p) == TF_CHAR ||
               kind(p) == TF_TRUE || kind(p) == TF_FALSE) n = literal(p,true,false,(TfSpan){0});
    else if (eat(p,TF_LPAREN)) { n = node(p,TF_AST_PATTERN_UNIT,start); (void)expect(p,TF_RPAREN); }
    else if (kind(p) == TF_IDENT) {
        TfAstNode *path = node(p,TF_AST_PATH,start); path->as.path.segments = segments(p); finish(p,path);
        if (path->as.path.segments.count == 1) {
            const TfAstNode *leaf = tf_ast_get(p->result->ast,path->as.path.segments.items[0]);
            n = node(p,TF_AST_PATTERN_BINDER,start); if (leaf != NULL) n->as.binder.name = leaf->as.path.name;
        } else {
            n = node(p,TF_AST_PATTERN_VARIANT,start); n->as.pattern_variant.path = path->id;
            if (eat(p,TF_LPAREN)) {
                n->as.pattern_variant.parentheses = true;
                do { append(p,&n->as.pattern_variant.arguments,pattern(p)->id); } while (eat(p,TF_COMMA) && kind(p) != TF_RPAREN);
                (void)expect(p,TF_RPAREN);
            }
        }
    } else {
        diagnose(p,"E110",peek(p)->span,"expected a pattern"); n = node(p,TF_AST_ERROR,start);
        if (!expression_stop(kind(p)) && !top_boundary(p)) (void)take(p);
    }
    --p->depth; return finish(p,n);
}
static TfAstNode *primary(Parser *p) {
    size_t start = peek(p)->span.start;
    TfTokenKind k = kind(p); TfAstNode *n;
    if (top_boundary(p)) {
        diagnose(p,"E110",span(p,start,start),"expected expression before following declaration");
        return node(p,TF_AST_ERROR,start);
    }
    if (k == TF_INT || k == TF_FLOAT || k == TF_TEXT || k == TF_CHAR || k == TF_TRUE || k == TF_FALSE)
        return literal(p,false,false,(TfSpan){0});
    if (k == TF_IDENT) { n = node(p,TF_AST_PATH,start); n->as.path.segments = segments(p); return finish(p,n); }
    if (eat(p,TF_NEXT)) {
        n = node(p,TF_AST_NEXT,start); (void)expect(p,TF_DOT); n->as.next.name = name(p); return finish(p,n);
    }
    if (eat(p,TF_LPAREN)) {
        if (eat(p,TF_RPAREN)) return finish(p,node(p,TF_AST_UNIT,start));
        n = node(p,TF_AST_GROUP,start); n->as.group.expression = expression(p,0)->id;
        (void)expect(p,TF_RPAREN); return finish(p,n);
    }
    if (eat(p,TF_LBRACKET)) {
        n = node(p,TF_AST_LIST,start);
        if (kind(p) != TF_RBRACKET) do { append(p,&n->as.list.elements,expression(p,0)->id); } while (eat(p,TF_COMMA) && kind(p) != TF_RBRACKET);
        (void)expect(p,TF_RBRACKET); return finish(p,n);
    }
    if (k == TF_LBRACE) return block(p,false);
    if (eat(p,TF_IF)) {
        n = node(p,TF_AST_IF,start); n->as.conditional.condition = expression(p,0)->id;
        n->as.conditional.then_block = block(p,false)->id; (void)expect(p,TF_ELSE);
        n->as.conditional.else_block = block(p,false)->id; return finish(p,n);
    }
    if (eat(p,TF_FOR)) {
        n = node(p,TF_AST_FOR,start); n->as.loop.name = name(p); (void)expect(p,TF_IN);
        n->as.loop.iterable = expression(p,0)->id; n->as.loop.body = block(p,false)->id; return finish(p,n);
    }
    if (eat(p,TF_FN)) {
        n = node(p,TF_AST_CLOSURE,start); n->as.closure.parameters = parameters(p);
        (void)expect(p,TF_ARROW); n->as.closure.result_type = type(p)->id;
        n->as.closure.body = block(p,false)->id; return finish(p,n);
    }
    if (eat(p,TF_MATCH)) {
        n = node(p,TF_AST_MATCH,start); n->as.matching.value = expression(p,0)->id; (void)expect(p,TF_LBRACE);
        if (kind(p) == TF_RBRACE) diagnose(p,"E110",peek(p)->span,"match requires an arm");
        while (kind(p) != TF_RBRACE && kind(p) != TF_EOF && !top_boundary(p)) {
            size_t before = p->at;
            TfAstNode *arm = node(p,TF_AST_ARM,peek(p)->span.start);
            arm->as.arm.pattern = pattern(p)->id; (void)expect(p,TF_FAT_ARROW);
            arm->as.arm.body = block(p,false)->id; finish(p,arm); append(p,&n->as.matching.arms,arm->id);
            if (!eat(p,TF_COMMA)) break;
            if (p->at == before) break;
        }
        (void)expect(p,TF_RBRACE); return finish(p,n);
    }
    if (k != TF_INVALID) diagnose(p,"E110",peek(p)->span,"expected an expression");
    n = node(p,TF_AST_ERROR,start);
    if (!expression_stop(k) && !top_boundary(p)) (void)take(p);
    return finish(p,n);
}
static unsigned binding_power(TfTokenKind k) {
    switch (k) {
        case TF_OR_OR: return 1; case TF_AND_AND: return 2;
        case TF_EQUAL_EQUAL: case TF_BANG_EQUAL: return 3;
        case TF_LESS: case TF_LESS_EQUAL: case TF_GREATER: case TF_GREATER_EQUAL: return 4;
        case TF_PLUS: case TF_MINUS: case TF_PLUS_PLUS: return 5;
        case TF_STAR: case TF_SLASH: case TF_DIV: case TF_PERCENT: return 6;
        case TF_CARET: return 8;
        default: return 0;
    }
}
static bool named_argument(Parser *p) {
    size_t at;
    if (kind(p) != TF_IDENT) return false;
    at = p->at+1; while (at < p->lex->token_count && trivia(p->lex->tokens[at].kind)) ++at;
    return at < p->lex->token_count && p->lex->tokens[at].kind == TF_COLON;
}
static TfAstNode *expression(Parser *p, unsigned minimum) {
    TfAstNode *left; unsigned nonassoc = 0;
    if (!enter(p)) return &p->dummy;
    if (kind(p) == TF_MINUS || kind(p) == TF_BANG) {
        TfToken op = take(p); left = node(p,TF_AST_UNARY,op.span.start);
        left->as.unary.op = op.kind; left->as.unary.operator_span = op.span;
        left->as.unary.operand = expression(p,7)->id; finish(p,left);
    } else left = primary(p);
    while (!p->failed) {
        TfTokenKind k = kind(p); unsigned power = binding_power(k);
        if (k == TF_LPAREN) {
            TfAstNode *call = node(p,TF_AST_CALL,left->span.start); int mode = -1;
            call->as.call.callee = left->id; (void)take(p);
            while (kind(p) != TF_RPAREN && kind(p) != TF_EOF && !top_boundary(p)) {
                size_t before = p->at; bool named = named_argument(p);
                TfAstNode *argument = node(p,TF_AST_ARGUMENT,peek(p)->span.start);
                argument->as.argument.named = named;
                if (mode != -1 && mode != (named ? 1 : 0)) diagnose(p,"E112",peek(p)->span,"mixed named and positional arguments");
                mode = named ? 1 : 0;
                if (named) { argument->as.argument.name = name(p); (void)expect(p,TF_COLON); }
                argument->as.argument.value = expression(p,0)->id; finish(p,argument); append(p,&call->as.call.arguments,argument->id);
                if (!eat(p,TF_COMMA)) break;
                if (p->at == before) break;
            }
            (void)expect(p,TF_RPAREN); left = finish(p,call); continue;
        }
        if (k == TF_DOT) {
            TfAstNode *project = node(p,TF_AST_PROJECT,left->span.start); (void)take(p);
            project->as.project.base = left->id; project->as.project.name = name(p); left = finish(p,project); continue;
        }
        if (power == 0 || power < minimum) break;
        {
            TfToken op = take(p); TfAstNode *binary = node(p,TF_AST_BINARY,left->span.start);
            if ((power == 3 || power == 4) && (nonassoc & (1u<<power)) != 0)
                diagnose(p,"E111",op.span,"comparison/equality operators do not chain");
            nonassoc |= 1u<<power;
            binary->as.binary.op = op.kind; binary->as.binary.operator_span = op.span; binary->as.binary.left = left->id;
            binary->as.binary.right = expression(p,k == TF_CARET ? 7 : power+1)->id; left = finish(p,binary);
        }
    }
    --p->depth; return left;
}
static TfAstNode *statement(Parser *p, bool io) {
    size_t start = peek(p)->span.start; TfTokenKind k = kind(p); TfAstNode *n;
    (void)take(p);
    if (k == TF_LET) {
        n = node(p,TF_AST_LET,start);
        if (kind(p) == TF_UNDERSCORE) { n->as.binding.discard = true; n->as.binding.name = token_text(p,take(p),false); }
        else n->as.binding.name = name(p);
        if (eat(p,TF_COLON)) n->as.binding.type = type(p)->id;
        (void)expect(p,TF_EQUAL); n->as.binding.value = expression(p,0)->id;
    } else if (k == TF_INPUT) {
        n = node(p,TF_AST_INPUT,start);
        if (!io) diagnose(p,"E208",span(p,start,p->end),"input is restricted to root public zero-argument main outer body");
        if (kind(p) == TF_UNDERSCORE || (kind(p) >= TF_AS && kind(p) <= TF_YIELDS)) {
            TfToken bad = take(p); n->as.input.name = token_text(p,bad,false);
            diagnose(p,"E208",bad.span,"input binder cannot be underscore or a keyword");
        } else {
            TfToken binder = expect(p,TF_IDENT); n->as.input.name = token_text(p,binder,false);
            if (binder.reserved) diagnose(p,"E208",binder.span,"input binder cannot be reserved");
        }
        (void)expect(p,TF_COLON); n->as.input.type = type(p)->id;
        /* Input type checking belongs to M06; parser retains the type syntax. */
        if (eat(p,TF_FROM)) { n->as.input.from_stdin = true; (void)expect(p,TF_STDIN); }
    } else {
        n = node(p,TF_AST_OUTPUT,start);
        if (!io) diagnose(p,"E208",span(p,start,p->end),"output is restricted to root public zero-argument main outer body");
        n->as.output.value = expression(p,0)->id;
        if (eat(p,TF_TO)) { n->as.output.to_stdout = true; (void)expect(p,TF_STDOUT); }
    }
    (void)expect(p,TF_SEMICOLON); return finish(p,n);
}
static TfAstNode *block(Parser *p, bool io) {
    size_t start = peek(p)->span.start; TfAstNode *n;
    if (!enter(p)) return &p->dummy;
    n = node(p,TF_AST_BLOCK,start); (void)expect(p,TF_LBRACE);
    while (kind(p) != TF_RBRACE && kind(p) != TF_EOF && !top_boundary(p)) {
        size_t before = p->at;
        if (kind(p) == TF_LET || kind(p) == TF_INPUT || kind(p) == TF_OUTPUT) {
            if (n->as.block.final_expression.index != 0) diagnose(p,"E110",peek(p)->span,"statement follows final expression");
            append(p,&n->as.block.statements,statement(p,io)->id);
        } else {
            TfAstNode *value = expression(p,0);
            n->as.block.final_expression = value->id;
            if (eat(p,TF_SEMICOLON)) {
                diagnose(p,"E110",span(p,p->end-1,p->end),"final block expression must not have a semicolon");
                append(p,&n->as.block.statements,value->id); n->as.block.final_expression = (TfNodeId){0};
            } else if (kind(p) != TF_RBRACE && kind(p) != TF_EOF && !top_boundary(p)) {
                diagnose(p,"E110",peek(p)->span,"expected closing brace after final expression");
                append(p,&n->as.block.statements,recover(p,false)->id);
            } else break;
        }
        if (p->at == before) {
            if (kind(p) == TF_EOF || kind(p) == TF_RBRACE || top_boundary(p)) break;
            TfAstNode *error = node(p,TF_AST_ERROR,peek(p)->span.start); (void)take(p);
            append(p,&n->as.block.statements,finish(p,error)->id);
        }
    }
    if (n->as.block.final_expression.index == 0) {
        diagnose(p,"E110",span(p,peek(p)->span.start,peek(p)->span.start),"block requires a final expression");
        n->as.block.final_expression = node(p,TF_AST_ERROR,peek(p)->span.start)->id;
    }
    (void)expect(p,TF_RBRACE); --p->depth; return finish(p,n);
}
static TfAstNode *machine_section(Parser *p) {
    TfToken t = take(p); TfAstNode *n;
    if (t.kind == TF_STATE) {
        n = node(p,TF_AST_STATE,t.span.start); n->as.state.name = name(p); (void)expect(p,TF_COLON);
        n->as.state.type = type(p)->id; (void)expect(p,TF_EQUAL); n->as.state.value = expression(p,0)->id;
    } else if (t.kind == TF_NEXT) {
        n = node(p,TF_AST_WRITER,t.span.start); n->as.writer.name = name(p); (void)expect(p,TF_EQUAL);
        n->as.writer.value = expression(p,0)->id;
    } else if (t.kind == TF_GUARD) {
        n = node(p,TF_AST_GUARD,t.span.start); n->as.guard.condition = expression(p,0)->id;
        (void)expect(p,TF_ELSE); n->as.guard.rejection = expression(p,0)->id;
    } else { n = node(p,TF_AST_EMIT,t.span.start); n->as.emission.value = expression(p,0)->id; }
    (void)expect(p,TF_SEMICOLON); return finish(p,n);
}
static TfAstNode *declaration(Parser *p) {
    size_t start = peek(p)->span.start; bool pub = eat(p,TF_PUB); TfTokenKind k = kind(p); TfAstNode *n;
    if (eat(p,TF_IMPORT)) {
        n = node(p,TF_AST_IMPORT,start);
        if (pub) diagnose(p,"E110",span(p,start,p->end),"imports cannot be public");
        n->as.import_decl.path = token_text(p,expect(p,TF_TEXT),true); (void)expect(p,TF_AS);
        n->as.import_decl.alias = name(p); (void)expect(p,TF_SEMICOLON);
    } else if (eat(p,TF_FN)) {
        n = node(p,TF_AST_FUNCTION,start); n->as.function.public_decl = pub; n->as.function.name = name(p);
        n->as.function.parameters = parameters(p); (void)expect(p,TF_ARROW); n->as.function.result_type = type(p)->id;
        n->as.function.body = block(p,p->options.is_root && pub && n->as.function.parameters.count == 0 &&
                                   text_is(n->as.function.name,"main"))->id;
    } else if (k == TF_RECORD || k == TF_ENUM) {
        (void)take(p); n = node(p,k == TF_RECORD ? TF_AST_RECORD : TF_AST_ENUM,start);
        n->as.aggregate.public_decl = pub; n->as.aggregate.name = name(p); (void)expect(p,TF_LBRACE);
        if (k == TF_ENUM && kind(p) == TF_RBRACE) diagnose(p,"E110",peek(p)->span,"enum requires a variant");
        while (kind(p) != TF_RBRACE && kind(p) != TF_EOF && !top_boundary(p)) {
            size_t before = p->at;
            TfAstNode *member = node(p,k == TF_RECORD ? TF_AST_PARAMETER : TF_AST_VARIANT,peek(p)->span.start);
            if (k == TF_RECORD) {
                member->as.parameter.name = name(p); (void)expect(p,TF_COLON); member->as.parameter.type = type(p)->id;
            } else {
                member->as.variant.name = name(p);
                if (eat(p,TF_LPAREN)) {
                    member->as.variant.parentheses = true;
                    do { append(p,&member->as.variant.types,type(p)->id); } while (eat(p,TF_COMMA) && kind(p) != TF_RPAREN);
                    (void)expect(p,TF_RPAREN);
                }
            }
            finish(p,member); append(p,&n->as.aggregate.members,member->id);
            if (!eat(p,TF_COMMA)) break;
            if (p->at == before) break;
        }
        (void)expect(p,TF_RBRACE);
    } else if (eat(p,TF_MACHINE)) {
        unsigned order = 0; size_t states = 0, writers = 0;
        n = node(p,TF_AST_MACHINE,start); n->as.machine.public_decl = pub; n->as.machine.name = name(p);
        (void)expect(p,TF_LPAREN);
        {
            TfAstNode *param = node(p,TF_AST_PARAMETER,peek(p)->span.start);
            param->as.parameter.name = name(p); (void)expect(p,TF_COLON); param->as.parameter.type = type(p)->id;
            finish(p,param); n->as.machine.parameter = param->id;
        }
        (void)expect(p,TF_RPAREN); (void)expect(p,TF_YIELDS); n->as.machine.yields = type(p)->id;
        (void)expect(p,TF_REJECTS); n->as.machine.rejects = type(p)->id; (void)expect(p,TF_LBRACE);
        while (kind(p) != TF_RBRACE && kind(p) != TF_EOF && !top_boundary(p)) {
            size_t before = p->at; TfTokenKind section = kind(p); unsigned stage;
            if (section == TF_STATE || section == TF_NEXT || section == TF_GUARD || section == TF_EMIT) {
                stage = section == TF_STATE ? 0u : section == TF_NEXT ? 1u : section == TF_GUARD ? 2u : 3u;
                if (stage < order) diagnose(p,"E110",peek(p)->span,"machine sections must be state, next, guard, emit in order");
                if (stage > order) order = stage;
                if (section == TF_STATE) ++states;
                if (section == TF_NEXT) ++writers;
                append(p,&n->as.machine.sections,machine_section(p)->id);
            } else if (section == TF_INPUT || section == TF_OUTPUT) {
                append(p,&n->as.machine.sections,statement(p,false)->id);
            } else {
                diagnose(p,"E110",peek(p)->span,"expected machine section");
                append(p,&n->as.machine.sections,recover(p,false)->id);
            }
            if (p->at == before && kind(p) != TF_EOF) {
                TfAstNode *error = node(p,TF_AST_ERROR,peek(p)->span.start); (void)take(p);
                append(p,&n->as.machine.sections,finish(p,error)->id);
            }
        }
        if (states == 0 || writers == 0) diagnose(p,"E110",peek(p)->span,"machine requires state and next sections");
        (void)expect(p,TF_RBRACE);
    } else {
        diagnose(p,"E110",peek(p)->span,"expected a top-level declaration");
        n = recover(p,true);
        if (p->end < start) p->end = start;
    }
    return finish(p,n);
}
void tf_parse_destroy(TfParseResult *result) {
    TfAllocator a;
    if (result == NULL) return;
    a = result->allocator; tf_ast_destroy(result->ast); tf_diagnostic_bag_destroy(result->diagnostics); a.deallocate(a.context,result);
}
const TfAst *tf_parse_ast(const TfParseResult *r) { return r != NULL ? r->ast : NULL; }
const TfDiagnosticBag *tf_parse_diagnostics(const TfParseResult *r) { return r != NULL ? r->diagnostics : NULL; }
const TfDiagnosticPhaseResult *tf_parse_phase(const TfParseResult *r) { return r != NULL ? &r->phase : NULL; }
static bool input_valid(const TfLexResult *lex) {
    size_t i, previous = 0; TfSpan checked;
    if (lex == NULL || lex->source == NULL || !lex->source->valid || lex->tokens == NULL || lex->token_count == 0 ||
        lex->token_count > SIZE_MAX/sizeof(*lex->tokens)) return false;
    for (i = 0; i < lex->token_count; ++i) {
        const TfToken *t = &lex->tokens[i];
        if (t->kind < TF_IDENT || t->kind > TF_COMMENT_BLOCK || t->span.source != lex->source ||
            !tf_source_span(lex->source,t->span.start,t->span.end,&checked) || t->span.start < previous ||
            (t->value_length != 0 && t->value == NULL)) return false;
        if (t->kind == TF_EOF) { if (i != lex->token_count-1 || t->span.start != lex->source->length || t->span.end != lex->source->length) return false; }
        else if (i == lex->token_count-1 || t->span.start == t->span.end) return false;
        previous = t->span.end;
    }
    return true;
}
static void parse_module(Parser *p) {
    bool declarations = false;
    TfAstNode *program = node(p,TF_AST_PROGRAM,0);
    while (kind(p) != TF_EOF) {
        size_t before = p->at; TfAstNode *decl;
        if (kind(p) == TF_IMPORT && declarations) diagnose(p,"E110",peek(p)->span,"imports must precede declarations");
        if (kind(p) != TF_IMPORT) declarations = true;
        decl = declaration(p); append(p,&program->as.program.declarations,decl->id);
        if (p->at == before && kind(p) != TF_EOF) {
            TfAstNode *error = node(p,TF_AST_ERROR,peek(p)->span.start); (void)take(p);
            append(p,&program->as.program.declarations,finish(p,error)->id);
        }
    }
    program->span.end = p->lex->source->length; tf_ast_set_root(p->result->ast,program->id);
}

TfStatus tf_parse(TfParseResult **out, const TfLexResult *lex,
                  const TfParseOptions *options, const TfAllocator *allocator) {
    TfAllocator a = tf_allocator(allocator); TfParseResult *r; Parser p = {0};
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (!tf_allocator_valid(a) || !input_valid(lex)) return TF_HOST_ERROR;
    p.options = options != NULL ? *options : (TfParseOptions){true,64,100};
    if (p.options.nesting_limit == 0 || p.options.nesting_limit > 128 || p.options.diagnostic_cap > 100) return TF_HOST_ERROR;
    r = a.allocate(a.context,sizeof(*r)); if (r == NULL) return TF_HOST_ERROR;
    *r = (TfParseResult){0}; r->allocator = a; r->ast = tf_ast_create(lex->source,a);
    if (r->ast == NULL || tf_diagnostic_bag_create(&r->diagnostics,p.options.diagnostic_cap,&a) != TF_OK ||
        tf_diagnostic_bag_lexer(r->diagnostics,lex) != TF_OK) goto fail;
    p.lex = lex; p.result = r; p.dummy.span.source = lex->source;
    parse_module(&p);
    if (p.failed) goto fail;
    if ((tf_diagnostic_bag_has_errors(r->diagnostics) ? tf_diagnostic_phase_error(&r->phase,r->diagnostics) :
         tf_diagnostic_phase_valid(&r->phase,r->ast,r->diagnostics)) != TF_OK) goto fail;
    *out = r; return TF_OK;
fail:
    tf_parse_destroy(r); return TF_HOST_ERROR;
}

TfStatus tf_parse_repl_prefix(const TfLexResult *lex,TfReplParseKind kind_requested,bool *incomplete) {
    if (incomplete==NULL) return TF_HOST_ERROR;
    *incomplete=false;
    if (!input_valid(lex)) return TF_HOST_ERROR;
    const TfAllocator a=lex->allocator;
    if (!tf_allocator_valid(a)) return TF_HOST_ERROR;
    TfParseResult r={.allocator=a};
    r.ast=tf_ast_create(lex->source,a);
    if (r.ast==NULL || tf_diagnostic_bag_create(&r.diagnostics,0,&a)!=TF_OK) {
        tf_ast_destroy(r.ast);
        return TF_HOST_ERROR;
    }
    Parser p={.lex=lex,.result=&r,.options={true,64,0}};
    p.dummy.span.source=lex->source;
    switch (kind_requested) {
    case TF_REPL_PARSE_MODULE: parse_module(&p); break;
    case TF_REPL_PARSE_LET:
        if (kind(&p)==TF_LET) (void)statement(&p,false);
        else p.failed=true;
        break;
    case TF_REPL_PARSE_EXPRESSION: (void)expression(&p,0); break;
    default: p.failed=true; break;
    }
    if (!p.failed && kind(&p)!=TF_EOF) p.interior_error=true;
    if (!p.failed) *incomplete=p.saw_error && !p.interior_error;
    tf_diagnostic_bag_destroy(r.diagnostics); tf_ast_destroy(r.ast);
    return p.failed ? TF_HOST_ERROR : TF_OK;
}
