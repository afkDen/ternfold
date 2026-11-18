#ifndef TERNFOLD_NATIVE_LIFETIMES_H
#define TERNFOLD_NATIVE_LIFETIMES_H

#include "ternfold/core_verify.h"

typedef struct TfNativeLifetimes TfNativeLifetimes;
/* Borrow verified Core only during analysis. The returned owner contains IDs,
 * no Core/value pointers. Unsupported control flow succeeds with eligible=false
 * and no releases: retain full frame cleanup. No partial plan on error.
 * Only single-block functions without If/SwitchTag/ListMap/map/fold qualify. */
TfStatus tf_native_lifetimes(TfNativeLifetimes **out, const TfVerifiedCore *core,
                             TfCoreFunctionId function, const TfAllocator *allocator);
bool tf_native_lifetimes_eligible(const TfNativeLifetimes *plan);
/* Query AFTER every observation at boundary after_let (0..let_count), before
 * the next operation/return. IDs are one-based Core values, in ascending order;
 * emit tf_release(&f->slots[id-1]), which clears each slot to Unit. At boundary
 * zero all parameter/capture retains and trace_begin must already be complete.
 * A suspending operation's boundary is reached ONLY at its resume label after
 * the child result is installed, never at call scheduling. The returned slot
 * is excluded: tf_stack_return moves it. Frame input arrays/scratch slots stay
 * runtime-owned and are not covered. Fault exits use normal frame cleanup.
 * Borrowed query result lives until plan destruction. */
TfCoreValueIds tf_native_lifetimes_releases(const TfNativeLifetimes *plan, size_t after_let);
void tf_native_lifetimes_destroy(TfNativeLifetimes *plan);

#endif
