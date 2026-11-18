#ifndef _WIN32
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#endif
#include "source_manager_internal.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#endif

typedef struct {
    char *path;
    const TfManagedSource *entry;
    void *native_key; /* owned UTF-16 on Windows; NULL on POSIX */
    bool canonical;
} Alias;
struct TfSourceManager {
    TfAllocator allocator;
    TfManagedSource **entries;
    size_t count, capacity;
    Alias *aliases;
    size_t alias_count, alias_capacity;
};

static void release(TfAllocator a, void *p) {
    if (p != NULL) a.deallocate(a.context, p);
}
static char *copy_path(TfAllocator a, const char *p) {
    size_t n = strlen(p);
    char *result;
    if (n == SIZE_MAX) return NULL;
    result = a.allocate(a.context, n + 1);
    if (result != NULL) memcpy(result, p, n + 1);
    return result;
}

#ifdef _WIN32
static char *absolute_path(TfAllocator a, const char *path) {
    DWORD needed, written;
    char *result;
    /* Exclude extended/device namespaces so equivalent ordinary requests have
     * the same lexical normalization. Canonical output may use extended paths. */
    if ((path[0] == '\\' || path[0] == '/') &&
        (path[1] == '\\' || path[1] == '/') &&
        (path[2] == '?' || path[2] == '.')) return NULL;
    needed = GetFullPathNameA(path, 0, NULL, NULL);
    if (needed == 0 || (uintmax_t)needed > (uintmax_t)SIZE_MAX) return NULL;
    result = a.allocate(a.context, (size_t)needed);
    if (result == NULL) return NULL;
    written = GetFullPathNameA(path, needed, result, NULL);
    if (written == 0 || written >= needed) { release(a, result); return NULL; }
    return result;
}
static char *resolved_path(TfAllocator a, const char *absolute) {
    HANDLE file = CreateFileA(absolute, FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    char *result = NULL;
    DWORD needed, written;
    BY_HANDLE_FILE_INFORMATION info;
    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        (void)CloseHandle(file); return NULL;
    }
    needed = GetFinalPathNameByHandleA(file, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (needed != 0 && (uintmax_t)needed < (uintmax_t)SIZE_MAX && needed < MAXDWORD) {
        result = a.allocate(a.context, (size_t)needed + 1);
        if (result != NULL) {
            written = GetFinalPathNameByHandleA(file, result, needed + 1,
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (written == 0 || written > needed) { release(a, result); result = NULL; }
            else if (strncmp(result, "\\\\?\\UNC\\", 8) == 0) {
                /* Return an ordinary native UNC path, usable by this API. */
                memmove(result + 2, result + 8, strlen(result + 8) + 1);
                result[0] = '\\'; result[1] = '\\';
            } else if (strncmp(result, "\\\\?\\", 4) == 0) {
                memmove(result, result + 4, strlen(result + 4) + 1);
            }
        }
    }
    if (!CloseHandle(file)) { release(a, result); result = NULL; }
    return result;
}
static wchar_t *wide_path(TfAllocator a, const char *path) {
    int count = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *out;
    if (count <= 0 || (size_t)count > SIZE_MAX / sizeof(*out)) return NULL;
    out = a.allocate(a.context, (size_t)count * sizeof(*out));
    if (out != NULL && MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS,
                                          path, -1, out, count) != count) {
        release(a, out); out = NULL;
    }
    return out;
}
static bool path_key_equal(const void *left, const void *right) {
    /* Native ordinal case comparison, not linguistic collation or file IDs. */
    return CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}
static bool path_separator(wchar_t c) { return c == L'\\' || c == L'/'; }
static bool insensitive_directory(const wchar_t *path) {
    HANDLE directory = CreateFileW(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    DWORD flags = 0;
    bool valid;
    if (directory == INVALID_HANDLE_VALUE) return false;
    /* FileCaseSensitiveInfo is FILE_INFO_BY_HANDLE_CLASS value23 (Win10 RS4).
     * A zero flag word establishes the supported case-insensitive policy.
     * Unsupported query/filesystem or unknown flags are host failures. */
    valid = GetFileInformationByHandle(directory, &info) != 0 &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        GetFileInformationByHandleEx(directory, (FILE_INFO_BY_HANDLE_CLASS)23,
                                     &flags, (DWORD)sizeof(flags)) != 0 && flags == 0;
    if (!CloseHandle(directory)) valid = false;
    return valid;
}
static bool supported_case_path(TfAllocator a, const wchar_t *path) {
    size_t length = wcslen(path), root, i;
    wchar_t *prefix;
    bool okay = true;
    if (length >= 3 && path[1] == L':' && path_separator(path[2])) root = 3;
    else if (length >= 2 && path_separator(path[0]) && path_separator(path[1])) {
        root = 2;
        while (root < length && !path_separator(path[root])) ++root;
        if (root == length) return false;
        ++root;
        while (root < length && !path_separator(path[root])) ++root;
        if (root == length) return false;
        ++root;
    } else return false;
    if (length == SIZE_MAX || length + 1 > SIZE_MAX / sizeof(*prefix)) return false;
    prefix = a.allocate(a.context, (length + 1) * sizeof(*prefix));
    if (prefix == NULL) return false;
    memcpy(prefix, path, (length + 1) * sizeof(*prefix));
    for (i = root; i < length; ++i) {
        if (i == root || path_separator(path[i])) {
            wchar_t saved = prefix[i];
            prefix[i] = L'\0';
            okay = insensitive_directory(prefix);
            prefix[i] = saved;
            if (!okay) break;
        }
    }
    release(a, prefix);
    return okay;
}
#else
static char *absolute_path(TfAllocator a, const char *path) {
    char *joined, *result;
    size_t length, at = 0, used = 1, root = 1;
    if (path[0] == '/') joined = copy_path(a, path);
    else {
        size_t capacity = 128, cwd_length, path_length = strlen(path);
        char *cwd;
        for (;;) {
            cwd = a.allocate(a.context, capacity);
            if (cwd == NULL) return NULL;
            if (getcwd(cwd, capacity) != NULL) break;
            {
                int failure = errno;
                release(a, cwd);
                if (failure != ERANGE || capacity > SIZE_MAX / 2) return NULL;
            }
            capacity *= 2;
        }
        cwd_length = strlen(cwd);
        if (path_length > SIZE_MAX - 2 || cwd_length > SIZE_MAX - path_length - 2) {
            release(a, cwd); return NULL;
        }
        joined = a.allocate(a.context, cwd_length + path_length + 2);
        if (joined != NULL) {
            memcpy(joined, cwd, cwd_length);
            joined[cwd_length] = '/';
            memcpy(joined + cwd_length + 1, path, path_length + 1);
        }
        release(a, cwd);
    }
    if (joined == NULL) return NULL;
    length = strlen(joined);
    result = a.allocate(a.context, length + 1);
    if (result == NULL) { release(a, joined); return NULL; }
    result[0] = '/';
    /* POSIX reserves exactly two initial slashes for implementation-defined
     * meaning. Preserve them, as well as trailing directory requirements. */
    if (length >= 2 && joined[1] == '/' && (length == 2 || joined[2] != '/')) {
        result[1] = '/'; used = 2; root = 2;
    }
    while (at < length) {
        size_t start, n;
        while (at < length && joined[at] == '/') ++at;
        start = at;
        while (at < length && joined[at] != '/') ++at;
        n = at - start;
        if (n == 0 || (n == 1 && joined[start] == '.' && at < length)) continue;
        /* Never collapse .. across a possible symlink component. */
        if (used > root) result[used++] = '/';
        memcpy(result + used, joined + start, n);
        used += n;
    }
    if (length != 0 && joined[length - 1] == '/' && result[used - 1] != '/')
        result[used++] = '/';
    result[used] = '\0';
    release(a, joined);
    return result;
}
static char *resolved_path(TfAllocator a, const char *absolute) {
#ifdef PATH_MAX
    char *result = a.allocate(a.context, (size_t)PATH_MAX);
    if (result != NULL && realpath(absolute, result) == NULL) {
        release(a, result); result = NULL;
    }
    return result;
#else
    (void)a; (void)absolute;
    return NULL;
#endif
}
#endif

/* SHA-256: unsigned fixed-width arithmetic is deliberately modulo 2^32. */
static uint32_t rotate(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }
static void sha_block(uint32_t h[8], const unsigned char block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64], v[8];
    size_t i;
    for (i = 0; i < 16; ++i) w[i] = ((uint32_t)block[4*i] << 24) |
        ((uint32_t)block[4*i+1] << 16) | ((uint32_t)block[4*i+2] << 8) | block[4*i+3];
    for (i = 16; i < 64; ++i) {
        uint32_t s0 = rotate(w[i-15],7) ^ rotate(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotate(w[i-2],17) ^ rotate(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    memcpy(v, h, sizeof(v));
    for (i = 0; i < 64; ++i) {
        uint32_t s1 = rotate(v[4],6) ^ rotate(v[4],11) ^ rotate(v[4],25);
        uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotate(v[0],2) ^ rotate(v[0],13) ^ rotate(v[0],22);
        uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        v[7]=v[6]; v[6]=v[5]; v[5]=v[4]; v[4]=v[3]+t1;
        v[3]=v[2]; v[2]=v[1]; v[1]=v[0]; v[0]=t1+s0+maj;
    }
    for (i = 0; i < 8; ++i) h[i] += v[i];
}
static void sha256(const unsigned char *bytes, size_t length, unsigned char out[32]) {
    uint32_t h[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                     0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    unsigned char tail[128] = {0};
    size_t at = 0, remaining, padded, i;
    uint64_t bits = (uint64_t)length * UINT64_C(8);
    while (length - at >= 64) { sha_block(h, bytes + at); at += 64; }
    remaining = length - at;
    memcpy(tail, bytes + at, remaining);
    tail[remaining] = 0x80;
    padded = remaining < 56 ? 64 : 128;
    for (i = 0; i < 8; ++i) tail[padded-1-i] = (unsigned char)(bits >> (8*i));
    sha_block(h, tail);
    if (padded == 128) sha_block(h, tail + 64);
    for (i = 0; i < 32; ++i) out[i] = (unsigned char)(h[i/4] >> (24-8*(i%4)));
}

TfStatus tf_source_manager_create(TfSourceManager **out, const TfAllocator *allocator) {
    TfAllocator a = tf_allocator(allocator);
    TfSourceManager *manager;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (!tf_allocator_valid(a)) return TF_HOST_ERROR;
    manager = a.allocate(a.context, sizeof(*manager));
    if (manager == NULL) return TF_HOST_ERROR;
    *manager = (TfSourceManager){0};
    manager->allocator = a;
    *out = manager;
    return TF_OK;
}
void tf_source_manager_destroy(TfSourceManager *manager) {
    size_t i;
    TfAllocator a;
    if (manager == NULL) return;
    a = manager->allocator;
    for (i = 0; i < manager->count; ++i) {
        TfManagedSource *entry = manager->entries[i];
        /* These pointers were allocated mutable here; const is for borrowers. */
        tf_source_destroy((TfSource *)entry->source);
        release(a, (void *)entry->source);
        release(a, (void *)entry->canonical_path);
        release(a, entry);
    }
    for (i = 0; i < manager->alias_count; ++i) {
        release(a, manager->aliases[i].path);
        release(a, manager->aliases[i].native_key);
    }
    release(a, manager->aliases);
    release(a, manager->entries);
    release(a, manager);
}
const TfManagedSource *tf_source_manager_lookup(const TfSourceManager *manager, TfSourceId id) {
    if (manager == NULL || id.owner != manager || id.index == 0 || id.index > manager->count)
        return NULL;
    return manager->entries[id.index - 1];
}
size_t tf_source_manager_count(const TfSourceManager *manager) {
    return manager != NULL ? manager->count : 0;
}
static const TfManagedSource *find_alias(const TfSourceManager *manager,
                                         const char *absolute, const void *key) {
    size_t i;
#ifdef _WIN32
    (void)absolute;
#else
    (void)key;
#endif
    for (i = 0; i < manager->alias_count; ++i) {
#ifdef _WIN32
        if (path_key_equal(manager->aliases[i].native_key, key))
#else
        if (strcmp(manager->aliases[i].path, absolute) == 0)
#endif
            return manager->aliases[i].entry;
    }
    return NULL;
}
TfStatus tf_source_manager_cached(TfSourceManager *manager, const char *path,
                                  const TfManagedSource **out) {
    TfAllocator a;
    char *absolute;
    void *key = NULL;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (manager == NULL || path == NULL || path[0] == '\0') return TF_HOST_ERROR;
    a = manager->allocator;
    absolute = absolute_path(a,path);
    if (absolute == NULL) return TF_HOST_ERROR;
#ifdef _WIN32
    key = wide_path(a,absolute);
    if (key == NULL) { release(a,absolute); return TF_HOST_ERROR; }
#endif
    *out = find_alias(manager,absolute,key);
    release(a,absolute);
    release(a,key);
    return *out != NULL ? (*out)->status : TF_OK;
}
static TfStatus load(TfSourceManager *manager, const char *path, const char *resolved,
                     const TfManagedSource **out) {
    TfAllocator a;
    char *absolute = NULL, *canonical = NULL, *canonical_alias = NULL;
    TfManagedSource *entry = NULL;
    TfSource *source = NULL;
    TfStatus status;
    size_t i;
    void *storage;
    void *absolute_key = NULL, *canonical_key = NULL;
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (manager == NULL || path == NULL || path[0] == '\0') return TF_HOST_ERROR;
    a = manager->allocator;
    absolute = absolute_path(a, path);
    if (absolute == NULL) goto fail;
#ifdef _WIN32
    absolute_key = wide_path(a, absolute);
    if (absolute_key == NULL) goto fail;
#endif
    *out = find_alias(manager,absolute,absolute_key);
    if (*out != NULL) {
        release(a,absolute);
        release(a,absolute_key);
        return (*out)->status;
    }
    if (resolved != NULL) canonical = copy_path(a,resolved);
    else {
#ifdef _WIN32
        if (!supported_case_path(a, absolute_key)) goto fail;
        canonical = resolved_path(a, absolute);
#else
        /* Resolve the original spelling so alias normalization cannot change
         * native traversal requirements for an uncached request. */
        canonical = resolved_path(a, path);
#endif
    }
    if (canonical == NULL) goto fail;
    canonical_alias = absolute_path(a, canonical);
    if (canonical_alias == NULL) goto fail;
#ifdef _WIN32
    canonical_key = wide_path(a, canonical_alias);
    if (canonical_key == NULL || !supported_case_path(a, canonical_key)) goto fail;
#endif
    storage = manager->aliases;
    if (manager->alias_count > SIZE_MAX - 2 ||
        !tf_grow(&storage, &manager->alias_capacity,
                 manager->alias_count + 2, sizeof(*manager->aliases), a)) goto fail;
    manager->aliases = storage;
#ifdef _WIN32
    for (i = 0; i < manager->alias_count; ++i) {
        if (manager->aliases[i].canonical &&
            path_key_equal(manager->aliases[i].native_key, canonical_key)) {
            *out = manager->aliases[i].entry;
#else
    for (i = 0; i < manager->count; ++i) {
        if (strcmp(manager->entries[i]->canonical_path, canonical) == 0) {
            *out = manager->entries[i];
#endif
            manager->aliases[manager->alias_count++] = (Alias){absolute, *out, absolute_key, false};
            release(a, canonical);
            release(a, canonical_alias);
            release(a, canonical_key);
            return (*out)->status;
        }
    }
    storage = manager->entries;
    if (manager->count == SIZE_MAX ||
        !tf_grow(&storage, &manager->capacity,
                 manager->count + 1, sizeof(*manager->entries), a)) goto fail;
    manager->entries = storage;
    entry = a.allocate(a.context, sizeof(*entry));
    source = a.allocate(a.context, sizeof(*source));
    if (source != NULL) *source = (TfSource){0};
    if (entry == NULL || source == NULL) goto fail;
    status = tf_source_load(source, canonical, &a);
    if (status == TF_HOST_ERROR) goto fail;
    /* SHA-256's input domain is fewer than 2^64 bits. */
    if ((uintmax_t)source->length > UINT64_MAX / UINT64_C(8)) goto fail;
    *entry = (TfManagedSource){0};
    entry->id = (TfSourceId){manager, manager->count + 1};
    entry->source = source;
    entry->canonical_path = canonical;
    entry->status = status;
    sha256(source->bytes, source->length, entry->sha256);
    manager->entries[manager->count++] = entry;
    manager->aliases[manager->alias_count++] = (Alias){absolute, entry, absolute_key, false};
    manager->aliases[manager->alias_count++] = (Alias){canonical_alias, entry, canonical_key, true};
    *out = entry;
    return status;
fail:
    if (source != NULL) tf_source_destroy(source);
    release(a, source);
    release(a, entry);
    release(a, canonical);
    release(a, canonical_alias);
    release(a, absolute);
    release(a, absolute_key);
    release(a, canonical_key);
    return TF_HOST_ERROR;
}
TfStatus tf_source_manager_load(TfSourceManager *manager, const char *path,
                                const TfManagedSource **out) {
    return load(manager,path,NULL,out);
}
TfStatus tf_source_manager_load_resolved(TfSourceManager *manager, const char *request,
                                         const char *resolved, const TfManagedSource **out) {
    if (resolved == NULL || resolved[0] == '\0') {
        if (out != NULL) *out = NULL;
        return TF_HOST_ERROR;
    }
    return load(manager,request,resolved,out);
}
