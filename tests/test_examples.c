/* Run from repository root: test_examples COMPILER CC RUNTIME_ROOT [--sanitize].
 * Standalone C17 suite; platform APIs are confined to processes/directory walks.
 * Budgets: 1 MiB/file or capture, 32768 JSON tokens, depth 32, 256 cases/files,
 * 1024 directory entries, directory depth 16, 60 seconds/child. No phase skips.
 * Exit expectations: success 0, static/lexical diagnostic 1, runtime fault 3.
 * Captures remain under tmp/m12-manifest for inspection. */
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
#define EXE ".exe"
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define MAKE_DIR(p) mkdir(p, 0700)
#define EXE ""
#endif

enum { PATH_CAP = 4096, BYTE_CAP = 1024 * 1024, TIMEOUT_MS = 60000 };
static const char scratch[] = "tmp/m12-manifest";
static const char *current;
static char work[PATH_CAP], out[PATH_CAP], err[PATH_CAP], input[PATH_CAP], native[PATH_CAP];

#define CHECK(ok) do { if (!(ok)) { \
    fprintf(stderr,"examples:%d: %s (%s); captures in %s\n",__LINE__,#ok,current,work); \
    exit(1); \
} } while (0)

typedef struct { char *data; size_t length; } Bytes;
#include "fixture_json.h"
static void join(char *path, const char *directory, const char *leaf) {
    const int n=snprintf(path,PATH_CAP,"%s/%s",directory,leaf);
    CHECK(n>0 && (size_t)n<PATH_CAP);
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
    si.hStdInput=stream("NUL",true);
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
        const int descriptors[]={open("/dev/null",O_RDONLY),
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

enum { CASE_CAP=256, ENTRY_CAP=1024, DIRECTORY_DEPTH=16 };
typedef struct {
    Bytes path, mode, stdout_bytes, stdin_bytes, diagnostic;
    bool has_stdout, has_stdin, found;
    int status;
} Example;
static Example examples[CASE_CAP];
static size_t example_count, file_count, entry_count, coverage_failures, executed;

static Json *new_json(void) {
    Json *j=calloc(1,sizeof *j); CHECK(j!=NULL);
    j->t=calloc(JSON_TOKENS,sizeof *j->t); CHECK(j->t!=NULL); return j;
}
static void free_json(Json *j) { free(j->t); free(j); }
static Json *document(Bytes b) {
    Json *j=new_json();
    if (!jparse(j,b.data,b.length)) {
        fprintf(stderr,"examples: malformed JSON or budget exceeded at byte %zu/%zu, token %zu/%d\n",
            j->at,b.length,j->count,JSON_TOKENS);
        CHECK(false);
    }
    return j;
}
static Bytes string_field(const Json *j, size_t object, const char *key, bool required) {
    const size_t id=jfield(j,object,key);
    if (id==j->count) { CHECK(!required); return (Bytes){NULL,0}; }
    return jbytes(j,id);
}
static bool text_is(Bytes b, const char *s) {
    return b.length==strlen(s) && memcmp(b.data,s,b.length)==0;
}
static void cstring(Bytes b) { CHECK(b.length>0 && memchr(b.data,0,b.length)==NULL); }
static void path_valid(Bytes b) {
    cstring(b);
    CHECK(b.length>13 && b.length<PATH_CAP && !memcmp(b.data,"examples/",9));
    CHECK(!memcmp(b.data+b.length-4,".tfd",4));
    CHECK(strchr(b.data,'\\')==NULL && strchr(b.data,':')==NULL);
    const char *p=b.data;
    while (*p) {
        const char *end=strchr(p,'/');
        const size_t n=end ? (size_t)(end-p) : strlen(p);
        CHECK(n>0 && !(n==1 && *p=='.') && !(n==2 && !memcmp(p,"..",2)));
        p=end ? end+1 : p+n;
    }
}
static void load_manifest(void) {
    const Bytes data=read_bytes("examples/manifest.json");
    Json *j=document(data);
    CHECK(jnumber(j,jfield(j,0,"schema"))==1);
    const size_t list=jfield(j,0,"cases");
    CHECK(list<j->count && j->t[list].type=='[');
    for (size_t i=list+1;i<j->t[list].next;i=j->t[i].next) {
        CHECK(example_count<CASE_CAP);
        Example *e=&examples[example_count++];
        e->path=string_field(j,i,"path",true); path_valid(e->path); current=e->path.data;
        e->mode=string_field(j,i,"mode",true); cstring(e->mode);
        CHECK(text_is(e->mode,"run") || text_is(e->mode,"check") || text_is(e->mode,"tokens"));
        Bytes ready=string_field(j,i,"ready_after",true); cstring(ready);
        /* Validate the canonical ID family; readiness never skips execution. */
        const bool milestone=ready.length==3 && ready.data[0]=='M' &&
            jdigit(ready.data[1]) && jdigit(ready.data[2]) &&
            (ready.data[1]=='0' || (ready.data[1]=='1' && ready.data[2]<='7'));
        CHECK(milestone || text_is(ready,"C00") || text_is(ready,"C01") ||
              text_is(ready,"S01") || text_is(ready,"X01") || text_is(ready,"C-MIGRATION-01"));
        free(ready.data);
        e->stdout_bytes=string_field(j,i,"stdout",text_is(e->mode,"run"));
        e->stdin_bytes=string_field(j,i,"stdin",false);
        e->diagnostic=string_field(j,i,"diagnostic",false);
        e->has_stdout=e->stdout_bytes.data!=NULL; e->has_stdin=e->stdin_bytes.data!=NULL;
        if (text_is(e->path,"examples/showcase/checkout.tfd")) {
            const Bytes transcript=read_bytes("examples/showcase/checkout.expected.txt");
            CHECK(text_is(e->mode,"run") && e->has_stdout &&
                  transcript.length==e->stdout_bytes.length &&
                  memcmp(transcript.data,e->stdout_bytes.data,transcript.length)==0);
            free(transcript.data);
        }
        if (e->diagnostic.data) {
            cstring(e->diagnostic);
            CHECK(e->diagnostic.length==4);
            CHECK(e->diagnostic.data[0]==(text_is(e->mode,"run") ? 'R' : 'E'));
            for (size_t k=1;k<4;++k) CHECK(jdigit(e->diagnostic.data[k]));
        }
        e->status=e->diagnostic.data ? (e->diagnostic.data[0]=='R' ? 3 : 1) : 0;
        CHECK(!e->has_stdin || text_is(e->mode,"run"));
        for (size_t k=0;k+1<example_count;++k) CHECK(strcmp(examples[k].path.data,e->path.data)!=0);
    }
    CHECK(example_count>0); free_json(j); free(data.data);
}
static void covered(const char *path) {
    const size_t n=strlen(path);
    if (n<4 || strcmp(path+n-4,".tfd")!=0) return;
    CHECK(++file_count<=CASE_CAP);
    size_t matches=0;
    for (size_t i=0;i<example_count;++i) if (!strcmp(path,examples[i].path.data)) {
        ++matches; examples[i].found=true;
    }
    if (matches!=1) {
        fprintf(stderr,"examples: uncovered source %s (manifest cases: %zu; expected exactly one)\n",path,matches);
        ++coverage_failures;
    }
}
static void inventory(const char *directory, unsigned depth) {
    CHECK(depth<=DIRECTORY_DEPTH);
    char path[PATH_CAP];
#ifdef _WIN32
    join(path,directory,"*");
    WIN32_FIND_DATAA info;
    HANDLE handle=FindFirstFileA(path,&info); CHECK(handle!=INVALID_HANDLE_VALUE);
    do {
        if (!strcmp(info.cFileName,".") || !strcmp(info.cFileName,"..")) continue;
        CHECK(++entry_count<=ENTRY_CAP);
        join(path,directory,info.cFileName);
        CHECK(!(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT));
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) inventory(path,depth+1);
        else covered(path);
    } while (FindNextFileA(handle,&info));
    CHECK(GetLastError()==ERROR_NO_MORE_FILES); CHECK(FindClose(handle));
#else
    DIR *dir=opendir(directory); CHECK(dir!=NULL);
    for (;;) {
        errno=0; const struct dirent *entry=readdir(dir);
        if (!entry) { CHECK(errno==0); break; }
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        CHECK(++entry_count<=ENTRY_CAP); join(path,directory,entry->d_name);
        struct stat info; CHECK(lstat(path,&info)==0);
        CHECK(!S_ISLNK(info.st_mode));
        if (S_ISDIR(info.st_mode)) inventory(path,depth+1);
        else { CHECK(S_ISREG(info.st_mode)); covered(path); }
    }
    CHECK(closedir(dir)==0);
#endif
}
static bool jliteral(const Json *j, size_t id, const char *s) {
    CHECK(id<j->count);
    return j->t[id].end-j->t[id].start==strlen(s) &&
        !memcmp(j->s+j->t[id].start,s,strlen(s));
}
typedef struct { size_t line, column; } Position;
static Position position(Bytes source, size_t offset) {
    CHECK(offset<=source.length); Position p={1,1};
    for (size_t i=0;i<offset;++i) {
        const unsigned char c=(unsigned char)source.data[i];
        if (c=='\r') { ++p.line; p.column=1; }
        else if (c=='\n') { if (!i || source.data[i-1]!='\r') ++p.line; p.column=1; }
        else if ((c & 0xc0u)!=0x80u) ++p.column;
    }
    return p;
}
/* Exact native primary fault prefix derived from validated reference JSON;
 * stack frames are executor-specific and are not parity observations. */
static Bytes fault_prefix(const Json *j, size_t diagnostic, const Example *e) {
    const size_t labels=jfield(j,diagnostic,"labels");
    CHECK(labels<j->count && j->t[labels].type=='[');
    size_t primary=j->count;
    for (size_t i=labels+1;i<j->t[labels].next;i=j->t[i].next) {
        if (jliteral(j,jfield(j,i,"primary"),"true")) { CHECK(primary==j->count); primary=i; }
    }
    CHECK(primary<j->count);
    const size_t span=jfield(j,primary,"span");
    Bytes path=string_field(j,span,"source",true); cstring(path);
    Bytes source=read_bytes(path.data);
    const size_t start=jnumber(j,jfield(j,span,"start_byte"));
    const size_t end=jnumber(j,jfield(j,span,"end_byte"));
    CHECK(start<=end && end<=source.length);
    Bytes text=string_field(j,span,"source_text",true);
    CHECK(text.length==end-start && !memcmp(text.data,source.data+start,text.length));
    CHECK(jliteral(j,jfield(j,span,"raw"),"false"));
    const Position a=position(source,start), b=position(source,end);
    const size_t sa=jfield(j,span,"start"), sb=jfield(j,span,"end");
    CHECK(jnumber(j,jfield(j,sa,"line"))==a.line && jnumber(j,jfield(j,sa,"column"))==a.column);
    CHECK(jnumber(j,jfield(j,sb,"line"))==b.line && jnumber(j,jfield(j,sb,"column"))==b.column);
    static const char *const messages[]={NULL,"arithmetic overflow or nonfinite result",
        "division or remainder by zero","execution resource limit exceeded",NULL,
        "negative exponent for Int power","not enough input lines"};
    CHECK(e->diagnostic.data[1]=='0' && e->diagnostic.data[2]=='0');
    const unsigned code=(unsigned)(e->diagnostic.data[3]-'0');
    CHECK(code<sizeof messages/sizeof *messages && messages[code]!=NULL);
    CHECK(jequal(j,jfield(j,diagnostic,"message"),messages[code]));
    Bytes result={malloc(BYTE_CAP),0}; CHECK(result.data!=NULL);
    int n=snprintf(result.data,BYTE_CAP,"error[%s]: %s\n  --> \"",e->diagnostic.data,messages[code]);
    CHECK(n>0 && (size_t)n<BYTE_CAP); result.length=(size_t)n;
    for (size_t i=0;i<path.length;++i) {
        const unsigned char c=(unsigned char)path.data[i]; CHECK(result.length+5<BYTE_CAP);
        if (c>=32 && c<=126 && c!='\\' && c!='"') result.data[result.length++]=(char)c;
        else {
            n=snprintf(result.data+result.length,BYTE_CAP-result.length,"\\x%02x",(unsigned)c);
            CHECK(n==4); result.length+=4;
        }
    }
    n=snprintf(result.data+result.length,BYTE_CAP-result.length,
        "\":%zu:%zu..%zu:%zu [bytes %zu..%zu)\n",a.line,a.column,b.line,b.column,start,end);
    CHECK(n>0 && (size_t)n<BYTE_CAP-result.length); result.length+=(size_t)n;
    free(path.data); free(source.data); free(text.data); return result;
}
static Bytes diagnostics(const Example *e) {
    if (!e->diagnostic.data) {
        const char empty_report[]="{\"schema_version\":2,\"diagnostics\":[],\"suppressed\":0,\"has_errors\":false}\n";
        expect(err,empty_report,sizeof empty_report-1); return (Bytes){NULL,0};
    }
    Bytes data=read_bytes(err); Json *j=document(data);
    CHECK(jnumber(j,jfield(j,0,"schema_version"))==2);
    CHECK(jnumber(j,jfield(j,0,"suppressed"))==0);
    CHECK(jliteral(j,jfield(j,0,"has_errors"),"true"));
    const size_t list=jfield(j,0,"diagnostics");
    CHECK(list<j->count && j->t[list].type=='[');
    size_t count=0; Bytes prefix={NULL,0};
    for (size_t i=list+1;i<j->t[list].next;i=j->t[i].next) {
        /* The singular manifest diagnostic is the primary/first error.
         * Token recovery can emit other error codes later in the source. */
        const size_t code=jfield(j,i,"code");
        if (count==0 || !text_is(e->mode,"tokens")) CHECK(jequal(j,code,e->diagnostic.data));
        else {
            Bytes later=jbytes(j,code);
            CHECK(later.length==4 && later.data[0]=='E' && jdigit(later.data[1]) &&
                jdigit(later.data[2]) && jdigit(later.data[3])); free(later.data);
        }
        CHECK(jequal(j,jfield(j,i,"severity"),"error")); ++count;
        if (e->diagnostic.data[0]=='R') { CHECK(count==1); prefix=fault_prefix(j,i,e); }
    }
    CHECK(count>0);
    if (!text_is(e->mode,"tokens")) CHECK(count==1);
    free_json(j); free(data.data); return prefix;
}
static void token_output(const Example *e) {
    const Bytes data=read_bytes(out), source=read_bytes(e->path.data);
    Json *j=document(data);
    CHECK(jnumber(j,jfield(j,0,"schema_version"))==1);
    CHECK(jliteral(j,jfield(j,0,"source_valid"),"true"));
    CHECK(jliteral(j,jfield(j,0,"has_errors"),e->diagnostic.data ? "true" : "false"));
    const size_t list=jfield(j,0,"tokens");
    CHECK(list<j->count && j->t[list].type=='[');
    size_t count=0, eof=0, previous=0, invalid=0;
    for (size_t i=list+1;i<j->t[list].next;i=j->t[i].next) {
        ++count;
        const size_t start=jnumber(j,jfield(j,i,"start_byte"));
        const size_t end=jnumber(j,jfield(j,i,"end_byte"));
        CHECK(start>=previous && end>=start && end<=source.length); previous=end;
        Bytes lexeme=string_field(j,i,"lexeme",true);
        CHECK(lexeme.length==end-start && !memcmp(lexeme.data,source.data+start,lexeme.length));
        free(lexeme.data);
        Bytes kind=string_field(j,i,"kind",true); cstring(kind);
        if (text_is(kind,"EOF")) {
            ++eof; CHECK(start==source.length && end==source.length && j->t[i].next==j->t[list].next);
        } else CHECK(end>start);
        if (text_is(kind,"INVALID")) ++invalid;
        free(kind.data);
    }
    CHECK(count>1 && eof==1);
    CHECK(e->diagnostic.data ? invalid>0 : invalid==0);
    if (e->has_stdout) expect(out,e->stdout_bytes.data,e->stdout_bytes.length);
    free_json(j); free(data.data); free(source.data);
}
static void execute(const char *compiler, const char *cc, const char *runtime_root, bool sanitize) {
    for (size_t i=0;i<example_count;++i) {
        const Example *e=&examples[i]; current=e->path.data;
        if (!e->found) {
            fprintf(stderr,"examples: manifest source does not exist in inventory: %s\n",current);
            ++coverage_failures; continue;
        }
        const Bytes source=read_bytes(current); free(source.data);
        printf("manifest: %s %s\n",e->mode.data,current); CHECK(fflush(stdout)==0);
        if (e->has_stdin) write_bytes(input,e->stdin_bytes.data,e->stdin_bytes.length);
        if (text_is(e->mode,"run")) interpreter(compiler,current,"examples",e->has_stdin,e->status);
        else if (text_is(e->mode,"tokens")) {
            const char *args[]={compiler,"tokens",current,"--json","--diagnostics-json",NULL}; run(args,e->status);
        } else {
            const char *args[]={compiler,"check",current,"--root","examples","--diagnostics-json",NULL}; run(args,e->status);
        }
        if (text_is(e->mode,"tokens")) token_output(e);
        else if (e->has_stdout) expect(out,e->stdout_bytes.data,e->stdout_bytes.length);
        else if (e->status==0) {
            const char check_success[]="Frontend checks succeeded.\n";
            expect(out,check_success,sizeof check_success-1);
        } else expect(out,"",0);
        Bytes fault=diagnostics(e);
        if (text_is(e->mode,"run")) {
            build(compiler,cc,runtime_root,current,"examples",sanitize,0); expect(err,"",0);
            const char *args[]={native,e->has_stdin ? "--stdin-file" : NULL,e->has_stdin ? input : NULL,NULL};
            run(args,e->status); expect(out,e->stdout_bytes.data,e->stdout_bytes.length);
            if (fault.data) {
                const Bytes actual=read_bytes(err);
                CHECK(actual.length>=fault.length && !memcmp(actual.data,fault.data,fault.length)); free(actual.data);
            } else expect(err,"",0);
            if (remove(native)!=0) { perror(native); CHECK(false); }
        }
        free(fault.data); ++executed;
    }
}
static void json_regressions(void) {
    static const char *const bad[]={"", "\"", "\"\\", "\"\\u", "\"\\u123", "[", "[1,]",
        "{\"a\":}", "00", "1e", "true x", "\"\\uD800\"", "\"\\uDC00\"", "\"\xc0\x80\""};
    Json *j=new_json();
    for (size_t i=0;i<sizeof bad/sizeof *bad;++i) CHECK(!jparse(j,bad[i],strlen(bad[i])));
    const char good[]="[\"a\\u0000b\",\"\\uD83D\\uDE42\",\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"]";
    CHECK(jparse(j,good,sizeof good-1));
    Bytes b=jbytes(j,1); CHECK(b.length==3 && !memcmp(b.data,"a\0b",3)); free(b.data);
    b=jbytes(j,2); CHECK(b.length==4 && !memcmp(b.data,"\xf0\x9f\x99\x82",4)); free(b.data);
    b=jbytes(j,3); CHECK(b.length==8 && !memcmp(b.data,"\"\\/\b\f\n\r\t",8)); free(b.data);
    const char nul[]={'[','0',']',0}; CHECK(!jparse(j,nul,sizeof nul));
    char deep[2*(JSON_DEPTH+2)];
    memset(deep,'[',sizeof deep/2); memset(deep+sizeof deep/2,']',sizeof deep/2);
    CHECK(!jparse(j,deep,sizeof deep));
    CHECK(!jparse(j,"[]",JSON_BYTES+1u));
    free_json(j);
}
int main(int argc, char **argv) {
    current="manifest setup";
    CHECK(argc==4 || (argc==5 && !strcmp(argv[4],"--sanitize")));
    setup(); json_regressions(); load_manifest(); inventory("examples",0);
    execute(argv[1],argv[2],argv[3],argc==5);
    current="manifest inventory";
    printf("manifest: executed %zu/%zu cases; inventoried %zu sources; %zu coverage failures; captures %s\n",
        executed,example_count,file_count,coverage_failures,work);
    for (size_t i=0;i<example_count;++i) {
        free(examples[i].path.data); free(examples[i].mode.data); free(examples[i].stdout_bytes.data);
        free(examples[i].stdin_bytes.data); free(examples[i].diagnostic.data);
    }
    return coverage_failures ? 1 : 0;
}
