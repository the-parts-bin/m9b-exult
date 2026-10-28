// Ultima VII on the m9b SoC: files and the POSIX functions Exult uses beyond
// picolibc and ../../core/runtime/syscalls.c (files through FatFs).
//
// Everything is under U7/ on the SD card. Exult opens relative paths
// (exult.cfg, EXULT/exult.flx, BLACKGATE/STATIC/..., the saves), and the
// linker sends open(), unlink(), rename() and mkdir() (fopen, the C++
// streams, remove, ... included) through the __wrap_ functions here, which
// put U7/ in front of them. stat, opendir and glob do the same.
#include <errno.h>
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define DIR DIR_   // FatFs's DIR, apart from <dirent.h>'s
#include "ff.h"
#undef DIR
#include <dirent.h>

#define DATA_DIR "U7/"

extern uint32_t m9b_ticks_ms(void);   // m9b_sdl.cc

static const char *data_path(const char *path, char *buf, size_t n) {
    if (path[0] == '/') return path;
    while (path[0] == '.' && path[1] == '/') path += 2;
    if (path[0] == '.' && path[1] == 0) path++;
    snprintf(buf, n, DATA_DIR "%s", path);
    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/') buf[--len] = 0;   // FatFs takes no trailing '/'
    return buf;
}

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...) {
    char buf[256];
    return __real_open(data_path(path, buf, sizeof(buf)), flags);
}

int __real_unlink(const char *path);
int __wrap_unlink(const char *path) {
    char buf[256];
    return __real_unlink(data_path(path, buf, sizeof(buf)));
}

int __real_rename(const char *a, const char *b);
int __wrap_rename(const char *a, const char *b) {
    char ba[256], bb[256];
    const char *pb = data_path(b, bb, sizeof(bb));
    f_unlink(pb);   // POSIX replaces the target; FatFs doesn't
    return __real_rename(data_path(a, ba, sizeof(ba)), pb);
}

// Missing parents are made too (as mkdir -p): Exult makes GAMEDAT/BG and
// SAVE/BG (exult.cfg) with one call each, and the card starts without them.
int __real_mkdir(const char *path, mode_t mode);
int __wrap_mkdir(const char *path, mode_t mode) {
    char buf[256];
    const char *p = data_path(path, buf, sizeof(buf));
    if (p != buf) return __real_mkdir(p, mode);
    for (char *q = buf + sizeof(DATA_DIR) - 1; *q; q++) {
        if (*q != '/') continue;
        *q = 0;
        __real_mkdir(buf, mode);   // (fails if it exists: fine)
        *q = '/';
    }
    return __real_mkdir(buf, mode);
}

static int fill_stat(const FILINFO *fi, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = (fi->fattrib & AM_DIR) ? S_IFDIR | 0755 : S_IFREG | 0644;
    st->st_size = (off_t)fi->fsize;
    st->st_nlink = 1;
    // FAT date and time to seconds since 1970 (roughly: 365-day years, 31-day months).
    uint32_t y = (fi->fdate >> 9) + 10, mo = (fi->fdate >> 5) & 15, d = fi->fdate & 31;
    st->st_mtime = (time_t)(((y * 365u + (mo ? mo - 1 : 0) * 31u + (d ? d - 1 : 0)) * 24u + (fi->ftime >> 11)) * 3600u +
                            ((fi->ftime >> 5) & 63) * 60u + (fi->ftime & 31) * 2u);
    return 0;
}

int stat(const char *path, struct stat *st) {
    char buf[256];
    const char *p = data_path(path, buf, sizeof(buf));
    if (!strcmp(p, "U7") || !strcmp(p, "/")) {   // the root and U7 itself (f_stat can't)
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFDIR | 0755;
        return 0;
    }
    FILINFO fi;
    if (f_stat(p, &fi) != FR_OK) {
        errno = ENOENT;
        return -1;
    }
    return fill_stat(&fi, st);
}

int lstat(const char *path, struct stat *st) { return stat(path, st); }

int fstat(int fd, struct stat *st) {
    memset(st, 0, sizeof(*st));
    if (fd < 3) {
        st->st_mode = S_IFCHR;
        return 0;
    }
    off_t pos = lseek(fd, 0, SEEK_CUR);
    if (pos < 0) return -1;
    off_t end = lseek(fd, 0, SEEK_END);
    lseek(fd, pos, SEEK_SET);
    st->st_mode = S_IFREG | 0644;
    st->st_size = end;
    st->st_nlink = 1;
    return 0;
}

int fchmod(int fd, mode_t mode) { (void)fd; (void)mode; return 0; }
int chmod(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }

int access(const char *path, int mode) {
    (void)mode;
    struct stat st;
    return stat(path, &st);
}

int isatty(int fd) { return fd < 3; }

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    uint32_t ms = m9b_ticks_ms();
    tv->tv_sec = ms / 1000u;
    tv->tv_usec = (ms % 1000u) * 1000u;
    return 0;
}

// No console input (the UART is the keyboard, see m9b_sdl.cc).
static int no_input(FILE *f) { (void)f; return EOF; }
static FILE input = FDEV_SETUP_STREAM(NULL, no_input, NULL, _FDEV_SETUP_READ);
FILE *const stdin = &input;

// -------------------------------------------------------------------------
// Directories (port/dirent.h)
// -------------------------------------------------------------------------
struct m9b_dir {
    DIR_ ff;   // FatFs's (ff.h's DIR, renamed above)
    struct dirent ent;
};

DIR *opendir(const char *path) {
    char buf[256];
    struct m9b_dir *d = malloc(sizeof(*d));
    if (!d) return NULL;
    if (f_opendir(&d->ff, data_path(path, buf, sizeof(buf))) != FR_OK) {
        free(d);
        errno = ENOENT;
        return NULL;
    }
    return d;
}

struct dirent *readdir(DIR *d) {
    FILINFO fi;
    if (f_readdir(&d->ff, &fi) != FR_OK || !fi.fname[0]) return NULL;
    d->ent.d_type = (fi.fattrib & AM_DIR) ? DT_DIR : DT_REG;
    snprintf(d->ent.d_name, sizeof(d->ent.d_name), "%s", fi.fname);
    return &d->ent;
}

int closedir(DIR *d) {
    f_closedir(&d->ff);
    free(d);
    return 0;
}

// glob(): "dir/pattern" with * and ? in the last part only (what Exult asks
// for: the saves, mods), case-insensitive as on FAT. The names keep the
// path as given (without U7/).
static int match(const char *p, const char *s) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            for (const char *t = s;; t++) {
                if (match(p + 1, t)) return 1;
                if (!*t) return 0;
            }
        }
        if (!*s) return 0;
        if (*p != '?' && (*p | 0x20) != (*s | 0x20)) return 0;
    }
    return !*s;
}

int glob(const char *restrict pattern, int flags, int (*errfunc)(const char *, int), glob_t *restrict g) {
    (void)flags; (void)errfunc;
    memset(g, 0, sizeof(*g));
    const char *slash = strrchr(pattern, '/');
    char dir[200] = ".";
    const char *pat = pattern;
    if (slash) {
        size_t n = (size_t)(slash - pattern);
        if (n >= sizeof(dir)) return GLOB_NOMATCH;
        memcpy(dir, pattern, n);
        dir[n] = 0;
        pat = slash + 1;
    }
    DIR *d = opendir(dir);
    if (!d) return GLOB_NOMATCH;
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!match(pat, e->d_name)) continue;
        if (g->gl_pathc + 1 >= cap) {
            cap = cap ? 2 * cap : 16;
            char **v = realloc(g->gl_pathv, sizeof(char *) * cap);
            if (!v) break;
            g->gl_pathv = v;
        }
        size_t len = strlen(pattern) + strlen(e->d_name) + 2;
        char *path = malloc(len);
        if (!path) break;
        if (slash) snprintf(path, len, "%.*s/%s", (int)(slash - pattern), pattern, e->d_name);
        else snprintf(path, len, "%s", e->d_name);
        g->gl_pathv[g->gl_pathc++] = path;
        g->gl_pathv[g->gl_pathc] = NULL;
    }
    closedir(d);
    return g->gl_pathc ? 0 : GLOB_NOMATCH;
}

void globfree(glob_t *g) {
    for (size_t i = 0; i < (size_t)g->gl_pathc; i++) free(g->gl_pathv[i]);
    free(g->gl_pathv);
    g->gl_pathv = NULL;
    g->gl_pathc = 0;
}
