#ifndef TERNFOLD_DRIVER_NATIVE_H
#define TERNFOLD_DRIVER_NATIVE_H

#include "ternfold/c_emit.h"
#include "ternfold/resolve.h"

typedef struct {
    const char *output;
    const char *compiler;
    const char *runtime;
    bool sanitize;
    TfCoreOptLevel optimization;
} TfDriverNativeOptions;

/* Paths follow the CLI/SourceManager native path convention. A NULL output
 * is permitted only for emission to stdout. */
const char *tf_driver_native(const TfVerifiedCore *core, const TfResolution *resolution,
                             const TfDriverNativeOptions *options, bool build);

#endif
