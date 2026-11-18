#include "ternfold/editor.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../internal.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define MESSAGE_LIMIT 1048576u
#define RESPONSE_LIMIT 2097152u
#define TOKEN_LIMIT 1024u
typedef struct {
    char kind; /* o object, a array, s string, n number, t/f/bool, z null */
    const char *text;
    size_t length, next, count;
} Json;
typedef struct {
    const unsigned char *bytes;
    size_t length, at, used, strings;
    char *decoded;
    Json tokens[TOKEN_LIMIT];
} Reader;
typedef struct { char *data; size_t length; bool failed; } Writer;
static void *alloc_default(void *c, size_t n) { (void)c; return malloc(n); }
static void free_default(void *c, void *p) { (void)c; free(p); }
static void put(Writer *w, const char *s, size_t n) {
    if (w->failed || n > RESPONSE_LIMIT - w->length) { w->failed = true; return; }
    memcpy(w->data + w->length, s, n);
    w->length += n;
}
static void literal(Writer *w, const char *s) { put(w, s, strlen(s)); }
static void number(Writer *w, uint64_t value) {
    char digits[32];
    size_t n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value != 0);
    while (n != 0) put(w, &digits[--n], 1);
}
static void signed_number(Writer *w,int64_t value) {
    if (value<0) { literal(w,"-"); number(w,(uint64_t)(-(value+1))+1u); }
    else number(w,(uint64_t)value);
}
static void quote(Writer *w, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    literal(w, "\"");
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { literal(w, "\\"); put(w, s + i, 1); }
        else if (c < 32) {
            char escaped[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            put(w, escaped, sizeof escaped);
        } else put(w, s + i, 1);
    }
    literal(w, "\"");
}
static void whitespace(Reader *r) {
    while (r->at < r->length && (r->bytes[r->at] == ' ' || r->bytes[r->at] == '\t' ||
           r->bytes[r->at] == '\n' || r->bytes[r->at] == '\r')) ++r->at;
}
static bool take(Reader *r, unsigned char c) {
    if (r->at >= r->length || r->bytes[r->at] != c) return false;
    ++r->at;
    return true;
}
static bool hex4(Reader *r, uint32_t *out) {
    uint32_t n = 0;
    for (size_t i = 0; i < 4; ++i) {
        unsigned char c;
        if (r->at >= r->length) return false;
        c = r->bytes[r->at++];
        n <<= 4;
        if (c >= '0' && c <= '9') n += (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') n += (uint32_t)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') n += (uint32_t)(c - 'A') + 10;
        else return false;
    }
    *out = n;
    return true;
}
static bool string(Reader *r, Json *t) {
    size_t start = r->strings;
    if (!take(r, '"')) return false;
    while (r->at < r->length) {
        unsigned char c = r->bytes[r->at++];
        if (c == '"') {
            t->kind = 's'; t->text = r->decoded + start; t->length = r->strings - start;
            /* No NUL terminator: all strings are length-aware. */
            return true;
        }
        if (c < 32) return false;
        if (c != '\\') { r->decoded[r->strings++] = (char)c; continue; }
        if (r->at == r->length) return false;
        c = r->bytes[r->at++];
        switch (c) {
            case '"': case '\\': case '/': r->decoded[r->strings++] = (char)c; break;
            case 'b': r->decoded[r->strings++] = '\b'; break;
            case 'f': r->decoded[r->strings++] = '\f'; break;
            case 'n': r->decoded[r->strings++] = '\n'; break;
            case 'r': r->decoded[r->strings++] = '\r'; break;
            case 't': r->decoded[r->strings++] = '\t'; break;
            case 'u': {
                uint32_t u, low;
                if (!hex4(r, &u)) return false;
                if (u >= 0xd800 && u <= 0xdbff) {
                    if (!take(r, '\\') || !take(r, 'u') || !hex4(r, &low) || low < 0xdc00 || low > 0xdfff) return false;
                    u = 0x10000 + (u - 0xd800) * 1024 + low - 0xdc00;
                } else if (u >= 0xdc00 && u <= 0xdfff) return false;
                if (u < 0x80) r->decoded[r->strings++] = (char)u;
                else {
                    if (u < 0x800) r->decoded[r->strings++] = (char)(0xc0u | (u >> 6));
                    else {
                        if (u < 0x10000) r->decoded[r->strings++] = (char)(0xe0u | (u >> 12));
                        else {
                            r->decoded[r->strings++] = (char)(0xf0u | (u >> 18));
                            r->decoded[r->strings++] = (char)(0x80u | ((u >> 12) & 63));
                        }
                        r->decoded[r->strings++] = (char)(0x80u | ((u >> 6) & 63));
                    }
                    r->decoded[r->strings++] = (char)(0x80u | (u & 63));
                }
                break;
            }
            default: return false;
        }
    }
    return false;
}
static bool value(Reader *r, size_t depth) {
    size_t id, start;
    unsigned char c;
    if (depth > 32 || r->used == TOKEN_LIMIT) return false;
    whitespace(r);
    if (r->at == r->length) return false;
    id = r->used++;
    c = r->bytes[r->at];
    if (c == '"') {
        if (!string(r, &r->tokens[id])) return false;
    } else if (c == '{' || c == '[') {
        bool object = c == '{';
        unsigned char close = object ? '}' : ']';
        r->tokens[id].kind = object ? 'o' : 'a';
        ++r->at;
        whitespace(r);
        if (!take(r, close)) {
            for (;;) {
                if (object) {
                    size_t key = r->used;
                    if (!value(r, depth + 1) || r->tokens[key].kind != 's') return false;
                    /* Duplicate keys are rejected, including escaped aliases. */
                    for (size_t k = id + 1; k < key; k = r->tokens[k + 1].next) {
                        if (r->tokens[k].length == r->tokens[key].length &&
                            memcmp(r->tokens[k].text, r->tokens[key].text, r->tokens[k].length) == 0) return false;
                    }
                    whitespace(r);
                    if (!take(r, ':')) return false;
                }
                if (!value(r, depth + 1)) return false;
                ++r->tokens[id].count;
                whitespace(r);
                if (take(r, close)) break;
                if (!take(r, ',')) return false;
            }
        }
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        start = r->at;
        (void)take(r, '-');
        if (!take(r, '0')) {
            if (r->at == r->length || r->bytes[r->at] < '1' || r->bytes[r->at] > '9') return false;
            do { ++r->at; } while (r->at < r->length && r->bytes[r->at] >= '0' && r->bytes[r->at] <= '9');
        }
        if (take(r, '.')) {
            size_t digits = r->at;
            while (r->at < r->length && r->bytes[r->at] >= '0' && r->bytes[r->at] <= '9') ++r->at;
            if (r->at == digits) return false;
        }
        if (take(r, 'e') || take(r, 'E')) {
            size_t digits;
            if (!take(r, '+')) (void)take(r, '-');
            digits = r->at;
            while (r->at < r->length && r->bytes[r->at] >= '0' && r->bytes[r->at] <= '9') ++r->at;
            if (r->at == digits) return false;
        }
        r->tokens[id] = (Json){'n', (const char *)r->bytes + start, r->at - start, 0, 0};
    } else {
        const char *word = c == 't' ? "true" : c == 'f' ? "false" : "null";
        size_t n = strlen(word);
        if (n > r->length - r->at || memcmp(r->bytes + r->at, word, n) != 0) return false;
        r->at += n;
        r->tokens[id].kind = c == 'n' ? 'z' : (char)c;
    }
    r->tokens[id].next = r->used;
    return true;
}
static bool equals(const Json *j, const char *s) {
    return j != NULL && j->kind == 's' && j->length == strlen(s) && memcmp(j->text, s, j->length) == 0;
}
static const Json *field(const Reader *r, const Json *o, const char *key) {
    if (o == NULL || o->kind != 'o') return NULL;
    size_t base = (size_t)(o - r->tokens);
    for (size_t i = base + 1; i < o->next; i = r->tokens[i + 1].next)
        if (equals(&r->tokens[i], key)) return &r->tokens[i + 1];
    return NULL;
}
static bool integer(const Json *j, int64_t *out) {
    uint64_t n = 0;
    if (j == NULL || j->kind != 'n' || j->length == 0) return false;
    const bool negative=j->text[0]=='-';
    const uint64_t limit=(uint64_t)INT64_MAX+(negative ? 1u : 0u);
    if (negative && j->length==1) return false;
    for (size_t i = negative ? 1u : 0u; i < j->length; ++i) {
        unsigned char c = (unsigned char)j->text[i];
        if (c < '0' || c > '9' || n > (limit - (uint64_t)(c - '0')) / 10) return false;
        n = n * 10 + (uint64_t)(c - '0');
    }
    *out = negative ? (n==(uint64_t)INT64_MAX+1u ? INT64_MIN : -(int64_t)n) : (int64_t)n;
    return true;
}
static bool valid_id(const Json *id) {
    int64_t unused;
    if (id == NULL || id->length > 1024) return false;
    if (id->kind == 's') return true;
    if (id->kind != 'n') return false;
    return integer(id, &unused);
}
static void id_write(Writer *w, const Json *id) {
    if (id == NULL) literal(w, "null");
    else if (id->kind == 's') quote(w, id->text, id->length);
    else put(w, id->text, id->length);
}
static void begin(Writer *w, const Json *id) {
    w->length = 0; w->failed = false;
    literal(w, "{\"jsonrpc\":\"2.0\",\"id\":"); id_write(w, id);
}
static bool send(FILE *out, Writer *w) {
    return !w->failed && fprintf(out, "Content-Length: %zu\r\n\r\n", w->length) >= 0 &&
           fwrite(w->data, 1, w->length, out) == w->length && fflush(out) == 0;
}
static bool error(FILE *out, Writer *w, const Json *id, const char *code, const char *message) {
    begin(w, id); literal(w, ",\"error\":{\"code\":"); literal(w, code);
    literal(w, ",\"message\":"); quote(w, message, strlen(message)); literal(w, "}}");
    return send(out, w);
}
static void position_write(Writer *w, TfEditorPosition p) {
    literal(w, "{\"line\":"); number(w, p.line); literal(w, ",\"character\":"); number(w, p.character); literal(w, "}");
}
static void range_write(Writer *w, TfEditorRange r) {
    literal(w, "{\"start\":"); position_write(w, r.start); literal(w, ",\"end\":"); position_write(w, r.end); literal(w, "}");
}
static const TfSource *primary_source(const TfDiagnosticRecord *d,const TfSource *fallback) {
    for (size_t i=0;i<d->label_count;++i)
        if (d->labels[i].primary) return d->labels[i].span.source;
    return d->label_count ? d->labels[0].span.source : fallback;
}
static void source_uri(Writer *w,const TfSource *source) {
    const unsigned char *path=(const unsigned char *)source->name;
#ifdef _WIN32
    wchar_t wide[4097]; char utf8[16385];
    if (!MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,source->name,-1,wide,4097) ||
        !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,utf8,16385,NULL,NULL)) {
        w->failed=true; return;
    }
    path=(const unsigned char *)utf8;
#endif
    literal(w,"\"file://");
    if (*path!='/') literal(w,"/");
    static const char hex[]="0123456789ABCDEF";
    for (;*path;++path) {
        unsigned char c=*path;
#ifdef _WIN32
        if (c=='\\') c='/';
#endif
        if ((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
            c=='/' || c==':' || c=='-' || c=='.' || c=='_' || c=='~') put(w,(const char *)&c,1);
        else { const char escaped[]={'%',hex[c>>4],hex[c&15u]}; put(w,escaped,3); }
    }
    literal(w,"\"");
}
static bool diagnostics_target(FILE *out,Writer *w,const char *uri,size_t uri_length,
    const TfDiagnosticBag *bag,const TfSource *root,const TfSource *target,
    bool has_version,int64_t version,bool clear) {
    w->length = 0; w->failed = false;
    literal(w, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
    if (target==root) quote(w,uri,uri_length);
    else source_uri(w,target);
    if (has_version) { literal(w, ",\"version\":"); signed_number(w,version); }
    literal(w, ",\"diagnostics\":[");
    size_t emitted=0;
    for (size_t i = 0; !clear && i < tf_diagnostic_bag_count(bag); ++i) {
        const TfDiagnosticRecord *d = tf_diagnostic_bag_get(bag, i);
        if (primary_source(d,root)!=target) continue;
        TfEditorRange range = {{0, 0}, {0, 0}};
        for (size_t j = 0; j < d->label_count; ++j) {
            TfSpan s = d->labels[j].span;
            if (d->labels[j].primary && s.source == target && target->valid) {
                if (!tf_editor_position(target, s.start, &range.start) ||
                    !tf_editor_position(target, s.end, &range.end)) range = (TfEditorRange){{0, 0}, {0, 0}};
                break;
            }
        }
        if (emitted++) literal(w, ",");
        literal(w, "{\"range\":"); range_write(w, range);
        literal(w, ",\"severity\":"); number(w, d->severity == TF_DIAG_ERROR ? 1u : d->severity == TF_DIAG_WARNING ? 2u : 3u);
        literal(w, ",\"code\":"); quote(w, d->code, 4);
        literal(w, ",\"source\":\"ternfold\",\"message\":"); quote(w, d->message.data, d->message.length); literal(w, "}");
    }
    literal(w, "]}}");
    return send(out, w);
}
static bool dependencies(FILE *out,Writer *w,const char *uri,size_t length,
                         const TfEditorSnapshot *snapshot,int64_t version,bool clear) {
    const TfDiagnosticBag *bag=tf_editor_diagnostics(snapshot,version);
    const TfSource *root=tf_editor_source(snapshot);
    for (size_t i=0;i<tf_diagnostic_bag_count(bag);++i) {
        const TfSource *source=primary_source(tf_diagnostic_bag_get(bag,i),root);
        if (source==root || !source) continue;
        bool previous=false;
        for (size_t j=0;j<i;++j)
            if (primary_source(tf_diagnostic_bag_get(bag,j),root)==source) { previous=true; break; }
        if (!previous && !diagnostics_target(out,w,uri,length,bag,root,source,false,0,clear)) return false;
    }
    return true;
}
static bool diagnostics(FILE *out,Writer *w,const char *uri,size_t length,
                         const TfEditorSnapshot *snapshot,int64_t version) {
    const TfDiagnosticBag *bag=tf_editor_diagnostics(snapshot,version);
    const TfSource *root=tf_editor_source(snapshot);
    if (snapshot && !bag) return true;
    return diagnostics_target(out,w,uri,length,bag,root,root,snapshot!=NULL,version,false) &&
           dependencies(out,w,uri,length,snapshot,version,false);
}
static void type_write(Writer *w, const TfTypeStore *store, TfConcreteTypeId id, size_t depth,size_t *work) {
    static const char *const names[] = {"Int", "Float", "Bool", "Text", "Char", "Unit"};
    if (!*work) { literal(w,"..."); return; }
    --*work;
    if (depth>32) { literal(w,"..."); return; }
    const TfConcreteType *t = tf_type_store_get(store, id);
    if (t == NULL) { literal(w,"<unavailable>"); return; }
    if (t->kind <= TF_CONCRETE_UNIT) { literal(w, names[(size_t)t->kind]); return; }
    switch (t->kind) {
        case TF_CONCRETE_LIST: case TF_CONCRETE_OPTION:
            literal(w, t->kind == TF_CONCRETE_LIST ? "List[" : "Option[");
            type_write(w, store, t->as.element, depth + 1,work); literal(w, "]"); break;
        case TF_CONCRETE_DECISION:
            literal(w, "Decision["); type_write(w, store, t->as.decision.machine, depth + 1,work);
            literal(w, ", "); type_write(w, store, t->as.decision.output, depth + 1,work);
            literal(w, ", "); type_write(w, store, t->as.decision.rejection, depth + 1,work); literal(w, "]"); break;
        case TF_CONCRETE_FUNCTION:
            literal(w, "fn(");
            for (size_t i = 0; i < t->as.function.arity; ++i) {
                if (i != 0) literal(w, ", ");
                if (!*work) { literal(w,"..."); break; }
                type_write(w, store, t->as.function.parameters[i], depth + 1,work);
            }
            literal(w, ") -> "); type_write(w, store, t->as.function.result, depth + 1,work); break;
        case TF_CONCRETE_RECORD: case TF_CONCRETE_ENUM: case TF_CONCRETE_MACHINE: {
            const TfResolvedType *nominal = tf_resolution_type(t->as.nominal.owner, t->as.nominal);
            if (nominal == NULL) { w->failed = true; return; }
            put(w, (const char *)nominal->name.data, nominal->name.length>128 ? 128 : nominal->name.length);
            if (nominal->name.length>128) literal(w,"...");
            break;
        }
        default: w->failed = true; break;
    }
}
/* Strict CRLF headers; length bounded before allocation. Unknown headers are
 * ignored (e.g. Content-Type), duplicate Content-Length is a framing failure. */
static int frame(FILE *in, size_t *length) {
    char line[4097];
    size_t total = 0, used = 0;
    bool have = false;
    for (;;) {
        int c = fgetc(in);
        if (c == EOF) return total == 0 && !ferror(in) ? 0 : -1;
        if (++total > 4096 || c == 0 || c > 127) return -1;
        line[used++] = (char)c;
        if (c != '\n') continue;
        if (used < 2 || line[used - 2] != '\r') return -1;
        used -= 2;
        line[used] = 0;
        if (used == 0) return have ? 1 : -1;
        char *colon = strchr(line, ':');
        if (colon == NULL) return -1;
        bool is_length = (size_t)(colon - line) == 14;
        static const char name[] = "content-length";
        for (size_t i = 0; is_length && i < 14; ++i) {
            char x = line[i];
            if (x >= 'A' && x <= 'Z') x = (char)(x + ('a' - 'A'));
            if (x != name[i]) is_length = false;
        }
        if (is_length) {
            size_t n = 0;
            char *p = colon + 1;
            if (have) return -1;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p < '0' || *p > '9') return -1;
            while (*p >= '0' && *p <= '9') {
                size_t digit = (size_t)(*p++ - '0');
                if (n > (MESSAGE_LIMIT - digit) / 10) return -1;
                n = n * 10 + digit;
            }
            while (*p == ' ' || *p == '\t') ++p;
            if (*p != 0 || n == 0 || n > MESSAGE_LIMIT) return -1;
            *length = n; have = true;
        }
        used = 0;
    }
}
TfStatus tf_editor_serve(FILE *input, FILE *output, const char *root, const TfAllocator *allocator) {
    TfAllocator a = {NULL, alloc_default, free_default};
    TfEditorSnapshot *snapshot = NULL;
    unsigned char *body = NULL;
    char *decoded = NULL, *uri = NULL, *response = NULL;
    size_t uri_length = 0;
    int64_t version = -1;
    bool initialized = false, shutdown = false;
    struct { char bytes[1024]; size_t length; char kind; } cancelled[32];
    size_t cancellation_count=0;
    TfStatus status = TF_HOST_ERROR;
    if (allocator != NULL) a = *allocator;
    if (input == NULL || output == NULL || input == output || a.allocate == NULL || a.deallocate == NULL) return TF_HOST_ERROR;
    body = a.allocate(a.context, MESSAGE_LIMIT);
    decoded = a.allocate(a.context, MESSAGE_LIMIT);
    response = a.allocate(a.context, RESPONSE_LIMIT);
    if (body == NULL || decoded == NULL || response == NULL) goto done;
    Writer w = {response, 0, false};
    for (;;) {
        size_t length = 0;
        int f = frame(input, &length);
        if (f == 0) { status = TF_OK; break; }
        if (f < 0 || fread(body, 1, length, input) != length) break;
        Reader r = {0};
        bool valid=true;
        for (size_t at=0,next;at<length;at=next) {
            uint32_t scalar;
            if (!tf_decode(body,length,at,&scalar,&next)) { valid=false; break; }
        }
        r.bytes = body; r.length = length; r.decoded = decoded;
        if (!valid || !value(&r, 0)) {
            if (!error(output, &w, NULL, "-32700", "Parse error")) break;
            continue;
        }
        whitespace(&r);
        if (r.at != r.length) {
            if (!error(output, &w, NULL, "-32700", "Parse error")) break;
            continue;
        }
        const Json *message = &r.tokens[0];
        const Json *id = field(&r, message, "id"), *method = field(&r, message, "method");
        const Json *params = field(&r, message, "params");
        if (message->kind != 'o' || !equals(field(&r, message, "jsonrpc"), "2.0") ||
            method == NULL || method->kind != 's' || (id != NULL && !valid_id(id))) {
            if (!error(output, &w, NULL, "-32600", "Invalid request")) break;
            continue;
        }
        if (equals(method, "exit")) { status = shutdown ? TF_OK : TF_HOST_ERROR; break; }
        if (equals(method, "initialize") && id != NULL && !initialized) {
            initialized = true;
            begin(&w, id);
            literal(&w, ",\"result\":{\"capabilities\":{\"positionEncoding\":\"utf-16\",\"textDocumentSync\":1,\"hoverProvider\":true}}}");
            if (!send(output, &w)) break;
            continue;
        }
        if (!initialized || shutdown) {
            if (id != NULL && !error(output, &w, id, !initialized ? "-32002" : "-32600", "Server unavailable")) break;
            continue;
        }
        if (equals(method, "shutdown") && id != NULL) {
            shutdown = true; begin(&w, id); literal(&w, ",\"result\":null}");
            if (!send(output, &w)) break;
            continue;
        }
        if (equals(method, "$/cancelRequest")) {
            const Json *target = field(&r, params, "id");
            if (valid_id(target)) {
                size_t found=0;
                while (found<cancellation_count &&
                       (cancelled[found].kind!=target->kind || cancelled[found].length!=target->length ||
                        memcmp(cancelled[found].bytes,target->text,target->length)!=0)) ++found;
                if (found==cancellation_count) {
                    if (cancellation_count==32) break;
                    ++cancellation_count;
                    memcpy(cancelled[found].bytes,target->text,target->length);
                    cancelled[found].length=target->length; cancelled[found].kind=target->kind;
                }
            }
            continue;
        }
        const Json *doc = field(&r, params, "textDocument");
        const Json *doc_uri = field(&r, doc, "uri");
        bool same = doc_uri != NULL && doc_uri->kind == 's' && uri != NULL &&
                    doc_uri->length == uri_length && memcmp(uri, doc_uri->text, uri_length) == 0;
        if (equals(method, "textDocument/didOpen") || equals(method, "textDocument/didChange")) {
            int64_t new_version;
            bool opening = equals(method, "textDocument/didOpen");
            const Json *text = field(&r, doc, "text");
            if (!opening) {
                const Json *changes = field(&r, params, "contentChanges");
                if (!same || changes == NULL || changes->kind != 'a' || changes->count==0) continue;
                bool accepted=true;
                for (size_t index=(size_t)(changes-r.tokens)+1;index<changes->next;index=r.tokens[index].next) {
                    const Json *change=&r.tokens[index];
                    const Json *next=field(&r,change,"text");
                    if (field(&r,change,"range") || field(&r,change,"rangeLength") || !next || next->kind!='s') {
                        accepted=false; break;
                    }
                    text=next;
                }
                if (!accepted) continue;
            }
            if ((opening && uri != NULL) || doc_uri == NULL || doc_uri->kind != 's' ||
                doc_uri->length == 0 || doc_uri->length > 4096 || memchr(doc_uri->text, 0, doc_uri->length) != NULL ||
                text == NULL || text->kind != 's' || !integer(field(&r, doc, "version"), &new_version) ||
                (!opening && new_version <= version)) continue;
            char *new_uri = a.allocate(a.context, doc_uri->length + 1);
            TfEditorSnapshot *next = NULL;
            if (new_uri == NULL) break;
            memcpy(new_uri, doc_uri->text, doc_uri->length); new_uri[doc_uri->length] = 0;
            if (tf_editor_snapshot_create(&next, new_uri, text->text, text->length, new_version, root, &a) != TF_OK) {
                a.deallocate(a.context, new_uri); break;
            }
            if (!dependencies(output,&w,uri,uri_length,snapshot,version,true)) {
                tf_editor_snapshot_destroy(next); a.deallocate(a.context,new_uri); break;
            }
            tf_editor_snapshot_destroy(snapshot);
            if (uri != NULL) a.deallocate(a.context, uri);
            snapshot = next; uri = new_uri; uri_length = doc_uri->length; version = new_version;
            if (!diagnostics(output, &w, uri, uri_length, snapshot, version)) break;
            continue;
        }
        if (equals(method, "textDocument/didClose")) {
            if (same) {
                if (!dependencies(output,&w,uri,uri_length,snapshot,version,true)) break;
                if (!diagnostics(output, &w, uri, uri_length, NULL, 0)) break;
                tf_editor_snapshot_destroy(snapshot); snapshot = NULL;
                a.deallocate(a.context, uri); uri = NULL; uri_length = 0; version = -1;
            }
            continue;
        }
        if (equals(method, "textDocument/hover") && id != NULL) {
            size_t cancelled_index=0;
            while (cancelled_index<cancellation_count &&
                   (cancelled[cancelled_index].kind!=id->kind || cancelled[cancelled_index].length!=id->length ||
                    memcmp(cancelled[cancelled_index].bytes,id->text,id->length)!=0)) ++cancelled_index;
            if (cancelled_index<cancellation_count) {
                --cancellation_count;
                if (cancelled_index!=cancellation_count) cancelled[cancelled_index]=cancelled[cancellation_count];
                if (!error(output, &w, id, "-32800", "Request cancelled")) break;
                continue;
            }
            int64_t line, character;
            const Json *p = field(&r, params, "position");
            if (!same || !integer(field(&r, p, "line"), &line) || !integer(field(&r, p, "character"), &character) ||
                line<0 || character<0 || (uint64_t)line > SIZE_MAX || (uint64_t)character > SIZE_MAX) {
                if (!error(output, &w, id, "-32602", "Invalid hover parameters")) break;
                continue;
            }
            TfEditorRange range;
            TfConcreteTypeId type = tf_editor_hover(snapshot, version, (TfEditorPosition){(size_t)line, (size_t)character}, &range);
            begin(&w, id); literal(&w, ",\"result\":");
            if (type.owner == NULL) literal(&w, "null}");
            else {
                /* Type names are ASCII identifiers/punctuation from compiler
                 * records, hence safe inside a JSON string. */
                literal(&w, "{\"contents\":{\"kind\":\"plaintext\",\"value\":\"");
                size_t type_work=128;
                type_write(&w, tf_editor_types(snapshot), type, 0,&type_work);
                literal(&w, "\"},\"range\":"); range_write(&w, range); literal(&w, "}}");
            }
            if (!send(output, &w)) break;
            continue;
        }
        if (id != NULL && !error(output, &w, id, "-32601", "Method not found")) break;
    }
done:
    tf_editor_snapshot_destroy(snapshot);
    if (uri != NULL) a.deallocate(a.context, uri);
    if (body != NULL) a.deallocate(a.context, body);
    if (decoded != NULL) a.deallocate(a.context, decoded);
    if (response != NULL) a.deallocate(a.context, response);
    return status;
}
