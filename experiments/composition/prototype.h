#ifndef X01_PROTOTYPE_H
#define X01_PROTOTYPE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Independent finite model, never a source/Core executor. All arguments borrow;
 * the returned record owns inline copies. IDs are ASCII bytes, not array slots. */
enum { X01_MEMBERS = 4, X01_EVENTS = 8 };
typedef enum { X01_ACCEPT, X01_REJECT, X01_FAULT, X01_INVALID, X01_CYCLE } X01Status;
typedef struct {
    unsigned id, dependencies; /* dependency bits index the input array */
    int64_t old, delta, minimum, maximum;
    unsigned candidate_fault, predicate_fault, reason_fault, emission_fault;
    unsigned target; /* application-defined command destination, no language effect */
} X01Member;
typedef struct {
    size_t count, event_limit;
    X01Member members[X01_MEMBERS];
    bool assert_total;
    int64_t expected_total;
} X01Group;
typedef struct { unsigned member, target, ordinal; int64_t value; } X01Event;
typedef struct {
    X01Status status;
    unsigned member, guard, fault;
    size_t count, event_count;
    int64_t state[X01_MEMBERS]; /* input slot order; old on any unsuccessful return */
    X01Event events[X01_EVENTS];
} X01Result;

/* Hypothesis: all DAG candidates, then member-ordered guards, optional runtime
 * assertion, then ordered emissions. Never asserts a static proof. */
X01Result x01_group(const X01Group *group);
/* Existing-library strategy: finish each independent child step in ASCII order,
 * retain its decision privately, then explicitly reject or concatenate lists.
 * Dependencies/assertions are outside this restricted comparison domain. */
X01Result x01_library(const X01Group *group);
#endif
