/* Run from the repository root: test_differential COMPILER CC RUNTIME_ROOT [--sanitize].
 * See tests/test_tokens_cli.c for the corresponding direct process runner. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#define REMOVE_DIR(p) _rmdir(p)
#define EXE ".exe"
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define MAKE_DIR(p) mkdir(p, 0700)
#define REMOVE_DIR(p) rmdir(p)
#define EXE ""
#endif

enum { PATH_CAP = 4096, BYTE_CAP = 128 * 1024, TIMEOUT_MS = 60000 };
static const char scratch[] = "tmp/m11-differential";
static const char *current;
static char work[PATH_CAP], out[PATH_CAP], err[PATH_CAP], input[PATH_CAP], native[PATH_CAP];
static size_t traced_execution_pairs;
static const char *child_stdin; /* NULL selects the null device. */

#define CHECK(ok) do { if (!(ok)) { \
    fprintf(stderr,"differential:%d: %s (%s); captures in %s\n",__LINE__,#ok,current,work); \
    exit(1); \
} } while (0)
#define FIX "tests/fixtures/differential/"
#define M09 "tests/fixtures/types/"
#define LIB "tests/fixtures/stdlib/"
#define LF_OK "{\"schema_version\":2,\"diagnostics\":[],\"suppressed\":0,\"has_errors\":false}\n"

typedef struct { char *data; size_t length; } Bytes;
#include "fixture_json.h"
typedef struct {
    const char *source, *output;
    size_t output_size;
    const char *stdin_bytes;
    size_t stdin_size;
    const char *code, *origin_file, *context;
    size_t origin_offset, origin_size;
    bool bad_input, staged_library;
} Case;

#define OK(path, bytes) {.source=path,.output=bytes,.output_size=sizeof(bytes)-1}
#define IO(path, bytes, data) \
    {.source=path,.output=bytes,.output_size=sizeof(bytes)-1,.stdin_bytes=data,.stdin_size=sizeof(data)-1}
#define FAULT(path, fault, file, anchor, offset, spelling) \
    {.source=path,.output="",.code=fault,.origin_file=file,.context=anchor, \
     .origin_offset=offset,.origin_size=sizeof(spelling)-1}
/* The fixture's context uniquely identifies the expected source occurrence.
 * Offsets are byte offsets within that context (not diagnostic line columns). */
static const Case cases[] = {
    OK("examples/intro/hello.tfd","Hello, Ternfold.\n"),
    OK("examples/intro/counter.tfd","4\n"),
    OK("examples/features/snapshot_swap.tfd","9\n2\n"),
    OK("examples/features/dependency_order.tfd","14\n"),
    OK("examples/features/guard_short_circuit.tfd","first reason\n0\n"),
    OK("examples/features/fold_and_closure.tfd","46\n"),
    OK("examples/features/nested_patterns.tfd","on\nmissing\n"),
    OK("examples/features/unicode.tfd","3\nNUL follows: \0end\n"),
    OK("examples/boundaries/checked_division.tfd","-3\n-1\n1\n"),
    FAULT("examples/boundaries/candidate_fault.tfd","R002",NULL,"1 div request",2,"div"),
    FAULT("examples/boundaries/minimum_overflow.tfd","R001",NULL,"minimum div -1",8,"div"),
    OK("examples/course/automata.tfd","Ternfold\n-3\n"),
    IO("examples/course/main_io.tfd","Hello Ada\n26\n-3\n","Ada\r\nextra\n"),
    OK("examples/course/operator_inventory.tfd","3.5e0\n3\n1\n8\ntrue\n"),
    OK("examples/course/precedence_bindings.tfd",
       "-4\n4\n512\n7\n20\n2.0e0\ntrue\ntrue\n19\n5\nold=10\nproposed=13\n26\n10\n"),
    OK("examples/showcase/branching_inventory.tfd",
       "Ternfold: inspect a change before accepting it\naccepted count=7\nfree=3\n"
       "rejected capacity; count=7\noriginal still=3\naccepted count=4\nfree=6\n"),
    OK(M09 "interpreter-core.tfd","accepted\nold snapshot\nrejected\nchecked\na\0b\n"),
    OK(M09 "interpreter-prelude.tfd",
       "-2\n-1\n0\n3\n123\n7\n0\n8\nnone\n-9223372036854775808\nnone\nnone\nnone\n-2\n"
       "1.0000000000000001e-1\n4.9406564584124654e-324\n4\n\xf0\x9f\x99\x82\n\0b\n\nnone\nfalse\n"),
    FAULT(M09 "interpreter-candidate-order.tfd","R001",NULL,"9223372036854775807 + 1",20,"+"),
    FAULT(M09 "interpreter-late-guard.tfd","R002",NULL,"guard 1 div 0",8,"div"),
    FAULT(M09 "interpreter-late-emission.tfd","R002",NULL,"emit int_text(1 div 0)",16,"div"),
    FAULT(M09 "interpreter-negative-power.tfd","R005",NULL,"2 ^ -1",2,"^"),
    {.source=M09 "interpreter-fault.tfd",.output="",.stdin_bytes="first\n",.stdin_size=6,
     .code="R006",.context="input second: Text;",.origin_size=sizeof("input second: Text;")-1},
    {.source=M09 "interpreter-fault.tfd",.output="",.stdin_bytes="first\nsecond",.stdin_size=12,
     .code="R002",.context="1 div 0",.origin_offset=2,.origin_size=3},
    {.source=M09 "interpreter-fault.tfd",.output="",.stdin_bytes="\xc0\x80",.stdin_size=2,.bad_input=true},
    /* Each executor must report its own documented limit at the source
     * invocation. We do not compare allocation counts or active frame depth. */
    FAULT(M09 "interpreter-recursion.tfd","R003",NULL,"recurse(x + 1)",0,"recurse(x + 1)"),
    FAULT(M09 "core-wrapper-root.tfd","R003",NULL,"r(-9223372036854775808, 1)",0,"r(-9223372036854775808, 1)"),
    IO(FIX "input & snapshot.tfd","first=A\0da\nempty=\nlast=fin\ral\r\nquote: \" slash: \\ tab: \t NUL: \0end\n",
       "A\0da\r\n\r\nfin\ral\r"),
    {.source=FIX "input & snapshot.tfd",.output="",.stdin_bytes="one\n\n",.stdin_size=5,
     .code="R006",.context="input last: Text;",.origin_size=sizeof("input last: Text;")-1},
    OK(FIX "numeric.tfd","-9223372036854775808\n9223372036854775807\n-3\n-1\n1\n1.25e0\n"
       "true\ntrue\n4.9406564584124654e-324\n0.0e0\n0.0e0\ntrue\n"),
    OK(FIX "loop-resume.tfd","true\ntrue\ntrue\n"),
    OK(FIX "backend-edges.tfd",
       "1121\ntrue\ntrue\ntrue\ntrue\ntrue\n"
       "true\ntrue\ntrue\ntrue\ntrue\n0\n9\n"),
    FAULT(FIX "backend-edge-record-div-first.tfd","R002",NULL,"7 div 0",2,"div"),
    FAULT(FIX "backend-edge-record-overflow-first.tfd","R001",NULL,
          "9223372036854775807 + 1",20,"+"),
    FAULT(FIX "callback-map.tfd","R002",FIX "callback-library.tfd","7 div n",2,"div"),
    FAULT(FIX "callback-fold.tfd","R005",NULL,"start ^ exponent",6,"^"),
#define LIB_OK(path, text) {.source=LIB path,.output=text,.output_size=sizeof(text)-1,.staged_library=true}
#define LIB_FAULT(path, fault, file, anchor, offset, spelling) \
    {.source=LIB path,.output="",.code=fault,.origin_file=file,.context=anchor, \
     .origin_offset=offset,.origin_size=sizeof(spelling)-1,.staged_library=true}
    LIB_OK("text-helpers.tfd",
        "join-empty=\njoin-one=\xf0\x9f\x99\x82\0\xc3\xb1\n"
        "join-many=|one||\xf0\x9f\x99\x82\0\xc3\xb1|\njoin-no-separator=abc\n"
        "lines-empty=true\nlines-one=true\nlines-many=true\nlines-no-final-lf=true\n"
        "lines-lf=true\nlines-two-lf=true\nlines-cr=true\nlines-scalar=true\n"
        "unlines-empty=true\nunlines-one=true\nunlines-many=true\nunlines-keeps-lf=true\nroundtrip=true\n"),
    LIB_OK("ints-helpers.tfd",
        "sum-empty=0\nsum-one=7\nsum-many=5\nsum-min=-9223372036854775808\n"
        "sum-max=9223372036854775807\nsum-extrema=-1\nsum-left=9223372036854775807\n"
        "summary-empty=none\nsummary-one=1,7,7,7\nsummary-many=5,5,-5,8\n"
        "summary-min=1,-9223372036854775808,-9223372036854775808,-9223372036854775808\n"
        "summary-max=1,9223372036854775807,9223372036854775807,9223372036854775807\n"
        "summary-extrema=2,-1,-9223372036854775808,9223372036854775807\nsummary-zeros=3,0,0,0\n"
        "summary-left=3,-9223372036854775808,-9223372036854775808,1\nsummary-public=true\n"),
    LIB_FAULT("sum-overflow.tfd","R001","stdlib/ints.tfd","total + value })",6,"+"),
    LIB_FAULT("sum-underflow.tfd","R001","stdlib/ints.tfd","total + value })",6,"+"),
    LIB_FAULT("summary-overflow.tfd","R001","stdlib/ints.tfd","held.total + value",11,"+"),
    LIB_FAULT("summary-underflow.tfd","R001","stdlib/ints.tfd","held.total + value",11,"+"),
    LIB_FAULT("sum-argument-order.tfd","R002",NULL,"1 div 0",2,"div"),
    LIB_FAULT("text-argument-order.tfd","R002",NULL,"1 div 0",2,"div")
};
#undef LIB_FAULT
#undef LIB_OK
#undef OK
#undef IO
#undef FAULT

static void join(char *path, const char *directory, const char *leaf) {
    const int n=snprintf(path,PATH_CAP,"%s/%s",directory,leaf);
    CHECK(n>0 && (size_t)n<PATH_CAP);
}
static void origin_path(const char *relative, char path[static PATH_CAP]) {
#ifdef _WIN32
    const DWORD n=GetFullPathNameA(relative,PATH_CAP,path,NULL);
    CHECK(n>0 && n<PATH_CAP);
#else
    char cwd[PATH_CAP];
    CHECK(getcwd(cwd,sizeof cwd)!=NULL);
    join(path,cwd,relative);
#endif
}
static void setup(void) {
#ifdef _WIN32
    const unsigned long pid=(unsigned long)GetCurrentProcessId();
    (void)SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
    const unsigned long pid=(unsigned long)getpid();
#endif
    CHECK(MAKE_DIR("tmp")==0 || errno==EEXIST);
    CHECK(MAKE_DIR(scratch)==0 || errno==EEXIST);
    unsigned int i;
    for (i=0;i<64;++i) {
        const int n=snprintf(work,sizeof work,"%s/run & (!) %lu-%u",scratch,pid,i);
        CHECK(n>0 && (size_t)n<sizeof work);
        if (MAKE_DIR(work)==0) break;
        CHECK(errno==EEXIST);
    }
    CHECK(i<64);
    join(out,work,"stdout.bin"); join(err,work,"stderr.bin");
    /* LLVM-MinGW LLD rejects '%' in its own output directory. Keep it in
     * the published filename to test argv and publication independently. */
    join(input,work,"stdin & data.bin"); join(native,work,"native & % program" EXE);
}
static void write_bytes(const char *path, const char *data, size_t length) {
    FILE *f=fopen(path,"wb");
    CHECK(f!=NULL); CHECK(fwrite(data,1,length,f)==length); CHECK(fclose(f)==0);
}
static Bytes read_bytes(const char *path) {
    FILE *f=fopen(path,"rb");
    CHECK(f!=NULL); CHECK(fseek(f,0,SEEK_END)==0);
    const long size=ftell(f);
    CHECK(size>=0 && size<=BYTE_CAP); CHECK(fseek(f,0,SEEK_SET)==0);
    Bytes result={malloc((size_t)size+1),(size_t)size};
    CHECK(result.data!=NULL); CHECK(fread(result.data,1,result.length,f)==result.length);
    CHECK(fgetc(f)==EOF && feof(f)); CHECK(fclose(f)==0);
    result.data[result.length]='\0';
    return result;
}
static bool library_staged;
static void stage_library(bool cleanup) {
    static const char *const names[]={"text.tfd","ints.tfd"};
    char directory[PATH_CAP],original[PATH_CAP],copy[PATH_CAP];
    join(directory,work,"stdlib");
    if (!cleanup) {
        if (library_staged) return;
        CHECK(MAKE_DIR(directory)==0);
    } else if (!library_staged) return;
    for (size_t i=0;i<sizeof names/sizeof *names;++i) {
        join(original,"stdlib",names[i]); join(copy,directory,names[i]);
        if (cleanup) CHECK(remove(copy)==0);
        else {
            const Bytes bytes=read_bytes(original);
            write_bytes(copy,bytes.data,bytes.length); free(bytes.data);
        }
    }
    if (cleanup) CHECK(REMOVE_DIR(directory)==0);
    library_staged=!cleanup;
}
static void expect(const char *path, const char *expected, size_t length) {
    const Bytes actual=read_bytes(path);
    if (actual.length!=length || memcmp(actual.data,expected,length)!=0) {
        size_t i=0;
        while (i<actual.length && i<length && actual.data[i]==expected[i]) ++i;
        fprintf(stderr,"%s: first difference at byte %zu (expected length %zu, got %zu)\n",
                path,i,length,actual.length);
        if (i<length) fprintf(stderr,"  expected 0x%02x\n",(unsigned)(unsigned char)expected[i]);
        if (i<actual.length) fprintf(stderr,"  actual   0x%02x\n",(unsigned)(unsigned char)actual.data[i]);
        CHECK(false);
    }
    free(actual.data);
}

#ifdef _WIN32
/* Windows has no argv system call. Quote each argument for CRT decoding
 * and provide the application name separately to CreateProcess. */
static char *command_line(const char *const args[]) {
    size_t cap=1;
    for (size_t i=0;args[i]!=NULL;++i) {
        const size_t n=strlen(args[i]);
        CHECK(n<=PATH_CAP && cap+2*n+3<=32767);
        cap+=2*n+3;
    }
    char *line=malloc(cap);
    CHECK(line!=NULL);
    char *next=line;
    for (size_t i=0;args[i]!=NULL;++i) {
        if (i!=0) *next++=' ';
        *next++='"';
        const char *p=args[i];
        while (*p!='\0') {
            size_t slashes=0;
            while (*p=='\\') { ++slashes; ++p; }
            if (*p=='"' || *p=='\0') slashes*=2;
            for (size_t j=0;j<slashes;++j) *next++='\\';
            if (*p=='"') *next++='\\';
            if (*p!='\0') *next++=*p++;
        }
        *next++='"';
    }
    *next='\0';
    return line;
}
static HANDLE stream(const char *path, bool reading) {
    SECURITY_ATTRIBUTES inheritable={sizeof inheritable,NULL,TRUE};
    HANDLE h=CreateFileA(path,reading ? GENERIC_READ : GENERIC_WRITE,FILE_SHARE_READ | FILE_SHARE_WRITE,
                         &inheritable,reading ? OPEN_EXISTING : CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    CHECK(h!=INVALID_HANDLE_VALUE);
    return h;
}
static int spawn(const char *const args[]) {
    STARTUPINFOA si={0};
    PROCESS_INFORMATION pi={0};
    si.cb=sizeof si;
    si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdInput=stream(child_stdin ? child_stdin : "NUL",true);
    si.hStdOutput=stream(out,false);
    si.hStdError=stream(err,false);
    char *line=command_line(args);
    const BOOL launched=CreateProcessA(args[0],line,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi);
    const DWORD launch_error=launched ? 0 : GetLastError();
    free(line);
    CHECK(CloseHandle(si.hStdInput)!=0);
    CHECK(CloseHandle(si.hStdOutput)!=0);
    CHECK(CloseHandle(si.hStdError)!=0);
    if (!launched) fprintf(stderr,"CreateProcess failed: %s (%lu)\n",args[0],(unsigned long)launch_error);
    CHECK(launched!=0);
    CHECK(CloseHandle(pi.hThread)!=0);
    const DWORD waited=WaitForSingleObject(pi.hProcess,TIMEOUT_MS);
    if (waited!=WAIT_OBJECT_0) {
        (void)TerminateProcess(pi.hProcess,125);
        (void)WaitForSingleObject(pi.hProcess,1000);
        (void)CloseHandle(pi.hProcess);
        fprintf(stderr,"%s: process wait failed (%lu)\n",args[0],(unsigned long)waited);
        CHECK(false);
    }
    DWORD code=0;
    CHECK(GetExitCodeProcess(pi.hProcess,&code)!=0); CHECK(CloseHandle(pi.hProcess)!=0);
    if (code>255) fprintf(stderr,"%s: Windows process error 0x%08lx\n",args[0],(unsigned long)code);
    CHECK(code<=255);
    return (int)code;
}
#else
static int spawn(const char *const args[]) {
    struct timespec start, now;
    const struct timespec pause={0,10000000L};
    CHECK(clock_gettime(CLOCK_MONOTONIC,&start)==0);
    const pid_t pid=fork();
    CHECK(pid>=0);
    if (pid==0) {
        if (setpgid(0,0)!=0) _exit(126);
        const int descriptors[]={open(child_stdin ? child_stdin : "/dev/null",O_RDONLY),
            open(out,O_WRONLY|O_CREAT|O_TRUNC,0600),open(err,O_WRONLY|O_CREAT|O_TRUNC,0600)};
        for (int i=0;i<3;++i) {
            if (descriptors[i]<0 || dup2(descriptors[i],i)<0) _exit(126);
            if (descriptors[i]>2) (void)close(descriptors[i]);
        }
        execv(args[0],(char *const *)args);
        _exit(127);
    }
    (void)setpgid(pid,pid);
    int status=0;
    for (;;) {
        const pid_t done=waitpid(pid,&status,WNOHANG);
        if (done==pid) break;
        CHECK(done==0 || errno==EINTR);
        const bool timed_out=clock_gettime(CLOCK_MONOTONIC,&now)!=0 ||
            difftime(now.tv_sec,start.tv_sec)*1000.0 +
            ((double)now.tv_nsec-(double)start.tv_nsec)/1000000.0>=TIMEOUT_MS;
        if (timed_out) {
            (void)kill(-pid,SIGKILL);
            (void)kill(pid,SIGKILL);
            while (waitpid(pid,&status,0)<0 && errno==EINTR) {}
            fprintf(stderr,"%s: child timed out or monotonic clock failed\n",args[0]);
            CHECK(false);
        }
        (void)nanosleep(&pause,NULL);
    }
    CHECK(WIFEXITED(status));
    return WEXITSTATUS(status);
}
#endif

static void run(const char *const args[], int expected) {
    const int actual=spawn(args);
    if (actual!=expected) {
        const Bytes diagnostic=read_bytes(err);
        fprintf(stderr,"%s: expected exit %d, got %d\n",args[0],expected,actual);
        (void)fwrite(diagnostic.data,1,diagnostic.length,stderr);
        free(diagnostic.data);
        CHECK(false);
    }
}
static void remove_native(void) {
#ifdef _WIN32
    /* A completed image can briefly remain open on Windows. Retry only
     * access/sharing failures, then report the exact host status. */
    DWORD error=0;
    for (unsigned i=0;i<20;++i) {
        if (DeleteFileA(native)) return;
        error=GetLastError();
        if (error!=ERROR_SHARING_VIOLATION && error!=ERROR_ACCESS_DENIED) break;
        Sleep(25);
    }
    fprintf(stderr,"delete native output: Windows error %lu\n",(unsigned long)error);
    CHECK(false);
#else
    CHECK(remove(native)==0);
#endif
}
static void build(const char *compiler, const char *cc, const char *root,
                  const char *source, const char *module_root, bool sanitize, int expected) {
    const char *args[]={compiler,"build",source,"-o",native,"--cc",cc,
                        "--runtime-root",root,"--root",module_root,sanitize ? "--sanitize" : NULL,NULL};
    run(args,expected);
    expect(out,"",0);
}
static void interpreter(const char *compiler, const char *source, const char *module_root,
                        bool with_input, int expected) {
    const char *args[]={compiler,"run",source,"--root",module_root,"--diagnostics-json",
                        with_input ? "--stdin-file" : NULL,with_input ? input : NULL,NULL};
    run(args,expected);
}
static void contains(const char *path, const char *fragment) {
    const Bytes bytes=read_bytes(path);
    CHECK(memchr(bytes.data,0,bytes.length)==NULL);
    if (strstr(bytes.data,fragment)==NULL) {
        fprintf(stderr,"missing %s in %s:\n%s",fragment,path,bytes.data);
        CHECK(false);
    }
    free(bytes.data);
}

/* Translate the JSON renderer's quoted source bytes into native launcher
 * escapes. Only the small escape subset actually emitted for path bytes is
 * accepted. This lets us require the same original file, not just the same
 * byte offsets in two possibly different files. */
static unsigned int hex(char c) {
    const char *digits="0123456789abcdef";
    const char *p=c=='\0' ? NULL : strchr(digits,c);
    CHECK(p!=NULL);
    return (unsigned int)(p-digits);
}
static const char *path_from_json(const char *json, char path[static PATH_CAP]) {
    size_t n=0;
    while (*json!='"') {
        CHECK(*json!='\0' && n<PATH_CAP-1);
        unsigned char c=(unsigned char)*json++;
        if (c=='\\') {
            c=(unsigned char)*json++;
            if (c=='u') {
                CHECK(strncmp(json,"00",2)==0);
                c=(unsigned char)(16*hex(json[2])+hex(json[3]));
                json+=4;
            } else CHECK(c=='\\' || c=='"');
        }
        CHECK(c!=0);
        path[n++]=(char)c;
    }
    path[n]='\0';
    return json+1;
}
typedef struct { size_t line, column; } Location;
static Location location(const char *source, size_t end) {
    Location loc={1,1};
    for (size_t i=0;i<end;++i) {
        const unsigned char c=(unsigned char)source[i];
        if (c=='\n') { ++loc.line; loc.column=1; }
        else if ((c&0xc0u)!=0x80u) ++loc.column;
    }
    return loc;
}
static void fault_report(const Case *test, char expected[static BYTE_CAP]) {
    const char *file=test->origin_file==NULL ? test->source : test->origin_file;
    const char *leaf=strrchr(file,'/');
    CHECK(leaf!=NULL);
    ++leaf;
    const Bytes source=read_bytes(file), json=read_bytes(err);
    CHECK(memchr(source.data,0,source.length)==NULL && memchr(json.data,0,json.length)==NULL);
    const char *origin=strstr(source.data,test->context);
    CHECK(origin!=NULL && strstr(origin+1,test->context)==NULL);
    CHECK(test->origin_offset+test->origin_size<=strlen(test->context));
    const size_t start=(size_t)(origin-source.data)+test->origin_offset;
    const size_t end=start+test->origin_size;
    const Location a=location(source.data,start), b=location(source.data,end);
    int n=snprintf(expected,BYTE_CAP,"{\"schema_version\":2,\"diagnostics\":[{\"code\":\"%s\",\"severity\":\"error\",",
                   test->code);
    CHECK(n>0 && (size_t)n<BYTE_CAP && strncmp(json.data,expected,(size_t)n)==0);
    const char label[]=",\"labels\":[{\"primary\":true,\"message\":\"\",\"span\":{\"source\":\"";
    const char *primary=strstr(json.data,label);
    CHECK(primary!=NULL);
    char path[PATH_CAP];
    const char *span=path_from_json(primary+sizeof label-1,path);
    const char *base=path;
    for (const char *p=path;*p!='\0';++p) if (*p=='/' || *p=='\\') base=p+1;
    CHECK(strcmp(base,leaf)==0);
    n=snprintf(expected,BYTE_CAP,
        ",\"start_byte\":%zu,\"end_byte\":%zu,\"start\":{\"line\":%zu,\"column\":%zu,",
        start,end,a.line,a.column);
    CHECK(n>0 && (size_t)n<BYTE_CAP && strncmp(span,expected,(size_t)n)==0);
    const char *end_field=strstr(span+(size_t)n,"},\"end\":");
    CHECK(end_field!=NULL);
    n=snprintf(expected,BYTE_CAP,"},\"end\":{\"line\":%zu,\"column\":%zu,",b.line,b.column);
    CHECK(n>0 && (size_t)n<BYTE_CAP && strncmp(end_field,expected,(size_t)n)==0);
    const char *text=strstr(end_field+(size_t)n,"},\"source_text\":");
    CHECK(text!=NULL);
    n=snprintf(expected,BYTE_CAP,"},\"source_text\":\"%.*s\",\"raw\":false}}",
               (int)test->origin_size,source.data+start);
    CHECK(n>0 && (size_t)n<BYTE_CAP && strncmp(text,expected,(size_t)n)==0);

    static const char *const messages[]={NULL,"arithmetic overflow or nonfinite result",
        "division or remainder by zero","execution resource limit exceeded",NULL,
        "negative exponent for Int power","not enough input lines"};
    const int code=test->code[3]-'0';
    CHECK(code>0 && (size_t)code<sizeof messages/sizeof *messages && messages[code]!=NULL);
    n=snprintf(expected,BYTE_CAP,"error[%s]: %s\n  --> \"",test->code,messages[code]);
    CHECK(n>0 && (size_t)n<BYTE_CAP);
    size_t used=(size_t)n;
    for (const unsigned char *p=(const unsigned char *)path;*p!=0;++p) {
        if (*p>=32 && *p<=126 && *p!='\\' && *p!='"') expected[used++]=(char)*p;
        else {
            CHECK(snprintf(expected+used,BYTE_CAP-used,"\\x%02x",(unsigned)*p)==4);
            used+=4;
        }
    }
    n=snprintf(expected+used,BYTE_CAP-used,"\":%zu:%zu..%zu:%zu [bytes %zu..%zu)\n",
               a.line,a.column,b.line,b.column,start,end);
    CHECK(n>0 && (size_t)n<BYTE_CAP-used);
    free(json.data); free(source.data);
}

static void native_fault(const char *expected) {
    /* Exact status and primary diagnostic; recursive cases can have a much
     * longer native stack trace than the reference diagnostic. */
    FILE *f=fopen(err,"rb");
    CHECK(f!=NULL);
    for (size_t i=0;expected[i]!='\0';++i) {
        if (fgetc(f)!=(unsigned char)expected[i]) {
            fprintf(stderr,"native fault differs at byte %zu; expected prefix:\n%s",i,expected);
            CHECK(false);
        }
    }
    CHECK(fclose(f)==0);
}

static void traced_case(const char *compiler,const Case *test,const char *module_root);
static void differential(const char *compiler, const char *cc, const char *root, bool sanitize) {
    char previous[PATH_CAP]="";
    for (size_t i=0;i<sizeof cases/sizeof *cases;++i) {
        Case invocation=cases[i];
        const Case *test=&invocation;
        char staged[PATH_CAP];
        current=cases[i].source;
        if (test->staged_library) {
            stage_library(false);
            const char *leaf=strrchr(current,'/');
            CHECK(leaf!=NULL && leaf[1]!='\0');
            join(staged,work,leaf+1);
            const Bytes bytes=read_bytes(current);
            write_bytes(staged,bytes.data,bytes.length); free(bytes.data);
            invocation.source=staged;
        }
        const char *module_root=test->staged_library ? work :
            strncmp(current,"examples/",sizeof "examples/"-1)==0 ? "examples" :
            strncmp(current,M09,sizeof M09-1)==0 ? "tests/fixtures/types" : "tests/fixtures/differential";
        if (strcmp(previous,current)!=0) {
            if (*previous!='\0') remove_native();
            build(compiler,cc,root,test->source,module_root,sanitize,0);
            expect(err,"",0);
            CHECK(strlen(current)<sizeof previous);
            (void)strcpy(previous,current);
        }
        const bool with_input=test->stdin_bytes!=NULL;
        if (with_input) write_bytes(input,test->stdin_bytes,test->stdin_size);
        const int status=test->bad_input ? 2 : test->code!=NULL ? 3 : 0;
        interpreter(compiler,test->source,module_root,with_input,status);
        expect(out,test->output,test->output_size);
        char *fault=malloc(BYTE_CAP);
        CHECK(fault!=NULL);
        if (test->bad_input) contains(err,"input snapshot is not valid UTF-8");
        else if (test->code!=NULL) fault_report(test,fault);
        else expect(err,LF_OK,sizeof LF_OK-1);
        const char *args[]={native,with_input ? "--stdin-file" : NULL,with_input ? input : NULL,NULL};
        run(args,status);
        expect(out,test->output,test->output_size);
        if (test->bad_input) contains(err,"input snapshot is not valid UTF-8");
        else if (test->code!=NULL) native_fault(fault);
        else expect(err,"",0);
        if (!test->bad_input) traced_case(compiler,test,module_root);
        free(fault);
        if (test->staged_library) CHECK(remove(staged)==0);
    }
    remove_native();
}

static void traced_case(const char *compiler,const Case *test,const char *module_root) {
    ++traced_execution_pairs;
    char reference[PATH_CAP],compiled[PATH_CAP],ref_map[PATH_CAP],native_map[PATH_CAP];
    join(reference,work,"reference.trace"); join(compiled,work,"native.trace");
    join(ref_map,work,"reference.trace.origins"); join(native_map,work,"native.trace.origins");
    const bool with_input=test->stdin_bytes!=NULL;
    const int status=test->code ? 3 : 0;
    const char *ref[]={compiler,"run",test->source,"--root",module_root,"--diagnostics-json",
                       "--trace",reference,with_input ? "--stdin-file" : NULL,with_input ? input : NULL,NULL};
    run(ref,status); expect(out,test->output,test->output_size);
    char *fault=malloc(BYTE_CAP); CHECK(fault);
    if (test->code) fault_report(test,fault); else expect(err,LF_OK,sizeof LF_OK-1);
    const Bytes trace=read_bytes(reference),map=read_bytes(ref_map);
    const char *args[]={native,"--trace",compiled,with_input ? "--stdin-file" : NULL,with_input ? input : NULL,NULL};
    run(args,status); expect(out,test->output,test->output_size);
    if (test->code) native_fault(fault); else expect(err,"",0);
    /* The engines have independent resource budgets and call limits.
     * Their R003 outcomes/spans are tested above; event counts are not equal
     * when either reaches its implementation-specific limit. */
    if (!test->code || strcmp(test->code,"R003")!=0) expect(compiled,trace.data,trace.length);
    expect(native_map,map.data,map.length);
    free(fault); free(trace.data); free(map.data);
    const char *replay[]={compiler,"replay",reference,NULL};
    run(replay,0); contains(out,"Trace:"); contains(out,"Companion source map"); expect(err,"",0);
    CHECK(remove(reference)==0 && remove(compiled)==0 && remove(ref_map)==0 && remove(native_map)==0);
}

static void absent(const char *path) {
    FILE *f=fopen(path,"rb");
    CHECK(f==NULL && errno==ENOENT);
}
static void bad_builds(const char *compiler, const char *cc, const char *root,
                       const char *self, bool sanitize) {
    static const struct { const char *source,*module_root,*code,*needle; } invalid[]={
        {FIX "entry-only.tfd","tests/fixtures/differential","\"code\":\"E208\"","error[E208]"},
        {"examples/showcase/models/shelf.tfd","examples","\"code\":\"E208\"","error[E208]"},
        {"examples/invalid/candidate_cycle.tfd","examples","\"code\":\"E404\"","error[E404]"}
    };
    static const char stale[]="previous executable\0\xff";
    for (size_t i=0;i<sizeof invalid/sizeof *invalid;++i) {
        current=invalid[i].source;
        interpreter(compiler,current,invalid[i].module_root,false,1);
        expect(out,"",0); contains(err,invalid[i].code);
        build(compiler,cc,root,current,invalid[i].module_root,sanitize,1);
        contains(err,invalid[i].needle); absent(native);
        write_bytes(native,stale,sizeof stale-1);
        build(compiler,cc,root,current,invalid[i].module_root,sanitize,1);
        contains(err,invalid[i].needle); expect(native,stale,sizeof stale-1);
        remove_native();
    }
    current="failed downstream C compiler";
    write_bytes(native,stale,sizeof stale-1);
    build(compiler,self,root,"examples/intro/hello.tfd","examples",sanitize,2);
    contains(err,"differential fake compiler wrote a partial executable");
    expect(native,stale,sizeof stale-1);
    remove_native();
}

static void source_alias(const char *compiler, const char *cc, const char *root, bool sanitize) {
    static const char program[]="pub fn main() -> List[Text] { [\"keep this source\"] }\n";
    char source[PATH_CAP];
    join(source,work,"valid & alias.tfd");
    current="output aliases valid source";
    write_bytes(source,program,sizeof program-1);
    interpreter(compiler,source,work,false,0);
    expect(out,"keep this source\n",sizeof "keep this source\n"-1);
    expect(err,LF_OK,sizeof LF_OK-1);
    const char *emit[]={compiler,"emit-c",source,"-o",source,"--root",work,NULL};
    const char *compile[]={compiler,"build",source,"-o",source,"--root",work,"--cc",cc,
                           "--runtime-root",root,sanitize ? "--sanitize" : NULL,NULL};
    run(emit,2);
    expect(out,"",0); contains(err,"aliases a Ternfold source");
    expect(source,program,sizeof program-1);
    run(compile,2);
    expect(out,"",0); contains(err,"aliases a Ternfold source");
    expect(source,program,sizeof program-1);
    CHECK(remove(source)==0);
}

static void library_rejections(const char *compiler,const char *cc,const char *root,bool sanitize) {
    static const struct { const char *file,*json,*text; } invalid[]={
        {"invalid-lines-type.tfd","\"code\":\"E302\"","error[E302]"},
        {"invalid-sum-type.tfd","\"code\":\"E302\"","error[E302]"},
        {"invalid-private-type.tfd","\"code\":\"E206\"","error[E206]"}
    };
    for (size_t i=0;i<sizeof invalid/sizeof *invalid;++i) {
        char original[PATH_CAP],copy[PATH_CAP];
        current=invalid[i].file;
        join(original,LIB,invalid[i].file); join(copy,work,invalid[i].file);
        const Bytes bytes=read_bytes(original);
        write_bytes(copy,bytes.data,bytes.length); free(bytes.data);
        const char *const check[]={compiler,"check",copy,"--root",work,"--diagnostics-json",NULL};
        run(check,1); expect(out,"",0); contains(err,invalid[i].json);
        build(compiler,cc,root,copy,work,sanitize,1); contains(err,invalid[i].text); absent(native);
        CHECK(remove(copy)==0);
    }
}

/* Probe actual stopping points. A later command must reject the same source
 * without exposing a partial dump, result, or output file. */
static void require_diagnostic(const char *code,const char *spelling,size_t start) {
    const Bytes bytes=read_bytes(err);
    Json json={0};
    json.t=calloc(JSON_TOKENS,sizeof *json.t);
    CHECK(json.t && jparse(&json,bytes.data,bytes.length));
    CHECK(jnumber(&json,jfield(&json,0,"schema_version"))==2);
    CHECK(jnumber(&json,jfield(&json,0,"suppressed"))==0);
    const size_t errors=jfield(&json,0,"has_errors");
    CHECK(errors<json.count && json.t[errors].type=='t');
    const size_t list=jfield(&json,0,"diagnostics");
    CHECK(list+1<json.count && json.t[list].type=='[' &&
          json.t[list+1].next==json.t[list].next);
    CHECK(jequal(&json,jfield(&json,list+1,"code"),code));
    CHECK(jequal(&json,jfield(&json,list+1,"severity"),"error"));
    const size_t labels=jfield(&json,list+1,"labels");
    CHECK(labels+1<json.count && json.t[labels].type=='[');
    size_t primary_count=0;
    for (size_t t=labels+1;t<json.t[labels].next;t=json.t[t].next) {
        const size_t primary=jfield(&json,t,"primary");
        CHECK(primary<json.count && (json.t[primary].type=='t' || json.t[primary].type=='f'));
        if (json.t[primary].type!='t') continue;
        ++primary_count;
        const size_t span=jfield(&json,t,"span");
        CHECK(jnumber(&json,jfield(&json,span,"start_byte"))==start);
        CHECK(jnumber(&json,jfield(&json,span,"end_byte"))==start+strlen(spelling));
        CHECK(jequal(&json,jfield(&json,span,"source_text"),spelling));
    }
    CHECK(primary_count==1);
    free(json.t);
    free(bytes.data);
}

static void stage_boundaries(const char *compiler,const char *sentinel,const char *root) {
    static const struct { const char *input, *code, *earlier, *spelling; size_t start; } rows[]={
        {"pub fn main() -> List[Text] { [@] }","E101",NULL,"@",31},
        {"pub fn main() -> List[Text] { [\"x\" }","E110","tokens","",35},
        {"pub fn main() -> List[Text] { [missing] }","E203","ast","missing",31},
        {"pub fn main() -> List[Text] { [true] }","E302","ast","true",31},
        {NULL,"E404","ast","next.a",133}
    };
    static const char *const later[]={"check","plan","core","run","emit-c","build"};
    static const char old[]="existing output\0must survive";
    char source[PATH_CAP], emitted[PATH_CAP], relative[PATH_CAP], witness[PATH_CAP];
    join(source,work,"boundary.tfd");
    join(emitted,work,"should-not-publish.c");
    join(relative,work,"downstream-compiler-invoked");
    origin_path(relative,witness);
    current="negative stage compiler sentinel positive control";
#ifdef _WIN32
    CHECK(SetEnvironmentVariableA("TF_TEST_CC_WITNESS",witness)!=0);
#else
    CHECK(setenv("TF_TEST_CC_WITNESS",witness,1)==0);
#endif
    const char *control[]={sentinel,"-std=c17","sentinel control",NULL};
    run(control,43); expect(witness,"called",6); CHECK(remove(witness)==0);
    for (size_t i=0;i<sizeof rows/sizeof *rows;++i) {
        const bool cycle=rows[i].input==NULL;
        current=rows[i].code;
        const char *file=cycle ? "examples/invalid/candidate_cycle.tfd" : source;
        const char *module_root=cycle ? "examples" : work;
        if (!cycle) write_bytes(source,rows[i].input,strlen(rows[i].input));
        const char *earlier=rows[i].earlier;
        if (earlier) {
            const char *args[]={compiler,earlier,file,"--diagnostics-json",NULL};
            run(args,0);
            const Bytes dump=read_bytes(out);
            CHECK(dump.length>0);
            free(dump.data); expect(err,LF_OK,sizeof LF_OK-1);
        }
        for (size_t j=0;j<sizeof later/sizeof *later;++j) {
            const bool builds=strcmp(later[j],"build")==0, emits=strcmp(later[j],"emit-c")==0;
            const char *target=builds ? native : emits ? emitted : NULL;
            const char *args[]={compiler,later[j],file,"--root",module_root,"--diagnostics-json",
                                target ? "-o" : NULL,target,
                                builds ? "--cc" : NULL,sentinel,"--runtime-root",root,NULL};
            run(args,1); expect(out,"",0);
            require_diagnostic(rows[i].code,rows[i].spelling,rows[i].start);
            absent(witness);
            if (target) {
                absent(target); write_bytes(target,old,sizeof old-1);
                run(args,1); expect(out,"",0); expect(target,old,sizeof old-1);
                require_diagnostic(rows[i].code,rows[i].spelling,rows[i].start);
                absent(witness);
                CHECK(remove(target)==0);
            }
        }
    }
#ifdef _WIN32
    CHECK(SetEnvironmentVariableA("TF_TEST_CC_WITNESS",NULL)!=0);
#else
    CHECK(unsetenv("TF_TEST_CC_WITNESS")==0);
#endif
    CHECK(remove(source)==0);
    printf("stage boundaries: 5 exact diagnostic sets, 40 blocked later commands\n");
}

static void perturb_environment(unsigned attempt) {
    const char *locale=attempt==1 ? "C" : "POSIX";
    const char *zone=attempt==1 ? "UTC0" : "EST5";
    /* On libc implementations that support it, alter fresh/freed heap fill
     * bytes. Child processes allocate independently even without this knob. */
    const char *heap=attempt==1 ? "19" : "187";
#ifdef _WIN32
    CHECK(SetEnvironmentVariableA("LC_ALL",locale)!=0);
    CHECK(SetEnvironmentVariableA("TZ",zone)!=0);
    CHECK(SetEnvironmentVariableA("MALLOC_PERTURB_",heap)!=0);
#else
    CHECK(setenv("LC_ALL",locale,1)==0);
    CHECK(setenv("TZ",zone,1)==0);
    CHECK(setenv("MALLOC_PERTURB_",heap,1)==0);
#endif
}

static void reproducible_pipeline(const char *compiler,const char *cc,
                                  const char *root,bool sanitize) {
    static const char *const stages[]={"tokens","ast","plan","core","emit-c"};
    static const char source[]="examples/course/precedence_bindings.tfd";
    static const char expected[]="8\n";
    for (size_t i=0;i<sizeof stages/sizeof *stages;++i) {
        current=stages[i];
        const bool tokens=i==0, ast=i==1;
        const char *args[]={compiler,current,source,"--diagnostics-json",
                            tokens || ast ? "--json" : "--root",tokens || ast ? NULL : "examples",NULL};
        perturb_environment(1);
        run(args,0); expect(err,LF_OK,sizeof LF_OK-1);
        const Bytes baseline=read_bytes(out); CHECK(baseline.length!=0);
        perturb_environment(2);
        run(args,0); expect(err,LF_OK,sizeof LF_OK-1);
        expect(out,baseline.data,baseline.length);
        free(baseline.data);
    }
    current="repeat native build in independent private stages";
    char entry[PATH_CAP];
    join(entry,work,"repeat.tfd");
    static const char program[]="pub fn main() -> List[Text] { [int_text(2 ^ 3)] }\n";
    write_bytes(entry,program,sizeof program-1);
    for (unsigned i=1;i<=2;++i) {
        perturb_environment(i);
        build(compiler,cc,root,entry,work,sanitize,0); expect(err,"",0);
        const char *args[]={native,NULL};
        run(args,0); expect(out,expected,sizeof expected-1); expect(err,"",0);
        remove_native();
    }
    CHECK(remove(entry)==0);
    printf("determinism: 5 byte-identical dumps and 2 independently built native runs\n");
}

static size_t lsp_frames(const Bytes bytes) {
    size_t at=0,frames=0;
    static const char header[]="Content-Length: ";
    while (at<bytes.length) {
        CHECK(bytes.length-at>=sizeof header-1 &&
              memcmp(bytes.data+at,header,sizeof header-1)==0);
        at+=sizeof header-1;
        size_t length=0,digits=0;
        while (at<bytes.length && bytes.data[at]>='0' && bytes.data[at]<='9') {
            const size_t digit=(size_t)(bytes.data[at++]-'0');
            CHECK(length<=(BYTE_CAP-digit)/10);
            length=length*10+digit; ++digits;
        }
        CHECK(digits && bytes.length-at>=4 && memcmp(bytes.data+at,"\r\n\r\n",4)==0);
        at+=4; CHECK(length<=bytes.length-at);
        Json json={0}; json.t=calloc(JSON_TOKENS,sizeof *json.t);
        CHECK(json.t && jparse(&json,bytes.data+at,length));
        CHECK(jequal(&json,jfield(&json,0,"jsonrpc"),"2.0"));
        free(json.t); at+=length; ++frames;
    }
    return frames;
}
static void tooling_cli(const char *compiler) {
    current="formatter/editor binary integration";
    char source[PATH_CAP],canonical[PATH_CAP];
    join(source,work,"format source.tfd"); join(canonical,work,"canonical.tfd");
    static const char original[]="pub fn main()->List[Text]{/*keep*/[\"value\"]}";
    write_bytes(source,original,sizeof original-1);
    const char *fmt[]={compiler,"fmt",source,NULL};
    run(fmt,0); expect(err,"",0); contains(out,"/*keep*/");
    const Bytes formatted=read_bytes(out); write_bytes(canonical,formatted.data,formatted.length);
    const char *again[]={compiler,"fmt",canonical,NULL};
    run(again,0); expect(out,formatted.data,formatted.length); expect(err,"",0);
    free(formatted.data);
    const char *check[]={compiler,"fmt",canonical,"--check",NULL};
    run(check,0); expect(out,"",0); expect(err,"",0);
    const char *diff[]={compiler,"fmt",source,"--check",NULL};
    run(diff,1); expect(out,"",0); contains(err,"differs");
    write_bytes(source,"pub fn bad( -> Int {",sizeof "pub fn bad( -> Int {"-1);
    run(fmt,1); expect(out,"",0); contains(err,"E110");
    CHECK(remove(source)==0 && remove(canonical)==0);

    static const char *const requests[]={
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}",
        ("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{"
        "\"uri\":\"file:///draft.tfd\",\"languageId\":\"ternfold\",\"version\":1,\"text\":\"fn f() -> Int { true }\"}}}"),
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\",\"params\":null}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}"
    };
    FILE *file=fopen(input,"wb"); CHECK(file);
    for (size_t i=0;i<sizeof requests/sizeof *requests;++i) {
        const size_t n=strlen(requests[i]);
        CHECK(fprintf(file,"Content-Length: %zu\r\n\r\n",n)>0);
        CHECK(fwrite(requests[i],1,n,file)==n);
    }
    CHECK(fclose(file)==0);
    const char *lsp[]={compiler,"lsp","--root",work,NULL};
    child_stdin=input;
    run(lsp,0); expect(err,"",0);
    const Bytes response=read_bytes(out);
    CHECK(lsp_frames(response)==3);
    free(response.data);
    contains(out,"\"positionEncoding\":\"utf-16\"");
    contains(out,"textDocument/publishDiagnostics"); contains(out,"E302");
    write_bytes(input,"Content-Length: -1\r\n\r\n",sizeof "Content-Length: -1\r\n\r\n"-1);
    run(lsp,2); expect(out,"",0); contains(err,"editor protocol");
    child_stdin=NULL;
    printf("tooling CLI: canonical format/check, diagnosed syntax and framed editor transcript passed\n");
}

/* A real downstream failure after writing into the compiler's private stage. */
static int fake_cc(int argc, char **argv) {
    const char *witness=getenv("TF_TEST_CC_WITNESS");
    current="sentinel C compiler";
    if (witness) write_bytes(witness,"called",6);
    for (int i=2;i+1<argc;++i) {
        if (strcmp(argv[i],"-o")==0) {
            FILE *f=fopen(argv[i+1],"wb");
            if (f==NULL) return 43;
            const bool wrote=fputs("partial",f)!=EOF;
            if (fclose(f)!=0 || !wrote) return 43;
            if (fputs("differential fake compiler wrote a partial executable\n",stderr)==EOF ||
                fflush(stderr)!=0) return 43;
            return 42;
        }
    }
    return 43;
}
int main(int argc, char **argv) {
    if (argc>2 && strcmp(argv[1],"-std=c17")==0) return fake_cc(argc,argv);
    if ((argc!=4 && argc!=5) || (argc==5 && strcmp(argv[4],"--sanitize")!=0)) {
        fprintf(stderr,"usage: %s COMPILER CC RUNTIME_ROOT [--sanitize]\n",argv[0]);
        return 2;
    }
    current="setup";
    setup();
    differential(argv[1],argv[2],argv[3],argc==5);
    bad_builds(argv[1],argv[2],argv[3],argv[0],argc==5);
    source_alias(argv[1],argv[2],argv[3],argc==5);
    library_rejections(argv[1],argv[2],argv[3],argc==5);
    stage_boundaries(argv[1],argv[0],argv[3]);
    reproducible_pipeline(argv[1],argv[2],argv[3],argc==5);
    tooling_cli(argv[1]);
    current="cleanup";
    stage_library(true);
    CHECK(remove(out)==0); CHECK(remove(err)==0); CHECK(remove(input)==0);
    CHECK(REMOVE_DIR(work)==0);
    printf("differential: %zu execution cases, 4 build rejections, 2 alias checks and 3 library rejections passed\n",
           sizeof cases/sizeof *cases);
    printf("tracing: %zu interpreter/native pairs, exact source maps and replay; R003 event counts remain engine-specific\n",
           traced_execution_pairs);
    return 0;
}
