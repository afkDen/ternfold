#include "../../experiments/composition/prototype.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(bool ok, int line) {
    if (!ok) { fprintf(stderr, "composition:%d failed\n", line); exit(1); }
}
#define CHECK(x) check((x), __LINE__)
static X01Group pair(void) {
    X01Group g = {0};
    g.count = 2; g.event_limit = X01_EVENTS;
    g.members[0] = (X01Member){.id=97,.minimum=0,.maximum=3,.target=7};
    g.members[1] = (X01Member){.id=122,.minimum=0,.maximum=3,.target=7};
    return g;
}
static void same(X01Result a, X01Result b) {
    CHECK(a.status == b.status && a.member == b.member && a.guard == b.guard && a.fault == b.fault);
    CHECK(a.count == b.count && a.event_count == b.event_count);
    for (size_t i=0; i<a.count; ++i) CHECK(a.state[i] == b.state[i]);
    for (size_t i=0; i<a.event_count; ++i) {
        CHECK(a.events[i].member == b.events[i].member && a.events[i].target == b.events[i].target);
        CHECK(a.events[i].ordinal == b.events[i].ordinal && a.events[i].value == b.events[i].value);
    }
}
static void private_failure(X01Group g, X01Result r) {
    CHECK(r.status != X01_ACCEPT && r.event_count == 0);
    for (size_t i=0; i<g.count; ++i) CHECK(r.state[i] == g.members[i].old);
    for (size_t i=0; i<X01_EVENTS; ++i) CHECK(r.events[i].member == 0);
}
static void observations(X01Group g, X01Result r) {
    if (r.status == X01_ACCEPT) {
        printf("A %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " ",
               g.members[0].old, g.members[1].old, r.state[0], r.state[1]);
        for (size_t i=0; i<r.event_count; ++i)
            printf("%u=%" PRId64 ";", r.events[i].target, r.events[i].value);
        puts("");
    } else {
        CHECK(r.status == X01_REJECT);
        printf("R %u %u %" PRId64 " %" PRId64 " 0\n",
               r.member, r.guard, r.state[0], r.state[1]);
    }
}
static void campaign(bool print) {
    size_t cases=0, accepts=0;
    for (int64_t a=0; a<3; ++a) for (int64_t z=0; z<3; ++z)
        for (int64_t da=-2; da<3; ++da) for (int64_t dz=-2; dz<3; ++dz) {
            X01Group g=pair();
            g.members[0].old=a; g.members[0].delta=da;
            g.members[1].old=z; g.members[1].delta=dz;
            const X01Result r=x01_group(&g);
            same(r,x01_library(&g));
            /* Hand oracle: candidates a+da,z+dz; first lower/upper error wins. */
            unsigned member=0, guard=0;
            if (a+da<0 || a+da>3) { member=97; guard=a+da<0 ? 1u : 2u; }
            else if (z+dz<0 || z+dz>3) { member=122; guard=z+dz<0 ? 1u : 2u; }
            CHECK(r.status == (member ? X01_REJECT : X01_ACCEPT));
            CHECK(r.member == member && r.guard == guard);
            if (member) private_failure(g,r);
            else {
                CHECK(r.state[0] == a+da && r.state[1] == z+dz && r.event_count == 4);
                for (size_t i=0;i<4;++i) {
                    CHECK(r.events[i].member == (i<2 ? 97u : 122u));
                    CHECK(r.events[i].ordinal == i%2 && r.events[i].target == 7);
                    CHECK(r.events[i].value == (i<2 ? a+da : z+dz));
                }
                ++accepts;
            }
            X01Group reversed=g;
            reversed.members[0]=g.members[1]; reversed.members[1]=g.members[0];
            X01Result p=x01_group(&reversed);
            const int64_t temp=p.state[0]; p.state[0]=p.state[1]; p.state[1]=temp;
            same(r,p);
            CHECK(g.members[0].old == a && g.members[1].old == z);
            if (print) observations(g,r);
            ++cases;
        }
    if (!print) printf("PASS bounded equivalence: %zu cases, %zu accepts, %zu rejects; 225 permutations\n",
                       cases, accepts, cases-accepts);
}
static void boundaries(void) {
    CHECK(x01_group(NULL).status == X01_INVALID);
    X01Group g=pair();
    g.count=0; CHECK(x01_group(&g).status == X01_INVALID);
    g.count=X01_MEMBERS+1u; CHECK(x01_group(&g).status == X01_INVALID);
    g=pair(); g.event_limit=X01_EVENTS+1u; CHECK(x01_group(&g).status == X01_INVALID);
    g=pair(); g.members[0].id=0; CHECK(x01_group(&g).status == X01_INVALID);
    g=pair(); g.members[0].minimum=4; CHECK(x01_group(&g).status == X01_INVALID);
    g=pair(); g.members[1].id=97; CHECK(x01_group(&g).status == X01_INVALID);
    g=pair(); g.members[0].dependencies=4; CHECK(x01_group(&g).status == X01_INVALID);
    g=pair(); g.members[0].dependencies=1; CHECK(x01_group(&g).status == X01_CYCLE);
    g=pair(); g.members[0].dependencies=2; g.members[1].dependencies=1;
    CHECK(x01_group(&g).status == X01_CYCLE);
    /* Explicit dependency supersedes ready-name order; z produces one, a uses it. */
    g=pair(); g.members[0].dependencies=2; g.members[0].delta=1; g.members[1].delta=1;
    X01Result r=x01_group(&g);
    CHECK(r.status == X01_ACCEPT && r.state[0] == 2 && r.state[1] == 1);
    CHECK(r.events[0].member == 122 && r.events[2].member == 97);
    CHECK(x01_library(&g).status == X01_INVALID);
    /* Max bound and capacity fault after a private event. */
    g=pair(); g.count=4;
    g.members[2]=g.members[0]; g.members[2].id=65;
    g.members[3]=g.members[0]; g.members[3].id=90;
    r=x01_group(&g); CHECK(r.status == X01_ACCEPT && r.event_count == 8);
    CHECK(r.events[0].member == 65 && r.events[2].member == 90);
    g.event_limit=7; r=x01_group(&g); CHECK(r.fault == 3); private_failure(g,r);
    g=pair(); g.count=1; g.event_limit=1; r=x01_group(&g); private_failure(g,r);
    g.event_limit=2; CHECK(x01_group(&g).status == X01_ACCEPT);
    g=pair(); g.members[0].old=INT64_MAX; g.members[0].delta=1;
    r=x01_group(&g); CHECK(r.status == X01_FAULT && r.fault == 1); private_failure(g,r);
    g.members[0].old=INT64_MIN; g.members[0].delta=-1;
    r=x01_group(&g); CHECK(r.fault == 1); private_failure(g,r);
    g=pair(); g.members[0].maximum=INT64_MAX; g.members[0].old=INT64_MAX;
    g.members[1].delta=1; g.assert_total=true;
    r=x01_group(&g); CHECK(r.fault == 1); private_failure(g,r);
    g=pair(); g.assert_total=true; g.expected_total=1;
    r=x01_group(&g); CHECK(r.status == X01_REJECT && r.member == 0 && r.guard == 3);
    private_failure(g,r); g.expected_total=0; CHECK(x01_group(&g).status == X01_ACCEPT);
    puts("PASS admission, cycles, DAG, ASCII order, capacity, Int64 bounds, runtime assertion");
}
static void counterexamples(void) {
    X01Group g=pair();
    g.members[0].delta=1; g.members[1].delta=-1;
    X01Result r=x01_group(&g); CHECK(r.status == X01_REJECT && r.member == 122);
    private_failure(g,r); same(r,x01_library(&g));
    puts("PASS partial inner accept: joint reject retains both old snapshots and no emissions");
    g=pair(); g.members[0].delta=1; g.members[1].delta=2;
    r=x01_group(&g); CHECK(r.event_count == 4 && r.events[0].target == r.events[2].target);
    CHECK(r.events[0].value == 1 && r.events[2].value == 2);
    puts("PASS conflicting target 7 remains four ordered values: 1,1,2,2; no automatic merge");
    /* Flattening delays a's guard/emission until after z's candidate. */
    g=pair(); g.members[0].emission_fault=2; g.members[1].candidate_fault=1;
    X01Result flat=x01_group(&g), library=x01_library(&g);
    CHECK(flat.fault == 1 && flat.member == 122 && library.fault == 2 && library.member == 97);
    private_failure(g,flat); private_failure(g,library);
    puts("COUNTEREXAMPLE emission/candidate: flattened R001(z), library R002(a)");
    g.members[0].emission_fault=0; g.members[0].predicate_fault=2;
    flat=x01_group(&g); library=x01_library(&g);
    CHECK(flat.fault == 1 && library.fault == 2);
    puts("COUNTEREXAMPLE predicate/candidate: flattened R001(z), library R002(a)");
    g.members[0].predicate_fault=0; g.members[0].reason_fault=2; g.members[0].delta=-1;
    flat=x01_group(&g); library=x01_library(&g);
    CHECK(flat.fault == 1 && library.fault == 2);
    puts("COUNTEREXAMPLE rejection-reason/candidate: flattened R001(z), library R002(a)");
    g.members[0].reason_fault=0;
    flat=x01_group(&g); library=x01_library(&g);
    CHECK(flat.fault == 1 && library.fault == 1); /* eager lets do not stop at Reject */
    g=pair(); g.members[1].emission_fault=2;
    flat=x01_group(&g); library=x01_library(&g);
    CHECK(flat.fault == 2 && library.fault == 2);
    private_failure(g,flat); private_failure(g,library);
    g.members[0].delta=-1;
    flat=x01_group(&g); library=x01_library(&g);
    CHECK(flat.status == X01_REJECT && library.status == X01_FAULT && library.fault == 2);
    puts("COUNTEREXAMPLE reject/late-emission: flattened Reject(a), eager library R002(z)");
}
int main(int argc, char **argv) {
    if (argc==2 && strcmp(argv[1],"--observations")==0) { campaign(true); return 0; }
    if (argc!=1) return 2;
    campaign(false); boundaries(); counterexamples();
    return 0;
}
