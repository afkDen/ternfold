#ifndef TERNFOLD_ALLOCATOR_H
#define TERNFOLD_ALLOCATOR_H
#include <stddef.h>
/* NULL selects the owning subsystem's defaults. A custom allocator and its
 * context must outlive all objects allocated through it. Both callbacks are
 * required, and returned storage must have malloc-compatible alignment. */
typedef struct {
    void *context;
    void *(*allocate)(void *context, size_t size);
    void (*deallocate)(void *context, void *pointer);
} TfAllocator;
#endif
