#include "ternfold/parser.h"
#include <stdlib.h>
#include <string.h>

static size_t checks;
static void check(bool okay, const char *expression, int line) {
    ++checks;
    if (!okay) { (void)fprintf(stderr,"parser:%d: %s\n",line,expression); exit(1); }
}
#define CHECK(x) check((x),#x,__LINE__)
typedef struct { TfSource source; TfParseResult *result; } Parsed;
static void parse_text(Parsed *p, const char *text, const TfParseOptions *options) {
    TfLexResult lex;
    CHECK(tf_source_init(&p->source,"test.tfd",text,strlen(text),NULL) == TF_OK);
    CHECK(tf_lex(&lex,&p->source,100,NULL) != TF_HOST_ERROR);
    CHECK(tf_parse(&p->result,&lex,options,NULL) == TF_OK);
    tf_lex_destroy(&lex); /* AST must own all decoded literals and token names. */
}
static void destroy(Parsed *p) { tf_parse_destroy(p->result); tf_source_destroy(&p->source); }
static bool has_code(const TfParseResult *r, const char *code) {
    const TfDiagnosticBag *bag = tf_parse_diagnostics(r); size_t i;
    for (i = 0; i < tf_diagnostic_bag_count(bag); ++i)
        if (strcmp(tf_diagnostic_bag_get(bag,i)->code,code) == 0) return true;
    return false;
}
static void diagnostics(const TfParseResult *r) {
    CHECK(tf_diagnostic_bag_write(stderr,tf_parse_diagnostics(r),TF_DIAG_TEXT,TF_DIAG_COLOR_NEVER) == TF_OK);
}
static const TfAstNode *get(const TfAst *a, TfNodeId id) {
    const TfAstNode *n = tf_ast_get(a,id); CHECK(n != NULL); return n;
}
static const TfAstNode *final_expression(const Parsed *p) {
    const TfAst *a = tf_parse_ast(p->result);
    const TfAstNode *program = get(a,tf_ast_root(a));
    const TfAstNode *fn = get(a,program->as.program.declarations.items[0]);
    const TfAstNode *body = get(a,fn->as.function.body);
    CHECK(fn->kind == TF_AST_FUNCTION && body->kind == TF_AST_BLOCK);
    return get(a,body->as.block.final_expression);
}
static void invariants(const Parsed *p) {
    const TfAst *a = tf_parse_ast(p->result); size_t i;
    CHECK(tf_ast_source(a) == &p->source);
    CHECK(tf_ast_count(a) != 0);
    CHECK(get(a,tf_ast_root(a))->kind == TF_AST_PROGRAM);
    for (i = 0; i < tf_ast_count(a); ++i) {
        const TfAstNode *n = tf_ast_at(a,i); TfSpan checked;
        CHECK(n->id.owner == a && n->id.index == i+1);
        CHECK(tf_ast_get(a,n->id) == n);
        CHECK(tf_source_span(&p->source,n->span.start,n->span.end,&checked));
        CHECK(strcmp(tf_ast_kind_name(n->kind),"UNKNOWN") != 0);
        if (n->kind == TF_AST_MISSING) CHECK(n->span.start == n->span.end);
    }
    CHECK(tf_ast_at(a,SIZE_MAX) == NULL);
    CHECK(tf_ast_get(a,(TfNodeId){a,SIZE_MAX}) == NULL);
    CHECK(tf_ast_get(a,(TfNodeId){0}) == NULL);
}
static void expression_case(Parsed *p, const char *expr) {
    char text[4096]; CHECK(snprintf(text,sizeof(text),"fn test() -> Int { %s }",expr) > 0);
    parse_text(p,text,NULL);
    if (tf_diagnostic_phase_kind(tf_parse_phase(p->result)) != TF_DIAG_PHASE_VALID) diagnostics(p->result);
    CHECK(tf_diagnostic_phase_kind(tf_parse_phase(p->result)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_diagnostic_phase_payload(tf_parse_phase(p->result)) == tf_parse_ast(p->result));
    invariants(p);
}
static void test_precedence(void) {
    static const struct { const char *expression; TfTokenKind outer, right; } cases[] = {
        {"1 + 2 * 3",TF_PLUS,TF_STAR},{"1 * 2 ^ 3",TF_STAR,TF_CARET},
        {"1 || 2 && 3",TF_OR_OR,TF_AND_AND},{"1 == 2 < 3",TF_EQUAL_EQUAL,TF_LESS},
        {"1 < 2 + 3",TF_LESS,TF_PLUS},{"1 ++ 2 * 3",TF_PLUS_PLUS,TF_STAR},
        {"2 ^ 3 ^ 4",TF_CARET,TF_CARET},{"1 div 2 ^ 3",TF_DIV,TF_CARET}
    };
    size_t i;
    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        Parsed p; const TfAst *a; const TfAstNode *n, *right;
        expression_case(&p,cases[i].expression); a = tf_parse_ast(p.result); n = final_expression(&p);
        CHECK(n->kind == TF_AST_BINARY && n->as.binary.op == cases[i].outer);
        right = get(a,n->as.binary.right); CHECK(right->kind == TF_AST_BINARY && right->as.binary.op == cases[i].right);
        destroy(&p);
    }
    {
        Parsed p; const TfAst *a; const TfAstNode *n;
        expression_case(&p,"-2 ^ 2"); a = tf_parse_ast(p.result); n = final_expression(&p);
        CHECK(n->kind == TF_AST_UNARY && n->as.unary.op == TF_MINUS);
        CHECK(get(a,n->as.unary.operand)->as.binary.op == TF_CARET); destroy(&p);
        expression_case(&p,"2 ^ -1"); a = tf_parse_ast(p.result); n = final_expression(&p);
        CHECK(n->as.binary.op == TF_CARET && get(a,n->as.binary.right)->kind == TF_AST_UNARY); destroy(&p);
        expression_case(&p,"10 - 3 - 2"); a = tf_parse_ast(p.result); n = final_expression(&p);
        CHECK(n->as.binary.op == TF_MINUS && get(a,n->as.binary.left)->kind == TF_AST_BINARY); destroy(&p);
        expression_case(&p,"!f(1).field ^ -g()(2)"); a = tf_parse_ast(p.result); n = final_expression(&p);
        CHECK(n->kind == TF_AST_UNARY && n->as.unary.op == TF_BANG);
        n = get(a,n->as.unary.operand); CHECK(n->kind == TF_AST_BINARY && n->as.binary.op == TF_CARET);
        CHECK(get(a,n->as.binary.left)->kind == TF_AST_PROJECT); destroy(&p);
        expression_case(&p,"(1 < 2) == true"); CHECK(final_expression(&p)->as.binary.op == TF_EQUAL_EQUAL); destroy(&p);
        expression_case(&p,"next.cell.field"); a = tf_parse_ast(p.result); n = final_expression(&p);
        CHECK(n->kind == TF_AST_PROJECT && get(a,n->as.project.base)->kind == TF_AST_NEXT); destroy(&p);
        expression_case(&p,"-9223372036854775808"); a = tf_parse_ast(p.result); n = final_expression(&p);
        CHECK(n->kind == TF_AST_UNARY);
        CHECK(n->as.unary.operator_span.end == get(a,n->as.unary.operand)->span.start);
        CHECK(get(a,n->as.unary.operand)->as.literal.value.length == 19); destroy(&p);
    }
}
static void test_forms(void) {
    static const char *const expressions[] = {
        "()","[1, 2,]","[]","if x { 1 } else { if y { 2 } else { 3 } }",
        "match x { true => { 1 }, false => { 0 }, _ => { 2 }, }",
        "match x { Enum::V(a, _, -1.2, 'a', \"x\", (), - /*gap*/ 2,) => { a } }",
        "fn (a: List[Int], b: fn(Int) -> Int,) -> Int { b(a) }",
        "for a in xs { for b in ys { a + b } }",
        "R(x: 1, y: 2,)","a::b::c()", "(fn(x: Int) -> Int { x })(1)",
        "{ let x: Option[Int] = Option::Some(1); let _ = f(); x }",
        "\"a\\u{0}\\u{1f642}\"", "'\\u{0}'"
    };
    size_t i;
    for (i = 0; i < sizeof(expressions)/sizeof(expressions[0]); ++i) { Parsed p; expression_case(&p,expressions[i]); destroy(&p); }
    {
        Parsed p; const TfAstNode *n;
        expression_case(&p,"\"a\\u{0}\\u{1f642}\""); n = final_expression(&p);
        CHECK(n->as.literal.value.length == 6);
        CHECK(memcmp(n->as.literal.value.data,"a\0\xf0\x9f\x99\x82",6) == 0);
        CHECK(n->as.literal.value.data[6] == 0); destroy(&p);
    }
}
static void test_examples(void) {
    static const char *const paths[] = {
        "examples/boundaries/candidate_fault.tfd","examples/boundaries/checked_division.tfd","examples/boundaries/minimum_overflow.tfd",
        "examples/course/automata.tfd","examples/course/main_io.tfd","examples/course/operator_inventory.tfd",
        "examples/features/dependency_order.tfd","examples/features/fold_and_closure.tfd","examples/features/guard_short_circuit.tfd",
        "examples/features/nested_patterns.tfd","examples/features/snapshot_swap.tfd","examples/features/unicode.tfd",
        "examples/intro/counter.tfd","examples/intro/hello.tfd","examples/invalid/candidate_cycle.tfd","examples/invalid/closure_candidate.tfd",
        "examples/invalid/duplicate_writer.tfd","examples/invalid/missing_writer.tfd","examples/invalid/non_bool_guard.tfd","examples/invalid/non_exhaustive.tfd",
        "examples/lexical/complete.tfd","examples/showcase/branching_inventory.tfd","examples/showcase/models/shelf.tfd",
        "tests/fixtures/parser/surface.tfd"
    };
    size_t i;
    /* lexical/invalid.tfd intentionally fails lexing; vocabulary.tfd is a token
     * inventory rather than a declaration program. Neither is a parser success
     * fixture. Semantic-invalid programs above must parse without resolution. */
    for (i = 0; i < sizeof(paths)/sizeof(paths[0]); ++i) {
        Parsed p; TfLexResult lex;
        CHECK(tf_source_load(&p.source,paths[i],NULL) == TF_OK);
        CHECK(tf_lex(&lex,&p.source,100,NULL) == TF_OK);
        CHECK(tf_parse(&p.result,&lex,NULL,NULL) == TF_OK); tf_lex_destroy(&lex);
        if (tf_diagnostic_phase_kind(tf_parse_phase(p.result)) != TF_DIAG_PHASE_VALID) {
            (void)fprintf(stderr,"example: %s\n",paths[i]); diagnostics(p.result);
        }
        CHECK(tf_diagnostic_phase_kind(tf_parse_phase(p.result)) == TF_DIAG_PHASE_VALID);
        invariants(&p); destroy(&p);
    }
}
static void test_recovery(void) {
    static const struct { const char *text, *code; } invalid[] = {
        {"fn f() -> Int { 1 < 2 < 3 }","E111"},{"fn f() -> Int { 1 == 2 != 3 }","E111"},
        {"fn f() -> Int { R(x: 1, 2) }","E112"},{"fn f() -> Int { R(1, x: 2) }","E112"},
        {"fn f() -> Int { 1; }","E110"},{"fn f() -> Int {}","E110"},
        {"fn f(a: Int -> Int { a } fn g() -> Int { 2 }","E110"},
        {"fn f() -> Int { let a = ; let b = ; 0 }","E110"},
        {"fn f() -> Int { output \"x\"; 0 }","E208"},
        {"pub fn main() -> List[Text] { { input x: Text; x } }","E208"},
        {"pub fn main() -> List[Text] { input _: Text; [] }","E208"},
        {"pub fn main() -> List[Text] { input Int: Text; [] }","E208"},
        {"pub fn main() -> List[Text] { input stdin: Text; [] }","E208"},
        {"pub fn main() -> List[Text] { input x: Text from; [] }","E110"},
        {"pub fn main() -> List[Text] { output \"x\" to; [] }","E110"},
        {"fn f() -> Int { for _ in xs { 1 } }","E110"},
        {"fn f() -> Int { 0 } import \"x\" as x;","E110"},
        {"pub import \"x\" as x;","E110"},{"enum Empty {}","E110"},
        {"machine M(x: Int,) yields Int rejects Text { state a: Int = 0; next a = 1; }","E110"},
        {"machine M(x: Int) yields Int rejects Text { next a = 1; state a: Int = 0; }","E110"},
        {"fn f() -> Int { @ }","E101"}, {"garbage (x { y; }); fn g() -> Int { 1 }","E110"}
    };
    size_t i;
    for (i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        Parsed p; parse_text(&p,invalid[i].text,NULL);
        CHECK(tf_diagnostic_phase_kind(tf_parse_phase(p.result)) == TF_DIAG_PHASE_ERROR);
        CHECK(tf_diagnostic_phase_payload(tf_parse_phase(p.result)) == NULL);
        if (!has_code(p.result,invalid[i].code)) diagnostics(p.result);
        CHECK(has_code(p.result,invalid[i].code)); invariants(&p); destroy(&p);
    }
    {
        Parsed p; const TfAst *a; const TfAstNode *program;
        parse_text(&p,"fn first() -> Int { 1 fn second() -> Int { 2 }",NULL);
        a = tf_parse_ast(p.result); program = get(a,tf_ast_root(a));
        CHECK(program->as.program.declarations.count == 2);
        CHECK(get(a,program->as.program.declarations.items[1])->kind == TF_AST_FUNCTION);
        CHECK(has_code(p.result,"E110")); invariants(&p); destroy(&p);
    }
    {
        char text[4096]; size_t used = 0; Parsed p; TfParseOptions options = {true,64,0};
        for (i = 0; i < 150; ++i) { memcpy(text+used,"bad;",4); used += 4; } text[used] = 0;
        parse_text(&p,text,NULL);
        CHECK(tf_diagnostic_bag_count(tf_parse_diagnostics(p.result)) == 100);
        CHECK(tf_diagnostic_bag_suppressed(tf_parse_diagnostics(p.result)) == 50);
        CHECK(tf_diagnostic_bag_has_errors(tf_parse_diagnostics(p.result))); invariants(&p); destroy(&p);
        parse_text(&p,text,&options); CHECK(tf_diagnostic_bag_count(tf_parse_diagnostics(p.result)) == 0);
        CHECK(tf_diagnostic_bag_suppressed(tf_parse_diagnostics(p.result)) == 150);
        CHECK(tf_diagnostic_phase_kind(tf_parse_phase(p.result)) == TF_DIAG_PHASE_ERROR); destroy(&p);
    }
}
typedef struct { size_t calls, fail_at, live; } Memory;
static void *allocate(void *context, size_t size) {
    Memory *m = context; void *p; ++m->calls; if (m->calls == m->fail_at) return NULL;
    p = malloc(size); if (p != NULL) ++m->live; return p;
}
static void deallocate(void *context, void *p) { Memory *m = context; CHECK(p != NULL && m->live != 0); --m->live; free(p); }
static void test_failures(void) {
    static const char *const cases[] = {"pub fn main() -> List[Text] { let x = \"a\\u{0}\"; [x] }",
        "fn f(a: Int -> Int { let x = ; 1 < 2 < 3; } garbage; fn g() -> Int { 2 }",
        "fn broken() -> Int { 1 junk({[ fn good() -> Int { 1 }",NULL};
    size_t which, position;
    for (which = 0; which < sizeof(cases)/sizeof(cases[0]); ++which) {
        TfSource source; TfLexResult lex; bool success = false;
        if (cases[which] != NULL) CHECK(tf_source_init(&source,"oom",cases[which],strlen(cases[which]),NULL) == TF_OK);
        else CHECK(tf_source_load(&source,"tests/fixtures/parser/surface.tfd",NULL) == TF_OK);
        CHECK(tf_lex(&lex,&source,100,NULL) == TF_OK);
        for (position = 1; position < 2000; ++position) {
            Memory m = {0,position,0}; TfAllocator allocator = {&m,allocate,deallocate}; TfParseResult *result = NULL;
            TfStatus status = tf_parse(&result,&lex,NULL,&allocator);
            if (status == TF_HOST_ERROR) CHECK(result == NULL);
            else { CHECK(status == TF_OK && result != NULL); success = true; }
            tf_parse_destroy(result); CHECK(m.live == 0); if (success) break;
        }
        CHECK(success); tf_lex_destroy(&lex); tf_source_destroy(&source);
    }
    {
        TfSource source; TfLexResult lex; TfParseResult *result; TfParseOptions o = {true,1,100};
        static const char nested[] = "fn f() -> Int { (((1))) }";
        CHECK(tf_source_init(&source,"depth",nested,sizeof(nested)-1,NULL) == TF_OK);
        CHECK(tf_lex(&lex,&source,100,NULL) != TF_HOST_ERROR);
        CHECK(tf_parse(&result,&lex,&o,NULL) == TF_HOST_ERROR && result == NULL);
        o.nesting_limit = 129; CHECK(tf_parse(&result,&lex,&o,NULL) == TF_HOST_ERROR);
        o.nesting_limit = 64; o.diagnostic_cap = 101; CHECK(tf_parse(&result,&lex,&o,NULL) == TF_HOST_ERROR);
        CHECK(tf_parse(NULL,&lex,NULL,NULL) == TF_HOST_ERROR);
        CHECK(tf_parse(&result,NULL,NULL,NULL) == TF_HOST_ERROR && result == NULL);
        {
            TfLexResult forged = lex; TfAllocator invalid = {0};
            CHECK(tf_parse(&result,&lex,NULL,&invalid) == TF_HOST_ERROR && result == NULL);
            forged.token_count = 0; CHECK(tf_parse(&result,&forged,NULL,NULL) == TF_HOST_ERROR && result == NULL);
            forged = lex; forged.source = NULL; CHECK(tf_parse(&result,&forged,NULL,NULL) == TF_HOST_ERROR && result == NULL);
        }
        tf_lex_destroy(&lex); tf_source_destroy(&source);
    }
    tf_parse_destroy(NULL);
    CHECK(tf_parse_ast(NULL) == NULL && tf_parse_diagnostics(NULL) == NULL && tf_parse_phase(NULL) == NULL);
    CHECK(tf_ast_get(NULL,(TfNodeId){0}) == NULL && tf_ast_at(NULL,0) == NULL);
    CHECK(tf_ast_count(NULL) == 0 && tf_ast_root(NULL).index == 0 && tf_ast_source(NULL) == NULL);
}
static void test_root_policy_and_ids(void) {
    Parsed root, imported; TfParseOptions options = {false,64,100}; const TfAst *a, *b;
    const char *text = "pub fn main() -> List[Text] { input x: Text from stdin; output x to stdout; [] }";
    parse_text(&root,text,NULL); parse_text(&imported,text,&options);
    CHECK(tf_diagnostic_phase_kind(tf_parse_phase(root.result)) == TF_DIAG_PHASE_VALID);
    CHECK(has_code(imported.result,"E208")); a = tf_parse_ast(root.result); b = tf_parse_ast(imported.result);
    CHECK(tf_ast_get(a,tf_ast_root(b)) == NULL); CHECK(tf_ast_get(b,tf_ast_root(a)) == NULL);
    destroy(&imported); destroy(&root);
}
static void test_review_recovery(void) {
    Parsed p; const TfAst *a; const TfAstNode *program, *last;
    parse_text(&p,"fn broken() -> Int { f( fn good() -> Int { 1 }",NULL);
    a = tf_parse_ast(p.result); program = get(a,tf_ast_root(a));
    CHECK(program->as.program.declarations.count == 2);
    last = get(a,program->as.program.declarations.items[1]);
    CHECK(last->kind == TF_AST_FUNCTION && last->as.function.name.length == 4 &&
          memcmp(last->as.function.name.data,"good",4) == 0);
    CHECK(has_code(p.result,"E110")); invariants(&p); destroy(&p);
    parse_text(&p,"machine M(x: Int) yields Int rejects Text { state a: Int = 0; next a = 1; input y: Text; output y; }",NULL);
    CHECK(has_code(p.result,"E208")); invariants(&p); destroy(&p);
    parse_text(&p,"fn f() -> Int { @ }",NULL);
    CHECK(has_code(p.result,"E101") && !has_code(p.result,"E110"));
    CHECK(tf_diagnostic_bag_count(tf_parse_diagnostics(p.result)) == 1); destroy(&p);
    {
        static const char openers[] = {'(', '[', '{'};
        static const TfTokenKind expected[] = {TF_RPAREN, TF_RBRACKET, TF_RBRACE};
        for(size_t i=0;i<sizeof(openers);++i){
            char text[128];size_t insertion;bool missing=false;
            CHECK(snprintf(text,sizeof(text),"fn broken() -> Int { 1 junk%c fn good() -> Int { 1 }",openers[i])>0);
            insertion=(size_t)(strstr(text,"fn good")-text);parse_text(&p,text,NULL);
            a=tf_parse_ast(p.result);program=get(a,tf_ast_root(a));
            CHECK(program->as.program.declarations.count==2);
            last=get(a,program->as.program.declarations.items[1]);
            CHECK(last->kind==TF_AST_FUNCTION && last->as.function.name.length==4 &&
                  memcmp(last->as.function.name.data,"good",4)==0);
            for(size_t j=0;j<tf_ast_count(a);++j){
                const TfAstNode *n=tf_ast_at(a,j);
                if(n->kind==TF_AST_MISSING&&n->as.missing.expected==expected[i]&&n->span.start==insertion){
                    CHECK(n->span.end==insertion);missing=true;
                }
            }
            CHECK(missing);CHECK(has_code(p.result,"E110"));invariants(&p);destroy(&p);
        }
        {
            const char *text="fn broken() -> Int { 1 junk({[ fn good() -> Int { 1 }";
            const TfTokenKind order[]={TF_RBRACKET,TF_RBRACE,TF_RPAREN};size_t count=0;
            size_t insertion=(size_t)(strstr(text,"fn good")-text);
            parse_text(&p,text,NULL);a=tf_parse_ast(p.result);program=get(a,tf_ast_root(a));
            CHECK(program->as.program.declarations.count==2);
            for(size_t j=0;j<tf_ast_count(a)&&count<3;++j){
                const TfAstNode *n=tf_ast_at(a,j);
                if(n->kind==TF_AST_MISSING&&n->span.start==insertion){
                    CHECK(n->as.missing.expected==order[count++]);CHECK(n->span.end==insertion);
                }
            }
            CHECK(count==3);invariants(&p);destroy(&p);
        }
        {
            TfSource source;TfLexResult lex;TfParseResult *result=NULL;char text[256];size_t used=0;
            const char *prefix="fn broken() -> Int { 1 junk";
            memcpy(text,prefix,strlen(prefix));used=strlen(prefix);
            for(size_t j=0;j<129;++j)text[used++]='(';
            text[used]=0;
            CHECK(tf_source_init(&source,"recovery-depth",text,used,NULL)==TF_OK);
            CHECK(tf_lex(&lex,&source,100,NULL)==TF_OK);
            CHECK(tf_parse(&result,&lex,NULL,NULL)==TF_HOST_ERROR&&result==NULL);
            tf_lex_destroy(&lex);tf_source_destroy(&source);
        }
    }
}
static bool ast_text_is(TfAstText text, const char *expected) {
    return text.length == strlen(expected) && memcmp(text.data,expected,text.length) == 0;
}
static void test_structural_surface(void) {
    Parsed p; TfLexResult lex; const TfAst *a; const TfAstNode *program, *n, *child, *block_node;
    const unsigned char *old_bytes; unsigned char *saved; size_t i;
    CHECK(tf_source_load(&p.source,"tests/fixtures/parser/surface.tfd",NULL) == TF_OK);
    saved = malloc(p.source.length); CHECK(saved != NULL); memcpy(saved,p.source.bytes,p.source.length); old_bytes = p.source.bytes;
    CHECK(tf_lex(&lex,&p.source,100,NULL) == TF_OK); CHECK(tf_parse(&p.result,&lex,NULL,NULL) == TF_OK);
    tf_lex_destroy(&lex); a = tf_parse_ast(p.result); program = get(a,tf_ast_root(a));
    CHECK(tf_diagnostic_phase_kind(tf_parse_phase(p.result)) == TF_DIAG_PHASE_VALID);
    CHECK(program->as.program.declarations.count == 6);
    n = get(a,program->as.program.declarations.items[0]);
    CHECK(n->kind == TF_AST_IMPORT && ast_text_is(n->as.import_decl.path,"models/x.tfd") && ast_text_is(n->as.import_decl.alias,"m"));
    n = get(a,program->as.program.declarations.items[1]);
    CHECK(n->kind == TF_AST_RECORD && n->as.aggregate.public_decl && n->as.aggregate.members.count == 2);
    child = get(a,n->as.aggregate.members.items[1]); CHECK(child->kind == TF_AST_PARAMETER && ast_text_is(child->as.parameter.name,"right"));
    child = get(a,child->as.parameter.type); CHECK(child->kind == TF_AST_TYPE_PATH && child->as.type_path.arguments.count == 1);
    CHECK(ast_text_is(get(a,child->as.type_path.segments.items[0])->as.path.name,"List"));
    n = get(a,program->as.program.declarations.items[2]); CHECK(n->kind == TF_AST_ENUM && n->as.aggregate.members.count == 2);
    child = get(a,n->as.aggregate.members.items[1]); CHECK(child->kind == TF_AST_VARIANT && child->as.variant.parentheses && child->as.variant.types.count == 2);
    n = get(a,program->as.program.declarations.items[3]); CHECK(n->kind == TF_AST_MACHINE && n->as.machine.public_decl);
    CHECK(n->as.machine.sections.count == 4);
    CHECK(get(a,n->as.machine.parameter)->kind == TF_AST_PARAMETER);
    CHECK(get(a,n->as.machine.sections.items[0])->kind == TF_AST_STATE);
    CHECK(get(a,n->as.machine.sections.items[1])->kind == TF_AST_WRITER);
    child = get(a,n->as.machine.sections.items[2]); CHECK(child->kind == TF_AST_GUARD && get(a,child->as.guard.condition)->kind == TF_AST_IF);
    CHECK(get(a,n->as.machine.sections.items[3])->kind == TF_AST_EMIT);
    n = get(a,program->as.program.declarations.items[4]); CHECK(n->kind == TF_AST_FUNCTION && !n->as.function.public_decl && n->as.function.parameters.count == 2);
    child = get(a,get(a,n->as.function.parameters.items[0])->as.parameter.type);
    CHECK(child->kind == TF_AST_TYPE_FUNCTION && child->as.type_function.parameters.count == 2);
    child = get(a,get(a,n->as.function.parameters.items[1])->as.parameter.type);
    CHECK(child->kind == TF_AST_TYPE_PATH && child->as.type_path.arguments.count == 3);
    block_node = get(a,n->as.function.body); CHECK(block_node->as.block.statements.count == 3);
    child = get(a,get(a,block_node->as.block.statements.items[0])->as.binding.value);
    CHECK(child->kind == TF_AST_CALL && child->as.call.arguments.count == 2);
    CHECK(get(a,child->as.call.arguments.items[0])->as.argument.named);
    child = get(a,get(a,block_node->as.block.statements.items[1])->as.binding.value);
    CHECK(child->kind == TF_AST_FOR && ast_text_is(child->as.loop.name,"row"));
    child = get(a,get(a,child->as.loop.body)->as.block.final_expression); CHECK(child->kind == TF_AST_FOR);
    child = get(a,get(a,block_node->as.block.statements.items[2])->as.binding.value); CHECK(child->kind == TF_AST_CLOSURE);
    child = get(a,block_node->as.block.final_expression); CHECK(child->kind == TF_AST_MATCH && child->as.matching.arms.count == 3);
    child = get(a,get(a,child->as.matching.arms.items[1])->as.arm.pattern); CHECK(child->kind == TF_AST_PATTERN_VARIANT && child->as.pattern_variant.arguments.count == 2);
    child = get(a,child->as.pattern_variant.arguments.items[1]); CHECK(child->kind == TF_AST_PATTERN_VARIANT);
    child = get(a,child->as.pattern_variant.arguments.items[0]); CHECK(child->kind == TF_AST_PATTERN_LITERAL && child->as.literal.negative);
    CHECK(child->as.literal.minus.end == child->as.literal.value.span.start);
    CHECK(ast_text_is(child->as.literal.value,"9223372036854775808"));
    n = get(a,program->as.program.declarations.items[5]); block_node = get(a,n->as.function.body);
    CHECK(block_node->as.block.statements.count == 2);
    child = get(a,block_node->as.block.statements.items[0]); CHECK(child->kind == TF_AST_INPUT && child->as.input.from_stdin);
    child = get(a,block_node->as.block.statements.items[1]); CHECK(child->kind == TF_AST_OUTPUT && child->as.output.to_stdout);
    CHECK(p.source.bytes == old_bytes && memcmp(saved,p.source.bytes,p.source.length) == 0);
    /* Leaf path contract used by the independent dump consumer. */
    for (i = 0; i < tf_ast_count(a); ++i) {
        n = tf_ast_at(a,i);
        if (n->kind == TF_AST_PATH && n->as.path.segments.count == 0) CHECK(n->as.path.name.data != NULL);
    }
    free(saved); invariants(&p); destroy(&p);
}
static void test_malformed_corpus(void) {
    static const char *const fragments[] = {"fn ","pub ","record ","enum ","machine ","x ","Int ","0 ","\"x\" ",
        "( ",") ","{ ","} ","[ ","] ",", ","; ",": ","-> ","=> ","let ","= ","+ ","if ","else ",
        "match ","_ ","next ",". ","input ","output ","from ","stdin ","for ","in ","@ ","state ","guard ","emit "};
    uint32_t seed = UINT32_C(0x04a57c17); size_t sample;
    for (sample = 0; sample < 256; ++sample) {
        char text[1024]; size_t used = 0, i; Parsed p;
        for (i = 0; i < 32; ++i) {
            const char *f; size_t length;
            seed = seed*UINT32_C(1664525)+UINT32_C(1013904223);
            f = fragments[seed % (sizeof(fragments)/sizeof(fragments[0]))]; length = strlen(f);
            CHECK(used+length < sizeof(text)); memcpy(text+used,f,length); used += length;
        }
        text[used] = 0; parse_text(&p,text,NULL);
        CHECK(tf_diagnostic_bag_count(tf_parse_diagnostics(p.result)) <= 100);
        CHECK(tf_ast_count(tf_parse_ast(p.result)) < 2048); invariants(&p); destroy(&p);
    }
    /* Truncate each byte boundary of a representative ASCII declaration. */
    {
        static const char text[] = "fn f(x: List[Int]) -> Int { let y = f(1, 2); match x { V::Some(a) => { -a ^ 2 }, _ => { 0 } } }";
        size_t length;
        for (length = 0; length < sizeof(text); ++length) {
            char prefix[sizeof(text)]; Parsed p;
            memcpy(prefix,text,length); prefix[length] = 0; parse_text(&p,prefix,NULL); invariants(&p); destroy(&p);
        }
    }
}
static void test_remaining_boundaries(void) {
    static const char *const good[] = {
        "record Empty {} enum E { V(fn() -> List[Int], Option[Int]), W } fn f() -> fn() -> Int { fn() -> Int { 1 } }",
        "fn f(x: User[One, Two], y: Option[Int, Text], z: a::b::c::d) -> Decision[Int] { () }",
        "pub fn main() -> Int { input x: Text; output x; () }",
        "fn f() -> Int { match x { - /*comment*/ 9223372036854775808 => { 1 }, 1.2 => { 2 }, 'a' => { 3 } } }"
    };
    static const char *const bad[] = {
        "fn f() -> Int { f(1, fn good() -> Int { 1 }",
        "fn f() -> Int { [1 + fn good() -> Int { 1 }",
        "fn f(a: fn good() -> Int { 1 }",
        "fn f() -> Int { if x { 1 } else if y { 2 } else { 3 } }",
        "pub fn main(x: Int) -> Int { output x; 0 }",
        "fn main() -> Int { input x: Text; 0 }",
        "pub fn main() -> Int { for x in xs { output x; x } }",
        "pub fn main() -> Int { let f = fn() -> Int { output \"x\"; 0 }; 0 }",
        "fn f() -> Int { (1, 2) }", "fn f() -> Int { match x { E::V() => { 1 } } }",
        "fn f() -> Int { match x {} }", "fn f() -> Int { 2 ^ }"
    };
    size_t i;
    for (i = 0; i < sizeof(good)/sizeof(good[0]); ++i) {
        Parsed p; parse_text(&p,good[i],NULL);
        CHECK(tf_diagnostic_phase_kind(tf_parse_phase(p.result)) == TF_DIAG_PHASE_VALID); invariants(&p); destroy(&p);
    }
    for (i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        Parsed p; parse_text(&p,bad[i],NULL);
        CHECK(tf_diagnostic_phase_kind(tf_parse_phase(p.result)) == TF_DIAG_PHASE_ERROR); invariants(&p); destroy(&p);
    }
    {
        char text[2048]; size_t used, j; TfSource source; TfLexResult lex; TfParseResult *result;
        Memory memory = {0}; TfAllocator a = {&memory,allocate,deallocate};
        const char prefix[] = "fn f() -> Int { ";
        used = sizeof(prefix)-1; memcpy(text,prefix,used);
        for (j = 0; j < 200; ++j) text[used++] = '(';
        text[used++] = '1';
        for (j = 0; j < 200; ++j) text[used++] = ')';
        text[used++] = '}'; text[used] = 0;
        CHECK(tf_source_init(&source,"limit",text,used,NULL) == TF_OK);
        CHECK(tf_lex(&lex,&source,100,NULL) == TF_OK);
        CHECK(tf_parse(&result,&lex,NULL,&a) == TF_HOST_ERROR && result == NULL);
        CHECK(memory.live == 0); tf_lex_destroy(&lex); tf_source_destroy(&source);
    }
}
int main(void) {
    test_review_recovery();
    test_precedence(); test_forms(); test_examples(); test_recovery(); test_failures(); test_root_policy_and_ids();
    test_structural_surface(); test_malformed_corpus();
    test_remaining_boundaries();
    (void)printf("parser: %zu checks passed\n",checks); return 0;
}
