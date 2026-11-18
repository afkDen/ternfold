/* Native development driver. Compiler processes receive argv, never shell text. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define MKDIR(path) _mkdir(path)
#define EXE ".exe"
#else
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define MKDIR(path) mkdir(path, 0777)
#define EXE ""
#endif

static int run_process(const char *const *args, unsigned timeout_ms) {
    fflush(NULL);
#ifdef _WIN32
    /* The Windows CRT spawn API joins argv without protecting spaces. Quote
     * every element using its backslash/quote rules, with no shell involved. */
    char *quoted[96];
    size_t count = 0;
    intptr_t status;
    for (; count < 95 && args[count]; ++count) {
        size_t length = strlen(args[count]), at = 0, i = 0;
        if (length > (SIZE_MAX - 3) / 2) { while (count) free(quoted[--count]); return 2; }
        quoted[count] = malloc(length * 2 + 3);
        if (!quoted[count]) { while (count) free(quoted[--count]); return 2; }
        quoted[count][at++] = '"';
        while (i < length) {
            size_t slashes = 0;
            while (i < length && args[count][i] == '\\') { ++slashes; ++i; }
            if (i == length || args[count][i] == '"') slashes *= 2;
            while (slashes) { quoted[count][at++] = '\\'; --slashes; }
            if (i < length) {
                if (args[count][i] == '"') quoted[count][at++] = '\\';
                quoted[count][at++] = args[count][i++];
            }
        }
        quoted[count][at++] = '"'; quoted[count][at] = '\0';
    }
    if (args[count] != NULL) { while (count) free(quoted[--count]); return 2; }
    quoted[count] = NULL;
    status = _spawnvp(timeout_ms != 0 ? _P_NOWAIT : _P_WAIT, args[0], (const char *const *)quoted);
    while (count) free(quoted[--count]);
    if (status == -1) { perror(args[0]); return 2; }
    if (timeout_ms != 0) {
        HANDLE child=(HANDLE)status;
        DWORD result=0;
        const DWORD wait=WaitForSingleObject(child,(DWORD)timeout_ms);
        if (wait!=WAIT_OBJECT_0 || !GetExitCodeProcess(child,&result)) {
            if (wait!=WAIT_OBJECT_0) {
                (void)TerminateProcess(child,2);
                (void)WaitForSingleObject(child,1000);
            }
            if (wait==WAIT_TIMEOUT) {
                (void)fprintf(stderr,"%s: native test timed out\n",args[0]);
            } else (void)fprintf(stderr,"%s: cannot collect native test status\n",args[0]);
            (void)CloseHandle(child);
            return 2;
        }
        (void)CloseHandle(child);
        status=(intptr_t)result;
    }
    if ((uint32_t)status > (uint32_t)INT_MAX) {
        fprintf(stderr,"%s: Windows process failure 0x%08" PRIX32 "\n",args[0],(uint32_t)status);
        return 2;
    }
    return (int)status;
#else
    pid_t pid = fork();
    int status;
    if (pid < 0) { perror("fork"); return 2; }
    if (pid == 0) { execvp(args[0], (char *const *)args); perror(args[0]); _exit(127); }
    if (timeout_ms!=0) {
        struct timespec start,now;
        bool done=false,timed_out=false,unreaped=true;
        if (clock_gettime(CLOCK_MONOTONIC,&start)==0) {
            for (;;) {
                const pid_t result=waitpid(pid,&status,WNOHANG);
                if (result==pid) { done=true; break; }
                if (result<0 && errno!=EINTR) { unreaped=errno!=ECHILD; break; }
                if (clock_gettime(CLOCK_MONOTONIC,&now)!=0) break;
                const double elapsed=(difftime(now.tv_sec,start.tv_sec)*1000.0)+
                                     ((double)now.tv_nsec-(double)start.tv_nsec)/1000000.0;
                if (elapsed>=(double)timeout_ms) { timed_out=true; break; }
                (void)nanosleep(&(struct timespec){0,10000000},NULL);
            }
        }
        if (!done) {
            if (unreaped) {
                (void)kill(pid,SIGKILL);
                while (waitpid(pid,&status,0)<0 && errno==EINTR) {}
            }
            (void)fprintf(stderr,"%s: %s\n",args[0],timed_out ? "native test timed out" : "cannot collect native test status");
            return 2;
        }
    } else while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) return 2; }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
#endif
}

static int run(const char *const *args) { return run_process(args,0); }

static int directory(const char *path) {
    if (MKDIR(path) != 0 && errno != EEXIST) { perror(path); return 2; }
    return 0;
}
static int audit_run(const char *path) {
    FILE *marker = fopen("LEXICAL_PACKAGE", "rb");
    const char *args[] = {path, marker ? "--lexical" : NULL, NULL};
    if (marker) fclose(marker);
    return run(args);
}

static int compile(const char *cc, const char *output, const char *last,
                   int sanitize, int library, int release) {
    const char *args[96]; /* Also bounded by the Windows argv launcher. */
    size_t n = 0;
    const bool compiler = strcmp(last,"src/compiler_main.c") == 0;
    const bool c_emit = strcmp(last,"tests/test_c_emit.c") == 0;
    const bool c_metadata = strcmp(last,"tests/test_c_metadata.c") == 0;
    const bool native_build = strcmp(last,"tests/test_native_build.c") == 0;
    const bool native_driver = strcmp(last,"tests/test_driver_native.c") == 0;
    const bool repl_input = strcmp(last,"tests/test_repl_input.c") == 0;
    const bool repl_resolve = strcmp(last,"tests/test_repl_resolve.c") == 0;
    const bool repl = strcmp(last,"tests/test_repl.c") == 0;
    const bool editor = strcmp(last,"tests/test_editor.c") == 0;
    const bool formatter = strcmp(last,"tests/test_formatter.c") == 0;
    const bool lifetime_test = strcmp(last,"tests/test_lifetimes.c") == 0;
    const bool optimization_test = strcmp(last,"tests/test_optimization.c") == 0;
    const bool optimizer = compiler || c_emit || native_driver || optimization_test;
    const bool runtime_calls = strcmp(last,"tests/native/test_runtime_calls.c") == 0;
    const bool runtime_numeric = strcmp(last,"tests/native/test_runtime_numeric.c") == 0;
    const bool runtime_constants = strcmp(last,"tests/native/test_runtime_constants.c") == 0;
    const bool runtime_io = strcmp(last,"tests/native/test_runtime_io.c") == 0;
    const bool runtime_builtins = strcmp(last,"tests/native/test_runtime_builtins.c") == 0;
    const bool runtime_stack = lifetime_test || strcmp(last,"tests/native/test_runtime_stack.c") == 0;
    const bool runtime_callbacks = strcmp(last,"tests/native/test_runtime_callbacks.c") == 0;
    const bool runtime_value_ops = strcmp(last,"tests/native/test_runtime_value_ops.c") == 0;
    const bool runtime_values = strcmp(last,"tests/native/test_runtime_values.c") == 0;
    const bool interpreter = compiler || repl || optimization_test || strcmp(last,"tests/test_repl_eval.c") == 0 ||
                             strcmp(last,"tests/test_trace_execution.c") == 0 ||
                             strcmp(last,"tests/test_interpreter_core.c") == 0;
    const bool core_lower = compiler || strcmp(last,"tests/test_core_lower.c") == 0 ||
                            strcmp(last,"tests/test_core_match.c") == 0 || interpreter || c_emit || native_driver || lifetime_test;
    const bool core_dump = compiler || strcmp(last,"tests/test_core_dump.c") == 0;
    const bool core_verify = core_lower || core_dump || c_metadata || strcmp(last,"tests/test_core_verify.c") == 0;
    const bool machine_analysis = compiler || strcmp(last,"tests/test_machine_plan.c") == 0 ||
                                  editor ||
                                  strcmp(last,"tests/property/test_plan_properties.c") == 0 ||
                                  strcmp(last,"tests/test_plan_dump.c") == 0 ||
                                  strcmp(last,"tests/test_c01_numeric.c") == 0 ||
                                  strcmp(last,"tests/test_core_layout.c") == 0 ||
                                  strcmp(last,"tests/test_core_initializers.c") == 0 || core_lower;
    const bool full_typing = machine_analysis || strcmp(last,"tests/test_check_constants.c") == 0;
    const bool builtins = full_typing || strcmp(last,"tests/test_check_builtins.c") == 0;
    const bool checked_bodies = builtins || strcmp(last,"tests/test_check.c") == 0;
    const bool checked_numeric = full_typing || runtime_numeric || runtime_builtins || runtime_callbacks || strcmp(last,"tests/test_checked_numeric.c") == 0;
    args[n++] = cc;
    args[n++] = "-std=c17";
    args[n++] = "-Wall"; args[n++] = "-Wextra"; args[n++] = "-Werror";
    args[n++] = "-pedantic"; args[n++] = "-Wconversion"; args[n++] = "-Wshadow";
    args[n++] = "-g";
    if (release) args[n++] = "-O2";
    if (sanitize) { args[n++] = "-fsanitize=address,undefined";
        args[n++] = "-fno-sanitize-recover=all";
        args[n++] = "-fno-omit-frame-pointer"; }
    args[n++] = "-Iinclude";
    if (c_emit) {
        args[n++] = "-DTF_TEST_EMIT_ALLOC";
        args[n++] = "-Dmalloc=tf_emit_test_malloc";
        args[n++] = "-Dcalloc=tf_emit_test_calloc";
        args[n++] = "-Dfree=tf_emit_test_free";
    }
    if (runtime_values || runtime_calls || runtime_numeric || runtime_constants || runtime_io || runtime_builtins || runtime_stack || runtime_callbacks || runtime_value_ops) {
        args[n++] = "-Iruntime/include";
        if (runtime_values || runtime_builtins || runtime_stack || runtime_value_ops) args[n++] = "-DTF_RT_TESTING";
        args[n++] = "runtime/src/ternfold_rt.c";
        if (runtime_calls || lifetime_test) args[n++] = "runtime/src/ternfold_rt_calls.c";
        if (runtime_numeric || runtime_callbacks) args[n++] = "runtime/src/ternfold_rt_numeric.c";
        if (runtime_constants) args[n++] = "runtime/src/ternfold_rt_constants.c";
        if (runtime_io) args[n++] = "runtime/src/ternfold_rt_io.c";
        if (runtime_builtins) { args[n++] = "runtime/src/ternfold_rt_builtins.c"; args[n++] = "-Isrc/execution"; }
        if (runtime_stack || runtime_callbacks) args[n++] = "runtime/src/ternfold_rt_stack.c";
        if (runtime_stack || runtime_callbacks) {
            args[n++] = "src/execution/trace.c";
            args[n++] = "src/execution/trace_value.c";
        }
        if (runtime_callbacks) args[n++] = "runtime/src/ternfold_rt_callbacks.c";
        if (runtime_value_ops) args[n++] = "runtime/src/ternfold_rt_value_ops.c";
    }
    if (library) {
        args[n++] = "src/source.c"; args[n++] = "src/lexer.c";
        args[n++] = "src/report.c";
    }
    if (!strcmp(last, "tests/test_source_manager.c")) args[n++] = "src/source_manager.c";
    if (!strcmp(last, "tests/test_execution_trace.c")) args[n++] = "src/execution/trace.c";
    if (!strcmp(last, "tests/test_trace_io.c")) {
        args[n++] = "src/execution/trace.c"; args[n++] = "src/execution/trace_io.c";
    }
    if (!strcmp(last, "tests/test_diagnostics.c")) args[n++] = "src/diagnostics.c";
    if (!strcmp(last, "tests/test_diagnostic_json.c")) args[n++] = "src/diagnostics.c";
    if (!strcmp(last, "tests/test_tokens_cli.c")) args[n++] = "src/token_dump.c";
    if (!strcmp(last, "tests/test_pattern_coverage.c") || !strcmp(last, "tests/test_typed_patterns.c"))
        args[n++] = "src/pattern_coverage.c";
    if (!strcmp(last, "tests/test_numeric.c") || !strcmp(last, "tests/test_literal_value.c") ||
        !strcmp(last, "tests/test_typed_patterns.c"))
        args[n++] = "src/numeric.c";
    if (checked_numeric) args[n++] = "src/checked_numeric.c";
    if (core_verify || !strcmp(last,"tests/test_core_primitives.c")) args[n++] = "src/core/primitive_rules.c";
    if (core_verify || !strcmp(last,"tests/test_core_nodes.c")) args[n++] = "src/core/nodes.c";
    if (core_verify) args[n++] = "src/core/verify.c";
    if (optimizer) {
        args[n++] = "-fno-fast-math"; args[n++] = "-ffp-contract=off";
        args[n++] = "src/core/optimize.c";
    }
    if (optimizer || lifetime_test) args[n++] = "src/backend/lifetimes.c";
    if (core_dump) args[n++] = "src/core/dump.c";
    if (core_lower || !strcmp(last,"tests/test_core_layout.c") || !strcmp(last,"tests/test_core_initializers.c"))
        args[n++] = "src/core/lower_types.c";
    if (core_lower || !strcmp(last,"tests/test_core_initializers.c")) args[n++] = "src/core/lower_constants.c";
    if (core_lower) { args[n++] = "src/core/lower.c"; args[n++] = "src/core/lower_match.c"; }
    if (optimizer) {
        args[n++] = "src/backend/c_nodes.c";
        args[n++] = "src/backend/c_emit.c";
    }
    if (optimizer || c_metadata) args[n++] = "src/backend/c_metadata.c";
    if (compiler || native_driver) args[n++] = "src/backend/driver_native.c";
    if (compiler || native_build || native_driver) args[n++] = "src/backend/native_build.c";
    if (interpreter) {
        args[n++] = "src/execution/values.c";
        args[n++] = "src/execution/operations.c";
        args[n++] = "src/execution/interpreter.c";
        args[n++] = "src/execution/builtins.c";
        args[n++] = "src/execution/repl_value.c";
        args[n++] = "src/execution/trace.c";
        args[n++] = "src/execution/trace_value.c";
    }
    if (interpreter || runtime_builtins || !strcmp(last,"tests/test_exec_scalars.c")) args[n++] = "src/execution/scalars.c";
    if (compiler || repl) {
        args[n++] = "src/execution/host_io.c"; args[n++] = "src/execution/driver_run.c";
        args[n++] = "src/execution/trace_io.c"; args[n++] = "src/execution/trace_artifact.c";
        args[n++] = "src/repl.c";
    }
    if (!strcmp(last,"tests/test_trace_execution.c")) {
        args[n++] = "src/execution/host_io.c"; args[n++] = "src/execution/trace_io.c";
        args[n++] = "src/execution/trace_artifact.c";
    }
    if (compiler || repl || repl_input) args[n++] = "src/repl_input.c";
    if (machine_analysis) args[n++] = "src/machine_plan.c";
    if (compiler || repl || !strcmp(last,"tests/test_plan_dump.c")) args[n++] = "src/plan_dump.c";
    if (checked_bodies) {
        args[n++] = "src/check.c";
        args[n++] = "src/typed_signatures.c";
        args[n++] = "src/type_operators.c";
        args[n++] = "src/types.c";
        args[n++] = "src/literal_value.c";
        args[n++] = "src/numeric.c";
    }
    if (builtins) args[n++] = "src/check_builtins.c";
    if (full_typing) {
        args[n++] = "src/check_constants.c";
        args[n++] = "src/check_pipeline.c";
        args[n++] = "src/typed_patterns.c";
        args[n++] = "src/pattern_coverage.c";
    }
    if (!strcmp(last, "tests/test_literal_value.c") || !strcmp(last, "tests/test_typed_patterns.c"))
        args[n++] = "src/literal_value.c";
    if (!strcmp(last, "tests/test_type_store.c") || !strcmp(last, "tests/test_typed_signatures.c") ||
        !strcmp(last, "tests/test_typed_patterns.c"))
        args[n++] = "src/types.c";
    if (!strcmp(last, "tests/test_typed_patterns.c")) {
        args[n++] = "src/typed_patterns.c";
        args[n++] = "src/typed_signatures.c";
    }
    if (!strcmp(last, "tests/test_typed_signatures.c")) {
        args[n++] = "src/typed_signatures.c";
        args[n++] = "src/type_operators.c";
    }
    if (!strcmp(last, "tests/test_parser.c") || !strcmp(last, "tests/test_frontend_fuzz.c") || formatter ||
        !strcmp(last, "tests/test_ast_dump.c") ||
        !strcmp(last, "tests/test_resolution.c") || compiler || repl_resolve || repl_input) {
        args[n++] = "src/ast.c";
        args[n++] = "src/parser.c";
        args[n++] = "src/ast_dump.c";
        args[n++] = "src/diagnostics.c";
    }
    if (compiler || !strcmp(last, "tests/test_resolution.c") || repl_resolve) {
        args[n++] = "src/source_manager.c";
        args[n++] = "src/resolve.c";
    }
    if (!strcmp(last, "tests/test_literal_value.c") || !strcmp(last, "tests/test_type_store.c") ||
        !strcmp(last, "tests/test_typed_signatures.c") || !strcmp(last, "tests/test_typed_patterns.c") ||
        (checked_bodies && !compiler)) {
        args[n++] = "src/ast.c";
        args[n++] = "src/parser.c";
        args[n++] = "src/diagnostics.c";
        if (strcmp(last, "tests/test_literal_value.c")) {
            args[n++] = "src/source_manager.c";
            args[n++] = "src/resolve.c";
        }
    }
    if (compiler) args[n++] = "src/token_dump.c";
    if (compiler || formatter) args[n++] = "src/formatting/format.c";
    if (compiler || editor) {
        args[n++] = "src/editor/snapshot.c"; args[n++] = "src/editor/protocol.c";
    }
    if (!strcmp(last,"tests/experiments/test_composition.c")) args[n++] = "experiments/composition/prototype.c";
    args[n++] = last;
#ifndef _WIN32
    if (checked_numeric) args[n++] = "-lm";
#endif
    args[n++] = "-o"; args[n++] = output; args[n] = NULL;
    return run(args);
}

static int metadata_native(const char *cc,int sanitize,int release,int empty) {
    const char *args[32];
    size_t n=0;
    const char *binary=empty ? "build/test-c-metadata-empty" EXE : "build/test-c-metadata-library" EXE;
    args[n++]=cc; args[n++]="-std=c17";
    args[n++]="-Wall"; args[n++]="-Wextra"; args[n++]="-Werror";
    args[n++]="-pedantic"; args[n++]="-Wconversion"; args[n++]="-Wshadow";
    if (sanitize) {
        args[n++]="-fsanitize=address,undefined"; args[n++]="-fno-sanitize-recover=all";
        args[n++]="-fno-omit-frame-pointer";
    }
    if (release) args[n++]="-O2";
    args[n++]="-g"; args[n++]="-Iruntime/include"; args[n++]="-Ibuild"; args[n++]="-Iinclude";
    args[n++]="-DTF_TEST_METADATA_GENERATED";
    if (empty) args[n++]="-DTF_TEST_METADATA_EMPTY";
    args[n++]="runtime/src/ternfold_rt.c"; args[n++]="runtime/src/ternfold_rt_constants.c";
    args[n++]="tests/test_c_metadata.c"; args[n++]="-o"; args[n++]=binary; args[n]=NULL;
    int status=run(args);
    if (status) return status;
    const char *const launch[]={binary,NULL};
    return run_process(launch,30000);
}

static int native_output_fault(const char *cc,int sanitize,int release) {
    const char *args[48];
    size_t n=0;
    const char *binary=sanitize ? "build/test-c-output-fault-san" EXE : "build/test-c-output-fault" EXE;
    args[n++]=cc; args[n++]="-std=c17";
    args[n++]="-Wall"; args[n++]="-Wextra"; args[n++]="-Werror";
    args[n++]="-pedantic"; args[n++]="-Wconversion"; args[n++]="-Wshadow";
    if (sanitize) {
        args[n++]="-fsanitize=address,undefined"; args[n++]="-fno-sanitize-recover=all";
        args[n++]="-fno-omit-frame-pointer";
    }
    if (release) args[n++]="-O2";
    args[n++]="-g"; args[n++]="-Iruntime/include"; args[n++]="-Iinclude"; args[n++]="-Ibuild";
    args[n++]="runtime/src/ternfold_rt.c"; args[n++]="runtime/src/ternfold_rt_stack.c";
    args[n++]="runtime/src/ternfold_rt_constants.c"; args[n++]="runtime/src/ternfold_rt_io.c";
    args[n++]="src/execution/trace.c"; args[n++]="src/execution/trace_value.c";
    args[n++]="src/execution/trace_io.c"; args[n++]="src/execution/trace_artifact.c";
    args[n++]="tests/test_c_output_fault.c"; args[n++]="-o"; args[n++]=binary; args[n]=NULL;
    int status=run(args);
    if (status) return status;
    const char *const launch[]={binary,NULL};
    return run_process(launch,30000);
}

static int native_trace_fault(const char *cc,int sanitize,int release,bool language_fault) {
    const char *args[64]; size_t n=0;
    const char *binary=language_fault ? "build/test-native-trace-language-fault" EXE :
                                       "build/test-native-trace-success" EXE;
    args[n++]=cc; args[n++]="-std=c17"; args[n++]="-Wall"; args[n++]="-Wextra";
    args[n++]="-Werror"; args[n++]="-pedantic"; args[n++]="-Wconversion"; args[n++]="-Wshadow";
    args[n++]="-g"; args[n++]="-Iruntime/include"; args[n++]="-Iinclude"; args[n++]="-Ibuild";
    args[n++]="-Isrc/execution";
    if (sanitize) { args[n++]="-fsanitize=address,undefined"; args[n++]="-fno-sanitize-recover=all"; args[n++]="-fno-omit-frame-pointer"; }
    if (release) args[n++]="-O2";
    if (language_fault) args[n++]="-DTF_TEST_TRACE_LANGUAGE_FAULT";
    const char *sources[]={
        "runtime/src/ternfold_rt.c","runtime/src/ternfold_rt_numeric.c","runtime/src/ternfold_rt_stack.c",
        "runtime/src/ternfold_rt_callbacks.c","runtime/src/ternfold_rt_constants.c","runtime/src/ternfold_rt_io.c",
        "runtime/src/ternfold_rt_builtins.c","runtime/src/ternfold_rt_value_ops.c","src/checked_numeric.c",
        "src/execution/scalars.c","src/execution/trace.c","src/execution/trace_value.c",
        "src/execution/trace_io.c","src/execution/trace_artifact.c","tests/test_c_trace_allocation.c"
    };
    for (size_t i=0;i<sizeof sources/sizeof *sources;++i) args[n++]=sources[i];
#ifndef _WIN32
    args[n++]="-lm";
#endif
    args[n++]="-o"; args[n++]=binary; args[n]=NULL;
    int status=run(args); if (status) return status;
    const char *const test[]={binary,NULL};
    return run_process(test,30000);
}

int main(int argc, char **argv) {
    const char *cc = getenv("TF_CC");
    const char *mode = argc > 1 ? argv[1] : "check";
    int status, sanitize = strcmp(mode, "sanitize") == 0;
    int release = strcmp(mode, "release") == 0;
    char lexer[96], unit[96], audit[96], integration[96], source_manager[96], diagnostics[96], diagnostic_json[96];
    char compiler[96], tokens_cli[96];
    char classroom[96], demo[96];
    char parser[96], ast_dump[96], resolution[96];
    FILE *package_marker;
    int standalone;
#ifdef _WIN32
    /* Children inherit this mode. A loader or crash failure must reach the
       gate as an exit status instead of waiting for a desktop dialog. */
    (void)SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (!cc || !*cc) cc = "cc";
    if (directory("build")) return 2;
    package_marker = fopen("LEXICAL_PACKAGE", "rb");
    standalone = package_marker != NULL;
    if (package_marker) fclose(package_marker);
    if (strcmp(mode, "environment") == 0 || strcmp(mode, "env") == 0) {
        const char *args[] = {cc, "--version", NULL};
        status = run(args);
        if (status) return status;
        status = compile(cc, "build/environment" EXE, "tools/environment.c", 0, 0, 0);
        if (status) return status;
        { const char *probe[] = {"build/environment" EXE, NULL}; return run(probe); }
    }
    if (strcmp(mode, "audit") == 0) {
        status = compile(cc, "build/audit" EXE, "tools/audit.c", 0, 0, 0);
        if (status) return status;
        return audit_run("build/audit" EXE);
    }
    if (strcmp(mode, "build") && strcmp(mode, "test") && strcmp(mode, "check") && !sanitize && !release) {
        fprintf(stderr, "tfdev: expected environment|build|test|audit|check|sanitize|release\n");
        return 2;
    }
    if (sanitize && !standalone) {
        const char *const probe[]={"build/sanitizer-probe" EXE,NULL};
        if (compile(cc,probe[0],"tools/environment.c",1,0,0)!=0 || run_process(probe,10000)!=0) {
            (void)fprintf(stderr,"tfdev: ASan/UBSan build or launch probe failed; sanitizer gate did not run\n");
            return 2;
        }
    }
    snprintf(lexer, sizeof lexer, "build/ternfold-lexer%s" EXE, sanitize ? "-san" : "");
    snprintf(unit, sizeof unit, "build/test-lexer%s" EXE, sanitize ? "-san" : "");
    snprintf(integration, sizeof integration, "build/test-integration%s" EXE, sanitize ? "-san" : "");
    snprintf(audit, sizeof audit, "build/audit%s" EXE, sanitize ? "-san" : "");
    snprintf(source_manager, sizeof source_manager, "build/test-source-manager%s" EXE, sanitize ? "-san" : "");
    snprintf(diagnostics, sizeof diagnostics, "build/test-diagnostics%s" EXE, sanitize ? "-san" : "");
    snprintf(diagnostic_json, sizeof diagnostic_json, "build/test-diagnostic-json%s" EXE, sanitize ? "-san" : "");
    snprintf(compiler, sizeof compiler, "build/ternfold%s" EXE, sanitize ? "-san" : "");
    snprintf(tokens_cli, sizeof tokens_cli, "build/test-tokens-cli%s" EXE, sanitize ? "-san" : "");
    snprintf(classroom, sizeof classroom, "build/test-classroom%s" EXE, sanitize ? "-san" : "");
    snprintf(demo, sizeof demo, "build/test-demo%s" EXE, sanitize ? "-san" : "");
    snprintf(parser, sizeof parser, "build/test-parser%s" EXE, sanitize ? "-san" : "");
    snprintf(ast_dump, sizeof ast_dump, "build/test-ast-dump%s" EXE, sanitize ? "-san" : "");
    snprintf(resolution, sizeof resolution, "build/test-resolution%s" EXE, sanitize ? "-san" : "");
    status = compile(cc, lexer, "src/main.c", sanitize, 1, release);
    if (status) return status;
    if (!standalone) {
        status = compile(cc, compiler, "src/compiler_main.c", sanitize, 1, release);
        if (status) return status;
    }
    if (!strcmp(mode, "build")) return 0;
    status = compile(cc, unit, "tests/test_lexer.c", sanitize, 1, release);
    if (status) return status;
    { const char *args[] = {unit, NULL}; status = run(args); if (status) return status; }
    status = compile(cc, integration, "tests/test_integration.c", sanitize, 1, release);
    if (status) return status;
    { const char *args[] = {integration, lexer, NULL}; status = run(args); if (status) return status; }
    status = compile(cc, classroom, "tests/test_classroom.c", sanitize, 1, release);
    if (status) return status;
    { const char *args[] = {classroom, NULL}; status = run(args); if (status) return status; }
    status = compile(cc, demo, "tests/test_demo.c", sanitize, 1, release);
    if (status) return status;
    { const char *args[] = {demo, NULL}; status = run(args); if (status) return status; }
    /* The submitted standalone package intentionally has no compiler manager.
     * In the development tree it is a required gate, never a capability skip. */
    { FILE *marker = fopen("LEXICAL_PACKAGE", "rb");
      if (marker) fclose(marker);
      else {
          const char *const lexical_gates[] = {"tests/test_dfa.c", "tests/test_trace.c"};
          for (size_t i = 0; i < sizeof lexical_gates / sizeof *lexical_gates; ++i) {
              const char *binary = i == 0 ? "build/test-dfa" EXE : "build/test-trace" EXE;
              const char *args[] = {binary, NULL};
              status = compile(cc, binary, lexical_gates[i], sanitize, 1, release);
              if (status) return status;
              status = run(args);
              if (status) return status;
          }
          status = compile(cc, source_manager, "tests/test_source_manager.c", sanitize, 1, release);
          if (status) return status;
          { const char *args[] = {source_manager, NULL}; status = run(args); if (status) return status; }
          status = compile(cc, diagnostics, "tests/test_diagnostics.c", sanitize, 1, release);
          if (status) return status;
          { const char *args[] = {diagnostics, NULL}; status = run(args); if (status) return status; }
          status = compile(cc, diagnostic_json, "tests/test_diagnostic_json.c", sanitize, 1, release);
          if (status) return status;
          { const char *args[] = {diagnostic_json, NULL}; status = run(args); if (status) return status; }
          status = compile(cc, tokens_cli, "tests/test_tokens_cli.c", sanitize, 1, release);
          if (status) return status;
          { const char *args[] = {tokens_cli, compiler, NULL}; status = run(args); if (status) return status; }
          status = compile(cc, parser, "tests/test_parser.c", sanitize, 1, release);
          if (status) return status;
          { const char *args[] = {parser, NULL}; status = run(args); if (status) return status; }
          {
              const char *binary=sanitize ? "build/test-frontend-fuzz-san" EXE : "build/test-frontend-fuzz" EXE;
              const char *const args[]={binary,NULL};
              status=compile(cc,binary,"tests/test_frontend_fuzz.c",sanitize,1,release);
              if (status) return status;
              status=run_process(args,30000);
              if (status) return status;
          }
          status = compile(cc, ast_dump, "tests/test_ast_dump.c", sanitize, 1, release);
          if (status) return status;
          { const char *args[] = {ast_dump, NULL}; status = run(args); if (status) return status; }
          status = compile(cc, resolution, "tests/test_resolution.c", sanitize, 1, release);
          if (status) return status;
          { const char *args[] = {resolution, NULL}; status = run(args); if (status) return status; }
          {
              const char *const type_gates[] = {"pattern_coverage", "numeric", "checked_numeric", "literal_value",
                                                "type_store", "typed_signatures", "typed_patterns",
                                                "check", "check_constants", "check_builtins",
                                                "machine_plan", "plan_dump", "c01_report", "c01_numeric",
                                                "core_initializers", "core_primitives", "core_layout", "core_nodes",
                                                "core_lower", "core_verify", "core_dump", "core_match",
                                                "interpreter_core", "exec_scalars", "c_emit", "c_metadata",
                                                "driver_native", "repl_input", "repl_resolve", "repl_eval", "repl", "trace_execution",
                                                "formatter", "editor", "optimization", "lifetimes"};
              for (size_t i = 0; i < sizeof type_gates / sizeof *type_gates; ++i) {
                  char test_source[96], test_binary[96];
                  const bool metadata=strcmp(type_gates[i],"c_metadata")==0;
                  const char *args[] = {test_binary,metadata ? "build/c_metadata_empty.inc" : NULL,
                                       metadata ? "build/c_metadata_library.inc" : NULL,NULL};
                  int n = snprintf(test_source,sizeof test_source,"tests/test_%s.c",type_gates[i]);
                  int m = snprintf(test_binary,sizeof test_binary,"build/test-%s%s" EXE,
                                   type_gates[i],sanitize ? "-san" : "");
                  if (n < 0 || (size_t)n >= sizeof test_source ||
                      m < 0 || (size_t)m >= sizeof test_binary) return 2;
                  status = compile(cc,test_binary,test_source,sanitize,1,release);
                  if (status) return status;
                  status = run(args);
                  if (status) return status;
                  if (metadata) {
                      status=metadata_native(cc,sanitize,release,1); if (status) return status;
                      status=metadata_native(cc,sanitize,release,0); if (status) return status;
                  }
              }
          }
          {
              const char *binary=sanitize ? "build/test-composition-san" EXE : "build/test-composition" EXE;
              const char *const args[]={binary,NULL};
              status=compile(cc,binary,"tests/experiments/test_composition.c",sanitize,0,release);
              if (status) return status;
              status=run(args); if (status) return status;
          }
          {
              const char *const args[]={compiler,"emit-c","examples/intro/hello.tfd",
                                       "-o","build/c_output_fixture.c",NULL};
              status=run(args); if (status) return status;
              status=native_output_fault(cc,sanitize,release); if (status) return status;
              const char *const fault[]={compiler,"emit-c","examples/boundaries/candidate_fault.tfd",
                                         "-o","build/c_trace_fault_fixture.c",NULL};
              status=run(fault); if (status) return status;
              status=native_trace_fault(cc,sanitize,release,false); if (status) return status;
              status=native_trace_fault(cc,sanitize,release,true); if (status) return status;
          }
          {
              const char *const gates[]={"values","calls","numeric","constants","io","builtins","stack","callbacks","value_ops"};
              for (size_t i=0; i<sizeof gates/sizeof *gates; ++i) {
                  char source[96], binary[96];
                  const char *args[]={binary,NULL};
                  const int n=snprintf(source,sizeof source,"tests/native/test_runtime_%s.c",gates[i]);
                  const int m=snprintf(binary,sizeof binary,"build/test-runtime-%s%s" EXE,gates[i],sanitize ? "-san" : "");
                  if (n<0 || (size_t)n>=sizeof source || m<0 || (size_t)m>=sizeof binary) return 2;
                  status=compile(cc,binary,source,sanitize,0,release);
                  if (status) return status;
                  status=run_process(args,30000);
                  if (status) return status;
              }
          }
          {
              const char *binary=sanitize ? "build/test-native-build-san" EXE : "build/test-native-build" EXE;
              const char *const args[]={binary,cc,".",sanitize ? "--sanitize" : NULL,NULL};
              status=compile(cc,binary,"tests/test_native_build.c",sanitize,0,release);
              if (status) return status;
              status=run_process(args,120000);
              if (status) return status;
          }
          {
              const char *binary=sanitize ? "build/test-differential-san" EXE : "build/test-differential" EXE;
              const char *const args[]={binary,compiler,cc,".",sanitize ? "--sanitize" : NULL,NULL};
              status=compile(cc,binary,"tests/test_differential.c",sanitize,0,release);
              if (status) return status;
              status=run_process(args,600000);
              if (status) return status;
          }
          {
              const char *binary=sanitize ? "build/test-m14-generated-san" EXE : "build/test-m14-generated" EXE;
              const char *const args[]={binary,compiler,cc,".",sanitize ? "--sanitize" : NULL,NULL};
              status=compile(cc,binary,"tests/property/m14_generated.c",sanitize,0,release);
              if (status) return status;
              status=run_process(args,600000);
              if (status) return status;
          }
          {
              const char *binary=sanitize ? "build/test-plan-properties-san" EXE : "build/test-plan-properties" EXE;
              const char *const args[]={binary,NULL};
              status=compile(cc,binary,"tests/property/test_plan_properties.c",sanitize,1,release);
              if (status) return status;
              status=run_process(args,30000);
              if (status) return status;
          }
          {
              const char *binary=sanitize ? "build/test-examples-san" EXE : "build/test-examples" EXE;
              const char *const args[]={binary,compiler,cc,".",sanitize ? "--sanitize" : NULL,NULL};
              status=compile(cc,binary,"tests/test_examples.c",sanitize,0,release);
              if (status) return status;
              status=run_process(args,600000);
              if (status) return status;
          }
          {
              const char *binary=sanitize ? "build/test-execution-trace-san" EXE : "build/test-execution-trace" EXE;
              const char *const args[]={binary,NULL};
              status=compile(cc,binary,"tests/test_execution_trace.c",sanitize,0,release);
              if (status) return status;
              status=run_process(args,30000);
              if (status) return status;
          }
          {
              const char *binary=sanitize ? "build/test-trace-io-san" EXE : "build/test-trace-io" EXE;
              const char *const args[]={binary,NULL};
              status=compile(cc,binary,"tests/test_trace_io.c",sanitize,0,release);
              if (status) return status;
              status=run_process(args,30000);
              if (status) return status;
          }
      }
    }
    if (!strcmp(mode, "check") || release || sanitize) {
        status = compile(cc, audit, "tools/audit.c", sanitize, 0, release);
        if (status) return status;
        return audit_run(audit);
    }
    return 0;
}
