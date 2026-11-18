#ifndef TERNFOLD_EXEC_VALUES_H
#define TERNFOLD_EXEC_VALUES_H

#include "ternfold/interpreter.h"

typedef struct TfExecChunk TfExecChunk;
typedef struct TfExecValue TfExecValue;

/* A single evaluator owns the arena and all vector/text payloads. Values may
 * share prior values; neither references nor payloads escape the run. The
 * Core tables and source snapshots are borrowed for the run only. */
typedef struct {
    TfAllocator allocator;
    TfExecChunk *chunks;
    size_t remaining;
} TfExecArena;

typedef struct { const TfExecValue *items; size_t count; } TfExecValues;
struct TfExecValue {
    TfCoreTypeId type;
    union {
        int64_t integer;
        uint64_t bits;
        size_t tag;
        TfCoreText text;
        struct { size_t identity; TfExecValues values; } aggregate;
    } as;
};

/* Uninitialized aligned scratch; zero count returns NULL without charging.
 * Individual objects are never released; destroy is iterative. */
void *tf_exec_allocate(TfExecArena *arena, size_t count, size_t width);
void tf_exec_arena_destroy(TfExecArena *arena);

bool tf_exec_copy_text(TfExecArena *arena, TfCoreText text, TfCoreText *out);
bool tf_exec_copy_values(TfExecArena *arena, TfExecValues values, TfExecValues *out);

/* Constants are already verified and child-first. Text bytes borrow the
 * verified owner; aggregate storage is allocated in the interpreter arena.
 * On failure the caller must discard the arena; no partial table escapes. */
bool tf_exec_constants(TfExecArena *arena, const TfCoreProgramData *program,
                       const TfExecValue **out);

#endif
