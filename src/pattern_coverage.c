#include "pattern_coverage.h"
#include "internal.h"

#include <string.h>

typedef struct Allocation Allocation;
struct Allocation {
    Allocation *next;
    max_align_t alignment;
};

typedef const TfCoveragePattern *const *Row;
typedef struct {
    const TfCoverageDomain *domain;
    const TfCoveragePattern *pattern;
} Column;
typedef struct WitnessItem WitnessItem;
struct WitnessItem {
    const TfCoveragePattern *pattern;
    WitnessItem *next;
};
struct TfCoverageWitness {
    TfAllocator allocator;
    Allocation *owned;
    const TfCoveragePattern *root;
};
typedef struct {
    TfCoverageWitness *result;
    Allocation *scratch;
    size_t work_left;
    bool failed;
} Search;

static bool spend(Search *s, size_t work) {
    if (s->failed || work > s->work_left) { s->failed = true; return false; }
    s->work_left -= work;
    return true;
}

static void release_to(TfAllocator a, Allocation **head, const Allocation *mark) {
    while (*head != mark) {
        Allocation *p = *head;
        *head = p->next;
        a.deallocate(a.context,p);
    }
}

static void *allocate(Search *s, Allocation **head, size_t count, size_t width) {
    Allocation *p;
    size_t bytes;
    TfAllocator a = s->result->allocator;
    if (!spend(s,count) || (width != 0 && count > (SIZE_MAX-sizeof *p)/width)) {
        s->failed = true;
        return NULL;
    }
    bytes = count*width;
    p = a.allocate(a.context,sizeof *p+bytes);
    if (p == NULL) { s->failed = true; return NULL; }
    p->next = *head;
    *head = p;
    return p+1;
}

static WitnessItem *prepend(Search *s, const TfCoveragePattern *pattern, WitnessItem *tail) {
    WitnessItem *item = allocate(s,&s->result->owned,1,sizeof *item);
    if (item != NULL) *item = (WitnessItem){pattern,tail};
    return item;
}

static const TfCoveragePattern *make_pattern(Search *s, const TfCoverageConstructor *ctor,
                                             const TfCoveragePattern *const *arguments) {
    TfCoveragePattern *pattern = allocate(s,&s->result->owned,1,sizeof *pattern);
    if (pattern != NULL) *pattern = (TfCoveragePattern){ctor,arguments};
    return pattern;
}

/* The public depth bound protects both matrix recursion and deep copies of
 * externally constructed queries. It is a host resource failure, never a
 * successful coverage decision. */
enum { MAX_DEPTH = 512 };
static bool covers(Search *s, const TfCoveragePattern *cover, const TfCoveragePattern *query,
                   size_t depth) {
    if (!spend(s,1)) return false;
    if (cover == NULL || cover == query) return true;
    if (query == NULL || cover->constructor != query->constructor) return false;
    if (depth == MAX_DEPTH) { s->failed = true; return false; }
    for (size_t i=0; i<cover->constructor->arity; ++i)
        if (!covers(s,cover->arguments[i],query->arguments[i],depth+1)) return false;
    return true;
}
static const TfCoveragePattern *copy_pattern(Search *s, const TfCoveragePattern *pattern,
                                             size_t depth) {
    const TfCoveragePattern **args = NULL;
    if (pattern == NULL || s->failed) return NULL;
    if (depth == MAX_DEPTH) { s->failed = true; return NULL; }
    if (pattern->constructor->arity != 0) {
        args = allocate(s,&s->result->owned,pattern->constructor->arity,sizeof *args);
        if (args == NULL) return NULL;
        for (size_t i=0; i<pattern->constructor->arity && !s->failed; ++i)
            args[i] = copy_pattern(s,pattern->arguments[i],depth+1);
    }
    return make_pattern(s,pattern->constructor,args);
}

static bool useful(Search *s, const Row *matrix, size_t rows, const Column *query,
                   size_t columns, size_t depth, WitnessItem **out);

/* Specialize the most constrained column. Keep all other columns in their
 * original order and restore the removed column in the resulting witness. */
static bool specialize(Search *s, const Row *matrix, size_t rows, const Column *query,
                       size_t columns, size_t chosen, size_t depth, const TfCoverageConstructor *ctor,
                       WitnessItem **out) {
    Allocation *mark = s->scratch;
    const size_t arity = ctor == NULL ? 0 : ctor->arity;
    size_t width, count = 0;
    Row *new_rows;
    Column *new_query;
    WitnessItem *witness = NULL;
    bool found = false;
    if (arity > SIZE_MAX-(columns-1)) { s->failed = true; return false; }
    width = arity+(columns-1);
    new_rows = allocate(s,&s->scratch,rows,sizeof *new_rows);
    new_query = allocate(s,&s->scratch,width,sizeof *new_query);
    if (s->failed) goto done;
    for (size_t i=0; i<arity; ++i) {
        const TfCoveragePattern *head = query[chosen].pattern;
        new_query[i] = (Column){ctor->payload[i],head != NULL ? head->arguments[i] : NULL};
    }
    if (chosen != 0) memcpy(new_query+arity,query,chosen*sizeof *new_query);
    if (chosen+1 < columns)
        memcpy(new_query+arity+chosen,query+chosen+1,(columns-chosen-1)*sizeof *new_query);
    for (size_t i=0; i<rows; ++i) {
        const TfCoveragePattern *head = matrix[i][chosen];
        const TfCoveragePattern **row;
        if (head != NULL && head->constructor != ctor) continue;
        if (arity == 0 && chosen == 0) { new_rows[count++] = matrix[i]+1; continue; }
        row = allocate(s,&s->scratch,width,sizeof *row);
        if (row == NULL) goto done;
        for (size_t j=0; j<arity; ++j) row[j] = head != NULL ? head->arguments[j] : NULL;
        if (chosen != 0) memcpy(row+arity,matrix[i],chosen*sizeof *row);
        if (chosen+1 < columns)
            memcpy(row+arity+chosen,matrix[i]+chosen+1,(columns-chosen-1)*sizeof *row);
        new_rows[count++] = row;
    }
    found = useful(s,new_rows,count,new_query,width,depth+1,&witness);
    if (found) {
        const TfCoveragePattern *head = NULL;
        if (ctor != NULL) {
            const TfCoveragePattern **args = NULL;
            if (arity != 0) {
                args = allocate(s,&s->result->owned,arity,sizeof *args);
                if (args == NULL) goto done;
                for (size_t i=0; i<arity; ++i) {
                    args[i] = witness->pattern;
                    witness = witness->next;
                }
            }
            head = make_pattern(s,ctor,args);
        }
        {
            WitnessItem **place = &witness;
            for (size_t i=0; i<chosen; ++i) place = &(*place)->next;
            *place = prepend(s,head,*place);
            *out = witness;
        }
    }
done:
    release_to(s->result->allocator,&s->scratch,mark);
    return found && !s->failed;
}

static bool useful(Search *s, const Row *matrix, size_t rows, const Column *query,
                   size_t columns, size_t depth, WitnessItem **out) {
    size_t chosen = 0, best = 0;
    if (s->failed || depth == MAX_DEPTH) { s->failed = true; return false; }
    *out = NULL;
    if (rows == 0) {
        /* A symbolic wildcard is a valid residual wherever no row covers it.
         * Do not enumerate the Cartesian product of unconstrained payloads. */
        for (size_t i=columns; i!=0 && !s->failed; --i) {
            const TfCoveragePattern *p = copy_pattern(s,query[i-1].pattern,0);
            *out = prepend(s,p,*out);
        }
        return !s->failed;
    }
    for (size_t i=0; i<rows; ++i) {
        size_t j = 0;
        while (j<columns && covers(s,matrix[i][j],query[j].pattern,0)) ++j;
        if (s->failed || j==columns) return false; /* This row subsumes the query. */
    }
    for (size_t j=0; j<columns; ++j) {
        size_t concrete = 0;
        if (query[j].pattern != NULL)
            return specialize(s,matrix,rows,query,columns,j,depth,query[j].pattern->constructor,out);
        if (!spend(s,rows)) return false;
        for (size_t i=0; i<rows; ++i) if (matrix[i][j] != NULL) ++concrete;
        if (concrete > best) { best = concrete; chosen = j; }
    }
    /* At least one column is constrained; all-wildcard rows returned above.
     * A sparse wide payload therefore consumes depth only on its constraints. */
    if (query[chosen].domain->count == 0)
        return specialize(s,matrix,rows,query,columns,chosen,depth,NULL,out);
    for (size_t i=0; i<query[chosen].domain->count && spend(s,1); ++i)
        if (specialize(s,matrix,rows,query,columns,chosen,depth,query[chosen].domain->constructors[i],out))
            return true;
    return false;
}

TfStatus tf_coverage_witness(TfCoverageWitness **out, const TfCoverageDomain *domain,
                             const TfCoveragePattern *const *previous, size_t count,
                             const TfCoveragePattern *query, const TfAllocator *allocator) {
    const TfAllocator a = tf_allocator(allocator);
    const Column column = {domain,query};
    WitnessItem *root = NULL;
    Search search = {0};
    Row *rows;
    bool found;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (domain == NULL || (count != 0 && previous == NULL) || !tf_allocator_valid(a))
        return TF_HOST_ERROR;
    search.work_left = (size_t)8*1024*1024;
    search.result = a.allocate(a.context,sizeof *search.result);
    if (search.result == NULL) return TF_HOST_ERROR;
    *search.result = (TfCoverageWitness){a,NULL,NULL};
    rows = allocate(&search,&search.scratch,count,sizeof *rows);
    if (rows != NULL)
        for (size_t i=0; i<count; ++i) rows[i] = previous+i;
    found = useful(&search,rows,count,&column,1,0,&root);
    release_to(a,&search.scratch,NULL);
    if (found) { search.result->root = root->pattern; *out = search.result; }
    else tf_coverage_destroy(search.result);
    return search.failed ? TF_HOST_ERROR : TF_OK;
}

const TfCoveragePattern *tf_coverage_root(const TfCoverageWitness *witness) {
    return witness == NULL ? NULL : witness->root;
}

void tf_coverage_destroy(TfCoverageWitness *witness) {
    if (witness != NULL) {
        const TfAllocator a = witness->allocator;
        release_to(a,&witness->owned,NULL);
        a.deallocate(a.context,witness);
    }
}
