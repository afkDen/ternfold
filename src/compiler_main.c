#include "ternfold/source_manager.h"
#include "ternfold/diagnostics.h"
#include "ternfold/token_dump.h"
#include "ternfold/parser.h"
#include "ternfold/ast_dump.h"
#include "ternfold/resolve.h"
#include "ternfold/check.h"
#include "typed_signatures.h"
#include "ternfold/plan_dump.h"
#include "ternfold/core_dump.h"
#include "ternfold/core_lower.h"
#include "execution/driver_run.h"
#include "backend/driver_native.h"

#include <signal.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static int driver_error(const char *message, int code) {
    if (fprintf(stderr,"ternfold: %s\n",message) < 0 || fflush(stderr) != 0) return 2;
    return code;
}
static bool extension(const char *path) {
    size_t length = strlen(path);
    return length >= 4 && strcmp(path+length-4,".tfd") == 0;
}
typedef enum { INSPECT_CHECK, INSPECT_PLAN, INSPECT_CORE, EXECUTE_RUN, EMIT_C, BUILD_NATIVE } DriverMode;

static int check_source(const char *path, const char *root, TfDiagnosticFormat format,
                        DriverMode mode, bool json, const char *input,
                        const TfDriverNativeOptions *native) {
    TfSourceManager *manager = NULL;
    TfResolution *resolution = NULL;
    TfTypedSignatures *signatures = NULL;
    TfTypedProgram *typed = NULL;
    TfMachineProgram *machines = NULL;
    TfCoreProgram *core = NULL;
    TfVerifiedCore *verified = NULL;
    TfResolveOptions options = {root, 100};
    const TfDiagnosticBag *shown;
    const char *failure = NULL;
    int code = 2;
    const bool native_mode=mode==EMIT_C || mode==BUILD_NATIVE;
    if (tf_source_manager_create(&manager,NULL) != TF_OK ||
        tf_resolve(&resolution,manager,path,&options,NULL) != TF_OK) {
        failure = "cannot resolve source (read/path capability, resource limit or allocation failure)";
        goto done;
    }
    shown = tf_resolution_diagnostics(resolution);
    if (!tf_diagnostic_bag_has_errors(shown)) {
        if (tf_signatures_build(&signatures,resolution,100,NULL) != TF_OK) {
            failure = "cannot build type signatures (resource limit or allocation failure)";
            goto done;
        }
        shown = tf_signatures_diagnostics(signatures);
        if (!tf_diagnostic_bag_has_errors(shown)) {
            if (tf_check(&typed,resolution,signatures,100,NULL) != TF_OK ||
                tf_diagnostic_phase_kind(tf_typed_program_phase(typed)) == TF_DIAG_PHASE_INVALID) {
                failure = "cannot check types (host arithmetic capability, resource limit or allocation failure)";
                goto done;
            }
            shown = tf_typed_program_diagnostics(typed);
            if (!tf_diagnostic_bag_has_errors(shown)) {
                if (tf_machine_program_build(&machines,typed,100,NULL) != TF_OK) {
                    failure = "cannot analyze machines (resource limit or allocation failure)";
                    goto done;
                }
                shown = tf_machine_program_diagnostics(machines);
            }
        }
    }
    code = tf_diagnostic_bag_has_errors(shown) ? 1 : 0;
    if (((mode!=EXECUTE_RUN && !native_mode) || code!=0) &&
        (tf_diagnostic_bag_write(stderr,shown,format,TF_DIAG_COLOR_NEVER) != TF_OK || fflush(stderr) != 0))
        failure = "cannot write diagnostics";
    else if (code == 0) {
        if (mode == INSPECT_PLAN) {
            if (tf_plan_dump_write(stdout,machines,json) != TF_OK) failure = "cannot write machine plan";
        } else if (mode == INSPECT_CORE || mode == EXECUTE_RUN || native_mode) {
            TfCoreLowerIssue lowering = {0};
            TfCoreVerifyIssue verification = {0};
            if (tf_core_lower(&core,machines,NULL,&lowering) != TF_OK) {
                failure = lowering.code == TF_CORE_LOWER_UNSUPPORTED_FORM ?
                    "source form is not yet supported by Core lowering" :
                    "cannot lower to Core (resource limit or compiler invariant failure)";
            } else if (tf_core_verify(&verified,core,NULL,&verification) != TF_OK) {
                failure = "cannot verify lowered Core (resource limit or compiler invariant failure)";
            } else if (mode==EXECUTE_RUN) {
                code=tf_run_verified(verified,shown,input,format,&failure);
            } else if (native_mode) {
                const TfCoreProgramData *data=tf_verified_core_data(verified);
                if (tf_core_function(data,data->entry)==NULL)
                    code=tf_driver_missing_entry(shown,format,mode==EMIT_C ?
                        "emit-c requires a root pub fn main() -> List[Text]" :
                        "build requires a root pub fn main() -> List[Text]",&failure);
                else if (tf_diagnostic_bag_write(stderr,shown,format,TF_DIAG_COLOR_NEVER)!=TF_OK ||
                         fflush(stderr)!=0)
                    failure="cannot write diagnostics";
                else failure=tf_driver_native(verified,resolution,native,mode==BUILD_NATIVE);
            } else if (tf_core_dump_write(stdout,verified) != TF_OK) {
                failure = "cannot write Core";
            }
        } else if (fputs("Frontend checks succeeded.\n",stdout) == EOF)
            failure = "cannot write check result";
        if (mode!=EXECUTE_RUN && fflush(stdout) != 0 && failure == NULL) failure = "cannot write inspection result";
    }
done:
    tf_verified_core_destroy(verified);
    tf_core_program_destroy(core);
    tf_machine_program_destroy(machines);
    tf_typed_program_destroy(typed);
    tf_signatures_destroy(signatures);
    tf_resolution_destroy(resolution);
    tf_source_manager_destroy(manager);
    return failure != NULL ? driver_error(failure,2) : code;
}
int main(int argc, char **argv) {
    TfSourceManager *manager = NULL;
    TfDiagnosticBag *bag = NULL;
    const TfManagedSource *entry = NULL;
    TfLexResult lex = {0};
    TfParseResult *parsed = NULL;
    const TfDiagnosticBag *shown;
    bool ast_mode, check_mode, plan_mode, core_mode, run_mode, emit_mode, build_mode, native_mode;
    TfDriverNativeOptions native={0};
    TfTokenDumpFormat dump_format = TF_TOKEN_DUMP_TEXT;
    TfDiagnosticFormat diagnostic_format = TF_DIAG_TEXT;
    const char *directory = NULL, *root = NULL, *input = NULL, *failure = NULL;
    TfStatus status;
    int i, code = 2;
#ifdef _WIN32
    (void)SetErrorMode(GetErrorMode()|SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if (_setmode(_fileno(stdout),_O_BINARY) == -1 || _setmode(_fileno(stderr),_O_BINARY) == -1) return 2;
#else
    if (signal(SIGPIPE,SIG_IGN) == SIG_ERR) return 2;
#endif
    if (argc == 2 && strcmp(argv[1],"--help") == 0) {
        if (fputs("Usage: ternfold tokens source.tfd [--json] [--diagnostics-json] [--symbol-table DIR]\n"
                  "Options follow source, in any order, once each. DIR must already exist.\n"
                  "       ternfold ast source.tfd [--json] [--diagnostics-json]\n"
                  "       ternfold check source.tfd [--root DIR] [--diagnostics-json]\n"
                  "       ternfold plan source.tfd [--root DIR] [--json] [--diagnostics-json]\n"
                  "       ternfold core source.tfd [--root DIR] [--diagnostics-json]\n"
                  "       ternfold run source.tfd [--root DIR] [--stdin-file FILE] [--diagnostics-json]\n"
                  "       ternfold emit-c source.tfd [-o FILE] [--root DIR] [--diagnostics-json]\n"
                  "       ternfold build source.tfd -o EXE [--cc PATH] [--runtime-root DIR] [--sanitize]\n"
                  "                                     [--root DIR] [--diagnostics-json]\n"
                  "Check validates the frontend, including machine scheduling.\n"
                  "Core prints verified, structured intermediate code without executing it.\n"
                  "Emit C17 to stdout or -o FILE. Build uses --cc or TF_CC and runtime root DIR (default .).\n"
                  "--json: tokens, AST or plan schema v1 on stdout; --diagnostics-json: diagnostic schema v2 on stderr.\n"
                  "Invalid source: empty tokens, source_valid=false; diagnostics carry raw byte ranges.\n"
                  "No Symbol Table.txt is written without --symbol-table. Driver errors remain text.\n"
                  "Paths follow native SourceManager capabilities (Windows ANSI, queryable insensitive ancestors).\n"
                  "AST JSON schema v1 uses stable node IDs; recovery trees are inspection only.\n"
                  "Invalid source: AST root=null, has_errors=true, empty nodes (no invented syntax).\n"
                  "Exit: 0 success, 1 diagnosed source error, 2 usage/host/output failure, 3 runtime fault.\n",stdout) == EOF ||
            fflush(stdout) != 0) return 2;
        return fflush(stderr) == 0 ? 0 : 2;
    }
    if (argc < 3 || (strcmp(argv[1],"tokens") != 0 && strcmp(argv[1],"ast") != 0 &&
                     strcmp(argv[1],"check") != 0 && strcmp(argv[1],"plan") != 0 &&
                     strcmp(argv[1],"core") != 0 && strcmp(argv[1],"run") != 0 &&
                     strcmp(argv[1],"emit-c") != 0 && strcmp(argv[1],"build") != 0) || argv[2][0] == '\0' || argv[2][0] == '-')
        return driver_error("expected tokens/ast/check/plan/core/run/emit-c/build source.tfd; use --help",2);
    ast_mode = strcmp(argv[1],"ast") == 0;
    check_mode = strcmp(argv[1],"check") == 0;
    plan_mode = strcmp(argv[1],"plan") == 0;
    core_mode = strcmp(argv[1],"core") == 0;
    run_mode = strcmp(argv[1],"run") == 0;
    emit_mode=strcmp(argv[1],"emit-c")==0;
    build_mode=strcmp(argv[1],"build")==0;
    native_mode=emit_mode || build_mode;
    for (i = 3; i < argc; ++i) {
        const bool has_value=i+1<argc && argv[i+1][0]!='\0' && argv[i+1][0]!='-';
        if (!check_mode && !core_mode && !run_mode && !native_mode && strcmp(argv[i],"--json") == 0 && dump_format == TF_TOKEN_DUMP_TEXT) dump_format = TF_TOKEN_DUMP_JSON;
        else if (strcmp(argv[i],"--diagnostics-json") == 0 && diagnostic_format == TF_DIAG_TEXT) diagnostic_format = TF_DIAG_JSON;
        else if (!ast_mode && !check_mode && !plan_mode && !core_mode && !run_mode && !native_mode &&
                 strcmp(argv[i],"--symbol-table")==0 && directory==NULL && has_value) directory=argv[++i];
        else if ((check_mode || plan_mode || core_mode || run_mode || native_mode) &&
                 strcmp(argv[i],"--root")==0 && root==NULL && has_value) root=argv[++i];
        else if (run_mode && strcmp(argv[i],"--stdin-file")==0 && input==NULL && has_value) input=argv[++i];
        else if (native_mode && strcmp(argv[i],"-o")==0 && native.output==NULL && has_value) native.output=argv[++i];
        else if (build_mode && strcmp(argv[i],"--cc")==0 && native.compiler==NULL && has_value) native.compiler=argv[++i];
        else if (build_mode && strcmp(argv[i],"--runtime-root")==0 && native.runtime==NULL && has_value) native.runtime=argv[++i];
        else if (build_mode && strcmp(argv[i],"--sanitize")==0 && !native.sanitize) native.sanitize=true;
        else return driver_error("unknown, duplicate or incomplete option; use --help",2);
    }
    if (!extension(argv[2])) return driver_error("source file extension must be exactly .tfd",1);
    if (build_mode && native.output==NULL) return driver_error("build requires -o EXE",2);
    if (check_mode || plan_mode || core_mode || run_mode || native_mode)
        return check_source(argv[2],root,diagnostic_format,build_mode ? BUILD_NATIVE : emit_mode ? EMIT_C :
                            run_mode ? EXECUTE_RUN : core_mode ? INSPECT_CORE :
                            plan_mode ? INSPECT_PLAN : INSPECT_CHECK,dump_format == TF_TOKEN_DUMP_JSON,input,&native);
    if (tf_source_manager_create(&manager,NULL) != TF_OK || tf_diagnostic_bag_create(&bag,100,NULL) != TF_OK) {
        failure = "cannot allocate compiler session"; goto cleanup;
    }
    status = tf_source_manager_load(manager,argv[2],&entry);
    if (status == TF_HOST_ERROR) { failure = "cannot load source (read/path capability/allocation failure)"; goto cleanup; }
    if (status == TF_SOURCE_ERROR) {
        if (tf_diagnostic_bag_source(bag,entry->source) != TF_OK) { failure = "cannot build source diagnostics"; goto cleanup; }
        code = 1;
    } else {
        status = tf_lex(&lex,entry->source,100,NULL);
        if (status == TF_HOST_ERROR || (!ast_mode && tf_diagnostic_bag_lexer(bag,&lex) != TF_OK)) {
            failure = "cannot build lexical result or diagnostics"; goto cleanup;
        }
        code = status == TF_LEX_ERROR ? 1 : 0;
        if (ast_mode) {
            if (tf_parse(&parsed,&lex,NULL,NULL) != TF_OK) {
                failure = "cannot build syntax result (allocation/nesting budget failure)"; goto cleanup;
            }
            code = tf_diagnostic_bag_has_errors(tf_parse_diagnostics(parsed)) ? 1 : 0;
        }
    }
    if (directory != NULL && tf_report_publish(entry->source->valid ? &lex : NULL,entry->source,directory) != TF_OK) {
        code = 2; failure = "cannot publish Symbol Table.txt";
    }
    if (ast_mode) {
        if (parsed != NULL) status = tf_ast_dump_write(stdout,tf_parse_ast(parsed),code == 1,dump_format == TF_TOKEN_DUMP_JSON);
        else status = fputs(dump_format == TF_TOKEN_DUMP_JSON ?
            "{\"schema_version\":1,\"root\":null,\"has_errors\":true,\"nodes\":[]}\n" :
            "AST root=null has_errors=true nodes=0\n",stdout) == EOF ? TF_HOST_ERROR : TF_OK;
    } else status = tf_token_dump_write(stdout,entry->source,entry->source->valid ? &lex : NULL,dump_format);
    if (status != TF_OK ||
        fflush(stdout) != 0) { code = 2; failure = ast_mode ? "cannot write AST output" : "cannot write token output"; }
    shown = parsed != NULL ? tf_parse_diagnostics(parsed) : bag;
    if (tf_diagnostic_bag_write(stderr,shown,diagnostic_format,TF_DIAG_COLOR_NEVER) != TF_OK ||
        fflush(stderr) != 0) { code = 2; failure = "cannot write diagnostics"; }
cleanup:
    tf_parse_destroy(parsed); tf_lex_destroy(&lex); tf_diagnostic_bag_destroy(bag); tf_source_manager_destroy(manager);
    if (failure != NULL) return driver_error(failure,2);
    return code;
}
