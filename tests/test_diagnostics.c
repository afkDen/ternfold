#include "ternfold/diagnostics.h"

#include <stdlib.h>
#include <string.h>

static size_t checks;
static void check(bool condition, const char *expression, int line) {
    ++checks;
    if (!condition) { (void)fprintf(stderr,"test_diagnostics.c:%d: %s\n",line,expression); exit(1); }
}
#define CHECK(x) check((x),#x,__LINE__)
#define TEXT(s) ((TfDiagnosticText){(s),sizeof(s)-1})
static TfDiagnosticRecord simple(const char *code, TfDiagnosticSeverity s) {
    TfDiagnosticRecord r = {0}; r.code = code; r.severity = s; r.message = TEXT("message"); return r;
}
static char *read_stream(FILE *f) {
    long end;
    char *bytes;
    CHECK(fseek(f,0,SEEK_END) == 0);
    end = ftell(f); CHECK(end >= 0 && (uintmax_t)end < (uintmax_t)SIZE_MAX);
    bytes = malloc((size_t)end + 1); CHECK(bytes != NULL);
    CHECK(fseek(f,0,SEEK_SET) == 0);
    CHECK(fread(bytes,1,(size_t)end,f) == (size_t)end); bytes[(size_t)end] = '\0';
    return bytes;
}
static char *render(const TfDiagnosticBag *b, TfDiagnosticFormat format, TfDiagnosticColor color) {
    FILE *f = tmpfile(); char *bytes;
    CHECK(f != NULL); CHECK(tf_diagnostic_bag_write(f,b,format,color) == TF_OK);
    CHECK(fflush(f) == 0);
    bytes = read_stream(f); CHECK(fclose(f) == 0); return bytes;
}
static void golden(const TfDiagnosticBag *bag, TfDiagnosticFormat format, const char *path) {
    FILE *file = fopen(path,"rb");
    char *expected, *actual = render(bag,format,TF_DIAG_COLOR_NEVER);
    CHECK(file != NULL); expected = read_stream(file); CHECK(fclose(file) == 0);
    if (strcmp(expected,actual) != 0) (void)fprintf(stderr,"golden %s\nEXPECTED:\n%sACTUAL:\n%s",path,expected,actual);
    CHECK(strcmp(expected,actual) == 0); free(expected); free(actual);
}

/* Small independent JSON grammar validator: consumes the actual writer output,
 * checks number grammar, escapes and nesting, and forbids control bytes. UTF-8
 * validity is separately checked with the accepted source decoder. */
typedef struct { const char *at; size_t depth; } Json;
static void whitespace(Json *j) { while (*j->at == ' ' || *j->at == '\n' || *j->at == '\r' || *j->at == '\t') ++j->at; }
static bool json_string(Json *j) {
    if (*j->at++ != '"') return false;
    while (*j->at != '\0' && *j->at != '"') {
        unsigned char c = (unsigned char)*j->at++;
        if (c < 0x20) return false;
        if (c == '\\') {
            char escape = *j->at++;
            if (escape == 'u') {
                size_t i;
                for (i = 0; i < 4; ++i) {
                    char h = *j->at;
                    if (h == '\0') return false;
                    ++j->at;
                    if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) return false;
                }
            } else if (escape == '\0' || strchr("\"\\/bfnrt",escape) == NULL) return false;
        }
    }
    if (*j->at != '"') return false;
    ++j->at; return true;
}
static bool json_value(Json *j) {
    char c;
    whitespace(j); c = *j->at;
    if (++j->depth > 32) return false;
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        ++j->at; whitespace(j);
        if (*j->at != close) for (;;) {
            if (c == '{') {
                if (*j->at != '"' || !json_string(j)) return false;
                whitespace(j); if (*j->at++ != ':') return false;
            }
            if (!json_value(j)) return false;
            whitespace(j); if (*j->at != ',') break;
            ++j->at; whitespace(j);
        }
        if (*j->at++ != close) return false;
    } else if (c == '"') { if (!json_string(j)) return false; }
    else if (c == 't' && strncmp(j->at,"true",4) == 0) j->at += 4;
    else if (c == 'f' && strncmp(j->at,"false",5) == 0) j->at += 5;
    else if (c == 'n' && strncmp(j->at,"null",4) == 0) j->at += 4;
    else {
        if (*j->at == '-') ++j->at;
        if (*j->at == '0') ++j->at;
        else { if (*j->at < '1' || *j->at > '9') return false; while (*j->at >= '0' && *j->at <= '9') ++j->at; }
        if (*j->at == '.') { ++j->at; if (*j->at < '0' || *j->at > '9') return false; while (*j->at >= '0' && *j->at <= '9') ++j->at; }
        if (*j->at == 'e' || *j->at == 'E') {
            ++j->at; if (*j->at == '+' || *j->at == '-') ++j->at;
            if (*j->at < '0' || *j->at > '9') return false;
            while (*j->at >= '0' && *j->at <= '9') ++j->at;
        }
    }
    --j->depth; return true;
}
static void validate_json(const char *text) {
    Json j = {text,0}; TfSource source;
    CHECK(tf_source_init(&source,"json",text,strlen(text),NULL) == TF_OK);
    CHECK(json_value(&j)); whitespace(&j); CHECK(*j.at == '\0');
    CHECK(strncmp(text,"{\"schema_version\":2,\"diagnostics\":[",sizeof("{\"schema_version\":2,\"diagnostics\":[")-1) == 0);
    CHECK(strstr(text,"\"suppressed\":") != NULL && strstr(text,"\"has_errors\":") != NULL);
    CHECK(strchr(text,'\x1b') == NULL);
    tf_source_destroy(&source);
}

static void test_goldens(void) {
    TfSource source, many, empty, raw;
    TfDiagnosticBag *bag;
    TfDiagnosticRecord r;
    TfDiagnosticLabel labels[3];
    TfDiagnosticText notes[2] = {TEXT("note"),TEXT("two")}, help = TEXT("help");
    TfDiagnosticEdit edit;
    char *json, *colored, *plain, *again;
    static const unsigned char invalid[] = {0xff};
    CHECK(tf_source_init(&source,"tab.tfd","\t\xc3\xa9\r\n",5,NULL) == TF_OK);
    CHECK(tf_source_init(&many,"many.tfd","\tA\xc3\xa9\r\n\nZ",8,NULL) == TF_OK);
    CHECK(tf_source_init(&empty,"empty.tfd",NULL,0,NULL) == TF_OK);
    CHECK(tf_source_init(&raw,"raw.tfd",invalid,sizeof(invalid),NULL) == TF_SOURCE_ERROR);
    CHECK(tf_diagnostic_bag_create(&bag,100,NULL) == TF_OK);
    labels[0] = (TfDiagnosticLabel){{&source,1,3},true,TEXT("here")};
    edit = (TfDiagnosticEdit){{&source,1,3},TEXT("x")};
    r = simple("E101",TF_DIAG_ERROR); r.message = TEXT("bad token");
    r.labels = labels; r.label_count = 1; r.notes = notes; r.note_count = 1;
    r.help = &help; r.help_count = 1; r.edits = &edit; r.edit_count = 1;
    CHECK(tf_diagnostic_bag_add(bag,&r) == TF_OK);
    golden(bag,TF_DIAG_TEXT,"tests/fixtures/diagnostics/basic.txt");
    golden(bag,TF_DIAG_JSON,"tests/fixtures/diagnostics/basic.json");
    json = render(bag,TF_DIAG_JSON,TF_DIAG_COLOR_ALWAYS); validate_json(json);
    {
        FILE *file = fopen("build/m02-diagnostics-v2.json","wb");
        CHECK(file != NULL); CHECK(fwrite(json,1,strlen(json),file) == strlen(json)); CHECK(fclose(file) == 0);
    }
    again = render(bag,TF_DIAG_JSON,TF_DIAG_COLOR_NEVER); CHECK(strcmp(json,again) == 0);
    free(again); free(json);
    colored = render(bag,TF_DIAG_TEXT,TF_DIAG_COLOR_ALWAYS);
    plain = render(bag,TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
    CHECK(strstr(colored,"\x1b[31merror[E101]\x1b[0m") != NULL && strchr(plain,'\x1b') == NULL);
    free(colored); free(plain); tf_diagnostic_bag_destroy(bag);
    CHECK(tf_diagnostic_bag_create(&bag,100,NULL) == TF_OK);
    r = simple("E302",TF_DIAG_WARNING); r.message = TEXT("mismatch");
    labels[0] = (TfDiagnosticLabel){{&many,1,8},true,TEXT("range")};
    labels[1] = (TfDiagnosticLabel){{&many,2,4},false,TEXT("overlap")};
    labels[2] = (TfDiagnosticLabel){{&empty,0,0},true,TEXT("eof")};
    notes[0] = TEXT("one"); help = TEXT("consider");
    r.labels = labels; r.label_count = 3; r.notes = notes; r.note_count = 2; r.help = &help; r.help_count = 1;
    CHECK(tf_diagnostic_bag_add(bag,&r) == TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(bag));
    golden(bag,TF_DIAG_TEXT,"tests/fixtures/diagnostics/multiline.txt");
    json = render(bag,TF_DIAG_JSON,TF_DIAG_COLOR_NEVER); validate_json(json);
    CHECK(strstr(json,"\"start_byte\":2,\"end_byte\":4") != NULL);
    CHECK(strstr(json,"\"source\":\"empty.tfd\"") != NULL); free(json);
    tf_diagnostic_bag_destroy(bag);
    CHECK(tf_diagnostic_bag_create(&bag,100,NULL) == TF_OK);
    CHECK(tf_diagnostic_bag_source(bag,&raw) == TF_OK);
    golden(bag,TF_DIAG_TEXT,"tests/fixtures/diagnostics/raw.txt");
    json = render(bag,TF_DIAG_JSON,TF_DIAG_COLOR_ALWAYS); validate_json(json);
    CHECK(strstr(json,"\"start\":null,\"end\":null,\"source_text\":\"\\u00ff\",\"raw\":true") != NULL);
    free(json); tf_diagnostic_bag_destroy(bag);
    tf_source_destroy(&raw); tf_source_destroy(&empty); tf_source_destroy(&many); tf_source_destroy(&source);
}

static void test_validation_caps(void) {
    static const char *const codes[] = {"E000","E004","E106","E109","E113","E209","E308","E309","E408","R000","R007","e101","E10","E1010","X101","E1a1"};
    TfDiagnosticBag *bag; TfSource source, invalid;
    TfDiagnosticRecord r = simple("E101",TF_DIAG_ERROR), malformed;
    TfDiagnosticLabel label;
    size_t i;
    CHECK(tf_source_init(&source,"u","a\xc3\xa9",3,NULL) == TF_OK);
    CHECK(tf_source_init(&invalid,"bad","\r",1,NULL) == TF_SOURCE_ERROR);
    CHECK(tf_diagnostic_bag_create(&bag,0,NULL) == TF_OK);
    for (i = 0; i < sizeof(codes)/sizeof(codes[0]); ++i) {
        malformed = r; malformed.code = codes[i];
        CHECK(!tf_diagnostic_code_registered(codes[i])); CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    }
    CHECK(!tf_diagnostic_code_registered(NULL));
    CHECK(tf_diagnostic_bag_suppressed(bag) == 0 && !tf_diagnostic_bag_has_errors(bag));
    CHECK(tf_diagnostic_bag_add(bag,&r) == TF_OK);
    CHECK(tf_diagnostic_bag_count(bag) == 0 && tf_diagnostic_bag_suppressed(bag) == 1 && tf_diagnostic_bag_has_errors(bag));
    malformed = r; malformed.severity = (TfDiagnosticSeverity)99;
    CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    malformed = r; malformed.message = (TfDiagnosticText){NULL,1}; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    malformed.message = TEXT("\xff"); CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    malformed.message = (TfDiagnosticText){"a",SIZE_MAX}; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    malformed = r; malformed.label_count = 1; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    malformed.label_count = SIZE_MAX; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    label = (TfDiagnosticLabel){{&source,2,3},true,TEXT("mid")};
    malformed = r; malformed.labels = &label; malformed.label_count = 1;
    CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    label.span = (TfSpan){&source,0,2}; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    label.span = (TfSpan){&source,3,4}; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    label.span = (TfSpan){&source,3,0}; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    label.span = (TfSpan){NULL,0,0}; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    label.span = (TfSpan){&source,0,3}; label.primary = false; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    label.primary = true; label.message = TEXT("\xff"); CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    label.message = TEXT(""); label.span = (TfSpan){&invalid,0,2}; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    label.span.end = 1; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_OK);
    CHECK(tf_diagnostic_bag_suppressed(bag) == 2);
    CHECK(tf_diagnostic_bag_get(bag,0) == NULL && tf_diagnostic_bag_get(NULL,0) == NULL);
    CHECK(tf_diagnostic_bag_add(NULL,&r) == TF_HOST_ERROR && tf_diagnostic_bag_add(bag,NULL) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_bag_source(bag,&source) == TF_OK && tf_diagnostic_bag_source(bag,NULL) == TF_HOST_ERROR);
    tf_diagnostic_bag_destroy(bag);
    CHECK(tf_diagnostic_bag_create(&bag,1,NULL) == TF_OK);
    r.severity = TF_DIAG_NOTE; CHECK(tf_diagnostic_bag_add(bag,&r) == TF_OK);
    r.severity = TF_DIAG_WARNING; CHECK(tf_diagnostic_bag_add(bag,&r) == TF_OK); CHECK(!tf_diagnostic_bag_has_errors(bag));
    r.severity = TF_DIAG_ERROR; CHECK(tf_diagnostic_bag_add(bag,&r) == TF_OK); CHECK(tf_diagnostic_bag_has_errors(bag));
    CHECK(tf_diagnostic_bag_count(bag) == 1 && tf_diagnostic_bag_suppressed(bag) == 2);
    { char *text = render(bag,TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER); CHECK(strstr(text,"2 further diagnostics suppressed\n") != NULL); free(text); }
    tf_diagnostic_bag_destroy(bag);
    CHECK(tf_diagnostic_bag_count(NULL) == 0 && tf_diagnostic_bag_suppressed(NULL) == 0 && !tf_diagnostic_bag_has_errors(NULL));
    CHECK(tf_diagnostic_bag_create(NULL,0,NULL) == TF_HOST_ERROR);
    tf_diagnostic_bag_destroy(NULL); tf_source_destroy(&source); tf_source_destroy(&invalid);
}

static void test_deep_ownership_growth(void) {
    TfSource source;
    TfDiagnosticBag *bag;
    char code[] = "R006", message[] = "a\0b\n\"\\\x1b";
    TfDiagnosticText texts[2] = {{message,sizeof(message)-1},TEXT("\xf0\x9f\x99\x82")};
    TfDiagnosticLabel labels[2]; TfDiagnosticEdit edits[2];
    TfDiagnosticRecord r;
    const TfDiagnosticRecord *first;
    char *json, *text; size_t i;
    CHECK(tf_source_init(&source,"n\xff\n\"", "\t\0\\\"\n",5,NULL) == TF_OK);
    labels[0] = (TfDiagnosticLabel){{&source,0,5},true,texts[0]}; labels[1] = labels[0]; labels[1].primary = false;
    edits[0] = (TfDiagnosticEdit){{&source,0,0},texts[0]}; edits[1] = (TfDiagnosticEdit){{&source,5,5},texts[1]};
    r = simple(code,TF_DIAG_ERROR); r.message = texts[0]; r.labels = labels; r.label_count = 2;
    r.notes = texts; r.note_count = 2; r.help = texts; r.help_count = 2; r.edits = edits; r.edit_count = 2;
    CHECK(tf_diagnostic_bag_create(&bag,100,NULL) == TF_OK); CHECK(tf_diagnostic_bag_add(bag,&r) == TF_OK);
    first = tf_diagnostic_bag_get(bag,0); CHECK(first != NULL && first->labels[0].span.source == &source);
    CHECK(first->code != code && first->message.data != message && first->labels != labels && first->edits != edits);
    CHECK(first->notes != texts && first->help != texts && first->notes[0].data != message);
    memset(message,'z',sizeof(message)); memset(code,'z',sizeof(code)); memset(texts,0,sizeof(texts)); memset(labels,0,sizeof(labels)); memset(edits,0,sizeof(edits));
    for (i = 0; i < 65; ++i) { r = simple("E208",TF_DIAG_NOTE); CHECK(tf_diagnostic_bag_add(bag,&r) == TF_OK); }
    CHECK(tf_diagnostic_bag_get(bag,0) == first && strcmp(first->code,"R006") == 0);
    CHECK(first->message.length == 7 && memcmp(first->message.data,"a\0b\n\"\\\x1b",7) == 0);
    json = render(bag,TF_DIAG_JSON,TF_DIAG_COLOR_ALWAYS); validate_json(json);
    CHECK(strstr(json,"a\\u0000b\\u000a\\\"\\\\\\u001b") != NULL);
    CHECK(strstr(json,"n\\u00ff\\u000a\\\"") != NULL);
    CHECK(strstr(json,"\"source_text\":\"\\u0009\\u0000\\\\\\\"\\u000a\"") != NULL);
    text = render(bag,TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER); CHECK(strchr(text,'\x1b') == NULL);
    CHECK(strstr(text,"\\u0000") != NULL); free(text); free(json);
    tf_diagnostic_bag_destroy(bag); tf_source_destroy(&source);
}

typedef struct { size_t calls, fail_at, live; } Memory;
static void *allocate(void *context, size_t size) {
    Memory *m = context; void *p; ++m->calls;
    if (m->calls == m->fail_at) return NULL;
    p = malloc(size); if (p != NULL) ++m->live; return p;
}
static void deallocate(void *context, void *p) {
    Memory *m = context; CHECK(p != NULL && m->live != 0); --m->live; free(p);
}
static void test_allocation_failures(void) {
    TfSource source;
    TfDiagnosticRecord r = simple("E302",TF_DIAG_ERROR), prior = simple("E101",TF_DIAG_NOTE);
    TfDiagnosticLabel labels[2]; TfDiagnosticText texts[2] = {TEXT("first"),TEXT("second")};
    TfDiagnosticEdit edits[2];
    size_t scenario, position;
    CHECK(tf_source_init(&source,"oom","abc",3,NULL) == TF_OK);
    labels[0] = (TfDiagnosticLabel){{&source,0,1},true,TEXT("first")}; labels[1] = labels[0];
    edits[0] = (TfDiagnosticEdit){{&source,0,1},TEXT("replace")}; edits[1] = edits[0];
    r.labels = labels; r.label_count = 2; r.notes = texts; r.note_count = 2; r.help = texts; r.help_count = 2; r.edits = edits; r.edit_count = 2;
    for (scenario = 0; scenario < 3; ++scenario) {
        bool success = false;
        for (position = 1; position < 80; ++position) {
            Memory memory = {0}; TfAllocator a = {&memory,allocate,deallocate};
            TfDiagnosticBag *bag; const TfDiagnosticRecord *first = NULL;
            size_t i, before = scenario == 0 ? 0 : scenario == 1 ? 1 : 16;
            char *old, *after; TfStatus status;
            TfDiagnosticPhaseResult phase;
            CHECK(tf_diagnostic_bag_create(&bag,100,&a) == TF_OK);
            for (i = 0; i < before; ++i) CHECK(tf_diagnostic_bag_add(bag,&prior) == TF_OK);
            if (before != 0) first = tf_diagnostic_bag_get(bag,0);
            CHECK(tf_diagnostic_phase_valid(&phase,&source,bag) == TF_OK);
            old = render(bag,TF_DIAG_JSON,TF_DIAG_COLOR_NEVER);
            memory.fail_at = memory.calls + position;
            status = tf_diagnostic_bag_add(bag,&r);
            if (status == TF_HOST_ERROR) {
                CHECK(tf_diagnostic_bag_count(bag) == before && !tf_diagnostic_bag_has_errors(bag));
                after = render(bag,TF_DIAG_JSON,TF_DIAG_COLOR_NEVER); CHECK(strcmp(after,old) == 0); free(after);
                CHECK(tf_diagnostic_bag_get(bag,0) == first);
                CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_VALID);
                CHECK(tf_diagnostic_phase_payload(&phase) == &source && tf_diagnostic_phase_bag(&phase) == bag);
                memory.fail_at = 0; CHECK(tf_diagnostic_bag_add(bag,&r) == TF_OK);
            } else { CHECK(status == TF_OK); success = true; }
            CHECK(tf_diagnostic_bag_count(bag) == before + 1 && tf_diagnostic_bag_has_errors(bag));
            CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_INVALID);
            CHECK(tf_diagnostic_phase_payload(&phase) == NULL && tf_diagnostic_phase_bag(&phase) == NULL);
            free(old); tf_diagnostic_bag_destroy(bag); CHECK(memory.live == 0);
            if (success) break;
        }
        CHECK(success);
    }
    {
        Memory m = {0,1,0}; TfAllocator a = {&m,allocate,deallocate}; TfDiagnosticBag *bag;
        CHECK(tf_diagnostic_bag_create(&bag,0,&a) == TF_HOST_ERROR && bag == NULL && m.live == 0);
        a.deallocate = NULL; CHECK(tf_diagnostic_bag_create(&bag,0,&a) == TF_HOST_ERROR && bag == NULL);
    }
    tf_source_destroy(&source);
}

static void test_adapters(void) {
    TfSource source, bad;
    TfLexResult lexer, capped;
    TfDiagnosticBag *bag;
    size_t position;
    bool success = false;
    CHECK(tf_source_init(&source,"lex.tfd","@ ` #",5,NULL) == TF_OK);
    CHECK(tf_lex(&lexer,&source,100,NULL) == TF_LEX_ERROR);
    CHECK(lexer.diagnostic_count == 3);
    CHECK(tf_lex(&capped,&source,0,NULL) == TF_LEX_ERROR);
    CHECK(tf_diagnostic_bag_create(&bag,0,NULL) == TF_OK);
    CHECK(tf_diagnostic_bag_lexer(bag,&capped) == TF_OK);
    CHECK(tf_diagnostic_bag_has_errors(bag) && tf_diagnostic_bag_suppressed(bag) == 3);
    tf_diagnostic_bag_destroy(bag);
    CHECK(tf_diagnostic_bag_create(&bag,1,NULL) == TF_OK);
    CHECK(tf_diagnostic_bag_lexer(bag,&lexer) == TF_OK);
    CHECK(tf_diagnostic_bag_count(bag) == 1 && tf_diagnostic_bag_suppressed(bag) == 2);
    CHECK(strcmp(tf_diagnostic_bag_get(bag,0)->code,"E101") == 0);
    tf_diagnostic_bag_destroy(bag);
    for (position = 1; position < 80; ++position) {
        Memory memory = {0}; TfAllocator a = {&memory,allocate,deallocate};
        TfDiagnosticRecord prior = simple("E208",TF_DIAG_NOTE);
        const TfDiagnosticRecord *first; TfStatus status;
        TfDiagnosticPhaseResult phase;
        char *old, *after;
        CHECK(tf_diagnostic_bag_create(&bag,100,&a) == TF_OK); CHECK(tf_diagnostic_bag_add(bag,&prior) == TF_OK);
        first = tf_diagnostic_bag_get(bag,0); old = render(bag,TF_DIAG_JSON,TF_DIAG_COLOR_NEVER);
        CHECK(tf_diagnostic_phase_valid(&phase,&source,bag) == TF_OK);
        memory.fail_at = memory.calls + position;
        status = tf_diagnostic_bag_lexer(bag,&lexer);
        if (status == TF_HOST_ERROR) {
            after = render(bag,TF_DIAG_JSON,TF_DIAG_COLOR_NEVER); CHECK(strcmp(after,old) == 0); free(after);
            CHECK(tf_diagnostic_bag_count(bag) == 1 && !tf_diagnostic_bag_has_errors(bag));
            CHECK(tf_diagnostic_bag_get(bag,0) == first);
            CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_VALID);
            CHECK(tf_diagnostic_phase_payload(&phase) == &source && tf_diagnostic_phase_bag(&phase) == bag);
        } else {
            CHECK(status == TF_OK && tf_diagnostic_bag_count(bag) == 4); success = true;
            CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_INVALID);
        }
        free(old); tf_diagnostic_bag_destroy(bag); CHECK(memory.live == 0); if (success) break;
    }
    CHECK(success);
    CHECK(tf_diagnostic_bag_create(&bag,100,NULL) == TF_OK);
    {
        TfDiagnostic copy[2] = {lexer.diagnostics[0],lexer.diagnostics[1]};
        TfLexResult forged = lexer;
        copy[1].span.end = SIZE_MAX; forged.diagnostics = copy; forged.diagnostic_count = 2;
        CHECK(tf_diagnostic_bag_lexer(bag,&forged) == TF_HOST_ERROR && tf_diagnostic_bag_count(bag) == 0);
        forged = capped; forged.suppressed = SIZE_MAX;
        CHECK(tf_diagnostic_bag_lexer(bag,&forged) == TF_OK);
        CHECK(tf_diagnostic_bag_lexer(bag,&capped) == TF_HOST_ERROR && tf_diagnostic_bag_suppressed(bag) == SIZE_MAX);
    }
    CHECK(tf_diagnostic_bag_lexer(bag,NULL) == TF_HOST_ERROR);
    CHECK(tf_source_init(&bad,"bom","\xef\xbb\xbf",3,NULL) == TF_SOURCE_ERROR);
    CHECK(tf_diagnostic_bag_source(bag,&bad) == TF_OK);
    CHECK(strcmp(tf_diagnostic_bag_get(bag,0)->code,"E002") == 0);
    tf_diagnostic_bag_destroy(bag); tf_source_destroy(&bad);
    tf_lex_destroy(&capped); tf_lex_destroy(&lexer); tf_source_destroy(&source);
}
static void test_stream_errors(void) {
    TfDiagnosticBag *bag; FILE *file;
    TfDiagnosticRecord r = simple("E101",TF_DIAG_ERROR);
    CHECK(tf_diagnostic_bag_create(&bag,10,NULL) == TF_OK); CHECK(tf_diagnostic_bag_add(bag,&r) == TF_OK);
    file = fopen("tests/fixtures/diagnostics/basic.json","rb"); CHECK(file != NULL);
    CHECK(setvbuf(file,NULL,_IONBF,0) == 0);
    CHECK(tf_diagnostic_bag_write(file,bag,TF_DIAG_JSON,TF_DIAG_COLOR_NEVER) == TF_HOST_ERROR);
    CHECK(ferror(file) != 0); CHECK(fclose(file) == 0);
    CHECK(tf_diagnostic_bag_write(NULL,bag,TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER) == TF_HOST_ERROR);
    file = tmpfile(); CHECK(file != NULL);
    CHECK(tf_diagnostic_bag_write(file,NULL,TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_bag_write(file,bag,(TfDiagnosticFormat)9,TF_DIAG_COLOR_NEVER) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_bag_write(file,bag,TF_DIAG_JSON,(TfDiagnosticColor)9) == TF_HOST_ERROR);
    CHECK(fclose(file) == 0); tf_diagnostic_bag_destroy(bag);
}
static void test_boundaries_and_invalid_vectors(void) {
    TfSource source, raw;
    TfDiagnosticBag *bag;
    TfDiagnosticRecord r = simple("R001",TF_DIAG_NOTE), malformed;
    TfDiagnosticLabel labels[2];
    TfDiagnosticText invalid_text = {NULL,1};
    TfDiagnosticEdit edit;
    char *json;
    size_t i;
    static const char *const registered[] = {"E001","E002","E003","E101","E102","E103","E104","E105",
        "E110","E111","E112","E201","E202","E203","E204","E205","E206","E207","E208",
        "E301","E302","E303","E304","E305","E306","E307","E310","E311","E312",
        "E401","E402","E403","E404","E405","E406","E407","R001","R002","R003","R004","R005","R006"};
    for (i = 0; i < sizeof(registered)/sizeof(registered[0]); ++i) CHECK(tf_diagnostic_code_registered(registered[i]));
    CHECK(tf_source_init(&source,"crlf.tfd","x\r\n",3,NULL) == TF_OK);
    CHECK(tf_diagnostic_bag_create(&bag,100,NULL) == TF_OK);
    labels[0] = (TfDiagnosticLabel){{&source,3,3},true,TEXT("eof")};
    labels[1] = (TfDiagnosticLabel){{&source,1,3},false,TEXT("newline")};
    r.message = TEXT("end"); r.labels = labels; r.label_count = 2;
    CHECK(tf_diagnostic_bag_add(bag,&r) == TF_OK);
    golden(bag,TF_DIAG_TEXT,"tests/fixtures/diagnostics/eof-crlf.txt");
    json = render(bag,TF_DIAG_JSON,TF_DIAG_COLOR_NEVER); validate_json(json);
    CHECK(strstr(json,"\"source_text\":\"\\u000d\\u000a\"") != NULL); free(json);
    malformed = r; malformed.notes = &invalid_text; malformed.note_count = 1;
    CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    malformed.notes = NULL; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    malformed.note_count = SIZE_MAX; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    malformed = r; malformed.help = &invalid_text; malformed.help_count = 1;
    CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    malformed.help = NULL; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    malformed.help_count = SIZE_MAX; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    malformed = r; malformed.edit_count = 1; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    edit = (TfDiagnosticEdit){{&source,0,1},invalid_text}; malformed.edits = &edit;
    CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    edit.replacement = TEXT("ok"); edit.span.end = 4; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    malformed.edit_count = SIZE_MAX; CHECK(tf_diagnostic_bag_add(bag,&malformed) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_bag_count(bag) == 1 && !tf_diagnostic_bag_has_errors(bag));
    tf_diagnostic_bag_destroy(bag); tf_source_destroy(&source);
    CHECK(tf_source_init(&raw,"bad","\xff\x80\0",3,NULL) == TF_SOURCE_ERROR);
    CHECK(tf_diagnostic_bag_create(&bag,10,NULL) == TF_OK);
    labels[0] = (TfDiagnosticLabel){{&raw,1,3},true,TEXT("raw range")};
    r = simple("E001",TF_DIAG_ERROR); r.labels = labels; r.label_count = 1;
    CHECK(tf_diagnostic_bag_add(bag,&r) == TF_OK);
    json = render(bag,TF_DIAG_JSON,TF_DIAG_COLOR_NEVER); validate_json(json);
    CHECK(strstr(json,"\"start\":null,\"end\":null,\"source_text\":\"\\u0080\\u0000\",\"raw\":true") != NULL);
    free(json); tf_diagnostic_bag_destroy(bag); tf_source_destroy(&raw);
}
static void test_source_adapter_failures(void) {
    TfSource source;
    size_t position;
    bool success = false;
    CHECK(tf_source_init(&source,"cr","\r",1,NULL) == TF_SOURCE_ERROR);
    for (position = 1; position < 30; ++position) {
        Memory m = {0}; TfAllocator a = {&m,allocate,deallocate}; TfDiagnosticBag *bag;
        TfStatus status;
        CHECK(tf_diagnostic_bag_create(&bag,100,&a) == TF_OK);
        m.fail_at = m.calls + position; status = tf_diagnostic_bag_source(bag,&source);
        if (status == TF_HOST_ERROR) CHECK(tf_diagnostic_bag_count(bag) == 0 && !tf_diagnostic_bag_has_errors(bag));
        else { CHECK(status == TF_OK && tf_diagnostic_bag_count(bag) == 1); success = true; }
        tf_diagnostic_bag_destroy(bag); CHECK(m.live == 0); if (success) break;
    }
    CHECK(success);
    {
        Memory m = {0}; TfAllocator a = {&m,allocate,deallocate}; TfDiagnosticBag *bag;
        size_t before;
        CHECK(tf_diagnostic_bag_create(&bag,0,&a) == TF_OK);
        before = m.calls; m.fail_at = m.calls + 1;
        CHECK(tf_diagnostic_bag_source(bag,&source) == TF_OK && m.calls == before);
        CHECK(tf_diagnostic_bag_has_errors(bag) && tf_diagnostic_bag_suppressed(bag) == 1);
        tf_diagnostic_bag_destroy(bag); CHECK(m.live == 0);
    }
    tf_source_destroy(&source);
}
static void test_line_number_gutters(void) {
    static const size_t lines[] = {9,10,99,100};
    size_t i, kind;
    for (i = 0; i < sizeof(lines)/sizeof(lines[0]); ++i) {
        for (kind = 0; kind < 3; ++kind) {
            unsigned char bytes[104];
            size_t line = lines[i], length = line - 1;
            TfSource source;
            TfDiagnosticBag *bag;
            TfDiagnosticRecord record = simple("E101",TF_DIAG_ERROR);
            TfDiagnosticLabel labels[2];
            char expected[256], *actual;
            const char *gap = line < 10 ? " " : line < 100 ? "  " : "   ";
            bool eof = kind == 2;
            memset(bytes,'\n',length);
            if (!eof) bytes[length++] = 'X';
            CHECK(tf_source_init(&source,"gutter.tfd",bytes,length,NULL) == TF_OK);
            labels[0] = (TfDiagnosticLabel){{&source,line-1,length},true,TEXT("primary")};
            labels[1] = (TfDiagnosticLabel){{&source,line-1,length},false,TEXT("secondary")};
            record.labels = labels; record.label_count = kind == 1 ? 2 : 1;
            CHECK(tf_diagnostic_bag_create(&bag,10,NULL) == TF_OK);
            CHECK(tf_diagnostic_bag_add(bag,&record) == TF_OK);
            actual = render(bag,TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER);
            CHECK(snprintf(expected,sizeof(expected),"  %zu | %s\n  %s | ^\n",line,eof ? "" : "X",gap) > 0);
            CHECK(strstr(actual,expected) != NULL);
            if (kind == 1) {
                CHECK(snprintf(expected,sizeof(expected),"  %zu | X\n  %s | -\n",line,gap) > 0);
                CHECK(strstr(actual,expected) != NULL);
            }
            free(actual); tf_diagnostic_bag_destroy(bag); tf_source_destroy(&source);
        }
    }
}
static void test_phase_results(void) {
    TfDiagnosticBag *bag;
    TfDiagnosticPhaseResult phase = {0}, copy, error;
    TfDiagnosticRecord warning = simple("E101",TF_DIAG_WARNING), diagnosed = simple("E101",TF_DIAG_ERROR);
    TfSource source;
    TfLexResult clean = {0}, capped = {0};
    Memory m = {0}; TfAllocator a = {&m,allocate,deallocate};
    const int payload = 42;
    size_t calls;
    CHECK(tf_diagnostic_phase_kind(NULL) == TF_DIAG_PHASE_INVALID);
    CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_INVALID);
    CHECK(tf_diagnostic_phase_payload(NULL) == NULL && tf_diagnostic_phase_bag(NULL) == NULL);
    CHECK(tf_diagnostic_phase_payload(&phase) == NULL && tf_diagnostic_phase_bag(&phase) == NULL);
    CHECK(tf_diagnostic_bag_create(&bag,1,&a) == TF_OK);
    CHECK(tf_diagnostic_phase_valid(&phase,&payload,bag) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_VALID);
    CHECK(tf_diagnostic_phase_payload(&phase) == &payload && tf_diagnostic_phase_bag(&phase) == bag);
    copy = phase;
    CHECK(tf_diagnostic_phase_error(&phase,bag) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_INVALID);
    CHECK(tf_diagnostic_phase_kind(&copy) == TF_DIAG_PHASE_VALID);
    CHECK(tf_diagnostic_phase_valid(NULL,&payload,bag) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_phase_error(NULL,bag) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_phase_valid(&phase,NULL,bag) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_phase_valid(&phase,&payload,NULL) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_phase_error(&phase,NULL) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_INVALID);
    CHECK(tf_diagnostic_bag_add(bag,&warning) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(&copy) == TF_DIAG_PHASE_INVALID);
    CHECK(tf_diagnostic_phase_valid(&phase,&payload,bag) == TF_OK);
    CHECK(tf_diagnostic_bag_add(bag,&warning) == TF_OK); /* suppressed warning */
    CHECK(!tf_diagnostic_bag_has_errors(bag) && tf_diagnostic_bag_suppressed(bag) == 1);
    CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_INVALID);
    CHECK(tf_diagnostic_phase_valid(&phase,&payload,bag) == TF_OK);
    CHECK(tf_diagnostic_bag_add(bag,&diagnosed) == TF_OK); /* suppressed error */
    CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_INVALID);
    CHECK(tf_diagnostic_phase_valid(&phase,&payload,bag) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_phase_payload(&phase) == NULL && tf_diagnostic_phase_bag(&phase) == NULL);
    calls = m.calls; m.fail_at = m.calls + 1;
    CHECK(tf_diagnostic_phase_error(&error,bag) == TF_OK && m.calls == calls);
    CHECK(tf_diagnostic_phase_kind(&error) == TF_DIAG_PHASE_ERROR);
    CHECK(tf_diagnostic_phase_payload(&error) == NULL && tf_diagnostic_phase_bag(&error) == bag);
    warning.code = "E999"; CHECK(tf_diagnostic_bag_add(bag,&warning) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_phase_kind(&error) == TF_DIAG_PHASE_ERROR);
    tf_diagnostic_bag_destroy(bag); CHECK(m.live == 0);
    /* Borrow handles are deliberately not used after bag destruction. */
    m.fail_at = 0;
    CHECK(tf_source_init(&source,"clean","",0,NULL) == TF_OK);
    CHECK(tf_diagnostic_bag_create(&bag,0,&a) == TF_OK);
    CHECK(tf_diagnostic_phase_valid(&phase,&payload,bag) == TF_OK);
    clean.source = &source;
    CHECK(tf_diagnostic_bag_source(bag,&source) == TF_OK);
    CHECK(tf_diagnostic_bag_lexer(bag,&clean) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_VALID); /* no-op adapters */
    { char *json = render(bag,TF_DIAG_JSON,TF_DIAG_COLOR_NEVER); free(json); }
    CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_VALID); /* render is read-only */
    capped.source = &source; capped.suppressed = 3; capped.has_errors = true;
    CHECK(tf_diagnostic_bag_lexer(bag,&capped) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_INVALID);
    CHECK(tf_diagnostic_phase_valid(&phase,&payload,bag) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_phase_error(&phase,bag) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_ERROR && tf_diagnostic_phase_payload(&phase) == NULL);
    warning.code = "E101"; CHECK(tf_diagnostic_bag_add(bag,&warning) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_INVALID);
    tf_diagnostic_bag_destroy(bag); CHECK(m.live == 0); tf_source_destroy(&source);
    /* A retained error also prevents the valid factory. */
    CHECK(tf_diagnostic_bag_create(&bag,1,NULL) == TF_OK);
    CHECK(tf_diagnostic_bag_add(bag,&diagnosed) == TF_OK);
    CHECK(tf_diagnostic_phase_valid(&phase,&payload,bag) == TF_HOST_ERROR);
    CHECK(tf_diagnostic_phase_error(&phase,bag) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(&phase) == TF_DIAG_PHASE_ERROR && tf_diagnostic_phase_payload(&phase) == NULL);
    tf_diagnostic_bag_destroy(bag);
}
int main(void) {
    test_goldens(); test_validation_caps(); test_deep_ownership_growth();
    test_allocation_failures(); test_adapters(); test_stream_errors();
    test_boundaries_and_invalid_vectors(); test_source_adapter_failures();
    test_line_number_gutters();
    test_phase_results();
    (void)printf("diagnostics: %zu checks passed\n",checks); return 0;
}
