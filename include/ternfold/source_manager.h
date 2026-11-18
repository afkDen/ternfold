#ifndef TERNFOLD_SOURCE_MANAGER_H
#define TERNFOLD_SOURCE_MANAGER_H

#include "ternfold/lexer.h"

typedef struct TfSourceManager TfSourceManager;
/* A distinct, manager-scoped identity. Zero is invalid. IDs and all borrowed
 * pointers expire at manager destruction; do not use them after that point. */
typedef struct { const TfSourceManager *owner; size_t index; } TfSourceId;

typedef struct {
    TfSourceId id;
    const TfSource *source;
    const char *canonical_path;
    unsigned char sha256[32]; /* SHA-256 of original bytes, including invalid UTF-8 */
    TfStatus status;         /* TF_OK or TF_SOURCE_ERROR */
} TfManagedSource;

/* Manager owns entries, individually allocated sources, bytes, indexes, paths
 * and request aliases. All returned records are borrowed and read-only, stable
 * until destroy, including across vector growth and failed loads. Never copy,
 * modify or destroy their sources (including the pointer fields in TfSource).
 * Destroy downstream borrowers before destroying the manager. Allocator/context
 * must outlive the manager. NULL selects the default allocator. Not thread safe.
 *
 * Paths are NUL-terminated native paths: Windows ANSI code page (A APIs, same
 * convention as tf_source_load/fopen), POSIX native byte paths. This API does
 * not promise UTF-8 Windows paths. Windows device namespace inputs are rejected;
 * drive/UNC disk paths require queryable case-insensitive directory ancestors,
 * including the resolved target's ancestors. Case-sensitive or unqueryable
 * traversal returns HOST_ERROR rather than guessing path identity. This needs
 * Windows FileCaseSensitiveInfo support and read-attributes access to ancestors.
 * POSIX adapter requires PATH_MAX/realpath.
 * No extension or import-root policy is applied.
 *
 * Normalized absolute request aliases are checked BEFORE resolving/opening.
 * Successful aliases retain the first snapshot even after edits/deletion/link
 * retargeting. Previously unseen aliases resolve canonical paths, deduplicating
 * by that path, never by inode/file ID: distinct hardlink paths remain separate.
 * Windows keys use ordinal case-insensitive UTF-16 comparison only after the
 * initial traversal establishes the supported directory policy. Cached aliases
 * are consulted without disk access, including case changes after replacement
 * or deletion. Session path policy is retained with those aliases; concurrent
 * directory-policy changes are outside the initial-filesystem trust contract.
 * Current working directory
 * is used at each call for relative requests. Resolution/open races are not a
 * security boundary. Hashing occurs once on retained bytes, never by rereading.
 * HOST_ERROR (including missing/unreadable paths, invalid arguments and OOM)
 * sets *out=NULL, caches no incomplete record/alias, preserves prior entries,
 * and may be retried. SOURCE_ERROR caches the complete invalid snapshot.
 */
TfStatus tf_source_manager_create(TfSourceManager **out, const TfAllocator *allocator);
void tf_source_manager_destroy(TfSourceManager *manager);
TfStatus tf_source_manager_load(TfSourceManager *manager, const char *path,
                                const TfManagedSource **out);
const TfManagedSource *tf_source_manager_lookup(const TfSourceManager *manager,
                                               TfSourceId id);
size_t tf_source_manager_count(const TfSourceManager *manager);

#endif
