#ifndef TERNFOLD_RT_IO_H
#define TERNFOLD_RT_IO_H

#include "ternfold_rt.h"
#include <stdio.h>

typedef enum { TF_IO_OK, TF_IO_HOST_ERROR, TF_IO_INVALID_UTF8, TF_IO_FAULT } tf_io_status;

/* Host operations on binary byte-oriented/unoriented streams. Caller owns
 * streams, flushing and closing. Capture uses context budget for scratch
 * and runtime values; it returns one owned List[Text]. LF splits lines,
 * CR immediately before LF is trimmed, terminal LF adds no empty line.
 * Language input operations later index this immutable snapshot (R006).
 * Failed capture returns Unit and publishes no snapshot. */
tf_io_status tf_input_capture(tf_context *ctx, tf_type_id text_list, FILE *stream, tf_value *out);

/* Generated hidden entry only. Snapshot is borrowed, out is a fresh owner.
 * Indexing beyond captured lines reports R006 at the input-clause span. */
tf_status tf_entry_input(tf_context *ctx,tf_span_id span,tf_value snapshot,size_t index,tf_value *out);

/* Only call after successful complete evaluation. Validate all output views
 * and fallible retains before writing any byte. No language fault can occur
 * once publication starts; a stream write error is a host failure. The
 * caller retains ownership of the completed List[Text] for the entire call. */
tf_io_status tf_output_publish(tf_context *ctx, FILE *stream, tf_value lines);

#endif
