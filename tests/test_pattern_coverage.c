#include "../src/pattern_coverage.h"

#include <stdlib.h>

static size_t checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { fprintf(stderr,"test_pattern_coverage:%d: %s\n",__LINE__,#condition); exit(1); } \
} while (0)

/* Option[Bool], with an independent three-value truth table below. */
static const TfCoverageDomain boolean, option, integer;
static const TfCoverageConstructor no = {&boolean,NULL,0}, yes = {&boolean,NULL,0};
static const TfCoverageConstructor *const booleans[] = {&no,&yes};
static const TfCoverageDomain boolean = {booleans,2};
static const TfCoverageDomain *const some_payload[] = {&boolean};
static const TfCoverageConstructor none = {&option,NULL,0}, some = {&option,some_payload,1};
static const TfCoverageConstructor *const options[] = {&none,&some};
static const TfCoverageDomain option = {options,2};
static const TfCoverageDomain integer = {NULL,0};
static const TfCoverageConstructor zero = {&integer,NULL,0}, one = {&integer,NULL,0};
static const TfCoveragePattern p_no = {&no,NULL}, p_yes = {&yes,NULL};
static const TfCoveragePattern *const no_arg[] = {&p_no}, *const yes_arg[] = {&p_yes};
static const TfCoveragePattern *const wild_arg[] = {NULL};
static const TfCoveragePattern p_none = {&none,NULL}, p_some_no = {&some,no_arg};
static const TfCoveragePattern p_some_yes = {&some,yes_arg}, p_some_any = {&some,wild_arg};
static const TfCoveragePattern p_zero = {&zero,NULL}, p_one = {&one,NULL};

static unsigned inhabitants(const TfCoveragePattern *pattern) {
    if (pattern == NULL) return 7;
    if (pattern->constructor == &none) return 1;
    CHECK(pattern->constructor == &some && pattern->arguments != NULL);
    if (pattern->arguments[0] == NULL) return 6;
    if (pattern->arguments[0]->constructor == &no) return 2;
    CHECK(pattern->arguments[0]->constructor == &yes);
    return 4;
}

static void finite_oracle(void) {
    const TfCoveragePattern *const candidates[] = {&p_none,&p_some_no,&p_some_yes,&p_some_any,NULL};
    const unsigned masks[] = {1,2,4,6,7};
    for (unsigned selected=0; selected<32; ++selected) {
        const TfCoveragePattern *rows[5];
        size_t count = 0;
        unsigned covered = 0;
        for (size_t i=0; i<5; ++i) {
            if ((selected & (1u << i)) != 0) {
                rows[count++] = candidates[i];
                covered |= masks[i];
            }
        }
        for (size_t i=0; i<5; ++i) {
            const unsigned expected = masks[i] & ~covered;
            TfCoverageWitness *w = NULL;
            CHECK(tf_coverage_witness(&w,&option,rows,count,candidates[i],NULL) == TF_OK);
            CHECK((w != NULL) == (expected != 0));
            if (w != NULL) {
                const unsigned matches = inhabitants(tf_coverage_root(w));
                CHECK(matches != 0 && (matches & expected) == matches);
            }
            tf_coverage_destroy(w);
        }
    }
}

/* A product of two finite columns needs specialization across both columns.
 * Neither column's individual completeness implies product coverage. */
static void correlated_columns(void) {
    const TfCoverageDomain *const payload[] = {&boolean,&boolean};
    /* The descriptor points at its actual finite domain, built in two steps. */
    TfCoverageDomain domain = {NULL,1};
    const TfCoverageConstructor actual_ctor = {&domain,payload,2};
    const TfCoverageConstructor *const actual_constructors[] = {&actual_ctor};
    const TfCoveragePattern *const ff[] = {&p_no,&p_no};
    const TfCoveragePattern *const tt[] = {&p_yes,&p_yes};
    const TfCoveragePattern *const ft[] = {&p_no,&p_yes};
    const TfCoveragePattern *const tf[] = {&p_yes,&p_no};
    const TfCoveragePattern a = {&actual_ctor,ff}, b = {&actual_ctor,tt};
    const TfCoveragePattern c = {&actual_ctor,ft}, d = {&actual_ctor,tf};
    const TfCoveragePattern *rows[] = {&a,&b,&c,&d};
    const TfCoveragePattern *p;
    TfCoverageWitness *w = NULL;
    domain.constructors = actual_constructors;
    CHECK(tf_coverage_witness(&w,&domain,rows,2,NULL,NULL) == TF_OK && w != NULL);
    p = tf_coverage_root(w);
    CHECK(p != NULL && p->constructor == &actual_ctor);
    CHECK(p->arguments[0]->constructor == &no && p->arguments[1]->constructor == &yes);
    tf_coverage_destroy(w);
    CHECK(tf_coverage_witness(&w,&domain,rows,4,NULL,NULL) == TF_OK && w == NULL);
}

typedef struct { size_t calls, fail_at, live; } Counter;
static void *counted_allocate(void *context, size_t bytes) {
    Counter *c = context;
    void *p;
    if (++c->calls == c->fail_at) return NULL;
    p = malloc(bytes);
    if (p != NULL) ++c->live;
    return p;
}
static void counted_release(void *context, void *p) {
    Counter *c = context;
    CHECK(c->live != 0);
    --c->live;
    free(p);
}
static void open_and_failure(void) {
    const TfCoveragePattern *const literals[] = {&p_zero,&p_one};
    const TfCoveragePattern *const partial[] = {&p_none,&p_some_no};
    TfCoverageWitness *w = NULL;
    Counter c = {0};
    TfAllocator a = {&c,counted_allocate,counted_release};
    size_t allocations;
    CHECK(tf_coverage_witness(&w,&integer,literals,2,&p_zero,NULL) == TF_OK && w == NULL);
    CHECK(tf_coverage_witness(&w,&integer,literals,2,NULL,NULL) == TF_OK && w != NULL);
    CHECK(tf_coverage_root(w) == NULL);
    tf_coverage_destroy(w);
    CHECK(tf_coverage_witness(&w,&option,partial,2,NULL,&a) == TF_OK && w != NULL);
    allocations = c.calls;
    tf_coverage_destroy(w);
    CHECK(c.live == 0 && allocations > 1);
    for (size_t i=1; i<=allocations; ++i) {
        c = (Counter){0,i,0};
        CHECK(tf_coverage_witness(&w,&option,partial,2,NULL,&a) == TF_HOST_ERROR);
        CHECK(w == NULL && c.live == 0);
    }
    printf("pattern witness: %zu allocation failures exercised\n",allocations);
}

/* Correlation in the final column covers the whole product. Splitting the
 * sparse prefix first revisits the same proof exponentially. */
static void wide_product(void) {
    enum { WIDTH = 540, ARMS = 26 };
    const TfCoverageDomain *payload[WIDTH];
    const TfCoveragePattern *args[ARMS][WIDTH];
    const TfCoveragePattern *rows[ARMS];
    TfCoveragePattern patterns[ARMS];
    TfCoverageDomain domain;
    const TfCoverageConstructor ctor = {&domain,payload,WIDTH};
    const TfCoverageConstructor *const ctors[] = {&ctor};
    TfCoverageWitness *w = NULL;
    Counter c = {0};
    const TfAllocator a = {&c,counted_allocate,counted_release};
    domain = (TfCoverageDomain){ctors,1};
    for (size_t j=0; j<WIDTH; ++j) payload[j] = &boolean;
    for (size_t i=0; i<ARMS; ++i) {
        for (size_t j=0; j<WIDTH; ++j) args[i][j] = NULL;
        if (i < ARMS-2) args[i][i] = &p_yes;
        args[i][WIDTH-1] = i == ARMS-1 ? &p_no : &p_yes;
        patterns[i] = (TfCoveragePattern){&ctor,args[i]};
        rows[i] = &patterns[i];
    }
    CHECK(tf_coverage_witness(&w,&domain,rows,ARMS,NULL,&a) == TF_OK && w == NULL);
    CHECK(c.calls < 1000 && c.live == 0);
    CHECK(tf_coverage_witness(&w,&domain,rows,ARMS-1,NULL,&a) == TF_OK && w != NULL);
    CHECK(tf_coverage_root(w)->arguments[WIDTH-1]->constructor == &no);
    CHECK(tf_coverage_root(w)->arguments[0] == NULL);
    tf_coverage_destroy(w);
    CHECK(c.live == 0);
    for (size_t j=0; j<WIDTH; ++j) {
        args[0][j] = &p_yes;
        args[1][j] = &p_yes;
    }
    /* Different pattern nodes with identical dense payloads are duplicates. */
    CHECK(tf_coverage_witness(&w,&domain,rows,1,&patterns[1],&a) == TF_OK && w == NULL);
    CHECK(c.live == 0);
}

int main(void) {
    finite_oracle();
    correlated_columns();
    open_and_failure();
    wide_product();
    printf("pattern witness: %zu checks passed\n",checks);
    return 0;
}
