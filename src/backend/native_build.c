#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "ternfold/native_build.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
typedef wchar_t PathChar;
typedef DWORD OsError;
#define P(s) L##s
#define path_length wcslen
#define PATH_SEPARATOR L'\\'
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
typedef char PathChar;
typedef int OsError;
#define P(s) s
#define path_length strlen
#define PATH_SEPARATOR '/'
#endif

/* A project limit, not a promise that the OS/compiler accepts every such path. */
enum { PATH_CAP = 4096, PIPE_CHUNK = 4096, PIPE_ROUNDS = 8 };
typedef enum { MISSING, REGULAR, DIRECTORY, OTHER, PATH_ERROR } PathKind;
#ifdef _WIN32
typedef BY_HANDLE_FILE_INFORMATION FileInfo;
#else
typedef struct stat FileInfo;
#endif

static void message(TfNativeBuildResult *result, const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(result->message, sizeof result->message, format, args);
    va_end(args);
}

static void os_message(TfNativeBuildResult *result, const char *operation,
                       const char *path, OsError error) {
#ifdef _WIN32
    char detail[192] = "";
    DWORD length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  NULL, error, 0, detail, (DWORD)sizeof detail, NULL);
    while (length && (detail[length - 1u] == '\r' || detail[length - 1u] == '\n'))
        detail[--length] = '\0';
    message(result, "%s: %s (Windows error %lu%s%s)", operation, path,
            (unsigned long)error, length ? ": " : "", detail);
#else
    message(result, "%s: %s (errno %d: %s)", operation, path, error, strerror(error));
#endif
}

static void capture(TfNativeBuildResult *result, const char *bytes, size_t length) {
    size_t remaining = TF_NATIVE_BUILD_DIAGNOSTICS_CAP - result->diagnostics_length;
    size_t copy = length < remaining ? length : remaining;
    memcpy(result->diagnostics + result->diagnostics_length, bytes, copy);
    result->diagnostics_length += copy;
    result->diagnostics[result->diagnostics_length] = '\0';
    if (copy < length) result->diagnostics_truncated = true;
}

static bool separator(PathChar c) {
#ifdef _WIN32
    return c == L'\\' || c == L'/';
#else
    return c == '/';
#endif
}

static PathChar *path_copy(const PathChar *path, size_t length) {
    if (length >= PATH_CAP) return NULL;
    PathChar *copy = malloc((length + 1u) * sizeof *copy);
    if (copy) {
        memcpy(copy, path, length * sizeof *copy);
        copy[length] = 0;
    }
    return copy;
}

static PathChar *join(const PathChar *parent, const PathChar *child) {
    size_t a = path_length(parent), b = path_length(child);
    size_t slash = a && !separator(parent[a - 1u]) ? 1u : 0u;
    if (a >= PATH_CAP || b >= PATH_CAP || a + slash + b >= PATH_CAP) return NULL;
    PathChar *out = malloc((a + slash + b + 1u) * sizeof *out);
    if (out) {
        memcpy(out, parent, a * sizeof *out);
        if (slash) out[a] = PATH_SEPARATOR;
        memcpy(out + a + slash, child, (b + 1u) * sizeof *out);
    }
    return out;
}

static bool has_file_leaf(const PathChar *path) {
    const PathChar *name = path;
    for (const PathChar *it = path; *it; ++it)
        if (separator(*it)) name = it + 1;
    return *name && !(name[0] == '.' &&
                      (name[1] == 0 || (name[1] == '.' && name[2] == 0)));
}

#ifdef _WIN32

static bool drive_letter(PathChar c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

static bool valid_win_path(const PathChar *path) {
    if (!*path || (separator(path[0]) && separator(path[1]))) return false;
    const PathChar *part = path;
    if (drive_letter(path[0]) && path[1] == L':') {
        if (!separator(path[2])) return false; /* No drive-relative paths. */
        part += 2;
    }
    while (*part) {
        if (separator(*part)) { ++part; continue; }
        const PathChar *end = part;
        size_t stem = 0;
        while (*end && !separator(*end)) {
            if (*end < 32 || wcschr(L":*?\"<>|", *end)) return false;
            ++end;
        }
        size_t length = (size_t)(end - part);
        if ((length == 1u && part[0] == L'.') ||
            (length == 2u && part[0] == L'.' && part[1] == L'.')) {
            part = end;
            continue;
        }
        if (end[-1] == L'.' || end[-1] == L' ') return false;
        while (stem < length && part[stem] != L'.') ++stem;
        while (stem && part[stem - 1u] == L' ') --stem;
        static const PathChar *const devices[] = {
            L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"
        };
        for (size_t i = 0; stem && i < sizeof devices / sizeof *devices; ++i)
            if (CompareStringOrdinal(part, (int)stem, devices[i], -1, TRUE) == CSTR_EQUAL)
                return false;
        if (stem == 4u && wcschr(L"0123456789\u00b9\u00b2\u00b3", part[3]) &&
            (CompareStringOrdinal(part, 3, L"COM", 3, TRUE) == CSTR_EQUAL ||
             CompareStringOrdinal(part, 3, L"LPT", 3, TRUE) == CSTR_EQUAL))
            return false;
        part = end;
    }
    return true;
}

static PathChar *absolute_path(const char *path, bool file) {
    PathChar input[PATH_CAP], full[PATH_CAP];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, input, PATH_CAP))
        return NULL;
    if (!valid_win_path(input) || (file && !has_file_leaf(input))) {
        SetLastError(ERROR_INVALID_NAME);
        return NULL;
    }
    DWORD length = GetFullPathNameW(input, PATH_CAP, full, NULL);
    if (!length) return NULL;
    if (length >= PATH_CAP || !drive_letter(full[0]) || full[1] != L':' ||
        !separator(full[2]) || !valid_win_path(full)) {
        SetLastError(ERROR_INVALID_NAME);
        return NULL;
    }
    PathChar *copy = path_copy(full, length);
    if (!copy) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return copy;
}

static PathKind path_info(const PathChar *path, bool follow, FileInfo *info, OsError *error) {
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (!follow) flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE file = CreateFileW(path, FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, flags, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return *error == ERROR_FILE_NOT_FOUND || *error == ERROR_PATH_NOT_FOUND
            ? MISSING : PATH_ERROR;
    }
    PathKind kind = OTHER;
    if (GetFileType(file) == FILE_TYPE_DISK) {
        if (!GetFileInformationByHandle(file, info)) {
            *error = GetLastError();
            kind = PATH_ERROR;
        } else if (follow || !(info->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            kind = (info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DIRECTORY : REGULAR;
        }
    }
    (void)CloseHandle(file);
    return kind;
}

static bool same_file(const FileInfo *a, const FileInfo *b) {
    return a->dwVolumeSerialNumber == b->dwVolumeSerialNumber &&
           a->nFileIndexHigh == b->nFileIndexHigh && a->nFileIndexLow == b->nFileIndexLow;
}

static bool usable_executable(const PathChar *path, const FileInfo *info) {
    (void)path;
    return info->nNumberOfLinks == 1u && (info->nFileSizeHigh || info->nFileSizeLow);
}

static PathChar *create_stage(const PathChar *parent, OsError *error) {
    PathChar name[80];
    DWORD pid = GetCurrentProcessId();
    ULONGLONG time = GetTickCount64();
    for (unsigned attempt = 0; attempt < 128u; ++attempt) {
        if (swprintf(name, sizeof name / sizeof *name, L".tf-native-%lu-%llu-%u",
                     (unsigned long)pid, (unsigned long long)time, attempt) < 0)
            break;
        PathChar *stage = join(parent, name);
        if (!stage) { *error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
        if (CreateDirectoryW(stage, NULL)) return stage;
        *error = GetLastError();
        free(stage);
        if (*error != ERROR_ALREADY_EXISTS) return NULL;
    }
    *error = ERROR_ALREADY_EXISTS;
    return NULL;
}

static bool publish(const PathChar *from, const PathChar *to, OsError *error) {
    if (MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    *error = GetLastError();
    return false;
}

static OsError discard_path(const PathChar *path, bool directory) {
    ULONGLONG start = GetTickCount64();
    for (;;) {
        if (directory ? RemoveDirectoryW(path) : DeleteFileW(path)) return 0;
        OsError error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return 0;
        /* A terminating job or filesystem scanner can still briefly hold an
         * executable or its working directory after the process exits. */
        if ((error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) ||
            GetTickCount64() - start >= 500u) return error;
        Sleep(10);
    }
}

static OsError discard_stage(const PathChar *dir, const PathChar *file) {
    OsError file_error = file ? discard_path(file, false) : 0;
    OsError dir_error = discard_path(dir, true);
    return file_error ? file_error : dir_error;
}

#else

static PathChar *absolute_path(const char *path, bool file) {
    char cwd[PATH_CAP];
    if (!*path || (file && !has_file_leaf(path))) { errno = EINVAL; return NULL; }
    if (strlen(path) >= PATH_CAP) { errno = ENAMETOOLONG; return NULL; }
    if (*path != '/' && !getcwd(cwd, sizeof cwd)) return NULL;
    errno = ENOMEM;
    return *path == '/' ? path_copy(path, strlen(path)) : join(cwd, path);
}

static PathKind path_info(const PathChar *path, bool follow, FileInfo *info, OsError *error) {
    if ((follow ? stat(path, info) : lstat(path, info)) != 0) {
        *error = errno;
        return errno == ENOENT ? MISSING : PATH_ERROR;
    }
    if (S_ISREG(info->st_mode)) return REGULAR;
    if (S_ISDIR(info->st_mode)) return DIRECTORY;
    return OTHER;
}

static bool same_file(const FileInfo *a, const FileInfo *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static bool usable_executable(const PathChar *path, const FileInfo *info) {
    return info->st_nlink == 1 && info->st_size > 0 && access(path, X_OK) == 0;
}

static PathChar *create_stage(const PathChar *parent, OsError *error) {
    PathChar *stage = join(parent, ".tf-native-XXXXXX");
    if (!stage) { *error = ENOMEM; return NULL; }
    if (mkdtemp(stage)) return stage;
    *error = errno;
    free(stage);
    return NULL;
}

static bool publish(const PathChar *from, const PathChar *to, OsError *error) {
    if (rename(from, to) == 0) return true;
    *error = errno;
    return false;
}

static OsError discard_stage(const PathChar *dir, const PathChar *file) {
    OsError error = 0;
    if (file && unlink(file) != 0 && errno != ENOENT) error = errno;
    if (rmdir(dir) != 0 && !error) error = errno;
    return error;
}

#endif

static PathChar *output_parent(const PathChar *output) {
    size_t length = path_length(output);
    while (length && !separator(output[length - 1u])) --length;
    return length ? path_copy(output, length) : NULL;
}


#ifdef _WIN32

/* CreateProcessW takes one command line, even without a shell. Encode argv
 * using the receiving CRT's quote/backslash rules. 32767 includes the NUL. */
static bool append_command(PathChar *line, size_t *length, PathChar c) {
    if (*length >= 32766u) return false;
    line[(*length)++] = c;
    return true;
}

static bool make_command(PathChar *line, PathChar *const argv[]) {
    size_t length = 0;
    for (size_t i = 0; argv[i]; ++i) {
        if (i && !append_command(line, &length, L' ')) return false;
        if (!append_command(line, &length, L'"')) return false;
        const PathChar *arg = argv[i];
        do {
            size_t slashes = 0;
            while (*arg == L'\\') { ++arg; ++slashes; }
            if (*arg == L'"' || !*arg) slashes *= 2u;
            while (slashes) {
                if (!append_command(line, &length, L'\\')) return false;
                --slashes;
            }
            if (!*arg) break;
            if (*arg == L'"' && !append_command(line, &length, L'\\')) return false;
            if (!append_command(line, &length, *arg++)) return false;
        } while (true);
        if (!append_command(line, &length, L'"')) return false;
    }
    line[length] = 0;
    return true;
}

static bool drain_pipe(HANDLE pipe, TfNativeBuildResult *result, OsError *error) {
    char buffer[PIPE_CHUNK];
    for (unsigned i = 0; i < PIPE_ROUNDS; ++i) {
        DWORD available, got;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL) ||
            (available && !ReadFile(pipe, buffer,
                                    available < sizeof buffer ? available : (DWORD)sizeof buffer,
                                    &got, NULL))) {
            *error = GetLastError();
            return *error == ERROR_BROKEN_PIPE;
        }
        if (!available) return true;
        capture(result, buffer, got);
    }
    return true;
}

static bool stop_job(HANDLE job, OsError *error) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION account;
    if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                   &account, sizeof account, NULL)) {
        *error = GetLastError();
        return false;
    }
    if (!account.ActiveProcesses) return true;
    if (!TerminateJobObject(job, 1)) {
        *error = GetLastError();
        return false;
    }
    ULONGLONG start = GetTickCount64();
    do {
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                       &account, sizeof account, NULL)) {
            *error = GetLastError();
            return false;
        }
        if (!account.ActiveProcesses) return true;
        Sleep(10);
    } while (GetTickCount64() - start < 1000u);
    *error = ERROR_TIMEOUT;
    return false;
}

static TfNativeBuildStatus run_compiler(PathChar *const argv[], const PathChar *stage,
                                       uint32_t timeout_ms,
                                       const char *compiler, TfNativeBuildResult *result) {
    TfNativeBuildStatus status = TF_NATIVE_BUILD_HOST_ERROR;
    SECURITY_ATTRIBUTES inheritable = {sizeof inheritable, NULL, TRUE};
    STARTUPINFOEXW startup = {0};
    PROCESS_INFORMATION child = {0};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    HANDLE reader = NULL, writer = NULL, input = INVALID_HANDLE_VALUE, job = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
    bool attributes_initialized = false, assigned = false;
    PathChar *line = malloc(32767u * sizeof *line);
    OsError error;
    const char *operation = "allocate compiler command line";
    if (!line) { error = ERROR_NOT_ENOUGH_MEMORY; goto failed; }
    if (!make_command(line, argv)) {
        operation = "compiler argument list exceeds the Windows limit";
        error = ERROR_BAD_LENGTH;
        goto failed;
    }

    operation = "create compiler job and standard handles";
    job = CreateJobObjectW(NULL, NULL);
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!job ||
        !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
        !CreatePipe(&reader, &writer, &inheritable, PIPE_CHUNK) ||
        !SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0)) {
        error = GetLastError();
        goto failed;
    }
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (input == INVALID_HANDLE_VALUE) { error = GetLastError(); goto failed; }

    operation = "configure compiler handle inheritance";
    SIZE_T attribute_size = 0;
    (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    if (!attribute_size) { error = GetLastError(); goto failed; }
    attributes = malloc(attribute_size);
    if (!attributes) { error = ERROR_NOT_ENOUGH_MEMORY; goto failed; }
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
        error = GetLastError();
        goto failed;
    }
    attributes_initialized = true;
    HANDLE inherited[] = {input, writer};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof inherited, NULL, NULL)) {
        error = GetLastError();
        goto failed;
    }
    startup.lpAttributeList = attributes;
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = writer;
    startup.StartupInfo.hStdError = writer;
    operation = "launch C17 compiler";
    if (!CreateProcessW(argv[0], line, NULL, NULL, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        NULL, stage, &startup.StartupInfo, &child)) {
        error = GetLastError();
        goto failed;
    }
    (void)CloseHandle(writer);
    writer = NULL;
    operation = "assign compiler to its process job";
    if (!AssignProcessToJobObject(job, child.hProcess)) {
        error = GetLastError();
        goto failed;
    }
    assigned = true;
    operation = "resume C17 compiler";
    ULONGLONG start = GetTickCount64();
    if (ResumeThread(child.hThread) == (DWORD)-1) { error = GetLastError(); goto failed; }

    for (;;) {
        operation = "read compiler diagnostics";
        if (!drain_pipe(reader, result, &error)) goto failed;
        ULONGLONG elapsed = GetTickCount64() - start;
        DWORD slice = elapsed >= timeout_ms ? 0 : timeout_ms - (DWORD)elapsed;
        DWORD waited = WaitForSingleObject(child.hProcess, slice < 10u ? slice : 10u);
        if (waited == WAIT_OBJECT_0) break;
        if (waited != WAIT_TIMEOUT) {
            operation = "wait for C17 compiler";
            error = GetLastError();
            goto failed;
        }
        if (!slice) { status = TF_NATIVE_BUILD_TIMED_OUT; goto done; }
    }
    operation = "get C17 compiler exit status";
    DWORD code;
    if (!GetExitCodeProcess(child.hProcess, &code)) { error = GetLastError(); goto failed; }
    result->compiler_exit_code = (uint32_t)code;
    status = code ? TF_NATIVE_BUILD_COMPILER_FAILED : TF_NATIVE_BUILD_OK;
    goto done;

failed:
    os_message(result, operation, compiler, error);
done:
    if (assigned) {
        if (!stop_job(job, &error)) {
            os_message(result, "stop compiler process tree", compiler, error);
            if (status == TF_NATIVE_BUILD_OK) status = TF_NATIVE_BUILD_HOST_ERROR;
        }
        if (!drain_pipe(reader, result, &error) && status == TF_NATIVE_BUILD_OK) {
            os_message(result, "read final compiler diagnostics", compiler, error);
            status = TF_NATIVE_BUILD_HOST_ERROR;
        }
    } else if (child.hProcess) {
        (void)TerminateProcess(child.hProcess, 1);
    }
    if (job) (void)CloseHandle(job);
    if (child.hThread) (void)CloseHandle(child.hThread);
    if (child.hProcess) (void)CloseHandle(child.hProcess);
    if (input != INVALID_HANDLE_VALUE) (void)CloseHandle(input);
    if (writer) (void)CloseHandle(writer);
    if (reader) (void)CloseHandle(reader);
    if (attributes_initialized) DeleteProcThreadAttributeList(attributes);
    free(attributes);
    free(line);
    return status;
}
#else

static bool make_pipe(int pair[2], OsError *error) {
    if (pipe(pair) != 0) { *error = errno; return false; }
    for (size_t i = 0; i < 2; ++i) {
        /* The caller may have closed some of fd 0/1/2. Put both pipe ends
         * above them before using dup2 file actions in the child. */
        if (pair[i] < 3) {
            int new_fd = fcntl(pair[i], F_DUPFD_CLOEXEC, 3);
            if (new_fd < 0) goto failed;
            (void)close(pair[i]);
            pair[i] = new_fd;
        } else if (fcntl(pair[i], F_SETFD, FD_CLOEXEC) < 0) goto failed;
    }
    if (fcntl(pair[0], F_SETFL, O_NONBLOCK) < 0) goto failed;
    return true;

failed:
    *error = errno;
    (void)close(pair[0]);
    (void)close(pair[1]);
    return false;
}

static bool drain_pipe(int pipe_fd, TfNativeBuildResult *result, OsError *error) {
    char buffer[PIPE_CHUNK];
    for (unsigned i = 0; i < PIPE_ROUNDS; ++i) {
        ssize_t count = read(pipe_fd, buffer, sizeof buffer);
        if (!count) return true;
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == EINTR) continue;
            *error = errno;
            return false;
        }
        capture(result, buffer, (size_t)count);
    }
    return true;
}

/* Only async-signal-safe operations between fork and exec. In particular,
 * execve never falls back to a shell for an unrecognized compiler file. */
static void exec_compiler(PathChar *const argv[], const PathChar *stage,
                          const int pair[2], const int launch[2]) {
    (void)close(pair[0]);
    (void)close(launch[0]);
    if (setpgid(0, 0) == 0 && chdir(stage) == 0) {
        int input = open("/dev/null", O_RDONLY);
        if (input >= 0 && (input == STDIN_FILENO || dup2(input, STDIN_FILENO) >= 0)) {
            if (input != STDIN_FILENO) (void)close(input);
            if (dup2(pair[1], STDOUT_FILENO) >= 0 && dup2(pair[1], STDERR_FILENO) >= 0) {
                (void)close(pair[1]);
                (void)execve(argv[0], argv, environ);
            }
        }
    }
    int error = errno;
    ssize_t count;
    do { count = write(launch[1], &error, sizeof error); } while (count < 0 && errno == EINTR);
    _exit(127);
}

static bool expired(const struct timespec *start, const struct timespec *now,
                    uint32_t timeout_ms) {
    uintmax_t seconds = (uintmax_t)now->tv_sec - (uintmax_t)start->tv_sec;
    long nanos = now->tv_nsec - start->tv_nsec;
    if (nanos < 0) {
        if (!seconds) return false;
        --seconds;
        nanos += 1000000000L;
    }
    return seconds > timeout_ms / 1000u ||
           (seconds == timeout_ms / 1000u &&
            (unsigned long)nanos >= (timeout_ms % 1000u) * 1000000UL);
}

static TfNativeBuildStatus run_compiler(PathChar *const argv[], const PathChar *stage,
                                       uint32_t timeout_ms,
                                       const char *compiler, TfNativeBuildResult *result) {
    int pair[2], launch[2], error, waited = 0;
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        os_message(result, "read compiler timeout clock", compiler, errno);
        return TF_NATIVE_BUILD_HOST_ERROR;
    }
    if (!make_pipe(pair, &error)) {
        os_message(result, "create compiler diagnostics pipe", compiler, error);
        return TF_NATIVE_BUILD_HOST_ERROR;
    }
    if (!make_pipe(launch, &error)) {
        os_message(result, "create compiler launch status pipe", compiler, error);
        (void)close(pair[0]);
        (void)close(pair[1]);
        return TF_NATIVE_BUILD_HOST_ERROR;
    }
    pid_t pid = fork();
    if (!pid) exec_compiler(argv, stage, pair, launch);
    error = errno;
    (void)close(pair[1]);
    (void)close(launch[1]);
    TfNativeBuildStatus status = TF_NATIVE_BUILD_HOST_ERROR;
    if (pid < 0) {
        os_message(result, "launch C17 compiler", compiler, error);
        (void)close(pair[0]);
        (void)close(launch[0]);
        return status;
    }

    /* Close the early-timeout race with the child's own setpgid. EACCES
     * means exec already happened, which requires successful child setpgid. */
    if (setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH) {
        os_message(result, "set compiler process group", compiler, errno);
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, &waited, 0) < 0 && errno == EINTR) {}
        (void)close(launch[0]);
        (void)close(pair[0]);
        return status;
    }

    bool exited = false, lost_child = false;
    for (;;) {
        if (!drain_pipe(pair[0], result, &error)) {
            os_message(result, "read compiler diagnostics", compiler, error);
            break;
        }
        siginfo_t child = {0};
        if (waitid(P_PID, (id_t)pid, &child, WEXITED | WNOHANG | WNOWAIT) == 0) {
            if (child.si_pid == pid) { exited = true; break; }
        } else if (errno != EINTR) {
            lost_child = errno == ECHILD;
            os_message(result, "wait for C17 compiler", compiler, errno);
            break;
        }
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            os_message(result, "read compiler timeout clock", compiler, errno);
            break;
        }
        if (expired(&start, &now, timeout_ms)) {
            status = TF_NATIVE_BUILD_TIMED_OUT;
            break;
        }
        const struct timespec delay = {0, 5000000L};
        (void)nanosleep(&delay, NULL);
    }

    /* Keep the group leader unreaped until after the signal, so its PID
     * cannot be reused as another process group's ID. */
    if (!lost_child) {
        if (kill(-pid, SIGKILL) != 0) {
            if (errno != ESRCH) {
                os_message(result, "stop compiler process group", compiler, errno);
                status = TF_NATIVE_BUILD_HOST_ERROR;
                exited = false;
            }
            /* A very early timeout can precede the child's setpgid. */
            (void)kill(pid, SIGKILL);
        }
        pid_t reaped;
        do { reaped = waitpid(pid, &waited, 0); } while (reaped < 0 && errno == EINTR);
        if (reaped != pid) {
            os_message(result, "reap C17 compiler", compiler, errno);
            status = TF_NATIVE_BUILD_HOST_ERROR;
            exited = false;
        }
    }
    if (!drain_pipe(pair[0], result, &error) && exited) {
        os_message(result, "read final compiler diagnostics", compiler, error);
        exited = false;
    }
    (void)close(pair[0]);
    if (exited) {
        ssize_t count;
        do { count = read(launch[0], &error, sizeof error); } while (count < 0 && errno == EINTR);
        if (count == (ssize_t)sizeof error)
            os_message(result, "launch C17 compiler", compiler, error);
        else if (count != 0)
            message(result, "could not read compiler launch status: %s", compiler);
        else {
            result->compiler_exit_code = WIFEXITED(waited) ? (uint32_t)WEXITSTATUS(waited) :
                WIFSIGNALED(waited) ? 128u + (uint32_t)WTERMSIG(waited) : 1u;
            status = result->compiler_exit_code ? TF_NATIVE_BUILD_COMPILER_FAILED : TF_NATIVE_BUILD_OK;
        }
    }
    (void)close(launch[0]);
    return status;
}
#endif


/* This list is also the alias protection set. Keep the source prefix in link
 * order; the remaining entries are the public/runtime dependency headers. */
static const PathChar *const runtime_files[] = {
    P("runtime/src/ternfold_rt.c"),
    P("runtime/src/ternfold_rt_numeric.c"),
    P("runtime/src/ternfold_rt_stack.c"),
    P("runtime/src/ternfold_rt_callbacks.c"),
    P("runtime/src/ternfold_rt_constants.c"),
    P("runtime/src/ternfold_rt_io.c"),
    P("runtime/src/ternfold_rt_builtins.c"),
    P("runtime/src/ternfold_rt_value_ops.c"),
    P("src/checked_numeric.c"),
    P("src/execution/scalars.c"),
    P("runtime/include/ternfold_rt.h"),
    P("runtime/include/ternfold_rt_numeric.h"),
    P("runtime/include/ternfold_rt_stack.h"),
    P("runtime/include/ternfold_rt_calls.h"),
    P("runtime/include/ternfold_rt_callbacks.h"),
    P("runtime/include/ternfold_rt_constants.h"),
    P("runtime/include/ternfold_rt_io.h"),
    P("runtime/include/ternfold_rt_builtins.h"),
    P("runtime/include/ternfold_rt_value_ops.h"),
    P("include/ternfold/checked_numeric.h"),
    P("src/execution/scalars.h")
};
static const PathChar *const include_dirs[] = {P("runtime/include"), P("include"), P("src/execution")};

enum {
    RUNTIME_SOURCES = 10,
    RUNTIME_FILES = sizeof runtime_files / sizeof *runtime_files,
    INCLUDE_DIRS = sizeof include_dirs / sizeof *include_dirs,
    INPUTS = RUNTIME_FILES + 2
};

static PathKind checked_path(const PathChar *path, bool follow, FileInfo *info,
                             const char *operation, const char *name,
                             TfNativeBuildResult *result) {
    OsError error = 0;
    PathKind kind = path_info(path, follow, info, &error);
    if (kind == PATH_ERROR || kind == MISSING) os_message(result, operation, name, error);
    return kind;
}

static TfNativeBuildStatus check_output(const PathChar *output, const char *name,
                                       const FileInfo inputs[INPUTS],
                                       TfNativeBuildResult *result) {
    FileInfo info;
    OsError error = 0;
    PathKind kind = path_info(output, false, &info, &error);
    if (kind == MISSING) return TF_NATIVE_BUILD_OK;
    if (kind == PATH_ERROR) {
        os_message(result, "inspect output path", name, error);
        return TF_NATIVE_BUILD_HOST_ERROR;
    }
    if (kind != REGULAR) {
        message(result, "output must be absent or a regular file (no symlink/reparse point): %s", name);
        return TF_NATIVE_BUILD_INVALID;
    }
    for (size_t i = 0; i < INPUTS; ++i) {
        if (same_file(&info, &inputs[i])) {
            message(result, "output aliases the generated C source, compiler, or runtime input: %s", name);
            return TF_NATIVE_BUILD_INVALID;
        }
    }
    return TF_NATIVE_BUILD_OK;
}

TfNativeBuildStatus tf_native_build(const TfNativeBuildOptions *options,
                                    TfNativeBuildResult *result) {
    TfNativeBuildResult scratch;
    if (!result) result = &scratch;
    *result = (TfNativeBuildResult){0};
    if (!options || !options->compiler_path || !*options->compiler_path ||
        !options->runtime_root || !*options->runtime_root ||
        !options->source_path || !*options->source_path ||
        !options->output_path || !*options->output_path) {
        message(result, "native build requires compiler, runtime root, generated C, and output paths");
        return TF_NATIVE_BUILD_INVALID;
    }

    const char *names[] = {options->compiler_path, options->source_path,
                           options->runtime_root, options->output_path};
    PathChar *paths[4] = {0}, *sources[RUNTIME_SOURCES] = {0}, *includes[INCLUDE_DIRS] = {0};
    PathChar *parent = NULL, *stage = NULL, *temporary = NULL;
    FileInfo inputs[INPUTS], ignored;
    TfNativeBuildStatus status = TF_NATIVE_BUILD_HOST_ERROR;
    OsError error = 0;
    for (size_t i = 0; i < 4u; ++i) {
        paths[i] = absolute_path(names[i], i != 2u);
        if (!paths[i]) {
#ifdef _WIN32
            error = GetLastError();
#else
            error = errno;
#endif
            os_message(result, "resolve native build path", names[i], error);
#ifdef _WIN32
            if (error == ERROR_INVALID_NAME || error == ERROR_NO_UNICODE_TRANSLATION ||
                error == ERROR_INSUFFICIENT_BUFFER) status = TF_NATIVE_BUILD_INVALID;
#else
            if (error == EINVAL || error == ENAMETOOLONG) status = TF_NATIVE_BUILD_INVALID;
#endif
            goto done;
        }
    }
    for (size_t i = 0; i < 3u; ++i) {
        PathKind kind = checked_path(paths[i], true, i == 2u ? &ignored : &inputs[i],
                                     "inspect native build input", names[i], result);
        if (kind == (i == 2u ? DIRECTORY : REGULAR)) continue;
        if (kind != PATH_ERROR && kind != MISSING)
            message(result, "expected a %s: %s", i == 2u ? "runtime root directory" : "regular input file",
                    names[i]);
        if (kind != PATH_ERROR) status = TF_NATIVE_BUILD_INVALID;
        goto done;
    }
    if (same_file(&inputs[0], &inputs[1])) {
        message(result, "generated C source aliases the C17 compiler: %s", options->source_path);
        status = TF_NATIVE_BUILD_INVALID;
        goto done;
    }
    parent = output_parent(paths[3]);
    if (!parent) { message(result, "allocate native output parent path"); goto done; }
    PathKind kind = checked_path(parent, true, &ignored, "inspect native output parent",
                                 options->output_path, result);
    if (kind != DIRECTORY) {
        if (kind != PATH_ERROR && kind != MISSING)
            message(result, "output parent must be an existing directory: %s", options->output_path);
        if (kind != PATH_ERROR) status = TF_NATIVE_BUILD_INVALID;
        goto done;
    }
    for (size_t i = 0; i < RUNTIME_FILES; ++i) {
        PathChar *path = join(paths[2], runtime_files[i]);
        if (!path) { message(result, "allocate runtime input path under %s", options->runtime_root); goto done; }
        /* These suffixes are ASCII constants; keep error messages on the same
         * UTF-8 boundary as the caller's root path. */
        char suffix[80];
        size_t length = path_length(runtime_files[i]);
        for (size_t j = 0; j <= length; ++j) suffix[j] = (char)runtime_files[i][j];
        kind = path_info(path, true, &inputs[i + 2u], &error);
        if (i < RUNTIME_SOURCES) sources[i] = path;
        else free(path);
        if (kind != REGULAR) {
            if (kind == PATH_ERROR || kind == MISSING)
                os_message(result, suffix, options->runtime_root, error);
            else
                message(result, "expected runtime input %s under %s", suffix, options->runtime_root);
            if (kind != PATH_ERROR) status = TF_NATIVE_BUILD_INVALID;
            goto done;
        }
        if (same_file(&inputs[1], &inputs[i + 2u]) || same_file(&inputs[0], &inputs[i + 2u])) {
            message(result, "generated C source or compiler aliases runtime input %s under %s",
                    suffix, options->runtime_root);
            status = TF_NATIVE_BUILD_INVALID;
            goto done;
        }
    }
    for (size_t i = 0; i < INCLUDE_DIRS; ++i) {
        includes[i] = join(paths[2], include_dirs[i]);
        if (!includes[i]) { message(result, "allocate runtime include path under %s", options->runtime_root); goto done; }
    }
    status = check_output(paths[3], options->output_path, inputs, result);
    if (status != TF_NATIVE_BUILD_OK) goto done;

    status = TF_NATIVE_BUILD_HOST_ERROR;
    stage = create_stage(parent, &error);
    if (!stage) {
        os_message(result, "create staging directory adjacent to output", options->output_path, error);
        goto done;
    }
#ifdef _WIN32
    PathChar *const local_executable = P("./program.exe");
#else
    PathChar *const local_executable = P("./program");
#endif
    temporary = join(stage, local_executable + 2);
    if (!temporary) { message(result, "allocate temporary executable path"); goto done; }

    PathChar *argv[40];
    size_t argc = 0;
    argv[argc++] = paths[0];
    argv[argc++] = P("-std=c17");
    argv[argc++] = P("-Wall");
    argv[argc++] = P("-Wextra");
    argv[argc++] = P("-Werror");
    argv[argc++] = P("-pedantic");
    argv[argc++] = P("-Wconversion");
    argv[argc++] = P("-Wshadow");
    argv[argc++] = P("-O0");
    argv[argc++] = P("-fno-fast-math");
    argv[argc++] = P("-ffp-contract=off");
    if (options->sanitize) {
        argv[argc++] = P("-g");
        argv[argc++] = P("-fsanitize=address,undefined");
        argv[argc++] = P("-fno-sanitize-recover=all");
        argv[argc++] = P("-fno-omit-frame-pointer");
    }
    for (size_t i = 0; i < INCLUDE_DIRS; ++i) {
        argv[argc++] = P("-I");
        argv[argc++] = includes[i];
    }
    argv[argc++] = P("-x");
    argv[argc++] = P("c");
    argv[argc++] = paths[1];
    for (size_t i = 0; i < RUNTIME_SOURCES; ++i) argv[argc++] = sources[i];
#ifndef _WIN32
    argv[argc++] = P("-lm");
#endif
    argv[argc++] = P("-o");
    /* LLVM's linker can interpret '%' as a temporary-name template in an
     * absolute -o path. Keep the tool's output path relative to its private
     * working directory. All inputs are already absolute. */
    argv[argc++] = local_executable;
    argv[argc] = NULL;

    uint32_t timeout = options->timeout_ms ? options->timeout_ms : TF_NATIVE_BUILD_DEFAULT_TIMEOUT_MS;
    status = run_compiler(argv, stage, timeout, options->compiler_path, result);
    if (status == TF_NATIVE_BUILD_TIMED_OUT && !*result->message)
        message(result, "C17 compiler timed out after %" PRIu32 " ms: %s", timeout, options->compiler_path);
    else if (status == TF_NATIVE_BUILD_COMPILER_FAILED && !*result->message)
        message(result, "C17 compiler exited with status %" PRIu32 ": %s",
                result->compiler_exit_code, options->compiler_path);
    if (status != TF_NATIVE_BUILD_OK) goto done;

    FileInfo built;
    kind = path_info(temporary, false, &built, &error);
    if (kind != REGULAR || !usable_executable(temporary, &built)) {
        if (kind == PATH_ERROR)
            os_message(result, "inspect compiler's staged executable", options->output_path, error);
        else
            message(result, "C17 compiler exited successfully without a usable executable for %s",
                    options->output_path);
        status = TF_NATIVE_BUILD_PUBLISH_FAILED;
        goto done;
    }
    status = check_output(paths[3], options->output_path, inputs, result);
    if (status != TF_NATIVE_BUILD_OK) goto done;
    if (!publish(temporary, paths[3], &error)) {
        os_message(result, "atomically replace native output", options->output_path, error);
        status = TF_NATIVE_BUILD_PUBLISH_FAILED;
    }

done:
    if (stage) {
        /* After a successful rename the private filename no longer exists. */
        error = discard_stage(stage, status == TF_NATIVE_BUILD_OK ? NULL : temporary);
        if (error) {
            result->cleanup_failed = true;
#ifdef _WIN32
            (void)snprintf(result->cleanup_message, sizeof result->cleanup_message,
                           "could not remove staging files adjacent to %s (Windows error %lu)",
                           options->output_path, (unsigned long)error);
#else
            (void)snprintf(result->cleanup_message, sizeof result->cleanup_message,
                           "could not remove staging files adjacent to %s (errno %d: %s)",
                           options->output_path, error, strerror(error));
#endif
        }
    }
    free(temporary);
    free(stage);
    free(parent);
    for (size_t i = 0; i < INCLUDE_DIRS; ++i) free(includes[i]);
    for (size_t i = 0; i < RUNTIME_SOURCES; ++i) free(sources[i]);
    for (size_t i = 0; i < 4u; ++i) free(paths[i]);
    return status;
}
