#include "ternfold/source_manager.h"
#include "ternfold/diagnostics.h"
#include "ternfold/token_dump.h"
#include "ternfold/parser.h"
#include "ternfold/ast_dump.h"
#include "ternfold/resolve.h"
#include "ternfold/check.h"
#include "typed_signatures.h"
#include "ternfold/plan_dump.h"

#include <signal.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static int driver_error(const char *message, int code) {
    if (fprintf(stderr,"ternfold: %s\n",message) < 0 || fflush(stderr) != 0) return 2;
    return code;
}
static bool extension(const char *path) {
    size_t length = strlen(path);
    return length >= 4 && strcmp(path+length-4,".tfd") == 0;
}
static int check_source(const char *path, const char *root, TfDiagnosticFormat format,
                        bool plan_mode, bool json) {
    TfSourceManager *manager = NULL;
    TfResolution *resolution = NULL;
    TfTypedSignatures *signatures = NULL;
    TfTypedProgram *typed = NULL;
    TfMachineProgram *machines = NULL;
    TfResolveOptions options = {root, 100};
    const TfDiagnosticBag *shown;
    const char *failure = NULL;
    int code = 2;
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
    if (tf_diagnostic_bag_write(stderr,shown,format,
                                TF_DIAG_COLOR_NEVER) != TF_OK || fflush(stderr) != 0)
        failure = "cannot write diagnostics";
    else if (code == 0) {
        if (plan_mode) {
            if (tf_plan_dump_write(stdout,machines,json) != TF_OK) failure = "cannot write machine plan";
        } else if (fputs("Frontend checks succeeded; execution and C generation are unavailable.\n",stdout) == EOF)
            failure = "cannot write check result";
        if (fflush(stdout) != 0 && failure == NULL)
            failure = plan_mode ? "cannot write machine plan" : "cannot write check result";
    }
done:
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
    bool ast_mode, check_mode, plan_mode;
    TfTokenDumpFormat dump_format = TF_TOKEN_DUMP_TEXT;
    TfDiagnosticFormat diagnostic_format = TF_DIAG_TEXT;
    const char *directory = NULL, *root = NULL, *failure = NULL;
    TfStatus status;
    int i, code = 2;
#ifdef _WIN32
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
                  "Check validates the frontend, including machine scheduling; run/compile are unavailable.\n"
                  "--json: token schema v1 on stdout; --diagnostics-json: diagnostic schema v2 on stderr.\n"
                  "Invalid source: empty tokens, source_valid=false; diagnostics carry raw byte ranges.\n"
                  "No Symbol Table.txt is written without --symbol-table. Driver errors remain text.\n"
                  "Paths follow native SourceManager capabilities (Windows ANSI, queryable insensitive ancestors).\n"
                  "AST JSON schema v1 uses stable node IDs; recovery trees are inspection only.\n"
                  "Invalid source: AST root=null, has_errors=true, empty nodes (no invented syntax).\n"
                  "Exit: 0 requested phase succeeded, 1 diagnosed input error, 2 usage/host/output failure.\n",stdout) == EOF ||
            fflush(stdout) != 0) return 2;
        return fflush(stderr) == 0 ? 0 : 2;
    }
    if (argc < 3 || (strcmp(argv[1],"tokens") != 0 && strcmp(argv[1],"ast") != 0 &&
                     strcmp(argv[1],"check") != 0 && strcmp(argv[1],"plan") != 0) || argv[2][0] == '\0' || argv[2][0] == '-')
        return driver_error("expected tokens/ast/check/plan source.tfd; use --help (run/compile are unavailable)",2);
    ast_mode = strcmp(argv[1],"ast") == 0;
    check_mode = strcmp(argv[1],"check") == 0;
    plan_mode = strcmp(argv[1],"plan") == 0;
    for (i = 3; i < argc; ++i) {
        if (!check_mode && strcmp(argv[i],"--json") == 0 && dump_format == TF_TOKEN_DUMP_TEXT) dump_format = TF_TOKEN_DUMP_JSON;
        else if (strcmp(argv[i],"--diagnostics-json") == 0 && diagnostic_format == TF_DIAG_TEXT) diagnostic_format = TF_DIAG_JSON;
        else if (!ast_mode && !check_mode && !plan_mode && strcmp(argv[i],"--symbol-table") == 0 && directory == NULL && i+1 < argc && argv[i+1][0] != '\0' &&
                 argv[i+1][0] != '-') directory = argv[++i];
        else if ((check_mode || plan_mode) && strcmp(argv[i],"--root") == 0 && root == NULL && i+1 < argc &&
                 argv[i+1][0] != '\0' && argv[i+1][0] != '-') root = argv[++i];
        else return driver_error("unknown, duplicate or incomplete option; use --help",2);
    }
    if (!extension(argv[2])) return driver_error("source file extension must be exactly .tfd",1);
    if (check_mode || plan_mode)
        return check_source(argv[2],root,diagnostic_format,plan_mode,dump_format == TF_TOKEN_DUMP_JSON);
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
