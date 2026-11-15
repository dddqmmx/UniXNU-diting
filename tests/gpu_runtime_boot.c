/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Real POSIX files behind the kernel VFS calls; no GPU or device simulation.
 * Tests refusal before writes, publication order and interrupted installation.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <assert.h>
#define __unused __attribute__((unused))
#define EAUTH 80
#define FREAD 1
#define FWRITE 2
#define MNT_RDONLY 1
#define MNT_WAIT 0
#define MNT_UPDATE 0x10000
#define Z_WAITOK 0
#define Z_ZERO 1
#define IO_NOAUTH 0
#define IO_UNIT 0
#define UIO_READ 0
#define UIO_WRITE 1
#define UIO_SYSSPACE 0
#define NODEV ((dev_t)-1)
#define VNODE_LOOKUP_NOFOLLOW_ANY 8
#define NULLVP NULL
#define XNU_OWNER_ROOT_VOLUME_UUID "test-os"
enum vtype { VREG, VDIR, VLNK };
struct fs_info { char f_fstypename[16]; };
struct test_mount { unsigned flags; struct fs_info info; };
struct test_vnode { int fd; enum vtype type; struct test_mount *mp; };
typedef struct test_mount *mount_t;
typedef struct test_vnode *vnode_t;
typedef void *proc_t;
typedef uintptr_t user_addr_t;
typedef uintptr_t user64_addr_t;
typedef void *vfs_context_t;
struct vnode_attr { uint64_t va_data_size; unsigned va_uid, va_gid, va_mode; };
#define VATTR_INIT(v) memset(v, 0, sizeof(*(v)))
#define VATTR_WANTED(v, key) ((void)(v))
#define VATTR_SET(v, key, val) ((v)->key = (val))
static struct test_mount root_mount = {MNT_RDONLY, {"apfs"}};
static struct test_vnode root_node = {-1, VDIR, &root_mount};
static vnode_t rootvnode = &root_node;
static int native_owner_cache_status, native_owner_cache_authorized = 1, native_owner_cache_root_recheck = 1;
static int native_owner_data_status, native_owner_data_readonly;
static uintptr_t native_owner_cache_current_root;
static dev_t native_owner_cache_bound_root_device = 1;
static unsigned mounts, writes, allocations, fail_write, rootful = 1, wrong_device;
static const char *fixture_root;
static vfs_context_t vfs_context_current(void) { return (void *)1; }
static proc_t vfs_context_proc(vfs_context_t c) { return c; }
static void *vfs_context_ucred(vfs_context_t c) { return c; }
static mount_t vnode_mount(vnode_t v) { return v->mp; }
static enum vtype vnode_vtype(vnode_t v) { return v->type; }
static unsigned vfs_flags(mount_t m) { return m->flags; }
static struct fs_info *vfs_statfs(mount_t m) { return &m->info; }
static void *kalloc_data(size_t n, int flags) { ++allocations; return flags & Z_ZERO ? calloc(1, n) : malloc(n); }
static void kfree_data(void *p, size_t n) { (void)n; free(p); }
static size_t host_strlcpy(char *d, const char *s, size_t n) {
    size_t length = strlen(s); if (n) snprintf(d, n, "%s", s); return length;
}
static int host_path(const char *path, char out[1024]) {
    assert(path[0] == '/');
    if (snprintf(out, 1024, "%s%s", fixture_root, path) >= 1024) return ENAMETOOLONG;
    /* Apply NOFOLLOW_ANY to every component, including the final one. */
    for (char *at = out + strlen(fixture_root); *at; ++at) {
        if (*at != '/' || at == out + strlen(fixture_root)) continue;
        *at = 0; struct stat st; int result = lstat(out, &st); *at = '/';
        if (result) return errno;
        if (S_ISLNK(st.st_mode)) return ELOOP;
    }
    struct stat st;
    if (!lstat(out, &st) && S_ISLNK(st.st_mode)) return ELOOP;
    return 0;
}
static int vnode_lookup(const char *path, int flags, vnode_t *out, vfs_context_t ctx) {
    (void)flags; (void)ctx; char host[1024]; int error = host_path(path, host);
    if (error) return error;
    int fd = open(host, O_RDONLY | O_NOFOLLOW); if (fd < 0) return errno;
    struct stat st; if (fstat(fd, &st)) { error = errno; close(fd); return error; }
    *out = calloc(1, sizeof(**out)); assert(*out); (*out)->fd = fd;
    (*out)->type = S_ISDIR(st.st_mode) ? VDIR : VREG; (*out)->mp = &root_mount; return 0;
}
static void vnode_put(vnode_t v) { close(v->fd); free(v); }
static int vnode_getattr(vnode_t v, struct vnode_attr *a, vfs_context_t c) {
    (void)c; struct stat st; if (fstat(v->fd, &st)) return errno;
    a->va_uid = a->va_gid = 0; a->va_mode = st.st_mode & 07777; a->va_data_size = st.st_size; return 0;
}
static int vnode_open(const char *path, int flags, int mode, int unused, vnode_t *out, vfs_context_t ctx) {
    (void)unused; (void)ctx; char host[1024]; int error = host_path(path, host); if (error) return error;
    if ((flags & FWRITE) && (root_mount.flags & MNT_RDONLY)) return EROFS;
    int options = flags & ~(FREAD | FWRITE);
    options |= flags & FWRITE ? O_WRONLY : O_RDONLY;
    int fd = open(host, options, mode); if (fd < 0) return errno;
    *out = calloc(1, sizeof(**out)); assert(*out); (*out)->fd = fd; (*out)->type = VREG; (*out)->mp = &root_mount; return 0;
}
static int vnode_close(vnode_t v, int flags, vfs_context_t c) {
    (void)flags; (void)c; int result = close(v->fd), error = result ? errno : 0; free(v); return error;
}
static int vn_rdwr(int operation, vnode_t v, char *data, int bytes, off_t offset,
    int space, int flags, void *cred, int *resid, proc_t p) {
    (void)space; (void)flags; (void)cred; (void)p;
    if (operation == UIO_WRITE && ++writes == fail_write) {
        ssize_t done = pwrite(v->fd, data, bytes / 2, offset); *resid = bytes - (done < 0 ? 0 : done); return EIO;
    }
    ssize_t done = operation == UIO_READ ? pread(v->fd, data, bytes, offset) : pwrite(v->fd, data, bytes, offset);
    *resid = bytes - (done < 0 ? 0 : done); return done < 0 ? errno : 0;
}
static int vnode_setattr(vnode_t v, struct vnode_attr *a, vfs_context_t c) { (void)c; return fchmod(v->fd, a->va_mode) ? errno : 0; }
static int host_fsync(vnode_t v, int f, vfs_context_t c) { (void)f; (void)c; return fsync(v->fd) ? errno : 0; }
#define VNOP_FSYNC host_fsync
struct mkdir_args { user_addr_t path; int mode; };
struct rename_args { user_addr_t from, to; };
struct unlink_args { user_addr_t path; };
struct mount_args { user_addr_t type, path; int flags; user_addr_t data; };
static int copyout(const void *source, user_addr_t target, size_t size) { memcpy((void *)target, source, size); return 0; }
static int host_mkdir(proc_t p, struct mkdir_args *r, int32_t *value) {
    (void)p; (void)value; char path[1024]; int error = host_path((const char *)r->path, path);
    if (error) return error; return mkdir(path, r->mode) ? errno : 0;
}
static int host_rename(proc_t p, struct rename_args *r, int32_t *value) {
    (void)p; (void)value; char from[1024], to[1024]; int error = host_path((const char *)r->from, from);
    if (!error) error = host_path((const char *)r->to, to); if (error) return error;
    /* Publication is never allowed before all three binary files are complete. */
    if (strstr(to, "DitingAdrenoMetal.publish")) {
        static const char *names[] = {"DitingAdrenoMetal", "DitingMetalBridge.dylib", "libvulkan_freedreno.dylib"};
        for (unsigned i = 0; i < 3; ++i) {
            char library[1024]; snprintf(library, sizeof(library), "%s/System/Library/Extensions/DitingAdrenoMetal.bundle/%s", fixture_root, names[i]);
            struct stat st; assert(!stat(library, &st) && st.st_size >= 32);
        }
    }
    return rename(from, to) ? errno : 0;
}
static int host_unlink(proc_t p, struct unlink_args *r, int32_t *value) {
    (void)p; (void)value; char path[1024]; int error = host_path((const char *)r->path, path);
    if (error) return error; return unlink(path) ? errno : 0;
}
static int host_mount(proc_t p, struct mount_args *r, int32_t *value) {
    (void)p; (void)value; assert(r->flags == MNT_UPDATE); assert(!strcmp((const char *)r->type, "apfs"));
    assert(!strcmp((const char *)r->path, "/")); ++mounts; root_mount.flags &= ~MNT_RDONLY; return 0;
}
static int native_diting_ufs_volume_device(const char *uuid, int cache, dev_t *dev, char *path, size_t bytes) {
    assert(!strcmp(uuid, "test-os") && !cache); *dev = wrong_device ? 2 : 1; snprintf(path, bytes, "/dev/test-os"); return 1;
}
static int PE_parse_boot_argn(const char *name, void *value, size_t bytes) {
    assert(bytes == sizeof(unsigned)); unsigned number = !strcmp(name, "diting_rootful") ? rootful : 1;
    memcpy(value, &number, bytes); return 1;
}
#define mkdir host_mkdir
#define strlcpy host_strlcpy
#define rename host_rename
#define unlink host_unlink
#define mount host_mount
#include "DitingGpuRuntimeInstall.h"
int main(int argc, char **argv) {
    assert(argc == 3); fixture_root = argv[1]; native_owner_cache_current_root = (uintptr_t)rootvnode;
    if (!strcmp(argv[2], "rootful-off")) rootful = 0;
    if (!strcmp(argv[2], "wrong-device")) wrong_device = 1;
    if (!strcmp(argv[2], "fail-write")) fail_write = 3;
    if (!strcmp(argv[2], "wrong-fs")) snprintf(root_mount.info.f_fstypename, 16, "foreign");
    uint8_t scratch[4096]; memset(scratch, 0x5a, sizeof(scratch));
    diting_gpu_runtime_prepare(NULL, (uintptr_t)scratch);
    for (unsigned i = 0x400; i < 0x800; ++i) assert(scratch[i] == 0x5a); /* dyld's prepared environment */
    printf("{\"error\":%d,\"stage\":%d,\"mounts\":%u,\"writes\":%u,\"allocations\":%u}\n",
        diting_gpu_runtime_error, diting_gpu_runtime_stage, mounts, writes, allocations);
    return 0;
}
