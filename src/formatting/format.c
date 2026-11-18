#include "ternfold/format.h"
#include "../internal.h"
#include <string.h>

static bool numeric(TfTokenKind k) { return k == TF_INT || k == TF_FLOAT; }
static bool touching_minus(const TfToken *a, const TfToken *b) {
    return a->kind == TF_MINUS && numeric(b->kind) && a->span.end == b->span.start;
}
static bool space(TfTokenKind a, TfTokenKind b) {
    /* Keep comments separate from punctuation that can otherwise form a new
     * slash token. Braces, statement ends and line comments are handled above. */
    if (a == TF_COMMENT_BLOCK || b == TF_COMMENT_BLOCK || b == TF_COMMENT_LINE) return true;
    if (b == TF_COMMA || b == TF_SEMICOLON || b == TF_COLON ||
        b == TF_RPAREN || b == TF_RBRACKET || b == TF_DOT || b == TF_DOUBLE_COLON) return false;
    if (a == TF_LPAREN || a == TF_LBRACKET || a == TF_DOT || a == TF_DOUBLE_COLON) return false;
    if ((b == TF_LPAREN || b == TF_LBRACKET) &&
        (a == TF_IDENT || a == TF_FN || a == TF_RPAREN || a == TF_RBRACKET)) return false;
    return true;
}
void tf_format_destroy(TfFormatResult *r) {
    if (r == NULL) return;
    if (r->bytes != NULL) r->allocator.deallocate(r->allocator.context, r->bytes);
    memset(r, 0, sizeof *r);
}
TfStatus tf_format(TfFormatResult *out, const TfSource *source,
                   const TfParseOptions *options, const TfAllocator *allocator) {
    TfAllocator a = tf_allocator(allocator);
    TfLexResult lex = {0}, again = {0};
    TfParseResult *parse = NULL, *reparse = NULL;
    TfSource candidate = {0};
    unsigned char *bytes = NULL;
    size_t cap = 0, used = 0, depth = 0;
    TfStatus status = TF_HOST_ERROR;
    if (out == NULL) return TF_HOST_ERROR;
    memset(out, 0, sizeof *out);
    if (source == NULL || !tf_allocator_valid(a)) return TF_HOST_ERROR;
    if (!source->valid) return TF_SOURCE_ERROR;
    TfStatus scanned = tf_lex(&lex, source, 100, &a);
    if (scanned == TF_HOST_ERROR) goto done;
    if (lex.has_errors) { status = TF_SOURCE_ERROR; goto done; }
    if (tf_parse(&parse, &lex, options, &a) != TF_OK) goto done;
    if (tf_diagnostic_phase_kind(tf_parse_phase(parse)) != TF_DIAG_PHASE_VALID) {
        status = TF_SOURCE_ERROR; goto done;
    }
    for (size_t i = 0; i + 1 < lex.token_count; ++i) {
        const TfToken *t = &lex.tokens[i];
        size_t gap = 0, length = t->span.end - t->span.start;
        bool newline = false;
        if (t->kind == TF_RBRACE && depth != 0) --depth;
        if (i != 0) {
            const TfToken *prev = &lex.tokens[i - 1];
            newline = prev->kind == TF_COMMENT_LINE || prev->kind == TF_SEMICOLON ||
                ((t->kind == TF_RBRACE || prev->kind == TF_LBRACE) &&
                 !(prev->kind == TF_LBRACE && t->kind == TF_RBRACE)) ||
                (prev->kind == TF_RBRACE && depth == 0 && t->kind != TF_COMMENT_LINE &&
                 t->kind != TF_COMMENT_BLOCK && t->kind != TF_ELSE);
            if (newline) gap = 1 + depth * 4;
            else if (!(prev->kind == TF_LBRACE && t->kind == TF_RBRACE) &&
                     !touching_minus(prev, t) && space(prev->kind, t->kind)) gap = 1;
        }
        if (used > SIZE_MAX - gap || length > SIZE_MAX - gap - used ||
            !tf_grow((void **)&bytes, &cap, used + gap + length, 1, a)) goto done;
        if (gap != 0) {
            bytes[used++] = (unsigned char)(newline ? '\n' : ' ');
            if (gap > 1) { memset(bytes + used, ' ', gap - 1); used += gap - 1; }
        }
        memcpy(bytes + used, source->bytes + t->span.start, length);
        used += length;
        if (t->kind == TF_LBRACE && ++depth > 128) goto done;
    }
    if (used > SIZE_MAX - 2 || !tf_grow((void **)&bytes, &cap, used + 2, 1, a)) goto done;
    if (used != 0) bytes[used++] = '\n';
    bytes[used] = 0;
    if (tf_source_init(&candidate, source->name, bytes, used, &a) != TF_OK ||
        tf_lex(&again, &candidate, 100, &a) != TF_OK || again.has_errors ||
        again.token_count != lex.token_count) goto done;
    for (size_t i = 0; i < lex.token_count; ++i) {
        const TfToken *l = &lex.tokens[i], *r = &again.tokens[i];
        size_t n = l->span.end - l->span.start;
        if (l->kind != r->kind || n != r->span.end - r->span.start ||
            memcmp(source->bytes + l->span.start, candidate.bytes + r->span.start, n) != 0 ||
            (i != 0 && touching_minus(&lex.tokens[i - 1], l) !=
                       touching_minus(&again.tokens[i - 1], r))) goto done;
    }
    if (tf_parse(&reparse, &again, options, &a) != TF_OK ||
        tf_diagnostic_phase_kind(tf_parse_phase(reparse)) != TF_DIAG_PHASE_VALID) goto done;
    *out = (TfFormatResult){bytes, used, a};
    bytes = NULL;
    status = TF_OK;
done:
    if (bytes != NULL) a.deallocate(a.context, bytes);
    tf_parse_destroy(reparse);
    tf_lex_destroy(&again);
    tf_source_destroy(&candidate);
    tf_parse_destroy(parse);
    tf_lex_destroy(&lex);
    return status;
}
