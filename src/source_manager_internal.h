#ifndef TERNFOLD_SOURCE_MANAGER_INTERNAL_H
#define TERNFOLD_SOURCE_MANAGER_INTERNAL_H

#include "ternfold/source_manager.h"

/* Internal resolver handoff. For an uncached request, resolved must be the
 * absolute canonical regular file already vetted by the module path adapter.
 * Open only that spelling, while retaining the original request as an alias.
 * The caller still checks containment on cache hits. Like the normal loader,
 * this assumes no hostile concurrent filesystem replacement. */
TfStatus tf_source_manager_load_resolved(TfSourceManager *manager, const char *request,
                                         const char *resolved, const TfManagedSource **out);

#endif
