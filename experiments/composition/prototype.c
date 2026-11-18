#include "prototype.h"
#include <limits.h>

static bool add(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
    *out = a + b;
    return true;
}
static X01Result initial(const X01Group *g) {
    X01Result r = {0};
    r.status = X01_INVALID;
    if (g && g->count <= X01_MEMBERS) {
        r.count = g->count;
        for (size_t i = 0; i < g->count; ++i) r.state[i] = g->members[i].old;
    }
    return r;
}
static X01Status schedule(const X01Group *g, size_t order[X01_MEMBERS]) {
    if (!g || !g->count || g->count > X01_MEMBERS || g->event_limit > X01_EVENTS)
        return X01_INVALID;
    const unsigned mask = (1u << (unsigned)g->count) - 1u;
    for (size_t i = 0; i < g->count; ++i) {
        const X01Member *m = &g->members[i];
        if (m->id < 65u || m->id > 122u || (m->id > 90u && m->id < 97u) ||
            (m->dependencies & ~mask) || m->minimum > m->maximum)
            return X01_INVALID;
        for (size_t j = 0; j < i; ++j)
            if (m->id == g->members[j].id) return X01_INVALID;
    }
    unsigned done = 0;
    for (size_t k = 0; k < g->count; ++k) {
        size_t best = g->count;
        for (size_t i = 0; i < g->count; ++i) {
            const unsigned bit = 1u << (unsigned)i;
            if (!(done & bit) && !(g->members[i].dependencies & ~done) &&
                (best == g->count || g->members[i].id < g->members[best].id)) best = i;
        }
        if (best == g->count) return X01_CYCLE;
        order[k] = best;
        done |= 1u << (unsigned)best;
    }
    return X01_ACCEPT;
}
static bool fault(X01Result *r, unsigned member, unsigned code) {
    if (!code) return false;
    r->status = X01_FAULT; r->member = member; r->fault = code;
    return true;
}
static bool candidate(const X01Group *g, size_t i, int64_t values[X01_MEMBERS], X01Result *r) {
    const X01Member *m = &g->members[i];
    if (fault(r, m->id, m->candidate_fault)) return false;
    if (!add(m->old, m->delta, &values[i])) { (void)fault(r, m->id, 1); return false; }
    for (size_t j = 0; j < g->count; ++j) {
        if ((m->dependencies & (1u << (unsigned)j)) && !add(values[i], values[j], &values[i])) {
            (void)fault(r, m->id, 1); return false;
        }
    }
    return true;
}
static bool guards(const X01Member *m, int64_t value, X01Result *r) {
    if (fault(r, m->id, m->predicate_fault)) return false;
    const unsigned failed = value < m->minimum ? 1u : value > m->maximum ? 2u : 0u;
    if (!failed) return true;
    if (fault(r, m->id, m->reason_fault)) return false;
    r->status = X01_REJECT; r->member = m->id; r->guard = failed;
    return false;
}
static bool emissions(const X01Member *m, int64_t value, X01Event events[X01_EVENTS],
                      size_t *count, size_t limit, X01Result *r) {
    /* Two clauses: fault in clause two must never publish clause one. */
    for (unsigned ordinal = 0; ordinal < 2; ++ordinal) {
        if (ordinal == 1 && fault(r, m->id, m->emission_fault)) return false;
        if (*count == limit) { (void)fault(r, m->id, 3); return false; }
        events[*count] = (X01Event){m->id, m->target, ordinal, value};
        ++*count;
    }
    return true;
}
X01Result x01_group(const X01Group *g) {
    X01Result r = initial(g);
    size_t order[X01_MEMBERS];
    r.status = schedule(g, order);
    if (r.status != X01_ACCEPT) return r;
    int64_t values[X01_MEMBERS] = {0};
    for (size_t k = 0; k < g->count; ++k)
        if (!candidate(g, order[k], values, &r)) return r;
    for (size_t k = 0; k < g->count; ++k) {
        const size_t i = order[k];
        if (!guards(&g->members[i], values[i], &r)) return r;
    }
    if (g->assert_total) {
        int64_t total = 0;
        for (size_t k = 0; k < g->count; ++k)
            if (!add(total, values[order[k]], &total)) { (void)fault(&r, 0, 1); return r; }
        if (total != g->expected_total) { r.status = X01_REJECT; r.guard = 3; return r; }
    }
    X01Event private_events[X01_EVENTS] = {{0}};
    size_t count = 0;
    for (size_t k = 0; k < g->count; ++k) {
        const size_t i = order[k];
        if (!emissions(&g->members[i], values[i], private_events, &count, g->event_limit, &r))
            return r;
    }
    for (size_t i = 0; i < g->count; ++i) r.state[i] = values[i];
    for (size_t i = 0; i < count; ++i) r.events[i] = private_events[i];
    r.event_count = count;
    return r;
}
X01Result x01_library(const X01Group *g) {
    X01Result r = initial(g);
    size_t order[X01_MEMBERS];
    r.status = schedule(g, order);
    if (r.status != X01_ACCEPT) return r;
    if (g->assert_total) { r.status = X01_INVALID; return r; }
    for (size_t i = 0; i < g->count; ++i)
        if (g->members[i].dependencies) { r.status = X01_INVALID; return r; }
    int64_t values[X01_MEMBERS] = {0};
    X01Result first_reject = r;
    bool rejected = false;
    X01Event private_events[X01_EVENTS] = {{0}};
    size_t count = 0;
    /* Eager let a = step(...); let z = step(...); match a { ...match z... }.
     * A typed Reject does not short circuit those two let bindings. */
    for (size_t k = 0; k < g->count; ++k) {
        const size_t i = order[k];
        if (!candidate(g, i, values, &r)) return r;
        if (!guards(&g->members[i], values[i], &r)) {
            if (r.status == X01_FAULT) return r;
            if (!rejected) first_reject = r;
            rejected = true; r = initial(g); r.status = X01_ACCEPT;
        } else if (!emissions(&g->members[i], values[i], private_events, &count,
                              X01_EVENTS, &r)) return r;
    }
    if (rejected) return first_reject;
    if (count > g->event_limit) { (void)fault(&r, 0, 3); return r; }
    for (size_t i = 0; i < g->count; ++i) r.state[i] = values[i];
    for (size_t i = 0; i < count; ++i) r.events[i] = private_events[i];
    r.event_count = count;
    return r;
}
