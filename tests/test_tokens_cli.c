#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "ternfold/token_dump.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <process.h>
#define MAKE_DIRECTORY(path) _mkdir(path)
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define MAKE_DIRECTORY(path) mkdir(path,0700)
#endif

enum { PATH_CAPACITY = 4096, CHILD_TIMEOUT_MS = 15000 };
typedef enum { CAPTURE, BROKEN_PIPE } StreamMode;
static size_t checks, cases;
static char root[PATH_CAPACITY], output_path[PATH_CAPACITY], error_path[PATH_CAPACITY];
static void check(bool okay, const char *expression, int line) {
    ++checks;
    if (!okay) { (void)fprintf(stderr,"tokens_cli:%d: %s\nfixtures: %s\n",line,expression,root); exit(1); }
}
#define CHECK(x) check((x),#x,__LINE__)
static void join_path(char *out, const char *base, const char *leaf) {
    int count = snprintf(out,PATH_CAPACITY,"%s/%s",base,leaf);
    CHECK(count > 0 && (size_t)count < PATH_CAPACITY);
}
static void write_bytes(const char *path, const void *data, size_t length) {
    FILE *file = fopen(path,"wb"); CHECK(file != NULL);
    CHECK(fwrite(data,1,length,file) == length); CHECK(fclose(file) == 0);
}
static void write_text(const char *path, const char *text) { write_bytes(path,text,strlen(text)); }
static char *read_text(const char *path) {
    FILE *file = fopen(path,"rb"); char *data; long length;
    CHECK(file != NULL); CHECK(fseek(file,0,SEEK_END) == 0); length = ftell(file);
    CHECK(length >= 0 && (uintmax_t)length < SIZE_MAX);
    data = malloc((size_t)length+1); CHECK(data != NULL); CHECK(fseek(file,0,SEEK_SET) == 0);
    CHECK(fread(data,1,(size_t)length,file) == (size_t)length); data[(size_t)length] = '\0';
    CHECK(memchr(data,0,(size_t)length) == NULL); CHECK(fclose(file) == 0); return data;
}
static void contains(const char *path, const char *fragment) {
    char *text = read_text(path);
    if (strstr(text,fragment) == NULL) (void)fprintf(stderr,"missing %s in %s\n",fragment,text);
    CHECK(strstr(text,fragment) != NULL); free(text);
}
static void equal(const char *path, const char *expected) {
    char *text = read_text(path); CHECK(strcmp(text,expected) == 0); free(text);
}
static void absent(const char *path) {
    FILE *file = fopen(path,"rb"); CHECK(file == NULL && errno == ENOENT);
}
static size_t occurrences(const char *text, const char *needle) {
    size_t n = 0, width = strlen(needle); const char *at = text;
    while ((at = strstr(at,needle)) != NULL) { ++n; at += width; } return n;
}
static void setup(void) {
    unsigned long pid;
    unsigned attempt;
#ifdef _WIN32
    pid = (unsigned long)_getpid();
#else
    pid = (unsigned long)getpid();
#endif
    for (attempt = 0; attempt < 1000; ++attempt) {
        CHECK(snprintf(root,sizeof(root),"build/tokens-fixtures-%lu-%u",pid,attempt) > 0);
        if (MAKE_DIRECTORY(root) == 0) break;
        CHECK(errno == EEXIST);
    }
    CHECK(attempt < 1000); join_path(output_path,root,"stdout.txt"); join_path(error_path,root,"stderr.txt");
}
#ifdef _WIN32
/* Microsoft CRT command-line decoding: surround each argument with quotes,
 * double trailing backslashes and those preceding a quote, then escape quotes.
 * Application path is separate, so executable names never become shell text. */
static char *command_line(const char *const *args) {
    size_t total = 1, count = 0, n, at = 0;
    char *line;
    while (args[count] != NULL) {
        size_t length = strlen(args[count]);
        CHECK(length <= (SIZE_MAX - 4) / 2);
        CHECK(total <= SIZE_MAX - (length * 2 + 4));
        total += length * 2 + 4;
        ++count;
    }
    CHECK(total <= 32767);
    line = malloc(total);
    CHECK(line != NULL);
    for (n = 0; n < count; ++n) {
        size_t i = 0, length = strlen(args[n]);
        if (n != 0) line[at++] = ' ';
        line[at++] = '"';
        while (i < length) {
            size_t slashes = 0;
            while (i < length && args[n][i] == '\\') { ++slashes; ++i; }
            if (i == length || args[n][i] == '"') slashes *= 2;
            while (slashes != 0) { line[at++] = '\\'; --slashes; }
            if (i < length) {
                if (args[n][i] == '"') line[at++] = '\\';
                line[at++] = args[n][i++];
            }
        }
        line[at++] = '"';
    }
    line[at] = '\0';
    CHECK(at < total);
    return line;
}
static HANDLE child_stream(const char *path, StreamMode mode) {
    SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    HANDLE stream;
    if (mode == BROKEN_PIPE) {
        HANDLE reader;
        CHECK(CreatePipe(&reader, &stream, &attributes, 0) != 0);
        CHECK(CloseHandle(reader) != 0); /* no readers when child starts */
    } else {
        stream = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(stream != INVALID_HANDLE_VALUE);
    }
    return stream;
}
static int spawn_at(const char *const *args, StreamMode out_mode, StreamMode err_mode,
                    const char *working_directory) {
    SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process = {0};
    HANDLE input, output, error;
    char *line = command_line(args);
    DWORD waited, code;
    BOOL created;
    input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(input != INVALID_HANDLE_VALUE);
    output = child_stream(output_path, out_mode);
    error = child_stream(error_path, err_mode);
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = error;
    created = CreateProcessA(args[0], line, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                              NULL, working_directory, &startup, &process);
    free(line);
    CHECK(CloseHandle(input) != 0);
    CHECK(CloseHandle(output) != 0);
    CHECK(CloseHandle(error) != 0);
    if (!created) (void)fprintf(stderr, "CreateProcess failed for %s\n", args[0]);
    CHECK(created != 0);
    CHECK(CloseHandle(process.hThread) != 0);
    waited = WaitForSingleObject(process.hProcess, CHILD_TIMEOUT_MS);
    if (waited != WAIT_OBJECT_0) {
        (void)fprintf(stderr,"token child wait failed (%lu), command: %s\n",
                      (unsigned long)waited,args[0]);
        for (size_t i=1;args[i]!=NULL;++i) (void)fprintf(stderr,"  argv[%zu]=%s\n",i,args[i]);
        (void)TerminateProcess(process.hProcess, 125);
        (void)WaitForSingleObject(process.hProcess, 1000);
        (void)CloseHandle(process.hProcess);
        CHECK(false); /* timeout/infrastructure failure never passes */
    }
    CHECK(GetExitCodeProcess(process.hProcess, &code) != 0);
    CHECK(CloseHandle(process.hProcess) != 0);
    CHECK(code <= 255); /* includes sanitizer crashes and loader failures */
    return (int)code;
}
#else
static int child_stream(const char *path, StreamMode mode) {
    if (mode == BROKEN_PIPE) {
        int descriptors[2];
        CHECK(pipe(descriptors) == 0);
        CHECK(close(descriptors[0]) == 0);
        return descriptors[1];
    }
    {
        int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        CHECK(descriptor >= 0);
        return descriptor;
    }
}
static int spawn_at(const char *const *args, StreamMode out_mode, StreamMode err_mode,
                    const char *working_directory) {
    int input = open("/dev/null", O_RDONLY);
    int output = child_stream(output_path, out_mode), error = child_stream(error_path, err_mode);
    pid_t pid;
    int status;
    struct timespec start, now, pause = {0, 10000000L};
    CHECK(input >= 0);
    CHECK(fflush(NULL) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        if (working_directory != NULL && chdir(working_directory) != 0) _exit(126);
        if (dup2(input, STDIN_FILENO) < 0 || dup2(output, STDOUT_FILENO) < 0 ||
            dup2(error, STDERR_FILENO) < 0) _exit(126);
        if (input > STDERR_FILENO) (void)close(input);
        if (output > STDERR_FILENO) (void)close(output);
        if (error > STDERR_FILENO) (void)close(error);
        execv(args[0], (char *const *)args);
        _exit(127);
    }
    CHECK(close(input) == 0 && close(output) == 0 && close(error) == 0);
    for (;;) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (waited < 0) { CHECK(errno == EINTR); continue; }
        CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        if (now.tv_sec - start.tv_sec >= CHILD_TIMEOUT_MS / 1000) {
            (void)kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
            CHECK(false);
        }
        while (nanosleep(&pause, &pause) != 0) CHECK(errno == EINTR);
        pause = (struct timespec){0, 10000000L};
    }
    CHECK(WIFEXITED(status));
    return WEXITSTATUS(status);
}
#endif

static int spawn(const char *const *args, StreamMode out_mode, StreamMode err_mode) {
    return spawn_at(args, out_mode, err_mode, NULL);
}

static int tokens(const char *exe, const char *path, bool json, bool diagnostic_json, const char *table) {
    const char *args[10]; size_t n = 0;
    args[n++] = exe; args[n++] = "tokens"; args[n++] = path;
    if (json) args[n++] = "--json";
    if (diagnostic_json) args[n++] = "--diagnostics-json";
    if (table != NULL) { args[n++] = "--symbol-table"; args[n++] = table; }
    args[n] = NULL; ++cases; return spawn(args,CAPTURE,CAPTURE);
}
static void test_goldens(const char *exe) {
    char path[PATH_CAPACITY], table[PATH_CAPACITY];
    char *expected, *baseline;
    join_path(path,root,"source with spaces.tfd"); write_text(path,"Int 7 true \"\\u{0}\"\n");
    join_path(table,root,"Symbol Table.txt"); absent(table);
    CHECK(tokens(exe,path,false,false,NULL) == 0);
    expected = read_text("tests/fixtures/tokens/basic.txt"); equal(output_path,expected); free(expected);
    equal(error_path,""); absent(table);
    CHECK(tokens(exe,path,true,true,NULL) == 0);
    expected = read_text("tests/fixtures/tokens/basic.json"); equal(output_path,expected);
    baseline = read_text(output_path);
    CHECK(tokens(exe,path,true,true,NULL) == 0); equal(output_path,baseline); free(baseline); free(expected);
    equal(error_path,"{\"schema_version\":2,\"diagnostics\":[],\"suppressed\":0,\"has_errors\":false}\n");
    CHECK(tokens(exe,path,false,false,root) == 0);
    equal(table,"TOKEN TYPE\tLEXEME\nRESERVED_WORD\t\"Int\"\nINT_LITERAL\t\"7\"\nBOOL_LITERAL\t\"true\"\nTEXT_LITERAL\t\"\\\"\\\\u{0}\\\"\"\n");
    CHECK(remove(table) == 0);
    {
        const char *args[] = {exe,"tokens",path,"--symbol-table",root,"--diagnostics-json","--json",NULL};
        CHECK(spawn(args,CAPTURE,CAPTURE) == 0); CHECK(remove(table) == 0);
    }
}
static void test_payloads(const char *exe) {
    char path[PATH_CAPACITY], text[5000], *output;
    size_t i;
    join_path(path,root,"payload.tfd");
    write_text(path,"\t\"\xc3\xa9\\u{0}\" '\xf0\x9f\x99\x82' false () 1e999 //c\r\n/*b*/ letx Intx _ @");
    CHECK(tokens(exe,path,true,false,NULL) == 1);
    contains(output_path,"\"payload\":\"\xc3\xa9\\u0000\",\"payload_bytes\":3");
    contains(output_path,"\"payload\":\"\xf0\x9f\x99\x82\",\"payload_bytes\":4");
    contains(output_path,"\"kind\":\"FALSE\",\"reserved\":false,\"lexeme\":\"false\",\"payload\":false");
    contains(output_path,"\"kind\":\"LPAREN\""); contains(output_path,"\"kind\":\"RPAREN\"");
    contains(output_path,"\"kind\":\"FLOAT\",\"reserved\":false,\"lexeme\":\"1e999\",\"payload\":\"1e999\"");
    contains(output_path,"\"start\":{\"scalar\":1,\"line\":1,\"column\":2,\"display_column\":5}");
    contains(output_path,"\"kind\":\"COMMENT_LINE\""); contains(output_path,"\"kind\":\"COMMENT_BLOCK\"");
    contains(output_path,"\"kind\":\"IDENT\",\"reserved\":false,\"lexeme\":\"letx\"");
    contains(output_path,"\"kind\":\"IDENT\",\"reserved\":false,\"lexeme\":\"Intx\"");
    contains(output_path,"\"kind\":\"UNDERSCORE\""); contains(output_path,"\"error\":\"E101\"");
    contains(error_path,"error[E101]");
    output = read_text(output_path); CHECK(occurrences(output,"\"kind\":\"EOF\"") == 1); free(output);
    for (i = 0; i < 4000; ++i) text[i] = '9';
    text[4000] = '\0'; write_text(path,text);
    CHECK(tokens(exe,path,true,false,NULL) == 0);
    contains(output_path,text); contains(output_path,"\"payload_bytes\":4000"); equal(error_path,"");
    write_text(path,""); CHECK(tokens(exe,path,true,false,NULL) == 0);
    contains(output_path,"\"kind\":\"EOF\""); contains(output_path,"\"start_byte\":0,\"end_byte\":0");
    output = read_text(output_path); CHECK(occurrences(output,"\"kind\":") == 1); free(output);
}
static void test_errors_caps(const char *exe) {
    char path[PATH_CAPACITY], bytes[400], *output;
    size_t i;
    static const unsigned char invalid[][3] = {{0xff,0,0},{0xef,0xbb,0xbf},{'\r',0,0}};
    static const size_t lengths[] = {1,3,1};
    static const char *const codes[] = {"E001","E002","E003"};
    join_path(path,root,"invalid.tfd");
    for (i = 0; i < 3; ++i) {
        write_bytes(path,invalid[i],lengths[i]); CHECK(tokens(exe,path,true,true,NULL) == 1);
        equal(output_path,"{\"schema_version\":1,\"source_valid\":false,\"has_errors\":true,\"tokens\":[]}\n");
        contains(error_path,codes[i]); contains(error_path,"\"start\":null,\"end\":null");
        contains(error_path,"\"raw\":true");
    }
    for (i = 0; i < 137; ++i) { bytes[2*i] = '@'; bytes[2*i+1] = ' '; }
    write_bytes(path,bytes,274); CHECK(tokens(exe,path,true,true,NULL) == 1);
    output = read_text(error_path); CHECK(occurrences(output,"\"code\":\"E101\"") == 100);
    CHECK(strstr(output,"\"suppressed\":37,\"has_errors\":true") != NULL); free(output);
    output = read_text(output_path); CHECK(occurrences(output,"\"kind\":\"INVALID\"") == 137);
    CHECK(occurrences(output,"\"kind\":\"EOF\"") == 1); free(output);
    /* Bounded malformed byte runs terminate and preserve a single EOF. */
    for (i = 0; i < 12; ++i) {
        static const char *const fragments[] = {"\"bad\\q\" ","'ab' ","00 ","1e++ ","@ ","/*end"};
        write_text(path,fragments[i%6]); CHECK(tokens(exe,path,true,true,NULL) == 1);
        output = read_text(output_path); CHECK(occurrences(output,"\"kind\":\"EOF\"") == 1); free(output);
    }
}
static void test_usage_host(const char *exe) {
    char path[PATH_CAPACITY], bad_path[PATH_CAPACITY], missing[PATH_CAPACITY], table[PATH_CAPACITY];
    const char *commands[] = {"compile","unknown"}; size_t i;
    join_path(path,root,"valid.tfd"); write_text(path,"let a = 1;");
    join_path(missing,root,"missing");
    for (i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i) {
        const char *args[] = {exe,commands[i],path,NULL}; CHECK(spawn(args,CAPTURE,CAPTURE) == 2);
        contains(error_path,"use --help"); equal(output_path,"");
    }
    {
        const char *none[] = {exe,NULL}; const char *help[] = {exe,"--help",NULL};
        const char *no_source[] = {exe,"tokens",NULL};
        CHECK(spawn(none,CAPTURE,CAPTURE) == 2); CHECK(spawn(no_source,CAPTURE,CAPTURE) == 2);
        CHECK(spawn(help,CAPTURE,CAPTURE) == 0); contains(output_path,"No Symbol Table.txt is written");
        CHECK(spawn(help,BROKEN_PIPE,CAPTURE) == 2);
    }
    {
        const char *duplicate[] = {exe,"tokens",path,"--json","--json",NULL};
        const char *incomplete[] = {exe,"tokens",path,"--symbol-table",NULL};
        const char *unknown[] = {exe,"tokens",path,"--other",NULL};
        const char *extra[] = {exe,"tokens",path,path,NULL};
        CHECK(spawn(duplicate,CAPTURE,CAPTURE) == 2); CHECK(spawn(incomplete,CAPTURE,CAPTURE) == 2);
        CHECK(spawn(unknown,CAPTURE,CAPTURE) == 2); CHECK(spawn(extra,CAPTURE,CAPTURE) == 2);
    }
    join_path(bad_path,root,"bad.TFD"); write_text(bad_path,"x"); CHECK(tokens(exe,bad_path,false,false,NULL) == 1);
    contains(error_path,"exactly .tfd"); equal(output_path,"");
    join_path(bad_path,root,"missing.tfd"); CHECK(tokens(exe,bad_path,false,false,NULL) == 2);
    contains(error_path,"cannot load source"); equal(output_path,"");
    CHECK(tokens(exe,path,false,false,missing) == 2); contains(error_path,"cannot publish");
    join_path(table,root,"Symbol Table.txt"); absent(table);
    {
        const char *args[] = {exe,"tokens",path,"--json",NULL};
        CHECK(spawn(args,BROKEN_PIPE,CAPTURE) == 2);
        write_text(path,"@"); CHECK(spawn(args,CAPTURE,BROKEN_PIPE) == 2);
    }
    /* Requested report may not overwrite its input, even through a hardlink. */
    write_text(path,"abc");
#ifdef _WIN32
    CHECK(CreateHardLinkA(table,path,NULL) != 0);
#else
    CHECK(link(path,table) == 0);
#endif
    CHECK(tokens(exe,path,false,false,root) == 2); equal(path,"abc"); CHECK(remove(table) == 0);
    write_text(path,"@"); CHECK(tokens(exe,path,false,false,root) == 1);
    equal(table,"TOKEN TYPE\tLEXEME\nINVALID\t\"@\"\n"); CHECK(remove(table) == 0);
    write_text(path,"\r"); CHECK(tokens(exe,path,false,false,root) == 1);
    equal(table,"TOKEN TYPE\tLEXEME\n"); CHECK(remove(table) == 0);
}
static void test_check_cli(const char *exe) {
    char entry[PATH_CAPACITY], helper[PATH_CAPACITY], table[PATH_CAPACITY];
    join_path(entry,root,"check root.tfd");
    join_path(helper,root,"helper.tfd");
    join_path(table,root,"Symbol Table.txt");
    {
        const char *args[] = {exe,"check",entry,"--root",root,"--diagnostics-json",NULL};
        const char *duplicate[] = {exe,"check",entry,"--root",root,"--root",root,NULL};
        const char *no_root[] = {exe,"check",entry,"--root",NULL};
        const char *wrong_option[] = {exe,"check",entry,"--symbol-table",root,NULL};
        write_text(helper,"pub fn one() -> Int { 1 }");
        write_text(entry,"import \"helper.tfd\" as helper; pub fn library() -> Int { helper::one() }");
        CHECK(spawn(args,CAPTURE,CAPTURE) == 0);
        contains(output_path,"Frontend checks succeeded");
        contains(error_path,"\"schema_version\":2");
        CHECK(spawn(args,BROKEN_PIPE,CAPTURE) == 2);
        CHECK(spawn(duplicate,CAPTURE,CAPTURE) == 2);
        CHECK(spawn(no_root,CAPTURE,CAPTURE) == 2);
        CHECK(spawn(wrong_option,CAPTURE,CAPTURE) == 2);
        write_text(entry,"import \"helper.tfd\" as helper; pub fn library() -> Bool { helper::one() }");
        CHECK(spawn(args,CAPTURE,CAPTURE) == 1); equal(output_path,"");
        contains(error_path,"\"code\":\"E302\"");
        write_text(helper,"fn one() -> Int { 1 }");
        CHECK(spawn(args,CAPTURE,CAPTURE) == 1); equal(output_path,"");
        contains(error_path,"\"code\":\"E206\""); contains(error_path,"helper.tfd");
        CHECK(spawn(args,CAPTURE,BROKEN_PIPE) == 2);
        CHECK(remove(helper) == 0);
        CHECK(spawn(args,CAPTURE,CAPTURE) == 1); contains(error_path,"\"code\":\"E204\"");
        write_text(entry,"pub fn library() -> Int { absent }");
        CHECK(spawn(args,CAPTURE,CAPTURE) == 1); contains(error_path,"\"code\":\"E203\"");
        write_text(entry,"pub fn main() -> List[Text] { input x: Text; output x; [x] }");
        CHECK(spawn(args,CAPTURE,CAPTURE) == 0);
        write_text(entry,"pub fn main() -> List[Text] { input x: Int; [] }");
        CHECK(spawn(args,CAPTURE,CAPTURE) == 1); contains(error_path,"\"code\":\"E302\"");
        write_text(entry,"pub fn main() -> List[Text] { output \"x\"; main() }");
        CHECK(spawn(args,CAPTURE,CAPTURE) == 1); contains(error_path,"\"code\":\"E208\"");
        write_text(entry,"pub fn main() -> List[Text] { [\"x\"] } fn library() -> List[Text] { main() }");
        CHECK(spawn(args,CAPTURE,CAPTURE) == 0);
        write_text(entry,"fn f(v: Option[Bool]) -> Bool { match v { Option::Some(true) => { true } } }");
        CHECK(spawn(args,CAPTURE,CAPTURE) == 1); contains(error_path,"\"code\":\"E310\"");
        write_text(entry,"machine M(x: Unit) yields Unit rejects Unit { state a: Int = 1 div 0; next a = a; }");
        CHECK(spawn(args,CAPTURE,CAPTURE) == 1); contains(error_path,"\"code\":\"E306\"");
        absent(table);
    }
}
static void test_examples(const char *exe) {
    static const char *const examples[] = {
        "boundaries/candidate_fault.tfd","boundaries/checked_division.tfd","boundaries/minimum_overflow.tfd",
        "course/automata.tfd","course/main_io.tfd","course/operator_inventory.tfd",
        "features/dependency_order.tfd","features/fold_and_closure.tfd","features/guard_short_circuit.tfd",
        "features/nested_patterns.tfd","features/snapshot_swap.tfd","features/unicode.tfd",
        "intro/counter.tfd","intro/hello.tfd","invalid/candidate_cycle.tfd","invalid/closure_candidate.tfd",
        "invalid/duplicate_writer.tfd","invalid/missing_writer.tfd","invalid/non_bool_guard.tfd","invalid/non_exhaustive.tfd",
        "lexical/complete.tfd","lexical/invalid.tfd","showcase/branching_inventory.tfd","showcase/models/shelf.tfd"
    };
    size_t i; char path[PATH_CAPACITY];
    for (i = 0; i < sizeof(examples)/sizeof(examples[0]); ++i) {
        const char *expected_check_error = NULL;
        join_path(path,"examples",examples[i]);
        CHECK(tokens(exe,path,true,true,NULL) == (strcmp(examples[i],"lexical/invalid.tfd") == 0 ? 1 : 0));
        contains(output_path,"\"schema_version\":1"); contains(output_path,"\"kind\":\"EOF\"");
        contains(error_path,"\"schema_version\":2");
        if (strcmp(examples[i],"lexical/invalid.tfd") == 0) expected_check_error = "E101";
        else if (strcmp(examples[i],"invalid/closure_candidate.tfd") == 0) expected_check_error = "E406";
        else if (strcmp(examples[i],"invalid/non_bool_guard.tfd") == 0) expected_check_error = "E302";
        else if (strcmp(examples[i],"invalid/non_exhaustive.tfd") == 0) expected_check_error = "E310";
        else if (strcmp(examples[i],"invalid/candidate_cycle.tfd") == 0) expected_check_error = "E404";
        else if (strcmp(examples[i],"invalid/duplicate_writer.tfd") == 0) expected_check_error = "E402";
        else if (strcmp(examples[i],"invalid/missing_writer.tfd") == 0) expected_check_error = "E401";
        const char *args[] = {exe,"check",path,"--root","examples","--diagnostics-json",NULL};
        CHECK(spawn(args,CAPTURE,CAPTURE) == (expected_check_error == NULL ? 0 : 1));
        if (expected_check_error != NULL) contains(error_path,expected_check_error);
        else contains(error_path,"\"diagnostics\":[]");
        const char *core[] = {exe,"core",path,"--root","examples","--diagnostics-json",NULL};
        const int core_status = spawn(core,CAPTURE,CAPTURE);
        const int expected_status = expected_check_error == NULL ? 0 : 1;
        if (core_status != expected_status) {
            char *diagnostics = read_text(error_path);
            fprintf(stderr,"core example %s: %s\n",examples[i],diagnostics);
            free(diagnostics);
        }
        CHECK(core_status == expected_status);
        if (expected_check_error != NULL) { equal(output_path,""); contains(error_path,expected_check_error); }
        else { contains(output_path,"Core entry=f"); contains(error_path,"\"diagnostics\":[]"); }
    }
    CHECK(tokens(exe,"examples/lexical/complete.tfd",false,false,root) == 0);
    {
        char table[PATH_CAPACITY]; char *golden = read_text("tests/fixtures/lexer/Symbol Table.txt");
        join_path(table,root,"Symbol Table.txt"); equal(table,golden); free(golden); CHECK(remove(table) == 0);
    }
}
static void test_plan_cli(const char *exe) {
    char path[PATH_CAPACITY], table[PATH_CAPACITY];
    const char *text_args[] = {exe,"plan","tests/fixtures/plan/ordered.tfd",NULL};
    const char *json_args[] = {exe,"plan","tests/fixtures/plan/ordered.tfd","--json","--diagnostics-json",NULL};
    const char *permuted[] = {exe,"plan","tests/fixtures/plan/permuted.tfd","--root","tests/fixtures/plan",NULL};
    const char *bad_option[] = {exe,"plan","tests/fixtures/plan/ordered.tfd","--symbol-table",root,NULL};
    const char *duplicate[] = {exe,"plan","tests/fixtures/plan/ordered.tfd","--json","--json",NULL};
    const char *check_json[] = {exe,"check","tests/fixtures/plan/ordered.tfd","--json",NULL};
    const char *errors[] = {exe,"plan",path,"--diagnostics-json",NULL};
    char *first;
    join_path(path,root,"plan errors.tfd");
    join_path(table,root,"Symbol Table.txt");
    CHECK(spawn(text_args,CAPTURE,CAPTURE) == 0);
    contains(output_path,"guards are runtime conditions");
    contains(output_path,"schedule: a B b z");
    contains(output_path,"schedule: left right");
    contains(output_path,"schedule: a z values");
    contains(output_path,"dependency a -> z");
    contains(output_path,"candidate context=writer");
    contains(output_path,"old context=writer");
    contains(output_path,"input context=writer");
    contains(output_path,"guard 0 predicate=");
    contains(output_path,"reject-on-false=");
    contains(output_path,"emission 1 expression=");
    CHECK(spawn(json_args,CAPTURE,CAPTURE) == 0);
    contains(output_path,"\"schema_version\":1");
    contains(output_path,"\"guards_proven\":false");
    contains(output_path,"\"schedule\":[1,2,3,0]");
    contains(error_path,"\"diagnostics\":[]");
    first = read_text(output_path);
    CHECK(spawn(json_args,CAPTURE,CAPTURE) == 0);
    equal(output_path,first);
    free(first);
    CHECK(spawn(permuted,CAPTURE,CAPTURE) == 0);
    contains(output_path,"schedule: a B b z");
    CHECK(spawn(json_args,BROKEN_PIPE,CAPTURE) == 2);
    CHECK(spawn(bad_option,CAPTURE,CAPTURE) == 2);
    CHECK(spawn(duplicate,CAPTURE,CAPTURE) == 2);
    CHECK(spawn(check_json,CAPTURE,CAPTURE) == 2);
    write_text(path,"machine M(x: Unit) yields Unit rejects Unit { state a: Int = 0; next a = next.a; }");
    CHECK(spawn(errors,CAPTURE,CAPTURE) == 1);
    equal(output_path,""); contains(error_path,"\"code\":\"E404\"");
    CHECK(spawn(errors,CAPTURE,BROKEN_PIPE) == 2);
    write_text(path,"machine M(x: Unit) yields Unit rejects Unit { state a: Int = 0; state b: Int = 0; next a = a; }");
    CHECK(spawn(errors,CAPTURE,CAPTURE) == 1);
    equal(output_path,""); contains(error_path,"\"code\":\"E401\"");
    write_text(path,"machine M(x: Unit) yields Unit rejects Unit { state a: Int = 0; next a = 0; next a = 1; }");
    CHECK(spawn(errors,CAPTURE,CAPTURE) == 1);
    equal(output_path,""); contains(error_path,"\"code\":\"E402\"");
    write_text(path,"fn library() -> Unit { () }");
    CHECK(spawn(errors,CAPTURE,CAPTURE) == 0);
    contains(output_path,"Machine plan;");
    absent(table);
}
static void test_core_cli(const char *exe) {
    char entry[PATH_CAPACITY], table[PATH_CAPACITY];
    join_path(entry,root,"core root.tfd"); join_path(table,root,"Symbol Table.txt");
    const char *args[] = {exe,"core",entry,"--diagnostics-json",NULL};
    const char *json[] = {exe,"core",entry,"--json",NULL};
    const char *symbol[] = {exe,"core",entry,"--symbol-table",root,NULL};
    write_text(entry,"pub fn main() -> List[Text] { input x: Text; output x; [x] }");
    CHECK(spawn(args,CAPTURE,CAPTURE)==0);
    contains(output_path,"name=\"$entry\""); contains(output_path,"InputAt index=0");
    contains(output_path,"Binary concat"); contains(error_path,"\"diagnostics\":[]");
    char *first=read_text(output_path);
    CHECK(spawn(args,CAPTURE,CAPTURE)==0); equal(output_path,first); free(first);
    CHECK(spawn(args,BROKEN_PIPE,CAPTURE)==2);
    CHECK(spawn(json,CAPTURE,CAPTURE)==2);
    CHECK(spawn(symbol,CAPTURE,CAPTURE)==2); absent(table);

    write_text(entry,"pub fn main() -> List[Text] { output \"prefix\"; main() }");
    CHECK(spawn(args,CAPTURE,CAPTURE)==1); equal(output_path,""); contains(error_path,"\"code\":\"E208\"");
    write_text(entry,"pub fn main() -> List[Text] { [\"pure\"] } fn f() -> List[Text] { main() }");
    CHECK(spawn(args,CAPTURE,CAPTURE)==0);
    contains(output_path,"ordinary name=\"main\""); contains(output_path," = Call f");
}
static void output_bytes(const char *expected, size_t length) {
    FILE *out=fopen(output_path,"rb");
    CHECK(out!=NULL);
    for (size_t i=0; i<length; ++i) CHECK(fgetc(out)==(unsigned char)expected[i]);
    CHECK(fgetc(out)==EOF && feof(out) && fclose(out)==0);
}

static void test_run_cli(const char *exe) {
    static const struct { const char *path,*output; size_t bytes; const char *fault; } examples[]={
#define RUN(path, text) {"examples/" path,text,sizeof(text)-1,NULL}
        RUN("intro/hello.tfd","Hello, Ternfold.\n"), RUN("intro/counter.tfd","4\n"),
        RUN("features/snapshot_swap.tfd","9\n2\n"), RUN("features/dependency_order.tfd","14\n"),
        RUN("features/guard_short_circuit.tfd","first reason\n0\n"), RUN("features/fold_and_closure.tfd","46\n"),
        RUN("features/nested_patterns.tfd","on\nmissing\n"), RUN("features/unicode.tfd","3\nNUL follows: \0end\n"),
        RUN("boundaries/checked_division.tfd","-3\n-1\n1\n"),
        RUN("course/automata.tfd","Ternfold\n-3\n"), RUN("course/operator_inventory.tfd","3.5e0\n3\n1\n8\ntrue\n"),
        RUN("showcase/branching_inventory.tfd","Ternfold: inspect a change before accepting it\naccepted count=7\n"
            "free=3\nrejected capacity; count=7\noriginal still=3\naccepted count=4\nfree=6\n"),
        {"examples/boundaries/candidate_fault.tfd","",0,"R002"},
        {"examples/boundaries/minimum_overflow.tfd","",0,"R001"}
#undef RUN
    };
    for (size_t i=0; i<sizeof examples/sizeof *examples; ++i) {
        const char *args[]={exe,"run",examples[i].path,"--root","examples","--diagnostics-json",NULL};
        const int status=spawn(args,CAPTURE,CAPTURE);
        if (status!=(examples[i].fault==NULL ? 0 : 3)) {
            char *error=read_text(error_path);
            fprintf(stderr,"run example %s: %s\n",examples[i].path,error); free(error);
        }
        CHECK(status==(examples[i].fault==NULL ? 0 : 3));
        output_bytes(examples[i].output,examples[i].bytes);
        contains(error_path,examples[i].fault==NULL ? "\"diagnostics\":[]" : examples[i].fault);
    }
    char input[PATH_CAPACITY]; join_path(input,root,"captured input.txt");
    write_text(input,"Ada\r\nextra\n");
    const char *course[]={exe,"run","examples/course/main_io.tfd","--stdin-file",input,"--diagnostics-json",NULL};
    CHECK(spawn(course,CAPTURE,CAPTURE)==0); equal(output_path,"Hello Ada\n26\n-3\n");
    CHECK(spawn(course,BROKEN_PIPE,CAPTURE)==2);

    const char *fault[]={exe,"run","tests/fixtures/types/interpreter-fault.tfd","--stdin-file",input,"--diagnostics-json",NULL};
    write_text(input,"first only\n");
    CHECK(spawn(fault,CAPTURE,CAPTURE)==3); output_bytes("",0); contains(error_path,"\"code\":\"R006\"");
    CHECK(spawn(fault,CAPTURE,BROKEN_PIPE)==2);
    write_text(input,"first\nsecond");
    CHECK(spawn(fault,CAPTURE,CAPTURE)==3); output_bytes("",0); contains(error_path,"\"code\":\"R002\"");
    write_bytes(input,"\xc0\x80",2);
    CHECK(spawn(fault,CAPTURE,CAPTURE)==2); output_bytes("",0); contains(error_path,"not valid UTF-8");

    const char *library[]={exe,"run","examples/showcase/models/shelf.tfd","--diagnostics-json",NULL};
    CHECK(spawn(library,CAPTURE,CAPTURE)==1); output_bytes("",0); contains(error_path,"\"code\":\"E208\"");
    const char *origin[]={exe,"run","tests/fixtures/types/core-wrapper-root.tfd","--diagnostics-json",NULL};
    CHECK(spawn(origin,CAPTURE,CAPTURE)==3); output_bytes("",0);
    contains(error_path,"\"code\":\"R003\""); contains(error_path,"core-wrapper-root.tfd");
    char *json=read_text(error_path); CHECK(strstr(json,"core-wrapper-library.tfd")==NULL); free(json);
    const char *fixtures[]={"interpreter-late-guard.tfd","interpreter-late-emission.tfd",
                             "interpreter-candidate-order.tfd","interpreter-negative-power.tfd"};
    const char *codes[]={"R002","R002","R001","R005"};
    for (size_t i=0; i<sizeof fixtures/sizeof *fixtures; ++i) {
        char path[PATH_CAPACITY]; join_path(path,"tests/fixtures/types",fixtures[i]);
        const char *args[]={exe,"run",path,"--diagnostics-json",NULL};
        CHECK(spawn(args,CAPTURE,CAPTURE)==3); output_bytes("",0); contains(error_path,codes[i]);
    }
}
static void test_ast_cli(const char *exe) {
    char path[PATH_CAPACITY], table[PATH_CAPACITY];
    const char *args[] = {exe,"ast",path,"--json","--diagnostics-json",NULL};
    char *first;
    join_path(path,root,"ast.tfd");join_path(table,root,"Symbol Table.txt");
    write_text(path,"pub fn main() -> List[Text] { input x: Text; output x; [x] }");
    CHECK(spawn(args,CAPTURE,CAPTURE)==0);contains(output_path,"\"kind\":\"INPUT\"");
    contains(output_path,"\"has_errors\":false");contains(error_path,"\"diagnostics\":[]");absent(table);
    first=read_text(output_path);CHECK(spawn(args,CAPTURE,CAPTURE)==0);equal(output_path,first);free(first);
    CHECK(spawn(args,BROKEN_PIPE,CAPTURE)==2);
    write_text(path,"fn bad() -> Int { f( fn good() -> Int { 1 }");
    CHECK(spawn(args,CAPTURE,CAPTURE)==1);contains(output_path,"\"has_errors\":true");
    contains(output_path,"\"name\":\"good\"");contains(error_path,"E110");
    CHECK(spawn(args,CAPTURE,BROKEN_PIPE)==2);
    write_text(path,"machine M(x: Int) yields Int rejects Text { state a: Int = 0; next a = 1; output x; }");
    CHECK(spawn(args,CAPTURE,CAPTURE)==1);contains(error_path,"E208");
    write_text(path,"fn main() -> Int { 1 } fn f() -> Int { main() }");
    CHECK(spawn(args,CAPTURE,CAPTURE)==0);contains(output_path,"\"kind\":\"CALL\"");
    write_text(path,"\r");CHECK(spawn(args,CAPTURE,CAPTURE)==1);
    equal(output_path,"{\"schema_version\":1,\"root\":null,\"has_errors\":true,\"nodes\":[]}\n");
    contains(error_path,"E003");
    write_text(path,"fn f() -> Int { @ }");CHECK(spawn(args,CAPTURE,CAPTURE)==1);
    contains(error_path,"E101");contains(output_path,"\"kind\":\"ERROR\"");
    {
        const char *bad[] = {exe,"ast",path,"--symbol-table",root,NULL};
        const char *duplicate[] = {exe,"ast",path,"--json","--json",NULL};
        const char *text[] = {exe,"ast","tests/fixtures/parser/surface.tfd",NULL};
        CHECK(spawn(bad,CAPTURE,CAPTURE)==2);absent(table);
        CHECK(spawn(duplicate,CAPTURE,CAPTURE)==2);
        CHECK(spawn(text,CAPTURE,CAPTURE)==0);contains(output_path,"AST root=1 has_errors=false");
    }
    {
        char deep[512];size_t at=0;
        memcpy(deep,"fn f() -> Int { ",16);at=16;
        for(size_t i=0;i<150;++i)deep[at++]='(';
        deep[at++]='1';
        for(size_t i=0;i<150;++i)deep[at++]=')';
        deep[at++]='}';deep[at]=0;write_text(path,deep);
        CHECK(spawn(args,CAPTURE,CAPTURE)==2);contains(error_path,"nesting budget");equal(output_path,"");
    }
    {
        const char *examples[]={"examples/intro/hello.tfd","examples/invalid/candidate_cycle.tfd",
            "examples/course/main_io.tfd","examples/lexical/complete.tfd"};
        for(size_t i=0;i<sizeof(examples)/sizeof(examples[0]);++i){
            const char *one[]={exe,"ast",examples[i],"--json",NULL};
            CHECK(spawn(one,CAPTURE,CAPTURE)==0);contains(output_path,"\"has_errors\":false");
        }
    }
}
typedef struct { size_t allocations; bool fail; } Memory;
static void *allocate(void *context, size_t size) {
    Memory *m = context; ++m->allocations; return m->fail ? NULL : malloc(size);
}
static void deallocate(void *context, void *p) { (void)context; free(p); }
static void test_writer_no_allocations(void) {
    Memory m = {0}; TfAllocator allocator = {&m,allocate,deallocate};
    TfSource source, invalid; TfLexResult result = {0}; FILE *file; size_t before;
    CHECK(tf_source_init(&source,"unit","\"\\u{0}\" 12",10,&allocator) == TF_OK);
    CHECK(tf_lex(&result,&source,100,&allocator) == TF_OK);
    before = m.allocations; m.fail = true; file = tmpfile(); CHECK(file != NULL);
    CHECK(tf_token_dump_write(file,&source,&result,TF_TOKEN_DUMP_TEXT) == TF_OK);
    CHECK(tf_token_dump_write(file,&source,&result,TF_TOKEN_DUMP_JSON) == TF_OK);
    CHECK(m.allocations == before); CHECK(fflush(file) == 0);
    CHECK(tf_token_dump_write(file,&source,NULL,TF_TOKEN_DUMP_TEXT) == TF_HOST_ERROR);
    CHECK(tf_token_dump_write(file,NULL,&result,TF_TOKEN_DUMP_TEXT) == TF_HOST_ERROR);
    CHECK(tf_token_dump_write(NULL,&source,&result,TF_TOKEN_DUMP_TEXT) == TF_HOST_ERROR);
    CHECK(tf_token_dump_write(file,&source,&result,(TfTokenDumpFormat)99) == TF_HOST_ERROR);
    CHECK(fclose(file) == 0); tf_lex_destroy(&result); tf_source_destroy(&source);
    CHECK(tf_source_init(&invalid,"raw","\r",1,NULL) == TF_SOURCE_ERROR);
    file = tmpfile(); CHECK(file != NULL); CHECK(tf_token_dump_write(file,&invalid,NULL,TF_TOKEN_DUMP_JSON) == TF_OK);
    CHECK(tf_token_dump_write(file,&invalid,&result,TF_TOKEN_DUMP_JSON) == TF_HOST_ERROR);
    CHECK(fclose(file) == 0); tf_source_destroy(&invalid);
    m.fail = false;
    CHECK(tf_source_init(&source,"unit","x",1,&allocator) == TF_OK); CHECK(tf_lex(&result,&source,100,&allocator) == TF_OK);
    file = fopen("tests/fixtures/tokens/basic.json","rb"); CHECK(file != NULL); CHECK(setvbuf(file,NULL,_IONBF,0) == 0);
    CHECK(tf_token_dump_write(file,&source,&result,TF_TOKEN_DUMP_JSON) == TF_HOST_ERROR);
    CHECK(fclose(file) == 0); tf_lex_destroy(&result); tf_source_destroy(&source);
}
int main(int argc, char **argv) {
    CHECK(argc == 2); setup();
    test_goldens(argv[1]); test_payloads(argv[1]); test_errors_caps(argv[1]);
    test_usage_host(argv[1]); test_examples(argv[1]); test_writer_no_allocations();
    test_ast_cli(argv[1]); test_check_cli(argv[1]); test_plan_cli(argv[1]); test_core_cli(argv[1]); test_run_cli(argv[1]);
    (void)printf("tokens CLI: %zu checks passed, %zu token invocations, all 24 examples\nfixtures: %s\n",checks,cases,root);
    return 0;
}
