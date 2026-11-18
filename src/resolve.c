#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "ternfold/resolve.h"
#include "internal.h"
#include "source_manager_internal.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <limits.h>
#include <wchar.h>
#else
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct Allocation Allocation;
struct Allocation { Allocation *next; max_align_t alignment; };
typedef struct { void **items; size_t count, capacity; } Table;
typedef struct Module Module;
typedef struct Edge Edge;
struct Edge { Edge *next; Module *target; const TfAstNode *node; };
struct Module {
    TfResolvedModule view;
    TfParseResult *parse;
    TfScopeId scope;
    Edge *edges, *tail;
    unsigned color;
};
typedef struct Closure Closure;
struct Closure { const Closure *parent; TfNodeId node; };
typedef struct {
    Module *module;
    TfScopeId scope;
    const Closure *closure;
    TfScopeId machine_scope;
} Environment;
struct TfResolution {
    TfAllocator allocator;
    TfSourceManager *manager;
    Allocation *allocations;
    TfDiagnosticBag *diagnostics;
    TfDiagnosticPhaseResult phase;
    Table modules, symbols, types, scopes, references, captures;
    TfNodeId *pending;
    size_t pending_count, pending_capacity;
    Module *root;
    char *root_directory;
    bool failed;
    size_t cap;
};

static void *allocate(TfResolution *r, size_t count, size_t width) {
    Allocation *a; size_t bytes;
    if (r->failed || (width != 0 && count > (SIZE_MAX-sizeof(*a))/width)) { r->failed = true; return NULL; }
    bytes = count*width;
    a = r->allocator.allocate(r->allocator.context,sizeof(*a)+bytes);
    if (a == NULL) { r->failed = true; return NULL; }
    a->next = r->allocations; r->allocations = a;
    memset(a+1,0,bytes); return a+1;
}
static bool append(TfResolution *r, Table *t, void *item) {
    void *data = t->items;
    if (r->failed || t->count == SIZE_MAX || !tf_grow(&data,&t->capacity,t->count+1,sizeof(*t->items),r->allocator)) {
        r->failed = true; return false;
    }
    t->items = data; t->items[t->count++] = item; return true;
}
static char *copy(TfResolution *r, const char *s, size_t n) {
    char *out;
    if (n == SIZE_MAX) { r->failed = true; return NULL; }
    out = allocate(r,n+1,1); if (out != NULL) memcpy(out,s,n); return out;
}
static bool text_equal(TfAstText a, TfAstText b) {
    return a.length == b.length && (a.length == 0 || memcmp(a.data,b.data,a.length) == 0);
}
static bool named(TfAstText a, const char *b) {
    return a.length == strlen(b) && (a.length == 0 || memcmp(a.data,b,a.length) == 0);
}
static TfDiagnosticText diagnostic_text(const char *s) { return (TfDiagnosticText){s,strlen(s)}; }
static bool same_type(TfTypeId a, TfTypeId b) {
    return a.owner == b.owner && a.module_index == b.module_index && a.declaration_index == b.declaration_index;
}
static bool same_node(TfNodeId a, TfNodeId b) { return a.owner == b.owner && a.index == b.index; }
static bool same_scope(TfScopeId a, TfScopeId b) { return a.owner == b.owner && a.index == b.index; }
static void diagnose(TfResolution *r, const char *code, TfSpan use, const TfSpan *declaration, const char *message) {
    TfDiagnosticLabel labels[2] = {0}; TfDiagnosticRecord d = {0};
    if (r->failed) return;
    d.code = code; d.severity = TF_DIAG_ERROR; d.message = (TfDiagnosticText){message,strlen(message)};
    if (use.source != NULL) {
        labels[0] = (TfDiagnosticLabel){use,true,{"used here",9}}; d.labels = labels; d.label_count = 1;
        if (declaration != NULL && declaration->source != NULL) {
            labels[1] = (TfDiagnosticLabel){*declaration,false,{"declared here",13}}; d.label_count = 2;
        }
    }
    if (tf_diagnostic_bag_add(r->diagnostics,&d) != TF_OK) r->failed = true;
}
static void copy_diagnostics(TfResolution *r, const TfDiagnosticBag *bag, const TfSource *source) {
    /* Parser diagnostics (including its lexer batch) are all errors. Transfer
     * the suppressed tail through M02's count-only adapter. At least 100
     * records preceded any such tail, so our cap is already full. */
    TfLexResult tail = {0};
    size_t i;
    for (i = 0; !r->failed && i < tf_diagnostic_bag_count(bag); ++i)
        if (tf_diagnostic_bag_add(r->diagnostics,tf_diagnostic_bag_get(bag,i)) != TF_OK) r->failed = true;
    tail.source = source;
    tail.suppressed = tf_diagnostic_bag_suppressed(bag);
    tail.has_errors = tail.suppressed != 0;
    if (!r->failed && tail.has_errors && tf_diagnostic_bag_lexer(r->diagnostics,&tail) != TF_OK)
        r->failed = true;
}
static const TfAstNode *node(Module *m, TfNodeId id) { return tf_ast_get(m->view.ast,id); }
static bool parsed(const Module *m) {
    return m->parse != NULL && tf_diagnostic_phase_kind(tf_parse_phase(m->parse)) == TF_DIAG_PHASE_VALID;
}

/* Native adapters perform probes before SourceManager. A missing-path probe is
 * E204; allocation/read/capability failures never become input diagnostics.
 * Cached canonical snapshots are consulted first, including deleted files. */
#ifdef _WIN32
static bool separator(char c) { return c == '/' || c == '\\'; }
static bool wide_separator(wchar_t c) { return c == L'/' || c == L'\\'; }
static wchar_t *wide(TfResolution *r, const char *s, UINT code_page) {
    int n = MultiByteToWideChar(code_page,MB_ERR_INVALID_CHARS,s,-1,NULL,0); wchar_t *p;
    if (n <= 0) { r->failed = true; return NULL; }
    p = allocate(r,(size_t)n,sizeof(*p));
    if (p != NULL && MultiByteToWideChar(code_page,MB_ERR_INVALID_CHARS,s,-1,p,n) != n) r->failed = true;
    return r->failed ? NULL : p;
}
static char *narrow(TfResolution *r, const wchar_t *s, UINT code_page, bool *unrepresentable) {
    bool utf8 = code_page == CP_UTF8 || (code_page == CP_ACP && GetACP() == CP_UTF8);
    DWORD flags = utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL used_default = FALSE, *used = utf8 ? NULL : &used_default;
    int n = WideCharToMultiByte(code_page,flags,s,-1,NULL,0,NULL,used);
    char *p;
    *unrepresentable = used_default != FALSE;
    if (*unrepresentable) return NULL;
    if (n <= 0) { r->failed = true; return NULL; }
    p = allocate(r,(size_t)n,1);
    if (p == NULL) return NULL;
    if (WideCharToMultiByte(code_page,flags,s,-1,p,n,NULL,used) != n) r->failed = true;
    *unrepresentable = used_default != FALSE;
    if (!r->failed && !*unrepresentable && !utf8) {
        const wchar_t *round_trip = wide(r,p,code_page);
        if (round_trip != NULL && wcscmp(s,round_trip) != 0) *unrepresentable = true;
    }
    return r->failed || *unrepresentable ? NULL : p;
}
static char *absolute(TfResolution *r, const char *path) {
    DWORD n, got; char *p;
    if (strlen(path) >= 4 && separator(path[0]) && separator(path[1]) && (path[2] == '?' || path[2] == '.')) {
        r->failed = true; return NULL;
    }
    n = GetFullPathNameA(path,0,NULL,NULL);
    if (n == 0) { r->failed = true; return NULL; }
    p = allocate(r,(size_t)n,1); if (p == NULL) return NULL;
    got = GetFullPathNameA(path,n,p,NULL);
    if (got == 0 || got >= n) { r->failed = true; return NULL; } return p;
}
/* Reject UNC, device, ADS and mapped network paths before opening anything.
 * Each component is then opened without following its final reparse point.
 * Only known local symlink/junction substitutes are eligible for traversal. */
static bool local_drive_path(const wchar_t *path) {
    wchar_t drive[4];
    UINT kind;
    if (!((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        path[1] != L':' || !wide_separator(path[2]) || wcschr(path+2,L':') != NULL) return false;
    drive[0] = path[0]; drive[1] = L':'; drive[2] = L'\\'; drive[3] = 0;
    kind = GetDriveTypeW(drive);
    return kind == DRIVE_FIXED || kind == DRIVE_REMOVABLE || kind == DRIVE_CDROM || kind == DRIVE_RAMDISK;
}
static wchar_t *follow_local_reparse(TfResolution *r, HANDLE h, const wchar_t *path,
                                    size_t start, size_t end, bool *missing) {
    unsigned char data[16*1024];
    DWORD got, tag, flags = 0, required;
    uint16_t size, offset, length;
    size_t base, count, prefix, suffix, i, skip = 0;
    wchar_t *substitute, *joined, *normalized;
    if (!DeviceIoControl(h,FSCTL_GET_REPARSE_POINT,NULL,0,data,(DWORD)sizeof(data),&got,NULL) || got < 8)
        goto bad;
    memcpy(&tag,data,sizeof(tag));
    memcpy(&size,data+4,sizeof(size));
    if ((size_t)size+8 != got) goto bad;
    if (tag == IO_REPARSE_TAG_SYMLINK && got >= 20) {
        memcpy(&flags,data+16,sizeof(flags));
        if (flags > 1) goto bad;
        base = 20;
    } else if (tag == IO_REPARSE_TAG_MOUNT_POINT && got >= 16) base = 16;
    else goto bad; /* Unknown filesystem redirection cannot be certified local. */
    memcpy(&offset,data+8,sizeof(offset));
    memcpy(&length,data+10,sizeof(length));
    if (length == 0 || length % sizeof(wchar_t) != 0 || offset % sizeof(wchar_t) != 0 ||
        (size_t)offset + length > got-base) goto bad;
    count = length/sizeof(wchar_t);
    substitute = allocate(r,count+1,sizeof(*substitute));
    if (substitute == NULL) return NULL;
    memcpy(substitute,data+base+offset,length);
    if (wcslen(substitute) != count) goto bad;
    if (flags != 0) {
        if (wide_separator(substitute[0]) || wcschr(substitute,L':') != NULL) goto forbidden;
        prefix = start;
        /* Win32 lexical normalization is safe for leading parent components
         * of a relative link, after its containing directory was vetted. */
        while (skip < count) {
            size_t next = skip;
            while (next < count && !wide_separator(substitute[next])) ++next;
            if (!(next-skip == 1 && substitute[skip] == L'.') &&
                !(next-skip == 2 && substitute[skip] == L'.' && substitute[skip+1] == L'.')) break;
            skip = next < count ? next+1 : next;
        }
    } else {
        if (wcsncmp(substitute,L"\\??\\",4) != 0 || !local_drive_path(substitute+4)) goto forbidden;
        substitute += 4; count -= 4; prefix = 0; skip = 3;
    }
    /* Refuse ambiguous Win32 versus NT spellings rather than normalizing a
     * different reparse target from the one the system would open. */
    for (i = skip; i < count; ++i) {
        if ((i+1 == count || wide_separator(substitute[i+1])) &&
            (substitute[i] == L'.' || substitute[i] == L' ')) goto forbidden;
    }
    suffix = wcslen(path+end);
    if (suffix > SIZE_MAX-count-1 || prefix > SIZE_MAX-count-suffix-1) goto bad;
    joined = allocate(r,prefix+count+suffix+1,sizeof(*joined));
    if (joined == NULL) return NULL;
    wmemcpy(joined,path,prefix);
    wmemcpy(joined+prefix,substitute,count);
    wmemcpy(joined+prefix+count,path+end,suffix+1);
    required = GetFullPathNameW(joined,0,NULL,NULL);
    if (required == 0) goto bad;
    normalized = allocate(r,(size_t)required,sizeof(*normalized));
    if (normalized == NULL) return NULL;
    got = GetFullPathNameW(joined,required,normalized,NULL);
    if (got == 0 || got >= required) goto bad;
    if (!local_drive_path(normalized)) goto forbidden;
    return normalized;
bad:
    r->failed = true;
    return NULL;
forbidden:
    *missing = true;
    return NULL;
}
static char *canonical(TfResolution *r, const char *path, bool directory, bool *missing) {
    wchar_t *current = wide(r,path,CP_ACP);
    size_t start = 3, end = 3, hops = 0;
    *missing = false;
    if (current == NULL) return NULL;
    if (!local_drive_path(current)) { *missing = true; return NULL; }
    for (;;) {
        HANDLE h;
        BY_HANDLE_FILE_INFORMATION info;
        wchar_t saved = current[end], *target = NULL;
        char *result = NULL;
        bool final = saved == 0, reparse = false;
        current[end] = 0;
        h = CreateFileW(current,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                        NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        current[end] = saved;
        if (h == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME ||
                error == ERROR_DIRECTORY) *missing = true;
            else r->failed = true;
            return NULL;
        }
        if (!GetFileInformationByHandle(h,&info)) r->failed = true;
        else if (GetFileType(h) != FILE_TYPE_DISK) *missing = true;
        else if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            reparse = true;
            if (start == end || ++hops > 63) *missing = true;
            else target = follow_local_reparse(r,h,current,start,end,missing);
        } else if (((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != (!final || directory))
            *missing = true;
        else {
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                DWORD case_flags = 0;
                if (!GetFileInformationByHandleEx(h,(FILE_INFO_BY_HANDLE_CLASS)23,&case_flags,
                                                  (DWORD)sizeof(case_flags)) || case_flags != 0) r->failed = true;
            }
            if (final && !r->failed) {
                DWORD n = GetFinalPathNameByHandleW(h,NULL,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS), got;
                wchar_t *p = n != 0 && n != MAXDWORD ? allocate(r,(size_t)n+1,sizeof(*p)) : NULL;
                bool unrepresentable;
                if (p == NULL) r->failed = true;
                else {
                    got = GetFinalPathNameByHandleW(h,p,n+1,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
                    if (got == 0 || got > n || wcsncmp(p,L"\\\\?\\",4) != 0 ||
                        !local_drive_path(p+4)) r->failed = true;
                    else {
                        result = narrow(r,p+4,CP_ACP,&unrepresentable);
                        if (unrepresentable) r->failed = true;
                    }
                }
            }
        }
        if (!CloseHandle(h)) r->failed = true;
        if (r->failed || *missing) return NULL;
        if (reparse) { current = target; start = 3; end = 3; continue; }
        if (final) return result;
        while (wide_separator(current[end])) ++end;
        start = end;
        while (current[end] != 0 && !wide_separator(current[end])) ++end;
    }
}
#else
static char *absolute(TfResolution *r, const char *path) {
    char cwd[PATH_MAX]; size_t a, b; char *out;
    if (path[0] == '/') return copy(r,path,strlen(path));
    if (getcwd(cwd,sizeof(cwd)) == NULL) { r->failed = true; return NULL; }
    a = strlen(cwd); b = strlen(path);
    if (b > SIZE_MAX-2 || a > SIZE_MAX-b-2) { r->failed = true; return NULL; }
    out = allocate(r,a+b+2,1);
    if (out != NULL) { memcpy(out,cwd,a); out[a] = '/'; memcpy(out+a+1,path,b+1); } return out;
}
static char *canonical(TfResolution *r, const char *path, bool directory, bool *missing) {
    char buffer[PATH_MAX]; struct stat info; *missing = false;
    if (realpath(path,buffer) == NULL) {
        int e = errno;
        if (e == ENOENT || e == ENOTDIR || e == ELOOP) *missing = true; else r->failed = true;
        return NULL;
    }
    if (stat(buffer,&info) != 0) { r->failed = true; return NULL; }
    if (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)) { *missing = true; return NULL; }
    return copy(r,buffer,strlen(buffer));
}
#endif
/* A directory name followed by another non-separator is a sibling prefix,
 * not containment. Windows boundaries and case comparisons use UTF-16. */
static bool contained(TfResolution *r, const char *path) {
#ifdef _WIN32
    const wchar_t *root = wide(r,r->root_directory,CP_ACP), *p = wide(r,path,CP_ACP);
    size_t n, length;
    if (root == NULL || p == NULL) return false;
    n = wcslen(root); length = wcslen(p);
    return n != 0 && n < length && n <= INT_MAX &&
        (wide_separator(root[n-1]) || wide_separator(p[n])) &&
        CompareStringOrdinal(root,(int)n,p,(int)n,TRUE) == CSTR_EQUAL;
#else
    size_t n = strlen(r->root_directory), length = strlen(path);
    return n != 0 && n < length && (path[n] == '/' || r->root_directory[n-1] == '/') &&
        memcmp(path,r->root_directory,n) == 0;
#endif
}
static char *parent(TfResolution *r, const char *path) {
    const char *p = path+strlen(path);
    size_t n;
#ifdef _WIN32
    while (p != path) {
        const char *prev = CharPrevA(path,p);
        if (p-prev == 1 && separator(*prev)) break;
        p = prev;
    }
#else
    while (p != path && p[-1] != '/') --p;
#endif
    n = (size_t)(p-path);
    if (n == 0) { r->failed = true; return NULL; }
#ifdef _WIN32
    if (n > 3) --n;
#else
    if (n > 2 || (n == 2 && path[1] != '/')) --n;
#endif
    return copy(r,path,n);
}
static char *relative_path(TfResolution *r, const char *path) {
#ifdef _WIN32
    const wchar_t *root = wide(r,r->root_directory,CP_ACP);
    wchar_t *p = wide(r,path,CP_ACP), *at;
    bool unrepresentable;
    if (root == NULL || p == NULL) return NULL;
    p += wcslen(root);
    while (wide_separator(*p)) ++p;
    for (at = p; *at != 0; ++at) if (*at == L'\\') *at = L'/';
    return narrow(r,p,CP_UTF8,&unrepresentable);
#else
    path += strlen(r->root_directory);
    while (*path == '/') ++path;
    return copy(r,path,strlen(path));
#endif
}
static bool import_path(TfAstText p) {
    size_t i, start = 0;
    if (p.length < 4 || p.data[0] == '/' || memcmp(p.data+p.length-4,".tfd",4) != 0) return false;
    for (i = 0; i <= p.length; ++i) {
        if (i < p.length && (p.data[i] == '\\' || p.data[i] == ':' || p.data[i] == 0)) return false;
        if (i == p.length || p.data[i] == '/') {
            if (i-start == 2 && p.data[start] == '.' && p.data[start+1] == '.') return false;
            start = i+1;
        }
    }
    return true;
}
static Module *load_module(TfResolution *r, const char *path, bool root, TfSpan use) {
    const TfManagedSource *source; const char *resolved; char *abs; bool missing; size_t i; Module *m; TfLexResult lex;
    TfStatus status; TfParseOptions options = {root,64,100};
    abs = absolute(r,path); if (abs == NULL) return NULL;
    if (tf_source_manager_cached(r->manager,abs,&source) == TF_HOST_ERROR) {
        r->failed = true; return NULL;
    }
    resolved = source != NULL ? source->canonical_path : canonical(r,abs,false,&missing);
    if (resolved == NULL) {
        if (!r->failed) diagnose(r,"E204",use,NULL,"module file not found or not an allowed local regular source file");
        return NULL;
    }
#ifdef _WIN32
    if (source != NULL) {
        const wchar_t *cached = wide(r,resolved,CP_ACP);
        if (cached == NULL) return NULL;
        if (!local_drive_path(cached)) { diagnose(r,"E204",use,NULL,"module snapshot is not on a local drive"); return NULL; }
    }
#endif
    if (r->root_directory == NULL) r->root_directory = parent(r,resolved);
    if (r->failed) return NULL;
    if (!contained(r,resolved)) { diagnose(r,"E204",use,NULL,"module escapes the canonical source root"); return NULL; }
    if (r->failed) return NULL;
    if (source == NULL) {
        status = tf_source_manager_load_resolved(r->manager,abs,resolved,&source);
        if (status == TF_HOST_ERROR) { r->failed = true; return NULL; }
        /* Recheck actual snapshot capability after the initial filesystem probe. */
        if (!contained(r,source->canonical_path)) { diagnose(r,"E204",use,NULL,"module snapshot escapes the canonical source root"); return NULL; }
    }
    for (i = 0; i < r->modules.count; ++i) {
        m = r->modules.items[i]; if (m->view.source == source) return m;
    }
    m = allocate(r,1,sizeof(*m)); if (m == NULL) return NULL;
    m->view.source = source; m->view.is_root = root;
    if (!append(r,&r->modules,m)) return NULL;
    if (source->status != TF_OK) {
        if (tf_diagnostic_bag_source(r->diagnostics,source->source) != TF_OK) r->failed = true;
        return m;
    }
    status = tf_lex(&lex,source->source,100,&r->allocator);
    if (status == TF_HOST_ERROR) { r->failed = true; return m; }
    if (tf_parse(&m->parse,&lex,&options,&r->allocator) != TF_OK) r->failed = true;
    tf_lex_destroy(&lex);
    if (r->failed) return m;
    m->view.ast = tf_parse_ast(m->parse); copy_diagnostics(r,tf_parse_diagnostics(m->parse),source->source);
    return m;
}
static char *import_request(TfResolution *r, Module *m, TfAstText path) {
    char *dir = parent(r,m->view.source->canonical_path), *out; size_t n, join;
    const char *bytes = (const char *)path.data;
    if (dir == NULL) return NULL;
#ifdef _WIN32
    {
        char *utf8 = copy(r,bytes,path.length);
        const wchar_t *decoded = utf8 != NULL ? wide(r,utf8,CP_UTF8) : NULL;
        bool unrepresentable;
        if (decoded == NULL) return NULL;
        bytes = narrow(r,decoded,CP_ACP,&unrepresentable);
        if (unrepresentable)
            diagnose(r,"E204",path.span,NULL,"import path is not representable in the native Windows code page");
        if (bytes == NULL) return NULL;
        path.length = strlen(bytes);
    }
#endif
    n = strlen(dir);
#ifdef _WIN32
    join = n != 3;
#else
    join = dir[n-1] != '/';
#endif
    if (path.length > SIZE_MAX-2 || n > SIZE_MAX-path.length-2) { r->failed = true; return NULL; }
    out = allocate(r,n+path.length+join+1,1);
    if (out != NULL) {
        memcpy(out,dir,n);
        if (join != 0) out[n] = '/';
        memcpy(out+n+join,bytes,path.length);
    }
    return out;
}
/* Iterative import graph expansion avoids a host-stack limit on long chains. */
static void load_graph(TfResolution *r) {
    size_t at;
    for (at = 0; at < r->modules.count && !r->failed; ++at) {
        Module *m = r->modules.items[at]; const TfAstNode *program; size_t i;
        if (!parsed(m)) continue;
        program = node(m,tf_ast_root(m->view.ast));
        for (i = 0; i < program->as.program.declarations.count && !r->failed; ++i) {
            const TfAstNode *n = node(m,program->as.program.declarations.items[i]); char *request; Edge *e;
            if (n->kind != TF_AST_IMPORT) continue;
            e = allocate(r,1,sizeof(*e)); if (e == NULL) break;
            e->node = n;
            if (m->tail == NULL) m->edges = e; else m->tail->next = e; m->tail = e;
            if (!import_path(n->as.import_decl.path)) {
                diagnose(r,"E204",n->as.import_decl.path.span,NULL,"import requires a relative POSIX .tfd path without parent traversal");
                continue;
            }
            request = import_request(r,m,n->as.import_decl.path);
            if (request != NULL) e->target = load_module(r,request,false,n->as.import_decl.path.span);
        }
    }
}
static int module_compare(const void *a, const void *b) {
    const Module *left = *(Module *const *)a, *right = *(Module *const *)b;
    return strcmp(left->view.relative_path,right->view.relative_path);
}
typedef struct { Module *module; Edge *next; Edge *incoming; } Visit;
/* POSIX paths can contain non-UTF-8 bytes. Keep displayable spelling and
 * escape non-printable or malformed bytes so diagnostics remain valid UTF-8. */
static TfDiagnosticText path_note(TfResolution *r, const char *path) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(path), i, j = 0;
    char *s;
    if (n > (SIZE_MAX-1)/4) { r->failed = true; return (TfDiagnosticText){0}; }
    s = allocate(r,n*4+1,1);
    if (s == NULL) return (TfDiagnosticText){0};
    for (i = 0; i < n;) {
        unsigned char c = (unsigned char)path[i];
        size_t end; uint32_t scalar;
        if (c >= 0x20 && c <= 0x7e && c != '\\') s[j++] = path[i++];
        else if (c >= 0x80 && tf_decode((const unsigned char *)path,n,i,&scalar,&end) && scalar >= 0xa0) {
            memcpy(s+j,path+i,end-i); j += end-i; i = end;
        } else { s[j++] = '\\'; s[j++] = 'x'; s[j++] = hex[c >> 4]; s[j++] = hex[c & 15]; ++i; }
    }
    return (TfDiagnosticText){s,j};
}
static Module **graph_order(TfResolution *r) {
    Visit *stack = allocate(r,r->modules.count,sizeof(*stack));
    Module **order = allocate(r,r->modules.count,sizeof(*order));
    size_t i, depth, finished = 0;
    if (r->failed) return NULL;
    for (i = 0; i < r->modules.count && !r->failed; ++i) {
        Module *m = r->modules.items[i]; if (m->color != 0) continue;
        depth = 1; stack[0] = (Visit){m,m->edges,NULL}; m->color = 1;
        while (depth != 0 && !r->failed) {
            Visit *v = &stack[depth-1]; Edge *e = v->next;
            if (e == NULL) { v->module->color = 2; order[finished++] = v->module; --depth; continue; }
            v->next = e->next; if (e->target == NULL) continue;
            if (e->target->color == 0) {
                e->target->color = 1; stack[depth++] = (Visit){e->target,e->target->edges,e};
            } else if (e->target->color == 1) {
                TfDiagnosticRecord d = {0}; TfDiagnosticLabel *labels; TfDiagnosticText *notes;
                size_t start = 0, k, count;
                d.code = "E205"; d.severity = TF_DIAG_ERROR;
                d.message = diagnostic_text("import cycle (ordered path chain follows)");
                if (tf_diagnostic_bag_count(r->diagnostics) == r->cap) {
                    if (tf_diagnostic_bag_add(r->diagnostics,&d) != TF_OK) r->failed = true;
                    continue;
                }
                while (start < depth && stack[start].module != e->target) ++start;
                count = depth-start;
                labels = allocate(r,count,sizeof(*labels)); notes = allocate(r,count+1,sizeof(*notes));
                if (r->failed) break;
                labels[0] = (TfDiagnosticLabel){e->node->as.import_decl.path.span,true,diagnostic_text("cycle closes here")};
                for (k = 1; k < count; ++k) labels[k] = (TfDiagnosticLabel){stack[start+k].incoming->node->as.import_decl.path.span,false,diagnostic_text("imports next module")};
                for (k = 0; k < count && !r->failed; ++k) notes[k] = path_note(r,stack[start+k].module->view.relative_path);
                if (r->failed) break;
                notes[count] = notes[0];
                d.labels = labels; d.label_count = count; d.notes = notes; d.note_count = count+1;
                if (tf_diagnostic_bag_add(r->diagnostics,&d) != TF_OK) r->failed = true;
            }
        }
    }
    return r->failed ? NULL : order;
}

static TfScopeId new_scope(TfResolution *r, TfModuleId module, TfScopeId parent_id, TfNodeId n, TfNodeId closure) {
    TfResolvedScope *s = allocate(r,1,sizeof(*s));
    if (s == NULL) return (TfScopeId){0};
    s->id = (TfScopeId){r,r->scopes.count+1}; s->parent = parent_id; s->module = module; s->node = n; s->closure = closure;
    if (!append(r,&r->scopes,s)) return (TfScopeId){0}; return s->id;
}
static TfResolvedSymbol *find_symbol(TfResolution *r, TfScopeId scope, TfAstText name) {
    size_t i;
    for (i = 0; i < r->symbols.count; ++i) {
        TfResolvedSymbol *s = r->symbols.items[i]; if (same_scope(s->scope,scope) && text_equal(s->name,name)) return s;
    }
    return NULL;
}
static TfResolvedType *find_type(TfResolution *r, size_t module, TfAstText name) {
    size_t i;
    for (i = 0; i < r->types.count; ++i) {
        TfResolvedType *t = r->types.items[i]; if (t->id.module_index == module && text_equal(t->name,name)) return t;
    }
    return NULL;
}
static void reference(TfResolution *r, TfNodeId id, TfReferenceKind kind, TfSymbolId symbol, TfTypeId type) {
    TfResolvedReference *ref;
    if (r->failed || id.index == 0) return;
    ref = allocate(r,1,sizeof(*ref)); if (ref == NULL) return;
    *ref = (TfResolvedReference){id,kind,symbol,type}; (void)append(r,&r->references,ref);
}
static TfResolvedSymbol *add_symbol(TfResolution *r, Environment env, TfAstText name, TfNodeId decl,
                                    TfSymbolKind kind, TfTypeId type, bool pub) {
    TfResolvedSymbol *old, *s; TfScopeId prelude = {r,1};
    if (r->failed) return NULL;
    old = find_symbol(r,prelude,name);
    if (old != NULL && env.scope.index != 1 && kind != TF_SYMBOL_FIELD && kind != TF_SYMBOL_VARIANT) {
        diagnose(r,"E201",name.span,NULL,"reserved prelude value name"); return NULL;
    }
    old = find_symbol(r,env.scope,name);
    if (old != NULL) {
        diagnose(r,kind == TF_SYMBOL_PATTERN ? "E312" : "E202",name.span,&old->name.span,"duplicate declaration in the same scope"); return NULL;
    }
    s = allocate(r,1,sizeof(*s)); if (s == NULL) return NULL;
    s->id = (TfSymbolId){r,r->symbols.count+1}; s->kind = kind; s->name = name;
    s->module = env.module != NULL ? env.module->view.id : (TfModuleId){0}; s->scope = env.scope;
    s->declaration = decl; s->closure = env.closure != NULL ? env.closure->node : (TfNodeId){0};
    s->type = type; s->public_decl = pub;
    if (!append(r,&r->symbols,s)) return NULL;
    if (decl.index != 0) reference(r,decl,TF_REFERENCE_DECLARATION,s->id,type);
    return s;
}
static TfResolvedType *add_type(TfResolution *r, Module *m, TfAstText name, TfNodeId decl,
                                TfNominalKind kind, size_t declaration_index, bool pub) {
    TfResolvedType *old, *t; size_t module = m != NULL ? m->view.id.index : 0;
    old = find_type(r,0,name);
    if (old != NULL) { diagnose(r,"E201",name.span,NULL,"reserved prelude type name"); return NULL; }
    old = find_type(r,module,name);
    if (old != NULL) { diagnose(r,"E202",name.span,&old->name.span,"duplicate type declaration"); return NULL; }
    if (m != NULL) {
        TfResolvedSymbol *s = find_symbol(r,m->scope,name);
        if (s != NULL && (s->kind == TF_SYMBOL_ALIAS || kind != TF_TYPE_ENUM)) {
            diagnose(r,"E202",name.span,&s->name.span,"declaration collides with an existing module value or alias"); return NULL;
        }
        if (kind != TF_TYPE_ENUM && find_symbol(r,(TfScopeId){r,1},name) != NULL) {
            diagnose(r,"E201",name.span,NULL,"constructor would redeclare a reserved prelude value name"); return NULL;
        }
    }
    t = allocate(r,1,sizeof(*t)); if (t == NULL) return NULL;
    *t = (TfResolvedType){{r,module,declaration_index},kind,name,decl,pub};
    if (!append(r,&r->types,t)) return NULL;
    return t;
}
static TfAstText builtin_text(const char *s) { return (TfAstText){(const unsigned char *)s,strlen(s),{0}}; }
static void prelude(TfResolution *r) {
    static const char *const types[] = {"Int","Float","Bool","Text","Char","Unit","List","Option","Decision"};
    static const char *const values[] = {"step","len","at","append","map","fold","range","int_text","float_text","int_float","float_int","bool_text","parse_int","text_len","text_at","text_slice"};
    static const char *const variants[] = {"Some","None","Accept","Reject"};
    size_t i; Environment env = {0};
    env.scope = new_scope(r,(TfModuleId){0},(TfScopeId){0},(TfNodeId){0},(TfNodeId){0});
    for (i = 0; i < sizeof(types)/sizeof(types[0]) && !r->failed; ++i) (void)add_type(r,NULL,builtin_text(types[i]),(TfNodeId){0},TF_TYPE_BUILTIN,i+1,true);
    for (i = 0; i < sizeof(values)/sizeof(values[0]) && !r->failed; ++i) (void)add_symbol(r,env,builtin_text(values[i]),(TfNodeId){0},TF_SYMBOL_BUILTIN,(TfTypeId){0},true);
    /* Qualified builtin variants are not bare values and cannot reserve bare
     * Some/None/Accept/Reject in user scopes. Their scope is absent. */
    env.scope = (TfScopeId){0};
    for (i = 0; i < 4 && !r->failed; ++i) {
        TfResolvedType *t = find_type(r,0,builtin_text(i < 2 ? "Option" : "Decision"));
        if (t != NULL) (void)add_symbol(r,env,builtin_text(variants[i]),(TfNodeId){0},TF_SYMBOL_VARIANT,t->id,true);
    }
}
static bool entry_io(Module *m, const TfAstNode *fn) {
    const TfAstNode *b; size_t i;
    if (!m->view.is_root || !fn->as.function.public_decl || !named(fn->as.function.name,"main")) return false;
    b = node(m,fn->as.function.body);
    for (i = 0; i < b->as.block.statements.count; ++i) {
        TfAstKind k = node(m,b->as.block.statements.items[i])->kind;
        if (k == TF_AST_INPUT || k == TF_AST_OUTPUT) return true;
    }
    return false;
}
static void declare_module(TfResolution *r, Module *m) {
    const TfAstNode *p; size_t i, ordinal = 0; Environment env = {0}; Edge *edge;
    if (!parsed(m)) return;
    p = node(m,tf_ast_root(m->view.ast)); env.module = m; env.scope = m->scope;
    for (edge = m->edges; edge != NULL && !r->failed; edge = edge->next) {
        const TfAstNode *n = edge->node;
        TfResolvedSymbol *s;
        if (find_type(r,0,n->as.import_decl.alias) != NULL) {
            diagnose(r,"E201",n->as.import_decl.alias.span,NULL,"import alias reserves a prelude type name"); continue;
        }
        s = add_symbol(r,env,n->as.import_decl.alias,n->id,TF_SYMBOL_ALIAS,(TfTypeId){0},false);
        if (s != NULL && edge->target != NULL) s->target_module = edge->target->view.id;
    }
    for (i = 0; i < p->as.program.declarations.count && !r->failed; ++i) {
        const TfAstNode *n = node(m,p->as.program.declarations.items[i]); TfResolvedType *t = NULL;
        if (n->kind == TF_AST_IMPORT) continue;
        ++ordinal;
        if (n->kind == TF_AST_FUNCTION) {
            TfResolvedSymbol *s = add_symbol(r,env,n->as.function.name,n->id,TF_SYMBOL_FUNCTION,(TfTypeId){0},n->as.function.public_decl);
            if (s != NULL) s->entry_only = entry_io(m,n);
        } else if (n->kind == TF_AST_RECORD || n->kind == TF_AST_ENUM || n->kind == TF_AST_MACHINE) {
            bool machine = n->kind == TF_AST_MACHINE;
            TfAstText name = machine ? n->as.machine.name : n->as.aggregate.name;
            bool pub = machine ? n->as.machine.public_decl : n->as.aggregate.public_decl;
            t = add_type(r,m,name,n->id,machine ? TF_TYPE_MACHINE : n->kind == TF_AST_ENUM ? TF_TYPE_ENUM : TF_TYPE_RECORD,ordinal,pub);
            if (n->kind != TF_AST_ENUM && t != NULL) (void)add_symbol(r,env,name,n->id,TF_SYMBOL_CONSTRUCTOR,t->id,pub);
            if (n->kind == TF_AST_ENUM && t != NULL) {
                size_t j; Environment variants = env;
                reference(r,n->id,TF_REFERENCE_DECLARATION,(TfSymbolId){0},t->id);
                variants.scope = new_scope(r,m->view.id,m->scope,n->id,(TfNodeId){0});
                for (j = 0; j < n->as.aggregate.members.count && !r->failed; ++j) {
                    const TfAstNode *v = node(m,n->as.aggregate.members.items[j]);
                    (void)add_symbol(r,variants,v->as.variant.name,v->id,TF_SYMBOL_VARIANT,t->id,pub);
                }
            }
        }
    }
}

static TfResolvedSymbol *lookup(TfResolution *r, TfScopeId scope, TfAstText name) {
    while (scope.index != 0 && !r->failed) {
        TfResolvedSymbol *s = find_symbol(r,scope,name); const TfResolvedScope *sc;
        if (s != NULL) return s;
        sc = tf_resolution_scope(r,scope); if (sc == NULL) break; scope = sc->parent;
    }
    return NULL;
}
static TfReferenceKind value_kind(const TfResolvedSymbol *s) {
    if (s->kind == TF_SYMBOL_CELL) return TF_REFERENCE_OLD;
    if (s->kind == TF_SYMBOL_MACHINE_INPUT) return TF_REFERENCE_INPUT;
    if (s->kind == TF_SYMBOL_ALIAS) return TF_REFERENCE_MODULE;
    return TF_REFERENCE_VALUE;
}
static void capture(TfResolution *r, Environment env, const TfResolvedSymbol *s, TfReferenceKind kind) {
    const Closure *c;
    if (s->kind == TF_SYMBOL_FUNCTION || s->kind == TF_SYMBOL_CONSTRUCTOR || s->kind == TF_SYMBOL_VARIANT || s->kind == TF_SYMBOL_BUILTIN || s->kind == TF_SYMBOL_ALIAS) return;
    for (c = env.closure; c != NULL && !same_node(c->node,s->closure) && !r->failed; c = c->parent) {
        size_t i; TfResolvedCapture *cap;
        for (i = 0; i < r->captures.count; ++i) {
            cap = r->captures.items[i];
            if (same_node(cap->closure,c->node) && cap->symbol.index == s->id.index && cap->kind == kind) break;
        }
        if (i != r->captures.count) continue;
        cap = allocate(r,1,sizeof(*cap)); if (cap == NULL) return;
        *cap = (TfResolvedCapture){c->node,s->id,kind}; (void)append(r,&r->captures,cap);
    }
}
static bool visible(TfResolution *r, Environment env, TfModuleId module, bool pub, TfSpan use, TfSpan decl) {
    if (module.index != env.module->view.id.index && module.index != 0 && !pub) {
        diagnose(r,"E206",use,&decl,"private declaration is not exported by this module"); return false;
    }
    return true;
}
static TfResolvedType *type_path(TfResolution *r, Environment env, const TfAstNode *n, TfNodeList segments, bool exposed) {
    size_t at = 0, module = env.module->view.id.index; const TfAstNode *leaf; TfResolvedType *t;
    if (segments.count == 0 || r->failed) return NULL;
    leaf = node(env.module,segments.items[0]);
    if (segments.count > 1) {
        TfResolvedSymbol *alias = find_symbol(r,env.module->scope,leaf->as.path.name);
        if (alias == NULL || alias->kind != TF_SYMBOL_ALIAS) {
            diagnose(r,"E203",leaf->span,NULL,"unresolved module alias"); return NULL;
        }
        reference(r,leaf->id,TF_REFERENCE_MODULE,alias->id,(TfTypeId){0}); module = alias->target_module.index; ++at;
        if (module == 0 || !parsed(r->modules.items[module-1])) return NULL;
    }
    if (segments.count != at+1) { diagnose(r,"E203",n->span,NULL,"unresolved type path"); return NULL; }
    leaf = node(env.module,segments.items[at]); t = find_type(r,module,leaf->as.path.name);
    if (t == NULL && at == 0) t = find_type(r,0,leaf->as.path.name);
    if (t == NULL) { diagnose(r,"E203",leaf->span,NULL,"unresolved type name"); return NULL; }
    if (!visible(r,env,(TfModuleId){r,t->id.module_index},t->public_decl,leaf->span,t->name.span)) return NULL;
    reference(r,leaf->id,TF_REFERENCE_TYPE,(TfSymbolId){0},t->id);
    reference(r,n->id,TF_REFERENCE_TYPE,(TfSymbolId){0},t->id);
    if (exposed && !t->public_decl) diagnose(r,"E207",leaf->span,&t->name.span,"public signature exposes a private nominal type");
    return t;
}
static void resolve_type(TfResolution *r, Environment env, TfNodeId id, bool exposed) {
    const TfAstNode *n = node(env.module,id); size_t i;
    if (n == NULL || r->failed) return;
    if (n->kind == TF_AST_TYPE_PATH) {
        (void)type_path(r,env,n,n->as.type_path.segments,exposed);
        for (i = 0; i < n->as.type_path.arguments.count && !r->failed; ++i) resolve_type(r,env,n->as.type_path.arguments.items[i],exposed);
    } else if (n->kind == TF_AST_TYPE_FUNCTION) {
        for (i = 0; i < n->as.type_function.parameters.count && !r->failed; ++i) resolve_type(r,env,n->as.type_function.parameters.items[i],exposed);
        resolve_type(r,env,n->as.type_function.result,exposed);
    }
}
static const TfResolvedSymbol *value_path(TfResolution *r, Environment env, const TfAstNode *n, bool variant_only) {
    TfNodeList list = n->as.path.segments; const TfAstNode *leaf; TfResolvedSymbol *s = NULL; TfResolvedType *t;
    size_t at = 0, module = env.module->view.id.index, i;
    if (list.count == 0 || r->failed) return NULL;
    leaf = node(env.module,list.items[0]);
    if (list.count == 1) {
        s = lookup(r,env.scope,leaf->as.path.name);
        if (s != NULL && s->kind == TF_SYMBOL_ALIAS) {
            diagnose(r,"E203",leaf->span,&s->name.span,"import alias requires a qualified member"); return NULL;
        }
    } else {
        TfResolvedSymbol *alias = find_symbol(r,env.module->scope,leaf->as.path.name);
        if (alias != NULL && alias->kind == TF_SYMBOL_ALIAS) {
            reference(r,leaf->id,TF_REFERENCE_MODULE,alias->id,(TfTypeId){0});
            module = alias->target_module.index; at = 1;
            if (module == 0 || !parsed(r->modules.items[module-1])) return NULL;
        }
        leaf = node(env.module,list.items[at]);
        if (list.count == at+1 && at != 0) {
            Module *target = r->modules.items[module-1]; s = find_symbol(r,target->scope,leaf->as.path.name);
        } else if (list.count == at+2) {
            t = find_type(r,module,leaf->as.path.name);
            if (t == NULL && at == 0) t = find_type(r,0,leaf->as.path.name);
            if (t == NULL) { diagnose(r,"E203",leaf->span,NULL,"unresolved enum qualifier"); return NULL; }
            if (!visible(r,env,(TfModuleId){r,t->id.module_index},t->public_decl,leaf->span,t->name.span)) return NULL;
            reference(r,leaf->id,TF_REFERENCE_TYPE,(TfSymbolId){0},t->id);
            leaf = node(env.module,list.items[at+1]);
            for (i = 0; i < r->symbols.count; ++i) {
                TfResolvedSymbol *candidate = r->symbols.items[i];
                if (candidate->kind == TF_SYMBOL_VARIANT && same_type(candidate->type,t->id) && text_equal(candidate->name,leaf->as.path.name)) { s = candidate; break; }
            }
        }
    }
    if (s == NULL) { diagnose(r,"E203",leaf->span,NULL,"unresolved value name or qualified path"); return NULL; }
    if (!visible(r,env,s->module,s->public_decl,n->span,s->name.span)) return NULL;
    if (variant_only && s->kind != TF_SYMBOL_VARIANT) {
        diagnose(r,"E312",n->span,&s->name.span,"pattern path must name an enum variant"); return NULL;
    }
    reference(r,leaf->id,value_kind(s),s->id,s->type); reference(r,n->id,value_kind(s),s->id,s->type);
    if (s->entry_only) diagnose(r,"E208",n->span,&s->name.span,"I/O-bearing root main is host-entry-only and cannot be referenced");
    capture(r,env,s,value_kind(s));
    return s;
}
static void expression(TfResolution *r, Environment env, TfNodeId id);
static void pattern(TfResolution *r, Environment env, TfNodeId id) {
    const TfAstNode *n = node(env.module,id); size_t i;
    if (n == NULL || r->failed) return;
    if (n->kind == TF_AST_PATTERN_BINDER) (void)add_symbol(r,env,n->as.binder.name,id,TF_SYMBOL_PATTERN,(TfTypeId){0},false);
    else if (n->kind == TF_AST_PATTERN_VARIANT) {
        const TfResolvedSymbol *s = value_path(r,env,node(env.module,n->as.pattern_variant.path),true);
        if (s != NULL) reference(r,id,TF_REFERENCE_VALUE,s->id,s->type);
        for (i = 0; i < n->as.pattern_variant.arguments.count && !r->failed; ++i) pattern(r,env,n->as.pattern_variant.arguments.items[i]);
    }
}
static void block(TfResolution *r, Environment env, const TfAstNode *n, bool share_scope) {
    size_t i;
    if (!share_scope) env.scope = new_scope(r,env.module->view.id,env.scope,n->id,env.closure != NULL ? env.closure->node : (TfNodeId){0});
    for (i = 0; i < n->as.block.statements.count && !r->failed; ++i) {
        const TfAstNode *s = node(env.module,n->as.block.statements.items[i]);
        if (s->kind == TF_AST_LET) {
            resolve_type(r,env,s->as.binding.type,false); expression(r,env,s->as.binding.value);
            if (!s->as.binding.discard) (void)add_symbol(r,env,s->as.binding.name,s->id,TF_SYMBOL_LOCAL,(TfTypeId){0},false);
        } else if (s->kind == TF_AST_INPUT) {
            resolve_type(r,env,s->as.input.type,false);
            (void)add_symbol(r,env,s->as.input.name,s->id,TF_SYMBOL_ENTRY_INPUT,(TfTypeId){0},false);
        } else if (s->kind == TF_AST_OUTPUT) expression(r,env,s->as.output.value);
    }
    expression(r,env,n->as.block.final_expression);
}
static void parameters(TfResolution *r, Environment env, TfNodeList params, bool resolve_annotations) {
    size_t i;
    for (i = 0; i < params.count && !r->failed; ++i) {
        const TfAstNode *p = node(env.module,params.items[i]);
        if (resolve_annotations) resolve_type(r,env,p->as.parameter.type,false);
        (void)add_symbol(r,env,p->as.parameter.name,p->id,TF_SYMBOL_PARAMETER,(TfTypeId){0},false);
    }
}
static void push_expression(TfResolution *r, TfNodeId id) {
    void *data = r->pending;
    if (id.index == 0 || r->failed) return;
    if (r->pending_count == SIZE_MAX ||
        !tf_grow(&data,&r->pending_capacity,r->pending_count+1,sizeof(*r->pending),r->allocator)) {
        r->failed = true; return;
    }
    r->pending = data;
    r->pending[r->pending_count++] = id;
}
static void push_expressions(TfResolution *r, TfNodeList list) {
    size_t i;
    for (i = list.count; i != 0 && !r->failed; --i) push_expression(r,list.items[i-1]);
}
static void expression_step(TfResolution *r, Environment env, TfNodeId id) {
    const TfAstNode *n = node(env.module,id); size_t i;
    if (n == NULL || r->failed) return;
    switch (n->kind) {
    case TF_AST_PATH: (void)value_path(r,env,n,false); break;
    case TF_AST_NEXT: {
        TfResolvedSymbol *s = env.machine_scope.index != 0 ? find_symbol(r,env.machine_scope,n->as.next.name) : NULL;
        if (env.closure != NULL)
            diagnose(r,"E406",n->span,s != NULL && s->kind == TF_SYMBOL_CELL ? &s->name.span : NULL,"candidate projection underneath a closure");
        else if (env.machine_scope.index == 0) diagnose(r,"E405",n->span,NULL,"candidate projection requires a machine context");
        else if (s == NULL || s->kind != TF_SYMBOL_CELL) diagnose(r,"E403",n->as.next.name.span,NULL,"unknown candidate cell");
        else reference(r,id,TF_REFERENCE_CANDIDATE,s->id,s->type);
        break;
    }
    case TF_AST_GROUP: push_expression(r,n->as.group.expression); break;
    case TF_AST_LIST: push_expressions(r,n->as.list.elements); break;
    case TF_AST_UNARY: push_expression(r,n->as.unary.operand); break;
    case TF_AST_BINARY: push_expression(r,n->as.binary.right); push_expression(r,n->as.binary.left); break;
    case TF_AST_CALL:
        push_expressions(r,n->as.call.arguments);
        push_expression(r,n->as.call.callee);
        break;
    case TF_AST_ARGUMENT: push_expression(r,n->as.argument.value); break;
    case TF_AST_PROJECT: push_expression(r,n->as.project.base); break;
    case TF_AST_BLOCK: block(r,env,n,false); break;
    case TF_AST_IF:
        push_expression(r,n->as.conditional.else_block); push_expression(r,n->as.conditional.then_block); push_expression(r,n->as.conditional.condition); break;
    case TF_AST_MATCH:
        expression(r,env,n->as.matching.value);
        for (i = 0; i < n->as.matching.arms.count && !r->failed; ++i) {
            const TfAstNode *a = node(env.module,n->as.matching.arms.items[i]); Environment arm = env;
            arm.scope = new_scope(r,env.module->view.id,env.scope,a->id,env.closure != NULL ? env.closure->node : (TfNodeId){0});
            pattern(r,arm,a->as.arm.pattern); block(r,arm,node(env.module,a->as.arm.body),false);
        }
        break;
    case TF_AST_CLOSURE: {
        Closure closure = {env.closure,id}; Environment body = env;
        resolve_type(r,env,n->as.closure.result_type,false);
        body.closure = &closure; body.scope = new_scope(r,env.module->view.id,env.scope,id,id);
        parameters(r,body,n->as.closure.parameters,true); block(r,body,node(env.module,n->as.closure.body),true); break;
    }
    case TF_AST_FOR: {
        Environment loop = env; expression(r,env,n->as.loop.iterable);
        loop.scope = new_scope(r,env.module->view.id,env.scope,id,env.closure != NULL ? env.closure->node : (TfNodeId){0});
        (void)add_symbol(r,loop,n->as.loop.name,id,TF_SYMBOL_LOOP,(TfTypeId){0},false);
        block(r,loop,node(env.module,n->as.loop.body),false); break;
    }
    default: break; /* literals, Unit and pattern literals carry no names */
    }
}
/* Pratt's left and postfix spines can be arbitrarily long even within M04's
 * nesting budget. Walk them on a checked work vector, in source order. Nested
 * calls only cross scoped constructs whose nesting the parser already bounds. */
static void expression(TfResolution *r, Environment env, TfNodeId id) {
    size_t base = r->pending_count;
    push_expression(r,id);
    while (r->pending_count > base && !r->failed) {
        TfNodeId next = r->pending[--r->pending_count];
        expression_step(r,env,next);
    }
    r->pending_count = base;
}
static void signatures(TfResolution *r, Module *m) {
    const TfAstNode *p; Environment env = {0}; size_t i, j, ordinal = 0;
    if (!parsed(m)) return;
    p = node(m,tf_ast_root(m->view.ast)); env.module = m; env.scope = m->scope;
    for (i = 0; i < p->as.program.declarations.count && !r->failed; ++i) {
        const TfAstNode *n = node(m,p->as.program.declarations.items[i]);
        if (n->kind == TF_AST_IMPORT) continue;
        ++ordinal;
        if (n->kind == TF_AST_FUNCTION) {
            for (j = 0; j < n->as.function.parameters.count && !r->failed; ++j) resolve_type(r,env,node(m,n->as.function.parameters.items[j])->as.parameter.type,n->as.function.public_decl);
            resolve_type(r,env,n->as.function.result_type,n->as.function.public_decl);
        } else if (n->kind == TF_AST_RECORD || n->kind == TF_AST_ENUM) {
            Environment fields = env;
            const TfResolvedType *owner = tf_resolution_type(r,(TfTypeId){r,m->view.id.index,ordinal});
            if (n->kind == TF_AST_RECORD && owner != NULL) fields.scope = new_scope(r,m->view.id,m->scope,n->id,(TfNodeId){0});
            for (j = 0; j < n->as.aggregate.members.count && !r->failed; ++j) {
                const TfAstNode *member = node(m,n->as.aggregate.members.items[j]); size_t k;
                if (n->kind == TF_AST_RECORD) {
                    resolve_type(r,env,member->as.parameter.type,n->as.aggregate.public_decl);
                    if (owner != NULL) (void)add_symbol(r,fields,member->as.parameter.name,member->id,TF_SYMBOL_FIELD,owner->id,n->as.aggregate.public_decl);
                } else for (k = 0; k < member->as.variant.types.count && !r->failed; ++k) resolve_type(r,env,member->as.variant.types.items[k],n->as.aggregate.public_decl);
            }
        } else if (n->kind == TF_AST_MACHINE) {
            resolve_type(r,env,node(m,n->as.machine.parameter)->as.parameter.type,n->as.machine.public_decl);
            resolve_type(r,env,n->as.machine.yields,n->as.machine.public_decl); resolve_type(r,env,n->as.machine.rejects,n->as.machine.public_decl);
            for (j = 0; j < n->as.machine.sections.count && !r->failed; ++j) {
                const TfAstNode *s = node(m,n->as.machine.sections.items[j]);
                if (s->kind == TF_AST_STATE) resolve_type(r,env,s->as.state.type,n->as.machine.public_decl);
            }
        }
    }
}
static void machine_body(TfResolution *r, Environment env, const TfAstNode *n, size_t declaration_index) {
    size_t i; const TfAstNode *param = node(env.module,n->as.machine.parameter);
    const TfResolvedType *t = tf_resolution_type(r,(TfTypeId){r,env.module->view.id.index,declaration_index});
    Environment init = env;
    if (t == NULL) return;
    env.scope = new_scope(r,env.module->view.id,env.scope,n->id,(TfNodeId){0});
    env.machine_scope = env.scope;
    (void)add_symbol(r,env,param->as.parameter.name,param->id,TF_SYMBOL_MACHINE_INPUT,t->id,false);
    for (i = 0; i < n->as.machine.sections.count && !r->failed; ++i) {
        const TfAstNode *s = node(env.module,n->as.machine.sections.items[i]);
        if (s->kind == TF_AST_STATE) (void)add_symbol(r,env,s->as.state.name,s->id,TF_SYMBOL_CELL,t->id,t->public_decl);
    }
    for (i = 0; i < n->as.machine.sections.count && !r->failed; ++i) {
        const TfAstNode *s = node(env.module,n->as.machine.sections.items[i]);
        if (s->kind == TF_AST_STATE) expression(r,init,s->as.state.value);
        else if (s->kind == TF_AST_WRITER) {
            TfResolvedSymbol *cell = find_symbol(r,env.scope,s->as.writer.name);
            if (cell == NULL || cell->kind != TF_SYMBOL_CELL) diagnose(r,"E403",s->as.writer.name.span,NULL,"unknown writer cell");
            else reference(r,s->id,TF_REFERENCE_WRITER,cell->id,cell->type);
            expression(r,env,s->as.writer.value);
        } else if (s->kind == TF_AST_GUARD) { expression(r,env,s->as.guard.condition); expression(r,env,s->as.guard.rejection); }
        else if (s->kind == TF_AST_EMIT) expression(r,env,s->as.emission.value);
    }
}
static void bodies(TfResolution *r, Module *m) {
    const TfAstNode *p; Environment env = {0}; size_t i, ordinal = 0;
    if (!parsed(m)) return;
    p = node(m,tf_ast_root(m->view.ast)); env.module = m; env.scope = m->scope;
    for (i = 0; i < p->as.program.declarations.count && !r->failed; ++i) {
        const TfAstNode *n = node(m,p->as.program.declarations.items[i]);
        if (n->kind == TF_AST_IMPORT) continue;
        ++ordinal;
        if (n->kind == TF_AST_FUNCTION) {
            Environment body = env; body.scope = new_scope(r,m->view.id,m->scope,n->id,(TfNodeId){0});
            parameters(r,body,n->as.function.parameters,false); block(r,body,node(m,n->as.function.body),true);
        } else if (n->kind == TF_AST_MACHINE) machine_body(r,env,n,ordinal);
    }
}
TfStatus tf_resolve(TfResolution **out, TfSourceManager *manager, const char *entry_path,
                    const TfResolveOptions *options, const TfAllocator *allocator) {
    TfAllocator a = tf_allocator(allocator); TfResolution *r; Module **order;
    size_t i; bool missing;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (manager == NULL || entry_path == NULL || entry_path[0] == 0 || !tf_allocator_valid(a) ||
        (options != NULL && options->diagnostic_cap > 100)) return TF_HOST_ERROR;
    r = a.allocate(a.context,sizeof(*r)); if (r == NULL) return TF_HOST_ERROR;
    *r = (TfResolution){0}; r->allocator = a; r->manager = manager; r->cap = options != NULL ? options->diagnostic_cap : 100;
    if (tf_diagnostic_bag_create(&r->diagnostics,r->cap,&a) != TF_OK) goto fail;
    if (options != NULL && options->root_directory != NULL) {
        char *abs = absolute(r,options->root_directory);
        if (abs == NULL) goto fail;
        r->root_directory = canonical(r,abs,true,&missing);
        if (r->root_directory == NULL) goto fail; /* invalid root is a host capability error */
#ifdef _WIN32
        {
            const char *root = r->root_directory;
            char *end = r->root_directory+strlen(root), *prev = CharPrevA(root,end);
            if (end-root > 3 && end-prev == 1 && separator(*prev)) *prev = 0;
        }
#endif
    }
    if (strlen(entry_path) < 4 || strcmp(entry_path+strlen(entry_path)-4,".tfd") != 0)
        diagnose(r,"E204",(TfSpan){0},NULL,"entry requires a .tfd extension");
    else r->root = load_module(r,entry_path,true,(TfSpan){0});
    load_graph(r);
    if (r->failed) goto fail;
    for (i = 0; i < r->modules.count && !r->failed; ++i) {
        Module *m = r->modules.items[i];
        m->view.relative_path = relative_path(r,m->view.source->canonical_path);
        if (m->view.relative_path == NULL) goto fail;
    }
    if (r->modules.count != 0) qsort(r->modules.items,r->modules.count,sizeof(*r->modules.items),module_compare);
    prelude(r);
    for (i = 0; i < r->modules.count && !r->failed; ++i) {
        Module *m = r->modules.items[i]; m->view.id = (TfModuleId){r,i+1};
        m->scope = new_scope(r,m->view.id,(TfScopeId){r,1},m->view.ast != NULL ? tf_ast_root(m->view.ast) : (TfNodeId){0},(TfNodeId){0});
    }
    order = graph_order(r);
    if (order == NULL) goto fail;
    for (i = 0; i < r->modules.count && !r->failed; ++i) declare_module(r,r->modules.items[i]);
    for (i = 0; i < r->modules.count && !r->failed; ++i) signatures(r,order[i]);
    for (i = 0; i < r->modules.count && !r->failed; ++i) bodies(r,order[i]);
    if (r->failed || (tf_diagnostic_bag_has_errors(r->diagnostics) ? tf_diagnostic_phase_error(&r->phase,r->diagnostics) :
         tf_diagnostic_phase_valid(&r->phase,r,r->diagnostics)) != TF_OK) goto fail;
    *out = r; return TF_OK;
fail:
    tf_resolution_destroy(r); return TF_HOST_ERROR;
}
void tf_resolution_destroy(TfResolution *r) {
    Allocation *p; size_t i; TfAllocator a; Table *tables[6];
    if (r == NULL) return;
    a = r->allocator;
    for (i = 0; i < r->modules.count; ++i) { Module *m = r->modules.items[i]; tf_parse_destroy(m->parse); }
    tf_diagnostic_bag_destroy(r->diagnostics);
    tables[0] = &r->modules; tables[1] = &r->symbols; tables[2] = &r->types; tables[3] = &r->scopes; tables[4] = &r->references; tables[5] = &r->captures;
    for (i = 0; i < 6; ++i) if (tables[i]->items != NULL) a.deallocate(a.context,tables[i]->items);
    if (r->pending != NULL) a.deallocate(a.context,r->pending);
    p = r->allocations;
    while (p != NULL) { Allocation *next = p->next; a.deallocate(a.context,p); p = next; }
    a.deallocate(a.context,r);
}
const TfDiagnosticBag *tf_resolution_diagnostics(const TfResolution *r) { return r != NULL ? r->diagnostics : NULL; }
const TfDiagnosticPhaseResult *tf_resolution_phase(const TfResolution *r) { return r != NULL ? &r->phase : NULL; }
TfModuleId tf_resolution_root(const TfResolution *r) { return r != NULL && r->root != NULL ? r->root->view.id : (TfModuleId){0}; }
const char *tf_resolution_root_directory(const TfResolution *r) { return r != NULL ? r->root_directory : NULL; }
#define TABLE_ACCESSORS(singular, plural, Type, field) \
size_t tf_resolution_##singular##_count(const TfResolution *r) { return r != NULL ? r->field.count : 0; } \
const Type *tf_resolution_##singular##_at(const TfResolution *r, size_t i) { return r != NULL && i < r->field.count ? r->field.items[i] : NULL; }
TABLE_ACCESSORS(symbol,symbols,TfResolvedSymbol,symbols)
TABLE_ACCESSORS(type,types,TfResolvedType,types)
TABLE_ACCESSORS(scope,scopes,TfResolvedScope,scopes)
TABLE_ACCESSORS(reference,references,TfResolvedReference,references)
TABLE_ACCESSORS(capture,captures,TfResolvedCapture,captures)
#undef TABLE_ACCESSORS
size_t tf_resolution_module_count(const TfResolution *r) { return r != NULL ? r->modules.count : 0; }
const TfResolvedModule *tf_resolution_module_at(const TfResolution *r, size_t i) { return r != NULL && i < r->modules.count ? &((Module *)r->modules.items[i])->view : NULL; }
const TfResolvedModule *tf_resolution_module(const TfResolution *r, TfModuleId id) { return r != NULL && id.owner == r && id.index != 0 ? tf_resolution_module_at(r,id.index-1) : NULL; }
const TfResolvedSymbol *tf_resolution_symbol(const TfResolution *r, TfSymbolId id) { return r != NULL && id.owner == r && id.index != 0 ? tf_resolution_symbol_at(r,id.index-1) : NULL; }
const TfResolvedScope *tf_resolution_scope(const TfResolution *r, TfScopeId id) { return r != NULL && id.owner == r && id.index != 0 ? tf_resolution_scope_at(r,id.index-1) : NULL; }
const TfResolvedType *tf_resolution_type(const TfResolution *r, TfTypeId id) {
    size_t i; if (r == NULL || id.owner != r || id.declaration_index == 0) return NULL;
    for (i = 0; i < r->types.count; ++i) { const TfResolvedType *t = r->types.items[i]; if (same_type(t->id,id)) return t; } return NULL;
}
const TfResolvedReference *tf_resolution_reference(const TfResolution *r, TfNodeId id) {
    size_t i; if (r == NULL || id.owner == NULL || id.index == 0) return NULL;
    for (i = r->references.count; i != 0; --i) { const TfResolvedReference *ref = r->references.items[i-1]; if (same_node(ref->node,id)) return ref; } return NULL;
}
