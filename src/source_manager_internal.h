#ifndef TERNFOLD_SOURCE_MANAGER_INTERNAL_H
#define TERNFOLD_SOURCE_MANAGER_INTERNAL_H

#include "ternfold/source_manager.h"

typedef struct {
    const TfSourceManager *manager;
    size_t sources, aliases;
} TfSourceManagerMark;

/* Exclusive append-only transaction. On success discard the mark. On failure
 * destroy all borrowers of the tentative tail before rolling back. Retained
 * entries and aliases stay immutable; tentative IDs expire and may be reused.
 * Marks are single-use and must not outlive the manager or their cache prefix. */
TfSourceManagerMark tf_source_manager_mark(const TfSourceManager *manager);
bool tf_source_manager_rollback(TfSourceManager *manager, TfSourceManagerMark mark);

/* Initialize an uncached descriptor with no manager identity. Source and
 * logical path are borrowed; the caller owns the descriptor. */
TfStatus tf_source_manager_overlay(TfManagedSource *out, const TfSource *source,
                                  const char *logical_path);

/* Internal resolver handoff. For an uncached request, resolved must be the
 * absolute canonical regular file already vetted by the module path adapter.
 * Open only that spelling, while retaining the original request as an alias.
 * The caller still checks containment on cache hits. Like the normal loader,
 * this assumes no hostile concurrent filesystem replacement. */
TfStatus tf_source_manager_load_resolved(TfSourceManager *manager, const char *request,
                                         const char *resolved, const TfManagedSource **out);

#endif
