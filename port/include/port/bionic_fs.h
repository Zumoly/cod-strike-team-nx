// Android bionic ABI for the file functions imported by libunity.so / libmono.so.
// The .so loader binds these in place of the libc names ("stat" -> b_stat, ...).
//
// A pass-through would corrupt state: bionic's struct stat, struct dirent, O_*
// flags, errno numbers and off_t all differ from newlib's.
#ifndef PORT_BIONIC_FS_H
#define PORT_BIONIC_FS_H

#include <stdint.h>
#include <stdio.h>

// ARM bionic struct stat == kernel stat64, 104 bytes.
struct bionic_stat {
    uint64_t st_dev;
    uint8_t __pad0[4];
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint8_t __pad3[4];
    uint8_t __pad3b[4];
    int64_t st_size;
    uint32_t st_blksize;
    uint8_t __pad4[4];
    uint64_t st_blocks;
    uint32_t st_atime_s;
    uint32_t st_atime_ns;
    uint32_t st_mtime_s;
    uint32_t st_mtime_ns;
    uint32_t st_ctime_s;
    uint32_t st_ctime_ns;
    uint64_t st_ino;
};
_Static_assert(sizeof(struct bionic_stat) == 104, "bionic stat size");
_Static_assert(__builtin_offsetof(struct bionic_stat, st_size) == 48, "bionic st_size");
_Static_assert(__builtin_offsetof(struct bionic_stat, st_blocks) == 64, "bionic st_blocks");
_Static_assert(__builtin_offsetof(struct bionic_stat, st_ino) == 96, "bionic st_ino");

struct bionic_dirent {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
} __attribute__((aligned(8)));
_Static_assert(sizeof(struct bionic_dirent) == 280, "bionic dirent size");
_Static_assert(__builtin_offsetof(struct bionic_dirent, d_name) == 19, "bionic d_name");

// bionic/Linux ARM open(2) flag values.
#define BIONIC_O_WRONLY    01
#define BIONIC_O_RDWR      02
#define BIONIC_O_CREAT     0100
#define BIONIC_O_EXCL      0200
#define BIONIC_O_TRUNC     01000
#define BIONIC_O_APPEND    02000
#define BIONIC_O_NONBLOCK  04000
#define BIONIC_O_DIRECTORY 040000

typedef struct bionic_dir bionic_DIR;

int *b_errno(void);
int b_errno_from_native(int e);
int b_flags_to_native(int bionic_flags);

int b_open(const char *path, int flags, ...);
int b_close(int fd);
void b_fs_forget(int fd); // for closes that bypass b_close
int32_t b_read(int fd, void *buf, uint32_t n);
int32_t b_write(int fd, const void *buf, uint32_t n);
int32_t b_lseek(int fd, int32_t offset, int whence); // bionic off_t is 32-bit
FILE *b_fopen(const char *path, const char *mode);
int b_stat(const char *path, struct bionic_stat *out);
int b_lstat(const char *path, struct bionic_stat *out);
int b_access(const char *path, int mode);
int b_fstat(int fd, struct bionic_stat *out);
bionic_DIR *b_opendir(const char *path);
struct bionic_dirent *b_readdir(bionic_DIR *d);
int b_closedir(bionic_DIR *d);
int b_mkdir(const char *path, uint32_t mode);
int b_rmdir(const char *path);
int b_unlink(const char *path);
int b_rename(const char *from, const char *to);
int b_truncate(const char *path, int32_t size);
int b_chdir(const char *path);

#endif
