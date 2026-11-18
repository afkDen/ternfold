#ifndef TERNFOLD_RT_CALLBACKS_H
#define TERNFOLD_RT_CALLBACKS_H

#include "ternfold_rt_stack.h"

/* Generic generated helper bodies for concrete map/fold signatures. Install
 * as ordinary bodies for direct calls or prelude wrappers for closure calls,
 * with at least four slots and no captures. Map takes (List[A], fn(A)->B)
 * and returns List[B]; fold takes
 * (List[A], B, fn(B,A)->B) and returns B. The caller supplies concrete native
 * signatures. Their invocation span is the effective builtin origin.
 * Uses no C recursion. Map moves callback results into a frame-owned buffer
 * and constructs the output list once. Frame teardown releases the buffer.
 */
tf_status tf_stack_map_resume(tf_stack *stack,tf_stack_frame *frame);
tf_status tf_stack_fold_resume(tf_stack *stack,tf_stack_frame *frame);

#endif
