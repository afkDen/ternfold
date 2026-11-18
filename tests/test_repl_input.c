#include "../src/repl_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t calls, fail_at, live;
} Memory;

static void *allocate(void *context, size_t size) {
    Memory *memory = context;
    void *pointer;
    if (++memory->calls == memory->fail_at) return NULL;
    pointer = malloc(size);
    if (pointer != NULL) ++memory->live;
    return pointer;
}

static void deallocate(void *context, void *pointer) {
    Memory *memory = context;
    if (pointer == NULL || memory->live == 0) abort();
    --memory->live;
    free(pointer);
}

static bool allocation_failures(void) {
    const char input[] = "let value = [\"a\", \"unterminated\\\"";
    Memory memory = {0};
    TfAllocator allocator = {&memory, allocate, deallocate};
    TfSource source;
    bool ok = true;
    size_t live, calls;
    if (tf_source_init(&source, "<repl>", input, sizeof input - 1, &allocator) != TF_OK)
        return false;
    live = memory.live;
    memory.calls = 0;
    if (tf_repl_classify(&source) != TF_REPL_UNIT_INCOMPLETE || memory.live != live)
        ok = false;
    calls = memory.calls;
    for (size_t fail = 1; fail <= calls; ++fail) {
        memory.calls = 0;
        memory.fail_at = fail;
        if (tf_repl_classify(&source) != TF_REPL_UNIT_HOST_ERROR || memory.live != live) {
            fprintf(stderr, "allocation failure %zu was not cleanly reported\n", fail);
            ok = false;
        }
    }
    tf_source_destroy(&source);
    return ok && calls != 0 && memory.live == 0;
}

int main(void) {
    static const struct {
        const char *input;
        TfReplUnitKind want;
    } cases[] = {
        {"", TF_REPL_UNIT_EMPTY},
        {" \t\r\n/*fn [*/ // }", TF_REPL_UNIT_EMPTY},
        {"/* open\n{", TF_REPL_UNIT_INCOMPLETE},
        {"/* open\n{ */", TF_REPL_UNIT_EMPTY},
        {"/* first /* non-nested */ 1", TF_REPL_UNIT_EXPRESSION},
        {"1 /* open", TF_REPL_UNIT_INCOMPLETE},
        {"fn /* name follows */ f() -> Int { 1 }", TF_REPL_UNIT_DECLARATION},
        {"fn // next line\n() -> Int { 1 }", TF_REPL_UNIT_EXPRESSION},
        {"pub fn main() -> List[Text] { [] }", TF_REPL_UNIT_DECLARATION},
        {"pub record R { x: Int }", TF_REPL_UNIT_DECLARATION},
        {"enum E { A }", TF_REPL_UNIT_DECLARATION},
        {"machine M(x: Int) yields Int rejects Text { state n: Int = 0; next n = x; }",
         TF_REPL_UNIT_DECLARATION},
        {"fn", TF_REPL_UNIT_INCOMPLETE},
        {"pub\n", TF_REPL_UNIT_INCOMPLETE},
        {"pub 1", TF_REPL_UNIT_DECLARATION},
        {"fn f() -> Int\n", TF_REPL_UNIT_INCOMPLETE},
        {"fn broken() -> 42\n", TF_REPL_UNIT_DECLARATION},
        {"let broken ]\n", TF_REPL_UNIT_BINDING},
        {"fn() -> Int", TF_REPL_UNIT_INCOMPLETE},
        {"fn f() -> Int {\n", TF_REPL_UNIT_INCOMPLETE},
        {"record R", TF_REPL_UNIT_INCOMPLETE},
        {"import \"x\" as x // ;", TF_REPL_UNIT_INCOMPLETE},
        {"import \"x;\" as x; // comment", TF_REPL_UNIT_DECLARATION},
        {"let x = { let y = 1; y }", TF_REPL_UNIT_INCOMPLETE},
        {"let x = { let y = 1; y };", TF_REPL_UNIT_BINDING},
        {"letdown + 1", TF_REPL_UNIT_EXPRESSION},
        {"{\"[)}\": ']'}", TF_REPL_UNIT_EXPRESSION},
        {"\"escaped \\\"[\":", TF_REPL_UNIT_EXPRESSION},
        {"\"unicode \xe2\x98\x83 and bracket }\"", TF_REPL_UNIT_EXPRESSION},
        {"[ ( 1 ) /* ] */", TF_REPL_UNIT_INCOMPLETE},
        {"{[ (1) ] }", TF_REPL_UNIT_EXPRESSION},
        {"((((((((((((((((((((1))))))))))))))))))))", TF_REPL_UNIT_EXPRESSION},
        {"((((((((((((((((((((1)))))))))))))))))))", TF_REPL_UNIT_INCOMPLETE},
        {"let x = ([)] /* open", TF_REPL_UNIT_BINDING},
        {"([)] {", TF_REPL_UNIT_EXPRESSION},
        {"} +", TF_REPL_UNIT_EXPRESSION},
        {"*/ (", TF_REPL_UNIT_EXPRESSION},
        {"1 + // note", TF_REPL_UNIT_INCOMPLETE},
        {"1 div\r\n", TF_REPL_UNIT_INCOMPLETE},
        {"1 ^ -", TF_REPL_UNIT_INCOMPLETE},
        {"E::", TF_REPL_UNIT_INCOMPLETE},
        {"x.", TF_REPL_UNIT_INCOMPLETE},
        {"!", TF_REPL_UNIT_INCOMPLETE},
        {"+", TF_REPL_UNIT_EXPRESSION},
        {"1 + 2", TF_REPL_UNIT_EXPRESSION},
        {"1e+", TF_REPL_UNIT_EXPRESSION},
        {"@ (", TF_REPL_UNIT_EXPRESSION},
        {"let x = \"bad\n(", TF_REPL_UNIT_BINDING},
        {"['x\r\n", TF_REPL_UNIT_EXPRESSION},
        {"\"", TF_REPL_UNIT_INCOMPLETE},
        {"\"open\\\"", TF_REPL_UNIT_INCOMPLETE},
        {"'x", TF_REPL_UNIT_INCOMPLETE},
        {"'\\'", TF_REPL_UNIT_INCOMPLETE},
        {"\"bad\\q\"", TF_REPL_UNIT_EXPRESSION},
        {"\"bad\\q\\\\\"", TF_REPL_UNIT_EXPRESSION},
        {"'ab'", TF_REPL_UNIT_EXPRESSION},
        {"let x = ''", TF_REPL_UNIT_BINDING},
        {"\xff", TF_REPL_UNIT_EXPRESSION},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        TfSource source;
        TfStatus status = tf_source_init(&source, "<repl>", cases[i].input,
                                         strlen(cases[i].input), NULL);
        TfReplUnitKind got = tf_repl_classify(&source);
        if (status == TF_HOST_ERROR || got != cases[i].want) {
            fprintf(stderr, "case %zu: got %d, expected %d\n", i,
                    (int)got, (int)cases[i].want);
            ok = false;
        }
        tf_source_destroy(&source);
    }
    if (tf_repl_classify(NULL) != TF_REPL_UNIT_HOST_ERROR || !allocation_failures())
        ok = false;
    if (!ok) return EXIT_FAILURE;
    printf("REPL input: %zu classification cases and allocation failure sweep passed\n",
           sizeof cases / sizeof cases[0]);
    return EXIT_SUCCESS;
}
