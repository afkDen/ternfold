#include "ternfold/parser.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

enum { INPUT_CAP = 2048, LANES = 6, DEFAULT_CASES = 768, MAX_CASES = 65536 };
#define DEFAULT_SEED UINT32_C(0x4d313446)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    unsigned char bytes[INPUT_CAP];
    size_t length, bad_start, bad_end;
    TfError bad_code;
    uint32_t seed, index;
    const char *lane;
} Case;

static void require(const Case *c, bool okay, const char *condition, int line) {
    if (!okay) {
        (void)fprintf(stderr, "frontend fuzz:%d: %s; seed=0x%08" PRIx32
                      " case=%" PRIu32 " lane=%s bytes=%zu\n",
                      line, condition, c->seed, c->index, c->lane, c->length);
        (void)fprintf(stderr, "replay: test_frontend_fuzz --case 0x%08" PRIx32
                      " %" PRIu32 "\nhex:", c->seed, c->index);
        for (size_t i = 0; i < c->length; ++i)
            (void)fprintf(stderr, "%02x", (unsigned)c->bytes[i]);
        (void)fputc('\n', stderr);
        exit(EXIT_FAILURE);
    }
}
#define CHECK(c, expr) require((c), (expr), #expr, __LINE__)

/* LCG, modulo 2^32; return the upper bits to avoid cycling low bits.
 * Each index starts independently, so single-case replay needs no prefix. */
static uint32_t pick(uint32_t *state, uint32_t bound) {
    *state = (uint32_t)((uint64_t)*state * UINT32_C(1664525) + UINT32_C(1013904223));
    return (*state >> 16) % bound;
}

static void put(Case *c, const char *s) {
    size_t n = strlen(s);
    CHECK(c, n <= sizeof(c->bytes) - c->length);
    memcpy(c->bytes + c->length, s, n);
    c->length += n;
}

static void gap(Case *c, uint32_t *state) {
    static const char *const gaps[] = {
        " ", "\t", "\r\n", "/* \xce\xa9 */", "// trivia\r\n"
    };
    put(c, gaps[pick(state, COUNT(gaps))]);
}

/* Syntax only: this deliberately makes no claims about type checking. */
static void expression(Case *c, uint32_t *state, unsigned depth) {
    static const char *const atoms[] = {
        "0", "1.25e-2", "true", "x", "letx", "_item", "()",
        "\"\\u{0}\\n\"", "'\xce\xa9'", "Other::Item"
    };
    switch (depth == 0 ? 0 : pick(state, 9)) {
    case 0: put(c, atoms[pick(state, COUNT(atoms))]); break;
    case 1:
        put(c, "-("); expression(c, state, depth - 1); put(c, ")"); break;
    case 2:
        put(c, "("); expression(c, state, depth - 1);
        put(c, ") ^ ("); expression(c, state, depth - 1); put(c, ")"); break;
    case 3:
        put(c, "["); expression(c, state, depth - 1);
        put(c, ","); gap(c, state); expression(c, state, depth - 1); put(c, ",]"); break;
    case 4:
        put(c, "if true {"); expression(c, state, depth - 1);
        put(c, "} else {"); expression(c, state, depth - 1); put(c, "}"); break;
    case 5:
        put(c, "for item in [1] {"); expression(c, state, depth - 1); put(c, "}"); break;
    case 6:
        put(c, "match x { _ => {"); expression(c, state, depth - 1); put(c, "}, }"); break;
    case 7:
        put(c, "fn(a: Int,) -> Int {"); expression(c, state, depth - 1); put(c, "}"); break;
    default:
        put(c, "f("); expression(c, state, depth - 1); put(c, ",).field"); break;
    }
}

static void generate(Case *c) {
    static const char *const lanes[] = {
        "raw", "invalid-token", "valid-grammar", "broken-grammar", "token-soup", "budget"
    };
    static const struct { const char *text; TfError code; } invalid[] = {
        {"@", TF_E101}, {"\xce\xa9", TF_E101},
        {"01.5e-3", TF_E102}, {"1e+", TF_E102}, {"1.2.3", TF_E102},
        {"\"\\q\"", TF_E103}, {"\"unfinished", TF_E103},
        {"/* unfinished", TF_E104}, {"''", TF_E105}, {"'ab'", TF_E105},
        {"'\\u{d800}'", TF_E105}
    };
    static const char *const fragments[] = {
        "fn", "pub", "record", "enum", "machine", "state", "guard", "next",
        "let", "match", "if", "else", "input", "output", "import", "as", "emit",
        "x", "1", "'x'", "\"x\"", "->", "=>", "{", "}", "[", "]", "(", ")",
        ",", ";", ":", "::", ".", "+", "++", "/", "*/", "&&", "&", "_", "@"
    };
    uint32_t group = c->index / LANES;
    uint32_t lane = c->index % LANES;
    uint32_t state = c->seed ^ (uint32_t)((uint64_t)group * UINT32_C(2654435761));
    c->lane = lanes[lane];

    switch (lane) {
    case 0:
        if (group != 0) {
            c->length = 1 + pick(&state, 96);
            for (size_t i = 0; i < c->length; ++i)
                c->bytes[i] = (unsigned char)pick(&state, group % 2 == 0 ? 128u : 256u);
        }
        break;
    case 1: {
        size_t which = (size_t)group % COUNT(invalid);
        for (size_t i = 1 + pick(&state, 5); i != 0; --i) gap(c, &state);
        c->bad_start = c->length;
        put(c, invalid[which].text);
        c->bad_code = invalid[which].code;
        c->bad_end = c->length;
        if (c->bad_code != TF_E104) put(c, "\r\nfn survivor() -> Int { 1 }");
        break;
    }
    case 2: case 3:
        put(c, "record Pair { first: Int, second: List[Int], }\n"
               "enum Choice { None, Some(Int,), }\nfn sample(x: Int,) -> Int {");
        gap(c, &state);
        put(c, "let _ = ");
        expression(c, &state, 3);
        put(c, ";");
        if (lane == 3 && group % 2 == 0) put(c, "let broken = ;");
        gap(c, &state);
        expression(c, &state, 3);
        if (lane == 2 || group % 2 == 0) put(c, "}");
        break;
    case 4:
        for (size_t i = 1 + pick(&state, 24); i != 0; --i) {
            put(c, fragments[pick(&state, COUNT(fragments))]);
            gap(c, &state);
        }
        break;
    default:
        if (group % 2 == 0) {
            /* Exercise suppression in both the scanner and diagnostic adapter. */
            for (size_t i = 101 + pick(&state, 20); i != 0; --i) put(c, "@ ");
        } else {
            /* The only lane expected to hit the explicit parser resource limit. */
            size_t depth = 16 + pick(&state, 16);
            put(c, "fn deep() -> Int { ");
            for (size_t i = 0; i < depth; ++i) put(c, "(");
            put(c, "1");
            for (size_t i = 0; i < depth; ++i) put(c, ")");
            put(c, " }");
        }
        break;
    }
}

static void span(const Case *c, const TfSource *source, TfSpan value) {
    TfSpan checked;
    CHECK(c, value.source == source);
    CHECK(c, tf_source_span(source, value.start, value.end, &checked));
}

static bool same_span(TfSpan a, TfSpan b) {
    return a.source == b.source && a.start == b.start && a.end == b.end;
}

static size_t check_lex(const Case *c, const TfSource *source, const TfLexResult *lex,
                        TfStatus status, size_t cap) {
    size_t previous = 0, invalid = 0, hits = 0;
    CHECK(c, lex->source == source && lex->tokens != NULL);
    CHECK(c, lex->token_count >= 1 && lex->token_count <= c->length + 1);
    for (size_t i = 0; i < lex->token_count; ++i) {
        const TfToken *token = &lex->tokens[i];
        span(c, source, token->span);
        CHECK(c, token->span.start >= previous);
        CHECK(c, token->kind >= TF_IDENT && token->kind <= TF_COMMENT_BLOCK);
        if (i + 1 == lex->token_count) {
            CHECK(c, token->kind == TF_EOF && token->span.start == c->length &&
                     token->span.end == c->length);
        } else {
            CHECK(c, token->kind != TF_EOF && token->span.start < token->span.end);
        }
        if (token->kind == TF_INVALID) {
            CHECK(c, token->error >= TF_E101 && token->error <= TF_E105);
            if (invalid < cap) {
                CHECK(c, invalid < lex->diagnostic_count && lex->diagnostics != NULL);
                CHECK(c, lex->diagnostics[invalid].code == token->error &&
                         same_span(lex->diagnostics[invalid].span, token->span));
            }
            if (c->bad_code == token->error && c->bad_start == token->span.start &&
                c->bad_end == token->span.end) {
                ++hits;
            }
            ++invalid;
        } else {
            CHECK(c, token->error == TF_E_NONE);
        }
        if (token->kind == TF_IDENT || token->kind == TF_INT || token->kind == TF_FLOAT) {
            CHECK(c, token->value == source->bytes + token->span.start &&
                     token->value_length == token->span.end - token->span.start);
        }
        previous = token->span.end;
    }
    CHECK(c, lex->diagnostic_count == (invalid < cap ? invalid : cap));
    CHECK(c, lex->suppressed == invalid - lex->diagnostic_count);
    CHECK(c, lex->has_errors == (invalid != 0));
    CHECK(c, status == (invalid == 0 ? TF_OK : TF_LEX_ERROR));
    if (c->bad_code != TF_E_NONE) CHECK(c, hits == 1 && invalid == 1);
    return invalid;
}

static void check_parse(const Case *c, const TfSource *source, const TfParseResult *parsed,
                        size_t cap, size_t invalid, bool clean, bool broken) {
    const TfAst *ast = tf_parse_ast(parsed);
    const TfDiagnosticBag *bag = tf_parse_diagnostics(parsed);
    const TfDiagnosticPhaseResult *phase = tf_parse_phase(parsed);
    CHECK(c, ast != NULL && bag != NULL && phase != NULL);
    bool errors = tf_diagnostic_bag_has_errors(bag);
    CHECK(c, tf_diagnostic_phase_kind(phase) ==
             (errors ? TF_DIAG_PHASE_ERROR : TF_DIAG_PHASE_VALID));
    CHECK(c, tf_diagnostic_phase_payload(phase) == (errors ? NULL : ast));
    CHECK(c, tf_diagnostic_phase_bag(phase) == bag);
    if (invalid != 0 || broken) CHECK(c, errors);
    if (clean) CHECK(c, !errors);
    size_t diagnostics = tf_diagnostic_bag_count(bag);
    size_t suppressed = tf_diagnostic_bag_suppressed(bag);
    CHECK(c, diagnostics <= cap);
    CHECK(c, suppressed <= SIZE_MAX - diagnostics && diagnostics + suppressed >= invalid);
    if (errors) CHECK(c, diagnostics != 0 || suppressed != 0);
    if (suppressed != 0) CHECK(c, diagnostics == cap);
    if (c->bad_code != TF_E_NONE && cap != 0) {
        const TfDiagnosticRecord *first = tf_diagnostic_bag_get(bag, 0);
        CHECK(c, first != NULL && strcmp(first->code, tf_error_code(c->bad_code)) == 0);
        CHECK(c, first->label_count == 1 && first->labels[0].primary);
        CHECK(c, same_span(first->labels[0].span,
                           (TfSpan){source, c->bad_start, c->bad_end}));
    }
    for (size_t i = 0; i < diagnostics; ++i) {
        const TfDiagnosticRecord *record = tf_diagnostic_bag_get(bag, i);
        CHECK(c, record != NULL && tf_diagnostic_code_registered(record->code));
        for (size_t j = 0; j < record->label_count; ++j) span(c, source, record->labels[j].span);
        for (size_t j = 0; j < record->edit_count; ++j) span(c, source, record->edits[j].span);
    }
    CHECK(c, tf_ast_source(ast) == source);
    const TfAstNode *root = tf_ast_get(ast, tf_ast_root(ast));
    CHECK(c, root != NULL && root->kind == TF_AST_PROGRAM &&
             root->span.start == 0 && root->span.end == c->length);
    if (clean && c->length != 0) CHECK(c, root->as.program.declarations.count == 3);
    for (size_t i = 0; i < root->as.program.declarations.count; ++i)
        CHECK(c, tf_ast_get(ast, root->as.program.declarations.items[i]) != NULL);
    if (c->bad_code != TF_E_NONE && c->bad_code != TF_E104) {
        TfNodeList decls = root->as.program.declarations;
        CHECK(c, decls.count > 0);
        const TfAstNode *last = tf_ast_get(ast, decls.items[decls.count - 1]);
        CHECK(c, last != NULL && last->kind == TF_AST_FUNCTION &&
                 last->span.start == c->bad_end + 2 && last->span.end == c->length);
        TfAstText name = last->as.function.name;
        CHECK(c, name.length == sizeof("survivor") - 1 &&
                 memcmp(name.data, "survivor", name.length) == 0);
    }
    bool missing_brace = false;
    for (size_t i = 0; i < tf_ast_count(ast); ++i) {
        const TfAstNode *node = tf_ast_at(ast, i);
        CHECK(c, node != NULL && tf_ast_get(ast, node->id) == node);
        CHECK(c, node->kind >= TF_AST_ERROR && node->kind <= TF_AST_PATTERN_UNIT);
        span(c, source, node->span);
        if (node->kind == TF_AST_MISSING) {
            CHECK(c, node->span.start == node->span.end);
            if (node->as.missing.expected == TF_RBRACE && node->span.start == c->length)
                missing_brace = true;
        }
        if (node->kind == TF_AST_MISSING || node->kind == TF_AST_ERROR) CHECK(c, errors);
    }
    if (broken && (c->index / LANES) % 2 != 0) CHECK(c, missing_brace);
}

static void run_case(uint32_t seed, uint32_t index, bool replay) {
    Case c = {.seed = seed, .index = index};
    generate(&c);
    uint32_t lane = index % LANES, group = index / LANES;
    static const size_t caps[] = {100, 0, 3};
    size_t cap = caps[group % COUNT(caps)];
    bool depth_limit = lane == 5 && group % 2 != 0;
    if (replay) {
        (void)fprintf(stderr, "frontend fuzz: seed=0x%08" PRIx32 " case=%" PRIu32
                      " lane=%s bytes=%zu\n", seed, index, c.lane, c.length);
        (void)fflush(stderr);
    }
    TfSource source;
    TfStatus status = tf_source_init(&source, "fuzz.tfd", c.bytes, c.length, NULL);
    CHECK(&c, status == TF_OK || (lane == 0 && group != 0 && status == TF_SOURCE_ERROR));
    CHECK(&c, source.length == c.length && memcmp(source.bytes, c.bytes, c.length) == 0);
    if (status == TF_SOURCE_ERROR) {
        CHECK(&c, !source.valid && source.error.span.source == &source);
        CHECK(&c, source.error.code >= TF_E001 && source.error.code <= TF_E003);
        CHECK(&c, source.error.span.start < source.error.span.end &&
                  source.error.span.end <= source.length);
    } else {
        TfLexResult lex;
        status = tf_lex(&lex, &source, cap, NULL);
        size_t invalid = check_lex(&c, &source, &lex, status, cap);
        if (lane == 5 && !depth_limit) CHECK(&c, invalid > 100);
        if (lane == 2 || lane == 3 || depth_limit) CHECK(&c, invalid == 0);

        TfParseResult *parsed = NULL;
        TfParseOptions options = {true, depth_limit ? 8u : 64u, cap};
        status = tf_parse(&parsed, &lex, &options, NULL);
        tf_lex_destroy(&lex);
        if (depth_limit) {
            CHECK(&c, status == TF_HOST_ERROR && parsed == NULL);
        } else {
            CHECK(&c, status == TF_OK);
            check_parse(&c, &source, parsed, cap, invalid, lane == 2 || c.length == 0, lane == 3);
        }
        tf_parse_destroy(parsed);
        CHECK(&c, memcmp(source.bytes, c.bytes, c.length) == 0);
    }
    tf_source_destroy(&source);
}

static bool number(const char *text, uint32_t *out) {
    /* Decimal or explicit hex; reject signs and whitespace consistently. */
    int base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    }
    if (*text == '\0' || text[strspn(text, base == 16 ? "0123456789abcdefABCDEF" :
                                                      "0123456789")] != '\0') return false;
    uintmax_t value = strtoumax(text, NULL, base);
    if (value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

int main(int argc, char **argv) {
    uint32_t seed = DEFAULT_SEED, count = DEFAULT_CASES, index;
    if (argc == 4 && strcmp(argv[1], "--case") == 0 &&
        number(argv[2], &seed) && number(argv[3], &index) && index < MAX_CASES) {
        run_case(seed, index, true);
        return EXIT_SUCCESS;
    }
    if (argc != 1 && !(argc == 3 && number(argv[1], &seed) &&
                       number(argv[2], &count) && count > 0 && count <= MAX_CASES)) {
        (void)fprintf(stderr, "usage: test_frontend_fuzz [SEED COUNT | --case SEED INDEX]\n"
                      "COUNT: 1..%d, INDEX: 0..%d\n", MAX_CASES, MAX_CASES - 1);
        return EXIT_FAILURE;
    }
    (void)printf("frontend fuzz: seed=0x%08" PRIx32 " cases=%" PRIu32 "\n", seed, count);
    (void)fflush(stdout);
    for (index = 0; index < count; ++index) {
        /* Keep replay progress available for crashes and external watchdogs. */
        if (getenv("TF_FRONTEND_FUZZ_TRACE") != NULL) {
            (void)fprintf(stderr, "frontend fuzz: entering case=%" PRIu32 "\n", index);
            (void)fflush(stderr);
        }
        run_case(seed, index, false);
    }
    (void)printf("frontend fuzz: passed %" PRIu32 " cases\n", count);
    return EXIT_SUCCESS;
}
