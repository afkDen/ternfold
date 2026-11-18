#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "ternfold/native_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define CHDIR _chdir
#define MKDIR(path) (_mkdir(path) == 0)
#define RMDIR _rmdir
#define EXE ".exe"
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHDIR chdir
#define MKDIR(path) (mkdir(path, 0700) == 0)
#define RMDIR rmdir
#define EXE ""
#endif

enum { TEST_PATH_CAP = 4096 };
static unsigned checks, failures;
#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { ++failures; fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } \
} while (0)

static const char valid_source[] = "@ source &(); %! $[] program.input";
static const char invalid_source[] = "@ bad &(); %! $[] program.c";
static const char final_output[] = "- executable &(); %! $[]" EXE;
static const char output_dir[] = "publish &() %! $[]";
static const char nested_output[] = "publish &() %! $[]/- new executable; &" EXE;
static const char fake_compiler[] = "./@ mock &(); %! $[]" EXE;
static const char marker[] = "compiler-child-escaped";
static const char sentinel[] = "previous executable bytes\r\n";
static const char program[] =
    "#include \"ternfold_rt.h\"\n"
    "#include \"ternfold/checked_numeric.h\"\n"
    "int main(void) {\n"
    "    int64_t sum = 0;\n"
    "    tf_value value = tf_unit();\n"
    "    return value.tag == TF_VALUE_UNIT && "
    "tf_checked_int_add(19, 23, &sum) == TF_CHECKED_NUMERIC_OK && sum == 42 ? 37 : 1;\n"
    "}\n";

static bool write_file(const char *path, const char *contents) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    size_t length = strlen(contents);
    bool ok = fwrite(contents, 1, length, file) == length;
    return fclose(file) == 0 && ok;
}

static bool has_contents(const char *path, const char *contents) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char buffer[512];
    size_t read = fread(buffer, 1, sizeof buffer, file);
    bool ok = !ferror(file) && feof(file) && read == strlen(contents) &&
              memcmp(buffer, contents, read) == 0;
    return fclose(file) == 0 && ok;
}

static void sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec delay = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
#endif
}

static bool absolute(char path[TEST_PATH_CAP], const char *input) {
#ifdef _WIN32
    DWORD length = GetFullPathNameA(input, TEST_PATH_CAP, path, NULL);
    return length && length < TEST_PATH_CAP;
#else
    if (*input == '/') {
        int n = snprintf(path, TEST_PATH_CAP, "%s", input);
        return n >= 0 && n < TEST_PATH_CAP;
    }
    char cwd[TEST_PATH_CAP];
    if (!getcwd(cwd, sizeof cwd)) return false;
    int n = snprintf(path, TEST_PATH_CAP, "%s/%s", cwd, input);
    return n >= 0 && n < TEST_PATH_CAP;
#endif
}

#ifdef _WIN32
static HANDLE launch(const char *path, const char *marker_path) {
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION child = {0};
    char line[TEST_PATH_CAP * 2 + 64];
    int n = marker_path ? snprintf(line, sizeof line, "\"%s\" --marker-child \"%s\"", path, marker_path)
                        : snprintf(line, sizeof line, "\"%s\"", path);
    if (n < 0 || (size_t)n >= sizeof line) return NULL;
    startup.cb = sizeof startup;
    if (!CreateProcessA(path, line, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &child)) return NULL;
    (void)CloseHandle(child.hThread);
    return child.hProcess;
}
#endif

static int run_built(const char *path) {
#ifdef _WIN32
    HANDLE child = launch(path, NULL);
    if (!child) return -1;
    DWORD code = 0;
    DWORD wait = WaitForSingleObject(child, 10000);
    bool ok = wait == WAIT_OBJECT_0 && GetExitCodeProcess(child, &code);
    if (wait != WAIT_OBJECT_0) (void)TerminateProcess(child, 1);
    (void)CloseHandle(child);
    return ok && code <= 255 ? (int)code : -1;
#else
    pid_t child = fork();
    if (child < 0) return -1;
    if (!child) { execl(path, path, (char *)NULL); _exit(127); }
    int status;
    pid_t reaped;
    do { reaped = waitpid(child, &status, 0); } while (reaped < 0 && errno == EINTR);
    return reaped == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static bool spawn_marker_child(const char *self, const char *source) {
    char marker_path[TEST_PATH_CAP];
    size_t prefix = strlen(source);
    while (prefix && source[prefix - 1u] != '/' && source[prefix - 1u] != '\\') --prefix;
    if (!prefix || prefix + sizeof marker > sizeof marker_path) return false;
    memcpy(marker_path, source, prefix);
    memcpy(marker_path + prefix, marker, sizeof marker);
#ifdef _WIN32
    HANDLE child = launch(self, marker_path);
    return child && CloseHandle(child);
#else
    (void)self;
    pid_t child = fork();
    if (child < 0) return false;
    if (!child) { sleep_ms(1500); _exit(write_file(marker_path, "escaped") ? 0 : 1); }
    return true;
#endif
}

static int mock_compiler(int argc, char **argv) {
    const char *out = NULL, *mode = NULL, *source = NULL;
    bool c17 = false, strict = false, no_fast = false, no_contract = false, unoptimized = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (strstr(argv[i], "mock-noisy.c")) mode = "noisy";
        else if (strstr(argv[i], "mock-timeout.c")) { mode = "timeout"; source = argv[i]; }
        else if (strstr(argv[i], "mock-no-output.c")) mode = "no-output";
        else if (!strcmp(argv[i], "-std=c17")) c17 = true;
        else if (!strcmp(argv[i], "-Werror")) strict = true;
        else if (!strcmp(argv[i], "-fno-fast-math")) no_fast = true;
        else if (!strcmp(argv[i], "-ffp-contract=off")) no_contract = true;
        else if (!strcmp(argv[i], "-O0")) unoptimized = true;
    }
    if (!out || strcmp(out, "./program" EXE) || !mode ||
        !c17 || !strict || !no_fast || !no_contract || !unoptimized) return 99;
    if (!strcmp(mode, "no-output")) {
        char cwd[TEST_PATH_CAP];
        return absolute(cwd, ".") && strstr(cwd, output_dir) ? 0 : 93;
    }
    if (!write_file(out, "partial staged artifact")) return 98;
    if (!strcmp(mode, "timeout")) {
        if (!spawn_marker_child(argv[0], source)) return 97;
        (void)fputs("timeout child started\n", stderr);
        (void)fflush(stderr);
        sleep_ms(5000);
        return 96;
    }
    static const char prefix[] = "compiler mock\0binary diagnostic\n";
    if (fwrite(prefix, 1, sizeof prefix - 1u, stdout) != sizeof prefix - 1u ||
        fflush(stdout) != 0) return 95;
    for (size_t i = 0; i < 65536u; ++i)
        if (fputc('x', stderr) == EOF) return 94;
    return 23;
}

static bool make_scratch(char path[TEST_PATH_CAP], bool sanitize) {
#ifdef _WIN32
    /* The Windows DLL loader can split semicolon-bearing application
     * directories even with DLLs beside the executable. Exercise ';' in
     * executable leaves in both modes, and in parents in strict mode. */
    const char *pattern = sanitize ? "@ build test &() %%! $[] %lu-%u" :
                                     "@ build test &(); %%! $[] %lu-%u";
    for (unsigned i = 0; i < 100u; ++i) {
        int n = snprintf(path, TEST_PATH_CAP, pattern,
                         (unsigned long)GetCurrentProcessId(), i);
        if (n < 0 || n >= TEST_PATH_CAP) return false;
        if (CreateDirectoryA(path, NULL)) return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
#else
    (void)sanitize;
    (void)strcpy(path, "@ build test &(); %! $[] XXXXXX");
    return mkdtemp(path) != NULL;
#endif
}

static bool no_staging(void) {
#ifdef _WIN32
    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(".tf-native-*", &entry);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    (void)FindClose(find);
    return false;
#else
    DIR *dir = opendir(".");
    if (!dir) return false;
    bool clean = true;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL)
        if (!strncmp(entry->d_name, ".tf-native-", 11u)) clean = false;
    if (errno) clean = false;
    return closedir(dir) == 0 && clean;
#endif
}

static bool hard_link(const char *original, const char *alias) {
#ifdef _WIN32
    return CreateHardLinkA(alias, original, NULL) != 0;
#else
    return link(original, alias) == 0;
#endif
}

#ifdef _WIN32
/* Optional side-by-side fallback for hosts whose sanitizer DLL search cannot
 * use the inherited PATH. The strict path and normal sanitizer path need no
 * copies. Only the private test directory receives these files. */
static const char *const sanitizer_dlls[] = {
    "libclang_rt.asan_dynamic-x86_64.dll", "libc++.dll", "libunwind.dll"
};

static void stage_test_dlls(const char *directory, const char *destination) {
    for (size_t i = 0; i < sizeof sanitizer_dlls / sizeof *sanitizer_dlls; ++i) {
        char path[TEST_PATH_CAP], target[TEST_PATH_CAP];
        int length = snprintf(path, sizeof path, "%s/%s", directory, sanitizer_dlls[i]);
        int out = snprintf(target, sizeof target, "%s/%s", destination, sanitizer_dlls[i]);
        CHECK(length > 0 && (size_t)length < sizeof path &&
              out > 0 && (size_t)out < sizeof target && CopyFileA(path, target, TRUE));
    }
}

static void discard_test_dlls(const char *directory) {
    for (size_t i = 0; i < sizeof sanitizer_dlls / sizeof *sanitizer_dlls; ++i) {
        char path[TEST_PATH_CAP];
        int length = snprintf(path, sizeof path, "%s/%s", directory, sanitizer_dlls[i]);
        CHECK(length > 0 && (size_t)length < sizeof path && remove(path) == 0);
    }
}
#endif

static void expect_build(const TfNativeBuildOptions *options, TfNativeBuildStatus expected,
                         TfNativeBuildResult *result) {
    TfNativeBuildStatus actual = tf_native_build(options, result);
    if (actual != expected) {
        fprintf(stderr, "build status %d; expected %d: %s\n", actual, expected, result->message);
        (void)fwrite(result->diagnostics, 1, result->diagnostics_length, stderr);
    }
    CHECK(actual == expected);
    if (result->cleanup_failed) fprintf(stderr, "%s\n", result->cleanup_message);
    CHECK(!result->cleanup_failed);
    CHECK(no_staging());
    CHECK(actual == TF_NATIVE_BUILD_OK || result->message[0] != '\0');
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-std=c17")) return mock_compiler(argc, argv);
    if (argc == 3 && !strcmp(argv[1], "--marker-child")) {
        sleep_ms(1500);
        return write_file(argv[2], "escaped") ? 0 : 1;
    }
#ifdef _WIN32
    /* Inherited by children. A loader error should report an exit code. */
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    const char *cc = argc > 1 ? argv[1] : getenv("TF_CC");
    const char *root = argc > 2 ? argv[2] : ".";
    const bool sanitize = argc > 3 && !strcmp(argv[3], "--sanitize");
    char compiler[TEST_PATH_CAP], runtime[TEST_PATH_CAP], scratch[TEST_PATH_CAP];
    char self[TEST_PATH_CAP], cwd[TEST_PATH_CAP], binary[TEST_PATH_CAP];
    if (argc > 4 || (argc > 3 && !sanitize) || !cc || !absolute(compiler, cc) ||
        !absolute(runtime, root) || !absolute(self, argv[0]) || !absolute(cwd, ".") ||
        !make_scratch(scratch, sanitize)) {
        fprintf(stderr, "test_native_build [COMPILER [RUNTIME_ROOT [--sanitize]]] (or TF_CC)\n");
        return 1;
    }
    if (CHDIR(scratch) != 0) { (void)RMDIR(scratch); return 1; }
#ifdef _WIN32
    const char *test_dll_dir = sanitize ? getenv("TF_NATIVE_BUILD_TEST_DLL_DIR") : NULL;
    if (test_dll_dir) stage_test_dlls(test_dll_dir, ".");
#endif

    TfNativeBuildOptions options = {compiler, runtime, valid_source, final_output, 0, sanitize};
    TfNativeBuildResult result;
    CHECK(tf_native_build(NULL, &result) == TF_NATIVE_BUILD_INVALID);
    CHECK(tf_native_build(NULL, NULL) == TF_NATIVE_BUILD_INVALID);
    CHECK(write_file(valid_source, program));
    CHECK(write_file(invalid_source, "#error intentional-m11-compiler-failure\n"));
    CHECK(write_file(final_output, sentinel));
    CHECK(MKDIR(output_dir));
#ifdef _WIN32
    if (test_dll_dir) stage_test_dlls(test_dll_dir, output_dir);
#endif
    CHECK(write_file(nested_output, sentinel));
    CHECK(hard_link(self, fake_compiler));
    CHECK(absolute(binary, final_output));

    options.source_path = invalid_source;
    expect_build(&options, TF_NATIVE_BUILD_COMPILER_FAILED, &result);
    CHECK(result.compiler_exit_code != 0u);
    CHECK(strstr(result.diagnostics, "intentional-m11-compiler-failure") != NULL);
    CHECK(has_contents(final_output, sentinel));
    options.source_path = valid_source;

    options.output_path = valid_source;
    expect_build(&options, TF_NATIVE_BUILD_INVALID, &result);
    CHECK(strstr(result.message, "aliases") != NULL);
    CHECK(has_contents(valid_source, program));
    CHECK(hard_link(valid_source, "source-link"));
    options.output_path = "./source-link";
    expect_build(&options, TF_NATIVE_BUILD_INVALID, &result);
    CHECK(strstr(result.message, "aliases") != NULL);
    CHECK(has_contents("source-link", program));
    options.output_path = "nonexistent/result.exe";
    expect_build(&options, TF_NATIVE_BUILD_INVALID, &result);
    options.output_path = "source-link/";
    expect_build(&options, TF_NATIVE_BUILD_INVALID, &result);
    options.output_path = final_output;

    char runtime_input[TEST_PATH_CAP];
    int length = snprintf(runtime_input, sizeof runtime_input, "%s/include/ternfold/checked_numeric.h", runtime);
    CHECK(length >= 0 && (size_t)length < sizeof runtime_input);
    CHECK(hard_link(runtime_input, "runtime-link"));
    options.output_path = "runtime-link";
    expect_build(&options, TF_NATIVE_BUILD_INVALID, &result);
    CHECK(strstr(result.message, "aliases") != NULL);
    options.source_path = "runtime-link";
    options.output_path = final_output;
    expect_build(&options, TF_NATIVE_BUILD_INVALID, &result);
    CHECK(strstr(result.message, "aliases") != NULL);
    options.source_path = valid_source;
    options.runtime_root = ".";
    expect_build(&options, TF_NATIVE_BUILD_INVALID, &result);
    CHECK(strstr(result.message, "ternfold_rt.c") != NULL);
    options.runtime_root = runtime;

#ifdef _WIN32
    options.output_path = "NUL.exe";
    expect_build(&options, TF_NATIVE_BUILD_INVALID, &result);
    options.output_path = "destination.exe:stream";
    expect_build(&options, TF_NATIVE_BUILD_INVALID, &result);
    options.output_path = final_output;
    HANDLE lock = CreateFileA(final_output, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(lock != INVALID_HANDLE_VALUE);
    if (lock != INVALID_HANDLE_VALUE) {
        expect_build(&options, TF_NATIVE_BUILD_PUBLISH_FAILED, &result);
        CHECK(strstr(result.message, "atomically replace") != NULL);
        CHECK(has_contents(final_output, sentinel));
        CHECK(CloseHandle(lock));
    }
#else
    CHECK(symlink(valid_source, "source-symlink") == 0);
    options.output_path = "source-symlink";
    expect_build(&options, TF_NATIVE_BUILD_INVALID, &result);
    CHECK(has_contents(valid_source, program));
    CHECK(remove("source-symlink") == 0);
    options.output_path = final_output;
#endif

    options.compiler_path = "not-a-compiler.exe";
    expect_build(&options, TF_NATIVE_BUILD_INVALID, &result);
    CHECK(has_contents(final_output, sentinel));
    CHECK(write_file("bad-format.exe", "echo unsafe > compiler-child-escaped\n"));
#ifndef _WIN32
    CHECK(chmod("bad-format.exe", 0700) == 0);
#endif
    options.compiler_path = "bad-format.exe";
    expect_build(&options, TF_NATIVE_BUILD_HOST_ERROR, &result);
    CHECK(strstr(result.message, "launch") != NULL);
    CHECK(has_contents(final_output, sentinel));

    options.compiler_path = fake_compiler;
    options.output_path = fake_compiler;
    expect_build(&options, TF_NATIVE_BUILD_INVALID, &result);
    CHECK(strstr(result.message, "aliases") != NULL);
    options.output_path = final_output;
    CHECK(write_file("mock-noisy.c", program));
    CHECK(write_file("mock-no-output.c", program));
    CHECK(write_file("mock-timeout.c", program));
    options.source_path = "mock-noisy.c";
    expect_build(&options, TF_NATIVE_BUILD_COMPILER_FAILED, &result);
    CHECK(result.compiler_exit_code == 23u);
    CHECK(result.diagnostics_truncated);
    CHECK(result.diagnostics_length == TF_NATIVE_BUILD_DIAGNOSTICS_CAP);
    CHECK(!memcmp(result.diagnostics, "compiler mock\0binary diagnostic", 31u));
    CHECK(result.diagnostics[TF_NATIVE_BUILD_DIAGNOSTICS_CAP] == '\0');
    CHECK(has_contents(final_output, sentinel));
    options.source_path = "mock-no-output.c";
    options.output_path = nested_output;
    expect_build(&options, TF_NATIVE_BUILD_PUBLISH_FAILED, &result);
    CHECK(!result.diagnostics_length && !result.diagnostics_truncated);
    CHECK(has_contents(nested_output, sentinel));
    CHECK(remove(nested_output) == 0);
    options.source_path = "mock-timeout.c";
    options.output_path = final_output;
    options.timeout_ms = 900u;
    expect_build(&options, TF_NATIVE_BUILD_TIMED_OUT, &result);
    CHECK(strstr(result.message, "900 ms") != NULL);
    CHECK(strstr(result.diagnostics, "timeout child started") != NULL);
    CHECK(has_contents(final_output, sentinel));
    sleep_ms(1600);
    FILE *escaped = fopen(marker, "rb");
    CHECK(!escaped);
    if (escaped) { (void)fclose(escaped); (void)remove(marker); }

    options.compiler_path = compiler;
    options.source_path = valid_source;
    options.timeout_ms = 0;
    expect_build(&options, TF_NATIVE_BUILD_OK, &result);
    CHECK(result.compiler_exit_code == 0u);
    CHECK(run_built(binary) == 37);
    options.source_path = invalid_source;
    expect_build(&options, TF_NATIVE_BUILD_COMPILER_FAILED, &result);
    CHECK(run_built(binary) == 37);
    options.source_path = valid_source;
    options.output_path = nested_output;
    expect_build(&options, TF_NATIVE_BUILD_OK, &result);
    CHECK(absolute(binary, nested_output));
    CHECK(run_built(binary) == 37);
    CHECK(remove(nested_output) == 0);
#ifdef _WIN32
    if (test_dll_dir) discard_test_dlls(output_dir);
#endif
    CHECK(RMDIR(output_dir) == 0);

    const char *files[] = {valid_source, invalid_source, final_output, fake_compiler,
                           "source-link", "runtime-link", "bad-format.exe",
                           "mock-noisy.c", "mock-no-output.c", "mock-timeout.c"};
    for (size_t i = 0; i < sizeof files / sizeof *files; ++i) CHECK(remove(files[i]) == 0);
#ifdef _WIN32
    if (test_dll_dir) discard_test_dlls(".");
#endif
    CHECK(CHDIR(cwd) == 0);
    CHECK(RMDIR(scratch) == 0);
    if (failures) {
        fprintf(stderr, "native build: %u of %u assertions failed\n", failures, checks);
        return 1;
    }
    printf("native build: %u assertions passed\n", checks);
    return 0;
}
