#include "port/vfs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_root[VFS_PATH_MAX];

// Longest prefixes first is not required: prefixes are mutually exclusive.
static const struct rule {
    const char *guest;
    const char *host;
    int is_file;
} g_rules[] = {
    {VFS_GUEST_APK, "game.apk", 1},
    {VFS_GUEST_OBB, "game.obb", 1},
    {"/sdcard/Android/obb/" VFS_PACKAGE "/" VFS_OBB_NAME, "game.obb", 1},
    {"/storage/emulated/0/Android/obb/" VFS_PACKAGE "/" VFS_OBB_NAME, "game.obb", 1},
    {"/proc/cpuinfo", "data/proc/cpuinfo", 1},
    {"/proc/meminfo", "data/proc/meminfo", 1},
    {"/dev/null", "data/proc/null", 1},
    {"/dev/urandom", "data/proc/random", 1},
    {"/dev/random", "data/proc/random", 1},
    {"/proc", "data/proc", 0}, // Mono refuses to start unless the directory exists
    {VFS_GUEST_DATA, "data/app", 0},
    {VFS_GUEST_EXT, "data/ext", 0},
    {"/sdcard/Android/data/" VFS_PACKAGE, "data/ext", 0},
    {"/storage/emulated/0/Android/data/" VFS_PACKAGE, "data/ext", 0},
    // Parent directories Unity may stat or list.
    {"/mnt/sdcard/Android/obb/" VFS_PACKAGE, "obbdir", 0},
    {"/sdcard/Android/obb/" VFS_PACKAGE, "obbdir", 0},
    {"/storage/emulated/0/Android/obb/" VFS_PACKAGE, "obbdir", 0},
};

// Reported to the guest as an ARMv7 CPU so Unity picks its NEON/VFPv3 paths.
static const char g_cpuinfo[] =
    "Processor\t: ARMv7 Processor rev 1 (v7l)\n"
    "processor\t: 0\n"
    "processor\t: 1\n"
    "processor\t: 2\n"
    "BogoMIPS\t: 38.40\n"
    "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\n"
    "CPU implementer\t: 0x41\n"
    "CPU architecture: 7\n"
    "CPU variant\t: 0x1\n"
    "CPU part\t: 0xd07\n"
    "CPU revision\t: 1\n"
    "\n"
    "Hardware\t: Tegra X1\n";

static int mkdir_host(const char *rel)
{
    char path[VFS_PATH_MAX + 32];
    if (snprintf(path, sizeof path, "%s/%s", g_root, rel) >= (int)sizeof path)
        return -ENAMETOOLONG;
    if (mkdir(path, 0777) != 0 && errno != EEXIST)
        return -errno;
    return 0;
}

int vfs_init(const char *host_root)
{
    if (strlen(host_root) >= sizeof g_root)
        return -ENAMETOOLONG;
    strcpy(g_root, host_root);

    static const char *const dirs[] = {"data",           "data/app",      "data/app/files", "data/app/cache", "data/ext",
                                       "data/ext/files", "data/ext/cache", "data/proc",      "data/proc/self", "obbdir",
                                       "data/ext/files/DeviceFileCache"};
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        int r = mkdir_host(dirs[i]);
        if (r)
            return r;
    }

    char path[VFS_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/data/proc/cpuinfo", g_root);
    FILE *f = fopen(path, "wb");
    if (!f)
        return -errno;
    fwrite(g_cpuinfo, 1, sizeof g_cpuinfo - 1, f);
    fclose(f);

    // Unity's SystemInfo.systemMemorySize reads this.  Without it the game picks its low-end
    // hardware profile on the first query (OptimisationManager.GetCurrentHardware).
    static const char meminfo[] = "MemTotal:        3670016 kB\n"
                                  "MemFree:         2097152 kB\n"
                                  "Buffers:               0 kB\n"
                                  "Cached:                0 kB\n";
    snprintf(path, sizeof path, "%s/data/proc/meminfo", g_root);
    f = fopen(path, "wb");
    if (!f)
        return -errno;
    fwrite(meminfo, 1, sizeof meminfo - 1, f);
    fclose(f);

    // Mono exits unless this file exists.  One well-formed line suits any parser.
    static const char maps[] = "00001000-00002000 r--p 00000000 00:00 0 [port]\n";
    snprintf(path, sizeof path, "%s/data/proc/self/maps", g_root);
    f = fopen(path, "wb");
    if (!f)
        return -errno;
    fwrite(maps, 1, sizeof maps - 1, f);
    fclose(f);

    snprintf(path, sizeof path, "%s/data/proc/null", g_root);
    f = fopen(path, "wb");
    if (!f)
        return -errno;
    fclose(f);

    // Read as a seed source.  Fixed contents are fine: nothing here needs secrecy.
    snprintf(path, sizeof path, "%s/data/proc/random", g_root);
    f = fopen(path, "wb");
    if (!f)
        return -errno;
    for (unsigned i = 0, v = 0x2F6E2B1u; i < 4096; i++) {
        v = v * 1664525u + 1013904223u;
        fputc((int)(v >> 24), f);
    }
    fclose(f);

    // Boehm counts "cpu<N>" lines here; three is what the game may use on Horizon.
    // It rejects anything under 100 bytes as a failed read, so the usual trailer stays.
    static const char stat_text[] =
        "cpu  0 0 0 0\ncpu0 0 0 0 0\ncpu1 0 0 0 0\ncpu2 0 0 0 0\n"
        "intr 0\nctxt 0\nbtime 0\nprocesses 1\nprocs_running 1\nprocs_blocked 0\n";
    snprintf(path, sizeof path, "%s/data/proc/stat", g_root);
    f = fopen(path, "wb");
    if (!f)
        return -errno;
    fwrite(stat_text, 1, sizeof stat_text - 1, f);
    fclose(f);
    return 0;
}

// Removes "", "." and ".." components.  ".." above the top is rejected rather than
// clamped so a guest path can never climb out of a mapped directory.
static int normalize(const char *in, char *out, size_t size)
{
    if (in[0] != '/')
        return -ENOENT;
    size_t len = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/')
            p++;
        const char *e = p;
        while (*e && *e != '/')
            e++;
        size_t n = (size_t)(e - p);
        if (n == 0)
            break;
        if (n == 1 && p[0] == '.') {
            // skip
        } else if (n == 2 && p[0] == '.' && p[1] == '.') {
            if (len == 0)
                return -ENOENT;
            while (len > 0 && out[--len] != '/')
                ;
        } else {
            if (len + 1 + n + 1 > size)
                return -ENAMETOOLONG;
            out[len++] = '/';
            memcpy(out + len, p, n);
            len += n;
        }
        p = e;
    }
    if (len == 0)
        out[len++] = '/';
    out[len] = '\0';
    return 0;
}

// Finds a directory entry ignoring case and appends its real name to path.
static int append_ci(char *path, size_t size, const char *name, size_t name_len)
{
    DIR *d = opendir(path);
    if (!d)
        return -ENOENT;
    size_t len = strlen(path);
    int result = -ENOENT;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strlen(e->d_name) == name_len && strncasecmp(e->d_name, name, name_len) == 0) {
            if (len + 1 + name_len + 1 > size) {
                result = -ENAMETOOLONG;
                break;
            }
            path[len] = '/';
            memcpy(path + len + 1, e->d_name, name_len + 1);
            result = 0;
            break;
        }
    }
    closedir(d);
    return result;
}

// Case fallback for relative part `rel` under `base`.  Only taken after the exact
// path missed: the SD card's FAT is case-insensitive, but exact-case lookups are
// what stays fast, and this also protects the romfs and host-test filesystems,
// which are case-sensitive.
static int resolve_case(char *path, size_t size, size_t base_len, int must_exist)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return 0;

    char rel[VFS_PATH_MAX];
    if (strlen(path + base_len) >= sizeof rel)
        return -ENAMETOOLONG;
    strcpy(rel, path + base_len);
    path[base_len] = '\0';

    const char *p = rel;
    while (*p) {
        while (*p == '/')
            p++;
        const char *e = p;
        while (*e && *e != '/')
            e++;
        size_t n = (size_t)(e - p);
        if (n == 0)
            break;
        int last = (*e == '\0');
        int r = append_ci(path, size, p, n);
        if (r == -ENOENT && last && !must_exist) {
            size_t len = strlen(path);
            if (len + 1 + n + 1 > size)
                return -ENAMETOOLONG;
            path[len] = '/';
            memcpy(path + len + 1, p, n + 1);
            return 0;
        }
        if (r)
            return r;
        p = e;
    }
    return 0;
}

int vfs_resolve(const char *guest, char *host, size_t host_size, int must_exist)
{
    char norm[VFS_PATH_MAX];
    int r = normalize(guest, norm, sizeof norm);
    if (r)
        return r;

    for (size_t i = 0; i < sizeof g_rules / sizeof *g_rules; i++) {
        const struct rule *rule = &g_rules[i];
        size_t n = strlen(rule->guest);
        if (strncmp(norm, rule->guest, n) != 0)
            continue;
        const char *tail = norm + n;
        if (*tail != '\0' && (*tail != '/' || rule->is_file))
            continue;

        int base = snprintf(host, host_size, "%s/%s", g_root, rule->host);
        if (base < 0 || (size_t)base >= host_size)
            return -ENAMETOOLONG;
        if (*tail == '\0') {
            struct stat st;
            return (!must_exist || stat(host, &st) == 0) ? 0 : -ENOENT;
        }
        if ((size_t)base + strlen(tail) + 1 > host_size)
            return -ENAMETOOLONG;
        strcpy(host + base, tail);
        return resolve_case(host, host_size, (size_t)base, must_exist);
    }
    return -ENOENT;
}
