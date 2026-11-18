#ifndef TERNFOLD_CORE_INTERNAL_H
#define TERNFOLD_CORE_INTERNAL_H

#include "ternfold/core.h"

typedef union TfCoreArenaChunk TfCoreArenaChunk;

/* The arena belongs exclusively to the unverified owner. Public views are
 * read-only, and the arena never moves a record after allocating it. */
struct TfCoreProgram {
    TfAllocator allocator;
    TfCoreArenaChunk *chunks;
    TfCoreProgramData data;
};

#endif
