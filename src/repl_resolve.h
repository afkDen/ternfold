#ifndef TERNFOLD_REPL_RESOLVE_H
#define TERNFOLD_REPL_RESOLVE_H

#include "ternfold/resolve.h"

/* Like tf_resolve, but the root parses an immutable, caller-owned TfSource
 * (including its diagnostic name). It must outlive the result and downstream
 * borrowers. The root descriptor is owned by the result and has no manager ID.
 * A loaded_root must be a live entry in manager. Its canonical path supplies
 * the module identity, and its parent is the import base. The containment root
 * is options->root_directory when supplied, otherwise the loaded parent.
 * Without loaded_root, the logical path is <repl> and the canonical root is
 * also the import base; it defaults to ".".
 * Imports resolving to loaded_root target the overlay. Only imports can add
 * cache entries or aliases. Cache rollback is the caller's responsibility. */
TfStatus tf_resolve_virtual(TfResolution **out, TfSourceManager *manager,
                            const TfSource *virtual_source, const TfManagedSource *loaded_root,
                            const TfResolveOptions *options, const TfAllocator *allocator);

#endif
