// Stand-in: picolibc has no <dirent.h>; m9b_posix.c implements these over
// FatFs. (Board builds only: the host has its own.)
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct m9b_dir DIR;
struct dirent { unsigned char d_type; char d_name[256]; };
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
#ifdef __cplusplus
}
#endif
