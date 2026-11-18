#include "ternfold/token_dump.h"
#include "internal.h"

static bool put(FILE *f, const char *s) { return fputs(s,f) != EOF; }
static bool quoted(FILE *f, const unsigned char *bytes, size_t length) {
    size_t at = 0;
    if (fputc('"',f) == EOF) return false;
    while (at < length) {
        unsigned char c = bytes[at];
        size_t end;
        uint32_t scalar;
        if (c == '"' || c == '\\') {
            if (fputc('\\',f) == EOF || fputc(c,f) == EOF) return false;
            ++at;
        } else if (c < 0x20 || c == 0x7f) {
            if (fprintf(f,"\\u%04x",(unsigned)c) < 0) return false;
            ++at;
        } else {
            if (!tf_decode(bytes,length,at,&scalar,&end) || fwrite(bytes+at,1,end-at,f) != end-at) return false;
            at = end;
        }
    }
    return fputc('"',f) != EOF;
}
static bool has_payload(TfTokenKind k) {
    return k == TF_IDENT || k == TF_INT || k == TF_FLOAT || k == TF_TEXT || k == TF_CHAR;
}
static bool payload(FILE *f, const TfToken *t) {
    if (has_payload(t->kind)) return quoted(f,t->value,t->value_length);
    return put(f,t->kind == TF_TRUE ? "true" : t->kind == TF_FALSE ? "false" : "null");
}
static bool location(FILE *f, TfLocation l) {
    return fprintf(f,"{\"scalar\":%zu,\"line\":%zu,\"column\":%zu,\"display_column\":%zu}",
                   l.scalar,l.line,l.column,l.display_column) >= 0;
}
static bool error(FILE *f, const TfToken *t) {
    if (t->error == TF_E_NONE) return put(f,"null");
    return fprintf(f,"\"%s\"",tf_error_code(t->error)) >= 0;
}
static bool valid_result(const TfSource *source, const TfLexResult *result) {
    size_t i, previous = 0;
    if (!source->valid) return result == NULL;
    if (result == NULL || result->source != source || result->tokens == NULL || result->token_count == 0 ||
        result->token_count > SIZE_MAX/sizeof(*result->tokens)) return false;
    for (i = 0; i < result->token_count; ++i) {
        const TfToken *t = &result->tokens[i]; TfSpan checked;
        if (t->span.source != source || t->kind < TF_IDENT || t->kind > TF_COMMENT_BLOCK ||
            !tf_source_span(source,t->span.start,t->span.end,&checked) || t->span.start < previous ||
            (t->value == NULL && t->value_length != 0) ||
            (t->error != TF_E_NONE && (t->error < TF_E101 || t->error > TF_E105))) return false;
        if (t->kind == TF_EOF) {
            if (i != result->token_count-1 || t->span.start != source->length || t->span.end != source->length) return false;
        } else if (i == result->token_count-1 || t->span.start == t->span.end) return false;
        previous = t->span.end;
    }
    return true;
}
TfStatus tf_token_dump_write(FILE *stream, const TfSource *source,
                              const TfLexResult *result, TfTokenDumpFormat format) {
    size_t i;
    bool json = format == TF_TOKEN_DUMP_JSON;
    bool errors;
    if (stream == NULL || source == NULL || source->bytes == NULL || source->name == NULL ||
        (format != TF_TOKEN_DUMP_TEXT && format != TF_TOKEN_DUMP_JSON) || ferror(stream) ||
        !valid_result(source,result)) return TF_HOST_ERROR;
    errors = !source->valid || result->has_errors;
    if (json) {
        if (fprintf(stream,"{\"schema_version\":1,\"source_valid\":%s,\"has_errors\":%s,\"tokens\":[",
                    source->valid ? "true" : "false",errors ? "true" : "false") < 0) return TF_HOST_ERROR;
    } else if (fprintf(stream,"source_valid=%s has_errors=%s\n",source->valid ? "true" : "false",
                       errors ? "true" : "false") < 0) return TF_HOST_ERROR;
    for (i = 0; source->valid && i < result->token_count; ++i) {
        const TfToken *t = &result->tokens[i];
        TfLocation start, end;
        size_t bytes = has_payload(t->kind) ? t->value_length : 0;
        if (!tf_source_location(source,t->span.start,&start) || !tf_source_location(source,t->span.end,&end)) return TF_HOST_ERROR;
        if (json) {
            if ((i != 0 && !put(stream,",")) ||
                fprintf(stream,"{\"kind\":\"%s\",\"reserved\":%s,\"lexeme\":",tf_token_name(t->kind),t->reserved ? "true" : "false") < 0 ||
                !quoted(stream,source->bytes+t->span.start,t->span.end-t->span.start) || !put(stream,",\"payload\":") ||
                !payload(stream,t) || fprintf(stream,",\"payload_bytes\":%zu,\"start_byte\":%zu,\"end_byte\":%zu,\"start\":",
                    bytes,t->span.start,t->span.end) < 0 || !location(stream,start) || !put(stream,",\"end\":") ||
                !location(stream,end) || !put(stream,",\"error\":") || !error(stream,t) || !put(stream,"}")) return TF_HOST_ERROR;
        } else {
            if (fprintf(stream,"%s reserved=%s bytes=[%zu,%zu) scalar=%zu:%zu..%zu:%zu display=%zu..%zu lexeme=",
                    tf_token_name(t->kind),t->reserved ? "true" : "false",t->span.start,t->span.end,
                    start.line,start.column,end.line,end.column,start.display_column,end.display_column) < 0 ||
                !quoted(stream,source->bytes+t->span.start,t->span.end-t->span.start) || !put(stream," payload=") ||
                !payload(stream,t) || fprintf(stream," payload_bytes=%zu error=",bytes) < 0 || !error(stream,t) ||
                !put(stream,"\n")) return TF_HOST_ERROR;
        }
    }
    if (json && !put(stream,"]}\n")) return TF_HOST_ERROR;
    return ferror(stream) ? TF_HOST_ERROR : TF_OK;
}
