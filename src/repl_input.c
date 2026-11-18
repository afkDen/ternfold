#include "repl_input.h"
#include "repl_parse.h"
#include "internal.h"

#include <string.h>

static bool declaration_head(TfTokenKind kind) {
    return kind == TF_FN || kind == TF_RECORD || kind == TF_ENUM || kind == TF_MACHINE;
}

static TfTokenKind expected_close(TfTokenKind kind) {
    switch (kind) {
        case TF_LPAREN: return TF_RPAREN;
        case TF_LBRACKET: return TF_RBRACKET;
        case TF_LBRACE: return TF_RBRACE;
        default: return TF_EOF;
    }
}

static bool wants_operand(TfTokenKind kind) {
    switch (kind) {
        case TF_MINUS: case TF_BANG:
        case TF_PLUS: case TF_STAR: case TF_SLASH: case TF_DIV: case TF_PERCENT:
        case TF_CARET: case TF_PLUS_PLUS: case TF_EQUAL_EQUAL: case TF_BANG_EQUAL:
        case TF_LESS: case TF_LESS_EQUAL: case TF_GREATER: case TF_GREATER_EQUAL:
        case TF_AND_AND: case TF_OR_OR: case TF_DOT: case TF_DOUBLE_COLON:
            return true;
        default: return false;
    }
}

/* E103/E105 conflate unclosed and malformed closed literals. If the last
 * byte could be the closing quote, let the lexer decide whether the diagnosed
 * token absorbs one more ordinary byte. No escape decoding is done here. */
static TfStatus quote_at_eof(const TfToken *token, bool *open) {
    const TfSource *source = token->span.source;
    size_t start = token->span.start, end = token->span.end, length = end - start;
    TfAllocator allocator = source->allocator;
    TfSource probe;
    TfLexResult lex;
    TfStatus status;
    unsigned char *bytes;

    *open = false;
    if (end != source->length) return TF_OK;
    if (length == 1 || source->bytes[end - 1] != source->bytes[start]) {
        *open = true;
        return TF_OK;
    }
    if (length == SIZE_MAX) return TF_HOST_ERROR;
    bytes = allocator.allocate(allocator.context, length + 1);
    if (bytes == NULL) return TF_HOST_ERROR;
    memcpy(bytes, source->bytes + start, length);
    bytes[length] = ' ';
    status = tf_source_init(&probe, source->name, bytes, length + 1, &allocator);
    allocator.deallocate(allocator.context, bytes);
    if (status == TF_OK) {
        status = tf_lex(&lex, &probe, 0, &allocator);
        if (status == TF_OK || status == TF_LEX_ERROR) {
            *open = lex.tokens[0].span.end > length;
            status = TF_OK;
        }
        tf_lex_destroy(&lex);
    }
    tf_source_destroy(&probe);
    return status;
}

TfReplUnitKind tf_repl_classify(const TfSource *source) {
    TfLexResult lex;
    TfReplUnitKind result = TF_REPL_UNIT_EXPRESSION;
    TfTokenKind head[3] = {TF_EOF, TF_EOF, TF_EOF}, last = TF_EOF;
    void *stack = NULL;
    size_t capacity = 0, depth = 0, head_count = 0;
    const TfToken *quote = NULL;
    bool hard_error = false, open_comment = false, semicolon = false, body = false;

    if (source == NULL) return TF_REPL_UNIT_HOST_ERROR;
    if (!source->valid)
        return source->error.code == TF_E_NONE ? TF_REPL_UNIT_HOST_ERROR : result;
    if (tf_lex(&lex, source, 0, &source->allocator) != TF_HOST_ERROR) {
        for (size_t i = 0; i < lex.token_count; ++i) {
            const TfToken *token = &lex.tokens[i];
            TfTokenKind kind = token->kind, close = expected_close(kind);
            if (kind == TF_EOF) break;
            if (kind == TF_COMMENT_LINE || kind == TF_COMMENT_BLOCK) continue;
            if (token->error == TF_E104) {
                open_comment = true;
                continue;
            }
            if (head_count < sizeof head / sizeof head[0]) head[head_count++] = kind;
            last = kind;
            if (hard_error) continue;
            if (kind == TF_INVALID) {
                if ((token->error == TF_E103 || token->error == TF_E105) &&
                    token->span.end == source->length)
                    quote = token;
                else
                    hard_error = true;
            } else if (close != TF_EOF) {
                /* Depth cannot exceed the number of lexer tokens visited. */
                if (!tf_grow(&stack, &capacity, depth + 1, sizeof(TfTokenKind),
                             lex.allocator)) {
                    result = TF_REPL_UNIT_HOST_ERROR;
                    goto done;
                }
                ((TfTokenKind *)stack)[depth++] = close;
            } else if (kind == TF_RPAREN || kind == TF_RBRACKET || kind == TF_RBRACE) {
                if (depth == 0 || ((TfTokenKind *)stack)[depth - 1] != kind)
                    hard_error = true;
                else if (--depth == 0 && kind == TF_RBRACE)
                    body = true;
            } else if (kind == TF_BLOCK_CLOSE) {
                hard_error = true;
            } else if (kind == TF_SEMICOLON && depth == 0) {
                semicolon = true;
            }
        }
    } else {
        result = TF_REPL_UNIT_HOST_ERROR;
        goto done;
    }

    if (head[0] == TF_EOF) result = TF_REPL_UNIT_EMPTY;
    if (head[0] == TF_LET) result = TF_REPL_UNIT_BINDING;
    if (head[0] == TF_IMPORT || head[0] == TF_PUB ||
        (head[0] != TF_FN && declaration_head(head[0])) ||
        (head[0] == TF_FN && head[1] == TF_IDENT))
        result = TF_REPL_UNIT_DECLARATION;

    if (!hard_error && quote != NULL) {
        bool open;
        if (quote_at_eof(quote, &open) != TF_OK)
            result = TF_REPL_UNIT_HOST_ERROR;
        else if (open)
            result = TF_REPL_UNIT_INCOMPLETE;
        goto done; /* Closed malformed quotes go straight to diagnostics. */
    }
    if (!hard_error) {
        size_t decl = head[0] == TF_PUB ? 1u : 0u;
        bool needs_body = declaration_head(head[decl]) &&
            (head[decl + 1] == TF_IDENT || head[decl + 1] == TF_EOF ||
             (decl == 0 && head[0] == TF_FN && head[1] == TF_LPAREN));
        if (open_comment || depth != 0 ||
            (!semicolon && (head[0] == TF_LET || head[0] == TF_IMPORT ||
                 (head[0] == TF_PUB && head[1] == TF_EOF) || (needs_body && !body))) ||
            (!semicolon && wants_operand(last) &&
                 (head_count > 1 || last == TF_MINUS || last == TF_BANG)))
            result = TF_REPL_UNIT_INCOMPLETE;
        else if (result!=TF_REPL_UNIT_EMPTY) {
            const TfReplParseKind syntax=result==TF_REPL_UNIT_DECLARATION ? TF_REPL_PARSE_MODULE :
                result==TF_REPL_UNIT_BINDING ? TF_REPL_PARSE_LET : TF_REPL_PARSE_EXPRESSION;
            bool incomplete=false;
            if (tf_parse_repl_prefix(&lex,syntax,&incomplete)!=TF_OK) result=TF_REPL_UNIT_HOST_ERROR;
            else if (incomplete) result=TF_REPL_UNIT_INCOMPLETE;
        }
    }

done:
    if (stack != NULL) lex.allocator.deallocate(lex.allocator.context, stack);
    tf_lex_destroy(&lex);
    return result;
}
