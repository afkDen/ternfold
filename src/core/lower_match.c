#include "lower_match.h"
#include "../check_internal.h"

#include <string.h>

/* A row is an arm plus a sparse list of pending tests and bindings. Each
 * cell names a value already visible on this path. Specialization replaces a
 * tested variant cell with cells for its projected payloads. Other cells
 * (especially binders) remain valid and are shared when possible. */
typedef struct Cell Cell;
struct Cell {
    const Cell *next;
    const TfAstNode *pattern;
    TfCoreValueId value;
};
typedef struct { const TfAstNode *arm; const Cell *cells; } Row;
typedef struct Job Job;
struct Job {
    Job *next;
    const Row *rows;
    size_t count;
    TfCoreBlockId block;
    const TfCoreMatchFact *facts;
};

/* Row links are one-based indices into a split's entries. Each bucket and
 * the residual retain source order; merging the two keeps first-arm priority. */
typedef struct { size_t first, last, count; } Rows;
typedef struct { const Cell *test; size_t prefix, next; } Entry;
typedef struct {
    size_t identity;
    TfCoreConstant literal;
} Key;
typedef struct { Rows rows; Key key; uint64_t hash; size_t tag; } Bucket;
typedef struct {
    const Job *job;
    const Cell *test;
    TfCoreTypeId type;
    TfCoreTypeKind kind;
    size_t tags; /* zero for open literal domains */
    Entry *entries;
    Bucket *buckets;
    size_t bucket_count;
    Rows residual;
} Split;

typedef struct {
    const TfCoreMatchBuilder *builder;
    const TfAstNode *match;
    TfCoreBlockId root;
    TfCoreTypeId result, boolean;
    TfCoreValueId value;
    Job *jobs;
    size_t work, bytes;
} Match;

/* Decision trees can duplicate residual rows and arm bodies. Bound both
 * traversal and retained matrix scratch; exhaustion is a host failure. */
enum { MATCH_WORK = 16 * 1024 * 1024, MATCH_BYTES = 64 * 1024 * 1024 };

static const TfAstNode *node(TfNodeId id) { return tf_ast_get(id.owner,id); }

static bool invalid(const Match *m, const TfAstNode *n) {
    (void)m->builder->invariant(m->builder->context,n != NULL ? n : m->match);
    return false;
}

static bool spend(Match *m, size_t units) {
    if (units > m->work || units > *m->builder->remaining_work) return false;
    m->work -= units;
    *m->builder->remaining_work -= units;
    return true;
}

static void *scratch(Match *m, size_t count, size_t width) {
    if (count == 0 || count > m->bytes/width || !spend(m,count)) return NULL;
    m->bytes -= count*width;
    return m->builder->allocate(m->builder->context,count,width);
}

static bool ignored(const TfAstNode *n) {
    /* Unit has exactly one inhabitant, so testing it would add no constraint. */
    return n->kind == TF_AST_PATTERN_WILDCARD || n->kind == TF_AST_PATTERN_UNIT;
}

static TfCoreTypeId pattern_type(const Match *m, const TfAstNode *n) {
    const TfTypedNode *typed=tf_typed_program_node(m->builder->typed,n->id);
    return typed != NULL ? tf_core_lower_type_id(m->builder->types,typed->type) : 0;
}

static uint64_t mix(uint64_t hash) {
    hash = (hash ^ (hash >> 30))*UINT64_C(0xbf58476d1ce4e5b9);
    hash = (hash ^ (hash >> 27))*UINT64_C(0x94d049bb133111eb);
    return hash ^ (hash >> 31);
}

static bool test_key(Match *m, const Split *s, const TfAstNode *n, Key *key, uint64_t *hash) {
    *key=(Key){0};
    if (s->tags != 0 && s->kind != TF_CORE_TYPE_BOOL) {
        const TfTypedNode *typed=tf_typed_program_node(m->builder->typed,n->id);
        if (n->kind != TF_AST_PATTERN_VARIANT || typed == NULL || typed->target.index == 0)
            return invalid(m,n);
        /* Group aliases of the same constructor by resolved identity. Map to
         * the layout ordinal once per group, rather than once per row. */
        key->identity=typed->target.index;
        *hash=mix((uint64_t)key->identity);
        return true;
    }
    const TfLiteralValue *literal=tf_typed_program_literal(m->builder->typed,n->id);
    if (n->kind != TF_AST_PATTERN_LITERAL || literal == NULL) return invalid(m,n);
    TfCoreConstant *k=&key->literal;
    k->type=s->type;
    uint64_t bits;
    switch (literal->kind) {
    case TF_LITERAL_BOOL:
        if (s->kind != TF_CORE_TYPE_BOOL) return invalid(m,n);
        key->identity=literal->as.boolean ? 1u : 0u; bits=key->identity;
        break;
    case TF_LITERAL_INT:
        if (s->kind != TF_CORE_TYPE_INT) return invalid(m,n);
        k->kind=TF_CORE_CONSTANT_INT; k->as.integer=literal->as.signed_pattern;
        bits=(uint64_t)k->as.integer;
        break;
    case TF_LITERAL_FLOAT:
        if (s->kind != TF_CORE_TYPE_FLOAT) return invalid(m,n);
        k->kind=TF_CORE_CONSTANT_FLOAT; k->as.float_bits=bits=literal->as.float_bits;
        break;
    case TF_LITERAL_CHAR:
        if (s->kind != TF_CORE_TYPE_CHAR) return invalid(m,n);
        k->kind=TF_CORE_CONSTANT_CHAR; k->as.character=literal->as.scalar; bits=k->as.character;
        break;
    case TF_LITERAL_TEXT:
        if (s->kind != TF_CORE_TYPE_TEXT) return invalid(m,n);
        k->kind=TF_CORE_CONSTANT_TEXT;
        k->as.text=(TfCoreText){literal->as.text.data,literal->as.text.length};
        if (!spend(m,k->as.text.length)) return false;
        bits=UINT64_C(14695981039346656037);
        for (size_t i=0; i<k->as.text.length; ++i)
            bits=(bits ^ k->as.text.bytes[i])*UINT64_C(1099511628211);
        break;
    case TF_LITERAL_UNIT: default:
        return invalid(m,n);
    }
    *hash=mix(bits);
    return true;
}

static bool same_key(Match *m, const Split *s, Key a, Key b, bool *equal) {
    if (s->tags != 0) { *equal=a.identity==b.identity; return true; }
    switch (a.literal.kind) {
    case TF_CORE_CONSTANT_INT: *equal=a.literal.as.integer==b.literal.as.integer; break;
    case TF_CORE_CONSTANT_FLOAT: *equal=a.literal.as.float_bits==b.literal.as.float_bits; break;
    case TF_CORE_CONSTANT_CHAR: *equal=a.literal.as.character==b.literal.as.character; break;
    case TF_CORE_CONSTANT_TEXT: {
        const TfCoreText x=a.literal.as.text, y=b.literal.as.text;
        if (x.length != y.length) { *equal=false; break; }
        if (!spend(m,x.length)) return false;
        *equal=x.length==0 || memcmp(x.bytes,y.bytes,x.length)==0;
        break;
    }
    default: return invalid(m,s->test->pattern);
    }
    return true;
}

static void append_row(Rows *rows, Entry *entries, size_t index) {
    if (rows->last != 0) entries[rows->last-1].next=index;
    else rows->first=index;
    rows->last=index;
    ++rows->count;
}

static bool partition(Match *m, Split *s) {
    const size_t count=s->job->count;
    size_t capacity=2;
    while (capacity/2 < count) {
        if (capacity > SIZE_MAX/2) return false;
        capacity *= 2;
    }
    s->entries=scratch(m,count,sizeof *s->entries);
    s->buckets=scratch(m,count,sizeof *s->buckets);
    size_t *slots=scratch(m,capacity,sizeof *slots);
    if (s->entries == NULL || s->buckets == NULL || slots == NULL) return false;
    memset(slots,0,capacity*sizeof *slots);
    for (size_t i=0; i<count; ++i) {
        const Cell *cell=s->job->rows[i].cells;
        size_t prefix=0;
        while (cell != NULL && cell->value != s->test->value) {
            if (!spend(m,1)) return false;
            ++prefix;
            cell=cell->next;
        }
        s->entries[i]=(Entry){cell,prefix,0};
        if (cell == NULL || cell->pattern->kind == TF_AST_PATTERN_BINDER) {
            append_row(&s->residual,s->entries,i+1);
            continue;
        }
        Key key;
        uint64_t hash;
        if (!test_key(m,s,cell->pattern,&key,&hash)) return false;
        size_t slot=(size_t)hash & (capacity-1);
        while (slots[slot] != 0) {
            const Bucket *bucket=&s->buckets[slots[slot]-1];
            bool equal=false;
            if (!spend(m,1) || (bucket->hash==hash && !same_key(m,s,key,bucket->key,&equal)))
                return false;
            if (equal) break;
            slot=(slot+1) & (capacity-1);
        }
        if (slots[slot] == 0) {
            size_t tag=key.identity;
            if (s->tags != 0 && s->kind != TF_CORE_TYPE_BOOL) {
                /* The nominal mapping hook can scan all declaration members. */
                if (!spend(m,s->tags)) return false;
                if (!m->builder->variant_tag(m->builder->context,cell->pattern->id,&tag) ||
                    tag >= s->tags) return invalid(m,cell->pattern);
            }
            s->buckets[s->bucket_count]=(Bucket){.key=key,.hash=hash,.tag=tag};
            slots[slot]=++s->bucket_count;
        }
        append_row(&s->buckets[slots[slot]-1].rows,s->entries,i+1);
    }
    return s->bucket_count != 0 || invalid(m,s->test->pattern);
}

/* A child only needs a projection when some retained pattern tests or binds
 * that payload slot. Reuse the same ValueId for that slot throughout this case. */
static bool expand(Match *m, const Split *s, TfCoreBlockId block, size_t tag,
                   TfCoreValueId *values, size_t arity, const Entry *entry, Row *row) {
    const Cell *test=entry->test;
    const TfAstNode *pattern=test->pattern;
    const TfNodeList children=pattern->kind==TF_AST_PATTERN_VARIANT ?
                              pattern->as.pattern_variant.arguments : (TfNodeList){0};
    if (children.count != arity) return invalid(m,pattern);
    if (arity > SIZE_MAX-entry->prefix) return false;
    const size_t capacity=entry->prefix+arity;
    Cell *cells=capacity != 0 ? scratch(m,capacity,sizeof *cells) : NULL;
    if (capacity != 0 && cells == NULL) return false;
    const Cell *head=row->cells;
    size_t used=0;
    for (size_t i=0; i<entry->prefix; ++i) {
        cells[used++]=*head;
        head=head->next;
    }
    for (size_t i=0; i<arity; ++i) {
        const TfAstNode *child=node(children.items[i]);
        if (child == NULL) return invalid(m,pattern);
        if (ignored(child)) continue;
        if (values[i] == 0) {
            const TfCoreTypeId type=pattern_type(m,child);
            if (type == 0) return invalid(m,child);
            TfCoreValueId *operand=scratch(m,1,sizeof *operand);
            if (operand == NULL) return false;
            *operand=s->test->value;
            values[i]=m->builder->emit(m->builder->context,block,type,
                (TfCoreOperation){.kind=TF_CORE_OP_PAYLOAD,.operands={operand,1},.as.payload={tag,i}},
                child->span);
            if (values[i] == 0) return false;
        }
        cells[used++]=(Cell){.pattern=child,.value=values[i]};
    }
    const Cell *tail=test->next;
    while (used != 0) {
        cells[--used].next=tail;
        tail=&cells[used];
    }
    row->cells=tail;
    return true;
}

static bool schedule(Match *m, const Split *s, const Bucket *bucket, TfCoreBlockId block,
                     const TfCoreMatchFact *facts) {
    const Rows exact=bucket != NULL ? bucket->rows : (Rows){0};
    if (block == 0) return false;
    if (exact.count > SIZE_MAX-s->residual.count) return false;
    const size_t count=exact.count+s->residual.count;
    if (count == 0) return invalid(m,s->test->pattern);
    Row *rows=scratch(m,count,sizeof *rows);
    Job *job=scratch(m,1,sizeof *job);
    if (rows == NULL || job == NULL) return false;
    size_t arity=0;
    TfCoreValueId *values=NULL;
    if (bucket != NULL) {
        const TfAstNode *pattern=s->entries[exact.first-1].test->pattern;
        if (pattern->kind == TF_AST_PATTERN_VARIANT) arity=pattern->as.pattern_variant.arguments.count;
        if (arity != 0) {
            values=scratch(m,arity,sizeof *values);
            if (values == NULL) return false;
            memset(values,0,arity*sizeof *values);
        }
    }
    size_t a=exact.first, b=s->residual.first;
    for (size_t i=0; i<count; ++i) {
        if (a != 0 && (b == 0 || a<b)) {
            if (bucket == NULL) return invalid(m,s->test->pattern);
            rows[i]=s->job->rows[a-1];
            const Entry *entry=&s->entries[a-1];
            if (!expand(m,s,block,bucket->tag,values,arity,entry,&rows[i])) return false;
            a=entry->next;
        } else if (b != 0) {
            rows[i]=s->job->rows[b-1];
            b=s->entries[b-1].next;
        } else return invalid(m,s->test->pattern);
    }
    *job=(Job){m->jobs,rows,count,block,facts};
    m->jobs=job;
    return true;
}

static bool finish(Match *m, TfCoreBlockId block, TfCoreValueId value, TfSpan span) {
    const TfCoreMatchBuilder *b=m->builder;
    if (value == 0) return false;
    if (block == m->root) m->value=value;
    else b->yield(b->context,block,value,span);
    return !b->failed(b->context);
}

static bool branch(Match *m, TfCoreBlockId block, TfCoreValueId value, TfCoreOperation op,
                   TfSpan span) {
    TfCoreValueId *subject=scratch(m,1,sizeof *subject);
    if (subject == NULL) return false;
    *subject=value;
    op.operands=(TfCoreValueIds){subject,1};
    return finish(m,block,m->builder->emit(m->builder->context,block,m->result,op,span),span);
}

static bool allows(Match *m, const TfCoreMatchFact *fact, TfCoreValueId subject, size_t tag,
                    bool *out) {
    *out=false;
    for (; fact!=NULL; fact=fact->prior) {
        if (!spend(m,1)) return false;
        if (fact->subject!=subject) continue;
        /* The newest exact fact was established only on a possible path. */
        if (fact->exact!=SIZE_MAX) { *out=tag==fact->exact; return true; }
        if (!spend(m,fact->excluded_count)) return false;
        for (size_t i=0; i<fact->excluded_count; ++i)
            if (tag==fact->excluded[i].tag) return true;
    }
    *out=true;
    return true;
}

static bool default_possible(Match *m, const TfCoreMatchFact *fallback, size_t tags, bool *out) {
    for (const TfCoreMatchFact *f=fallback->prior; f!=NULL; f=f->prior) {
        if (!spend(m,1)) return false;
        if (f->subject!=fallback->subject) continue;
        if (f->exact!=SIZE_MAX) return allows(m,fallback,fallback->subject,f->exact,out);
        /* With multiple exclusions, check the finite domain below. */
        for (size_t tag=0; tag<tags; ++tag) {
            if (!allows(m,fallback,fallback->subject,tag,out)) return false;
            if (*out) return true;
        }
        return true;
    }
    *out=fallback->excluded_count<tags;
    return true;
}

static bool finite(Match *m, const Split *s) {
    const TfCoreMatchBuilder *b=m->builder;
    const TfSpan span=s->test->pattern->span;
    if (s->kind == TF_CORE_TYPE_BOOL) {
        const Bucket *groups[2]={NULL,NULL};
        for (size_t i=0; i<s->bucket_count; ++i) groups[s->buckets[i].tag]=&s->buckets[i];
        const TfCoreBlockId yes=b->block(b->context,span);
        const TfCoreBlockId no=b->block(b->context,span);
        TfCoreMatchFact *facts=scratch(m,2,sizeof *facts);
        if (facts==NULL || yes==0 || no==0) return false;
        for (size_t tag=0; tag<2; ++tag) {
            const TfCoreBlockId block=tag==0 ? no : yes;
            bool possible;
            if (!allows(m,s->job->facts,s->test->value,tag,&possible)) return false;
            if (!possible) b->unreachable(b->context,block,span);
            else {
                facts[tag]=(TfCoreMatchFact){.prior=s->job->facts,.subject=s->test->value,.exact=tag};
                if (!schedule(m,s,groups[tag],block,&facts[tag])) return false;
            }
        }
        return branch(m,s->job->block,s->test->value,
                      (TfCoreOperation){.kind=TF_CORE_OP_IF,.as.branch={yes,no}},span);
    }
    TfCoreTagCase *cases=scratch(m,s->bucket_count,sizeof *cases);
    TfCoreMatchFact *facts=scratch(m,s->bucket_count,sizeof *facts);
    if (cases == NULL || facts == NULL) return false;
    TfCoreBlockId fallback=0;
    if (s->bucket_count < s->tags) fallback=b->block(b->context,span);
    for (size_t i=0; i<s->bucket_count; ++i) {
        const Bucket *bucket=&s->buckets[i];
        const TfSpan origin=s->entries[bucket->rows.first-1].test->pattern->span;
        const TfCoreBlockId block=b->block(b->context,origin);
        bool possible;
        if (block==0 || !allows(m,s->job->facts,s->test->value,bucket->tag,&possible)) return false;
        cases[i]=(TfCoreTagCase){bucket->tag,block};
        if (!possible) b->unreachable(b->context,block,origin);
        else {
            facts[i]=(TfCoreMatchFact){.prior=s->job->facts,.subject=s->test->value,.exact=bucket->tag};
            if (!schedule(m,s,bucket,block,&facts[i])) return false;
        }
    }
    if (fallback!=0) {
        TfCoreMatchFact *fact=scratch(m,1,sizeof *fact);
        bool possible;
        if (fact==NULL) return false;
        *fact=(TfCoreMatchFact){s->job->facts,s->test->value,SIZE_MAX,cases,s->bucket_count};
        if (!default_possible(m,fact,s->tags,&possible)) return false;
        if (!possible) b->unreachable(b->context,fallback,span);
        else if (!schedule(m,s,NULL,fallback,fact)) return false;
    }
    return branch(m,s->job->block,s->test->value,(TfCoreOperation){
        .kind=TF_CORE_OP_SWITCH_TAG,.as.switch_tag={cases,s->bucket_count,fallback}},span);
}

static bool open_literals(Match *m, const Split *s) {
    const TfCoreMatchBuilder *b=m->builder;
    if (m->boolean == 0 && tf_core_lower_bool(b->types,&m->boolean) != TF_OK) return false;
    TfCoreBlockId current=s->job->block;
    for (size_t i=0; i<s->bucket_count; ++i) {
        const Bucket *bucket=&s->buckets[i];
        const TfSpan span=s->entries[bucket->rows.first-1].test->pattern->span;
        TfCoreValueId *args=scratch(m,2,sizeof *args);
        if (args == NULL) return false;
        args[0]=s->test->value;
        args[1]=b->constant(b->context,current,bucket->key.literal,span);
        if (args[1] == 0) return false;
        const TfCoreValueId test=b->emit(b->context,current,m->boolean,(TfCoreOperation){
            .kind=TF_CORE_OP_BINARY,.operands={args,2},.as.binary=TF_CORE_BINARY_EQUAL},span);
        const TfCoreBlockId yes=b->block(b->context,span);
        const TfCoreBlockId no=b->block(b->context,span);
        if (test == 0 || no == 0 || !schedule(m,s,bucket,yes,s->job->facts) ||
            !branch(m,current,test,(TfCoreOperation){.kind=TF_CORE_OP_IF,.as.branch={yes,no}},span))
            return false;
        current=no;
    }
    /* Equality cannot prove an empty open domain. A checked matrix always
     * leaves binder/wildcard rows (or rows testing other occurrences) here. */
    return schedule(m,s,NULL,current,s->job->facts);
}

static bool lower_job(Match *m, const Job *job) {
    const TfCoreMatchBuilder *b=m->builder;
    const Row first=job->rows[0];
    const Cell *test=first.cells;
    while (test != NULL && test->pattern->kind == TF_AST_PATTERN_BINDER) {
        if (!spend(m,1)) return false;
        test=test->next;
    }
    if (test == NULL) {
        /* Bind only the winning row. Stable symbol IDs isolate other arms and
         * closures see the current branch's projections when they capture. */
        for (const Cell *c=first.cells; c != NULL; c=c->next)
            if (!b->bind(b->context,c->pattern->id,c->value)) return false;
        const TfAstNode *body=node(first.arm->as.arm.body);
        if (body == NULL) return invalid(m,first.arm);
        return finish(m,job->block,b->expression(b->context,job->block,body->id,job->facts),body->span);
    }

    TfCoreProgramData view={0};
    tf_core_lower_type_view(b->types,&view);
    const TfCoreTypeId id=pattern_type(m,test->pattern);
    const TfCoreType *type=tf_core_type(&view,id);
    if (type == NULL) return invalid(m,test->pattern);
    Split s={.job=job,.test=test,.type=id,.kind=type->kind};
    switch (type->kind) {
    case TF_CORE_TYPE_ENUM:
        s.tags=type->as.enumeration.variant_count;
        if (s.tags==0) return invalid(m,test->pattern);
        break;
    case TF_CORE_TYPE_OPTION: case TF_CORE_TYPE_DECISION: case TF_CORE_TYPE_BOOL: s.tags=2; break;
    case TF_CORE_TYPE_INT: case TF_CORE_TYPE_FLOAT: case TF_CORE_TYPE_CHAR: case TF_CORE_TYPE_TEXT: break;
    default: return invalid(m,test->pattern);
    }
    return partition(m,&s) && (s.tags != 0 ? finite(m,&s) : open_literals(m,&s));
}

TfCoreValueId tf_core_lower_match(const TfCoreMatchBuilder *builder, TfCoreBlockId block,
                                  const TfAstNode *match, TfCoreTypeId result,
                                  TfCoreValueId scrutinee) {
    if (builder->failed(builder->context)) return 0;
    if (match == NULL || match->kind != TF_AST_MATCH || !block || !result || !scrutinee)
        return builder->invariant(builder->context,match);
    const TfNodeList arms=match->as.matching.arms;
    if (arms.count == 0) return builder->invariant(builder->context,match);
    Match m={.builder=builder,.match=match,.root=block,.result=result,
             .work=MATCH_WORK,.bytes=MATCH_BYTES};
    Row *rows=scratch(&m,arms.count,sizeof *rows);
    Cell *cells=scratch(&m,arms.count,sizeof *cells);
    if (rows == NULL || cells == NULL) return 0;
    for (size_t i=0; i<arms.count; ++i) {
        const TfAstNode *arm=node(arms.items[i]);
        if (arm == NULL || arm->kind != TF_AST_ARM) return builder->invariant(builder->context,match);
        const TfAstNode *pattern=node(arm->as.arm.pattern);
        if (pattern == NULL) return builder->invariant(builder->context,arm);
        cells[i]=(Cell){NULL,pattern,scrutinee};
        rows[i]=(Row){arm,ignored(pattern) ? NULL : &cells[i]};
    }
    Job root={NULL,rows,arms.count,block,builder->inherited};
    m.jobs=&root;
    while (m.jobs != NULL) {
        const Job *job=m.jobs;
        m.jobs=job->next;
        if (!lower_job(&m,job)) return 0;
    }
    return m.value;
}
