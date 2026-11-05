#ifndef CDM_COMPAT_DIRENT_H
#define CDM_COMPAT_DIRENT_H

/*
 * Just enough of <dirent.h> for the sample browser on Windows: opendir,
 * readdir and closedir over FindFirstFileA. The browser only needs entry names
 * and "." / ".." / hidden entries skipped, which is what this provides.
 */
#if defined(_WIN32)

#include <windows.h>

#include <stdlib.h>
#include <string.h>

#define DT_DIR 4
#define DT_REG 8

struct dirent {
    char d_name[MAX_PATH];
    int d_type;
};

typedef struct {
    HANDLE handle;
    int first;
    WIN32_FIND_DATAA data;
    struct dirent entry;
} DIR;

static inline DIR *opendir(const char *path)
{
    char pattern[MAX_PATH];
    size_t n = strlen(path);
    if (n + 3 >= sizeof pattern)
        return NULL;
    memcpy(pattern, path, n);
    if (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        snprintf(pattern + n, sizeof pattern - n, "*");
    else
        snprintf(pattern + n, sizeof pattern - n, "/*");

    DIR *d = (DIR *)calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->handle = FindFirstFileA(pattern, &d->data);
    if (d->handle == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

static inline struct dirent *readdir(DIR *d)
{
    if (!d)
        return NULL;
    if (!d->first && !FindNextFileA(d->handle, &d->data))
        return NULL;
    d->first = 0;
    snprintf(d->entry.d_name, sizeof d->entry.d_name, "%s", d->data.cFileName);
    d->entry.d_type = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    return &d->entry;
}

static inline int closedir(DIR *d)
{
    if (!d)
        return -1;
    FindClose(d->handle);
    free(d);
    return 0;
}

#endif /* _WIN32 */

#endif
