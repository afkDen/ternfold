#include "driver_native.h"
#include "ternfold/native_build.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
typedef BY_HANDLE_FILE_INFORMATION FileId;
static bool file_id(const char *path, FileId *id, bool *missing) {
    HANDLE h=CreateFileA(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    *missing=h==INVALID_HANDLE_VALUE && GetLastError()==ERROR_FILE_NOT_FOUND;
    if (h==INVALID_HANDLE_VALUE) return false;
    const bool ok=GetFileInformationByHandle(h,id)!=0;
    return CloseHandle(h)!=0 && ok && (id->dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==0;
}
static bool same_file(const FileId *a, const FileId *b) {
    return a->dwVolumeSerialNumber==b->dwVolumeSerialNumber &&
           a->nFileIndexHigh==b->nFileIndexHigh && a->nFileIndexLow==b->nFileIndexLow;
}
static bool same_origin(const FileId *file,TfFileIdentity origin) {
    return origin.valid && origin.device==(uint64_t)file->dwVolumeSerialNumber &&
           origin.index==(((uint64_t)file->nFileIndexHigh<<32)|(uint64_t)file->nFileIndexLow);
}
static char *native_utf8(const char *text) {
    const int n=MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,text,-1,NULL,0);
    if (n==0 || (size_t)n>SIZE_MAX/sizeof(wchar_t)) return NULL;
    wchar_t *wide=malloc((size_t)n*sizeof *wide);
    if (wide==NULL) return NULL;
    char *out=NULL;
    if (MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,text,-1,wide,n)==n) {
        const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,NULL,0,NULL,NULL);
        if (bytes>0) {
            out=malloc((size_t)bytes);
            if (out!=NULL && WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,out,bytes,NULL,NULL)!=bytes) {
                free(out); out=NULL;
            }
        }
    }
    free(wide);
    return out;
}
#else
typedef struct stat FileId;
static bool file_id(const char *path, FileId *id, bool *missing) {
    const int rc=stat(path,id);
    *missing=rc!=0 && errno==ENOENT;
    return rc==0 && S_ISREG(id->st_mode);
}
static bool same_file(const FileId *a, const FileId *b) {
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino;
}
static bool same_origin(const FileId *file,TfFileIdentity origin) {
    return origin.valid && origin.device==(uint64_t)file->st_dev && origin.index==(uint64_t)file->st_ino;
}
#endif

static bool safe_destination(const TfResolution *resolution, const char *path) {
#ifdef _WIN32
    const size_t n=strlen(path);
    if (n==0 || path[n-1]=='.' || path[n-1]==' ' || strncmp(path,"\\\\?\\",4)==0 ||
        strncmp(path,"\\\\.\\",4)==0) return false;
    for (size_t i=0;i<n;++i)
        if (path[i]==':' && (i!=1 || !((path[0]>='A' && path[0]<='Z') ||
                                       (path[0]>='a' && path[0]<='z')))) return false;
#endif
    FileId output;
    bool missing;
    if (!file_id(path,&output,&missing)) return missing;
    for (size_t i=0;i<tf_resolution_module_count(resolution);++i) {
        const TfResolvedModule *module=tf_resolution_module_at(resolution,i);
        FileId source;
        if (module==NULL || module->source==NULL ||
            same_origin(&output,module->source->source->origin) ||
            !file_id(module->source->canonical_path,&source,&missing) ||
            same_file(&output,&source)) return false;
    }
    return true;
}

/* Exclusive creation keeps old files intact even if a stale temp name is
 * encountered. All paths share the destination's filesystem. */
static FILE *stage(const char *output, char **path) {
    const size_t n=strlen(output);
    if (n>SIZE_MAX-64) return NULL;
    char *name=malloc(n+64);
    if (name==NULL) return NULL;
#ifdef _WIN32
    const unsigned long pid=(unsigned long)GetCurrentProcessId();
#else
    const unsigned long pid=(unsigned long)getpid();
#endif
    memcpy(name,output,n);
    for (unsigned int i=0;i<64;++i) {
        const int count=snprintf(name+n,64,".tf-%lu-%u.c",pid,i);
        if (count<0 || count>=64) break;
        FILE *out=fopen(name,"wbx");
        if (out!=NULL) { *path=name; return out; }
        if (errno!=EEXIST) break;
    }
    free(name);
    return NULL;
}

static bool publish(const char *stage_path, const char *output) {
#ifdef _WIN32
    return MoveFileExA(stage_path,output,MOVEFILE_REPLACE_EXISTING)!=0;
#else
    return rename(stage_path,output)==0;
#endif
}

static const char *build_file(const char *cc, const char *root, const char *source,
                              const char *output, bool sanitize) {
    TfNativeBuildResult result;
    TfNativeBuildOptions options={cc,root,source,output,0,sanitize};
#ifdef _WIN32
    char *paths[]={native_utf8(cc),native_utf8(root),native_utf8(source),native_utf8(output)};
    bool ok=true;
    for (size_t i=0;i<sizeof paths/sizeof *paths;++i) if (paths[i]==NULL) ok=false;
    options.compiler_path=paths[0]; options.runtime_root=paths[1];
    options.source_path=paths[2]; options.output_path=paths[3];
    const TfNativeBuildStatus status=ok ? tf_native_build(&options,&result) : TF_NATIVE_BUILD_INVALID;
    for (size_t i=0;i<sizeof paths/sizeof *paths;++i) free(paths[i]);
    if (!ok) return "cannot convert native build paths to UTF-8";
#else
    const TfNativeBuildStatus status=tf_native_build(&options,&result);
#endif
    if (result.diagnostics_length!=0 &&
        fwrite(result.diagnostics,1,result.diagnostics_length,stderr)!=result.diagnostics_length)
        return "cannot write C compiler diagnostics";
    if ((result.diagnostics_truncated && fputs("\nternfold: C compiler diagnostics truncated\n",stderr)==EOF) ||
        (status!=TF_NATIVE_BUILD_OK && fprintf(stderr,"ternfold: %s\n",result.message)<0) ||
        (result.cleanup_failed && fprintf(stderr,"ternfold: %s\n",result.cleanup_message)<0) ||
        fflush(stderr)!=0) return "cannot write C compiler diagnostics";
    return status==TF_NATIVE_BUILD_OK ? NULL : "native build failed";
}

const char *tf_driver_native(const TfVerifiedCore *core, const TfResolution *resolution,
                             const TfDriverNativeOptions *options, bool build) {
    if (!build && options->output==NULL)
        return tf_c_emit_optimized(stdout,core,options->optimization)==TF_OK && fflush(stdout)==0 ? NULL : "cannot emit C17";
    const char *cc=options->compiler;
    if (build && cc==NULL) cc=getenv("TF_CC");
    if (build && (cc==NULL || *cc=='\0')) return "build requires --cc PATH or TF_CC";
    if (!safe_destination(resolution,options->output))
        return "cannot use output destination (unqueryable, nonregular or aliases a Ternfold source)";
    char *path=NULL;
    FILE *out=stage(options->output,&path);
    if (out==NULL) return "cannot create a temporary C17 file beside the output";
    const TfStatus status=tf_c_emit_optimized(out,core,options->optimization);
    const bool closed=fclose(out)==0;
    const char *failure=NULL;
    bool moved=false;
    if (status!=TF_OK || !closed) failure="cannot emit C17 to the temporary file";
    else if (build) failure=build_file(cc,options->runtime!=NULL ? options->runtime : ".",path,
                                       options->output,options->sanitize);
    else if (!(moved=publish(path,options->output))) failure="cannot replace the C17 output";
    if (!moved && remove(path)!=0) {
        if (fprintf(stderr,"ternfold: could not remove temporary file %s\n",path)<0 ||
            fflush(stderr)!=0) failure="cannot write temporary file warning";
    }
    free(path);
    return failure;
}
