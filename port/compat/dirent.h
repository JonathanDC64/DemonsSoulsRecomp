/* Minimal POSIX <dirent.h> shim for Windows/MSVC/clang-cl.
 *
 * ps3recomp's runtime/ppu/ppu_fs.cpp includes <dirent.h> unconditionally and
 * only guards <io.h> for MinGW, so it doesn't build under MSVC/clang-cl (which
 * ship no <dirent.h>). This provides just the subset ppu_fs.cpp uses:
 * DIR / opendir / readdir / closedir / struct dirent{d_name,d_type}, backed by
 * the Win32 FindFirstFile/FindNextFile API. Our own code (MIT-compatible),
 * upstream-worthy as a portability fix.
 */
#ifndef PS3RECOMP_COMPAT_DIRENT_H
#define PS3RECOMP_COMPAT_DIRENT_H
#ifdef _WIN32

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

struct dirent {
    char d_name[MAX_PATH];
    int  d_type;
};

typedef struct DIR {
    HANDLE           h;
    WIN32_FIND_DATAA fd;
    int              first;
    struct dirent    ent;
} DIR;

static inline DIR* opendir(const char* path)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    DIR* d = (DIR*)calloc(1, sizeof(DIR));
    if (!d) return NULL;
    d->h = FindFirstFileA(pattern, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1;
    return d;
}

static inline struct dirent* readdir(DIR* d)
{
    if (!d) return NULL;
    if (!d->first) {
        if (!FindNextFileA(d->h, &d->fd)) return NULL;
    }
    d->first = 0;
    strncpy(d->ent.d_name, d->fd.cFileName, sizeof d->ent.d_name - 1);
    d->ent.d_name[sizeof d->ent.d_name - 1] = '\0';
    d->ent.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                  ? DT_DIR : DT_REG;
    return &d->ent;
}

static inline int closedir(DIR* d)
{
    if (!d) return -1;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
    return 0;
}

#endif /* _WIN32 */
#endif /* PS3RECOMP_COMPAT_DIRENT_H */
