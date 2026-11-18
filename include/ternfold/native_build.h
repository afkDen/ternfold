#ifndef TERNFOLD_NATIVE_BUILD_H
#define TERNFOLD_NATIVE_BUILD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TF_NATIVE_BUILD_DIAGNOSTICS_CAP 8192u
#define TF_NATIVE_BUILD_DEFAULT_TIMEOUT_MS 120000u

typedef struct {
    const char *compiler_path;  /* Explicit executable path; no PATH lookup or flags. */
    const char *runtime_root;   /* Package root containing runtime/, include/, src/. */
    const char *source_path;    /* Prepared C translation unit (any extension). */
    const char *output_path;    /* Exact destination; parent directory must exist. */
    uint32_t timeout_ms;        /* Zero selects the default above. */
    bool sanitize;              /* Request ASan/UBSan; unsupported flags fail the build. */
} TfNativeBuildOptions;

typedef enum {
    TF_NATIVE_BUILD_OK = 0,
    TF_NATIVE_BUILD_INVALID,
    TF_NATIVE_BUILD_HOST_ERROR,
    TF_NATIVE_BUILD_COMPILER_FAILED,
    TF_NATIVE_BUILD_TIMED_OUT,
    TF_NATIVE_BUILD_PUBLISH_FAILED
} TfNativeBuildStatus;

typedef struct {
    char message[512];  /* Operation/path and OS error or compiler exit, as applicable. */
    char diagnostics[TF_NATIVE_BUILD_DIAGNOSTICS_CAP + 1u]; /* Raw merged stdout/stderr. */
    size_t diagnostics_length;
    bool diagnostics_truncated;
    bool cleanup_failed; /* Separate from publication status; see cleanup_message. */
    char cleanup_message[512];
    uint32_t compiler_exit_code; /* Valid for OK / COMPILER_FAILED (128+signal on POSIX). */
} TfNativeBuildResult;

/* GCC/Clang-style compiler. Borrow all options and strings for the duration of
 * the call. Paths are UTF-8 on Windows and native bytes on POSIX; relative
 * paths are relative to the caller's working directory. Windows accepts local
 * drive paths in the normal Win32 namespace. The compiler, prepared C file,
 * runtime tree and output parent are trusted and must stay stable during the
 * build; this is not a sandbox for hostile compilers or concurrent directory
 * writers. On POSIX the caller must allow the launcher to reap its own child.
 * The caller separately protects original Ternfold source paths.
 *
 * A non-OK return never replaces the destination. Success requires a zero
 * compiler exit and a nonempty regular output (also executable on POSIX).
 * Result is overwritten on every call, needs no cleanup, and may be NULL.
 * The diagnostic length excludes the trailing NUL; captured bytes can contain
 * embedded NUL. The child starts in the staging directory, inherits the
 * environment and receives no stdin.
 */
TfNativeBuildStatus tf_native_build(const TfNativeBuildOptions *options,
                                    TfNativeBuildResult *result);

#endif
