// Guest (Android) path -> host (Horizon SD card) mapping.
//
// Unity reads game data through the original zip containers ("<apk>/assets/..",
// "<obb>/assets/.."), so the archives stay intact and only their paths are mapped.
// Anything outside the mapped prefixes fails with ENOENT: the guest must not see
// or write the rest of the SD card.
#ifndef PORT_VFS_H
#define PORT_VFS_H

#include <stddef.h>

#define VFS_PACKAGE   "com.activision.callofduty.striketeam"
#define VFS_OBB_NAME  "main.15." VFS_PACKAGE ".obb"
#define VFS_GUEST_APK "/data/app/" VFS_PACKAGE "-1.apk"
#define VFS_GUEST_OBB "/mnt/sdcard/Android/obb/" VFS_PACKAGE "/" VFS_OBB_NAME
#define VFS_GUEST_DATA "/data/data/" VFS_PACKAGE
#define VFS_GUEST_EXT  "/mnt/sdcard/Android/data/" VFS_PACKAGE

#define VFS_PATH_MAX 512

// Sets the host root and creates the writable directories.  Returns 0 or -errno.
int vfs_init(const char *host_root);

// Maps a guest path to a host path, returns 0 or -errno (native errno values).
// must_exist=0 allows a final component that does not exist yet (create/rename).
int vfs_resolve(const char *guest, char *host, size_t host_size, int must_exist);

#endif
