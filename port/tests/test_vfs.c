#include <stdint.h>
int64_t b_monotonic_ns(void);
int64_t b_monotonic_ns(void) { return 1; } // fs_bridge.c seeds /dev/urandom with it
// Host test of the VFS + bionic bridge, run against the real packaged game data.
// It reads the containers the way Unity does: guest path, b_open, 32-bit lseek,
// then zip central directory.
//   usage: test_vfs <sd_root_with_game.apk_and_game.obb> <expected_obb_entries>
#include "port/bionic_fs.h"
#include "port/vfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        int ok_ = (c); /* once: many checks have side effects */         \
        printf("%s %s:%d %s\n", ok_ ? "ok  " : "FAIL", __FILE__, __LINE__, #c); \
        if (!ok_)                                                        \
            g_fail++;                                                    \
    } while (0)

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

static int read_at(int fd, uint32_t off, void *buf, uint32_t n)
{
    if (b_lseek(fd, (int32_t)off, SEEK_SET) != (int32_t)off)
        return -1;
    uint8_t *p = buf;
    while (n) {
        int32_t r = b_read(fd, p, n > (1u << 20) ? (1u << 20) : n);
        if (r <= 0)
            return -1;
        p += r;
        n -= (uint32_t)r;
    }
    return 0;
}

// Walks the central directory and CRC-checks one stored entry.
static void check_container(const char *guest, unsigned want_entries, const char *stored_name, uint32_t want_crc)
{
    struct bionic_stat st;
    CHECK(b_stat(guest, &st) == 0);
    CHECK(S_ISREG(st.st_mode));
    CHECK(st.st_size > 0);

    int fd = b_open(guest, 0);
    CHECK(fd >= 0);
    uint8_t eocd[22];
    CHECK(read_at(fd, (uint32_t)st.st_size - 22, eocd, 22) == 0);
    CHECK(rd32(eocd) == 0x06054b50);
    unsigned entries = rd16(eocd + 10);
    uint32_t cd_size = rd32(eocd + 12), cd_off = rd32(eocd + 16);
    printf("  %s: %u entries, central dir at %u\n", guest, entries, cd_off);
    CHECK(entries == want_entries);

    uint8_t *cd = malloc(cd_size);
    CHECK(read_at(fd, cd_off, cd, cd_size) == 0);
    unsigned walked = 0, found = 0;
    const uint8_t *p = cd;
    while (p < cd + cd_size && rd32(p) == 0x02014b50) {
        unsigned method = rd16(p + 10), nlen = rd16(p + 28), xlen = rd16(p + 30), clen = rd16(p + 32);
        uint32_t size = rd32(p + 24), hdr = rd32(p + 42);
        if (strlen(stored_name) == nlen && memcmp(p + 46, stored_name, nlen) == 0) {
            found = 1;
            CHECK(method == 0);
            uint8_t lh[30];
            CHECK(read_at(fd, hdr, lh, 30) == 0);
            uint32_t data = hdr + 30 + rd16(lh + 26) + rd16(lh + 28);
            uint8_t *buf = malloc(size);
            CHECK(read_at(fd, data, buf, size) == 0);
            CHECK(crc32_of(buf, size) == want_crc);
            free(buf);
        }
        walked++;
        p += 46 + nlen + xlen + clen;
    }
    CHECK(walked == entries);
    CHECK(found);
    free(cd);
    CHECK(b_close(fd) == 0);
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    CHECK(vfs_init(argv[1]) == 0);

    // End-to-end: the OBB and APK as Unity addresses them.
    check_container(VFS_GUEST_OBB, (unsigned)atoi(argv[2]), "assets/TBFSting_720p_HiQ.mp4", 0x4f39c6ca);
    check_container("/sdcard/Android/obb/" VFS_PACKAGE "/" VFS_OBB_NAME, (unsigned)atoi(argv[2]),
                    "assets/COD_INTRO.mp4", 0x0fe0e725);

    // Path traversal and unmapped paths must not reach the SD card.
    char host[VFS_PATH_MAX];
    CHECK(vfs_resolve(VFS_GUEST_DATA "/../../../etc/passwd", host, sizeof host, 0) == -ENOENT);
    CHECK(vfs_resolve(VFS_GUEST_DATA "/../../../../../../..", host, sizeof host, 0) == -ENOENT);
    CHECK(vfs_resolve("/etc/hosts", host, sizeof host, 0) == -ENOENT);
    CHECK(vfs_resolve("relative/path", host, sizeof host, 0) == -ENOENT);
    CHECK(vfs_resolve(VFS_GUEST_APK "/assets/bin", host, sizeof host, 0) == -ENOENT);
    CHECK(vfs_resolve(VFS_GUEST_DATA "x/file", host, sizeof host, 0) == -ENOENT);

    // Normalization.
    CHECK(vfs_resolve("/data//data/./" VFS_PACKAGE "/a/../files", host, sizeof host, 0) == 0);
    CHECK(strstr(host, "/data/app/files") != NULL);

    // Writable data round trip through bionic flag values.
    CHECK(b_mkdir(VFS_GUEST_DATA "/files", 0777) == 0 || *b_errno() == 17);
    int fd = b_open(VFS_GUEST_DATA "/files/Save.DAT",
                    BIONIC_O_WRONLY | BIONIC_O_CREAT | BIONIC_O_TRUNC, 0666);
    CHECK(fd >= 0);
    CHECK(b_write(fd, "hello", 5) == 5);
    CHECK(b_close(fd) == 0);

    // Case fallback: exact name differs in case at both file and directory level.
    struct bionic_stat st;
    CHECK(b_stat(VFS_GUEST_DATA "/FILES/save.dat", &st) == 0);
    CHECK(st.st_size == 5);

    fd = b_open(VFS_GUEST_DATA "/files/Save.DAT", BIONIC_O_RDWR | BIONIC_O_APPEND);
    CHECK(fd >= 0);
    CHECK(b_write(fd, "!", 1) == 1);
    CHECK(b_close(fd) == 0);
    CHECK(b_stat(VFS_GUEST_DATA "/files/Save.DAT", &st) == 0 && st.st_size == 6);

    // Guest errno must carry Linux numbers.
    CHECK(b_stat(VFS_GUEST_DATA "/missing", &st) == -1 && *b_errno() == 2);
    CHECK(b_open(VFS_GUEST_DATA "/files/Save.DAT", BIONIC_O_WRONLY | BIONIC_O_CREAT | BIONIC_O_EXCL, 0666) == -1
          && *b_errno() == 17);
    CHECK(b_rmdir(VFS_GUEST_DATA "/files") == -1 && *b_errno() == 39); // ENOTEMPTY

    // readdir ABI.
    bionic_DIR *d = b_opendir(VFS_GUEST_DATA "/files");
    CHECK(d != NULL);
    int seen = 0;
    struct bionic_dirent *e;
    while ((e = b_readdir(d)) != NULL)
        seen += strcmp(e->d_name, "Save.DAT") == 0;
    CHECK(seen == 1);
    CHECK(b_closedir(d) == 0);

    // Rename, truncate, unlink.
    CHECK(b_rename(VFS_GUEST_DATA "/files/Save.DAT", VFS_GUEST_DATA "/files/new.bin") == 0);
    CHECK(b_truncate(VFS_GUEST_DATA "/files/new.bin", 2) == 0);
    CHECK(b_stat(VFS_GUEST_DATA "/files/new.bin", &st) == 0 && st.st_size == 2);
    CHECK(b_unlink(VFS_GUEST_DATA "/files/new.bin") == 0);
    CHECK(b_rmdir(VFS_GUEST_DATA "/files") == 0);

    // cpuinfo is synthesized and reports NEON, which Unity's CPU probe needs.
    FILE *f = b_fopen("/proc/cpuinfo", "r");
    CHECK(f != NULL);
    char line[256];
    int neon = 0, procs = 0;
    while (f && fgets(line, sizeof line, f)) {
        neon |= strstr(line, "neon") != NULL;
        procs += strncmp(line, "processor", 9) == 0;
    }
    if (f)
        fclose(f);
    CHECK(neon && procs == 3);

    // Flag translation is table-driven, independent of the host's values.
    CHECK(b_flags_to_native(0) == O_RDONLY);
    CHECK(b_flags_to_native(BIONIC_O_RDWR | BIONIC_O_CREAT) == (O_RDWR | O_CREAT));
    CHECK(b_flags_to_native(BIONIC_O_WRONLY | BIONIC_O_APPEND | BIONIC_O_TRUNC) == (O_WRONLY | O_APPEND | O_TRUNC));

    printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail != 0;
}
