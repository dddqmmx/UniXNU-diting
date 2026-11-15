/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Included by kern_exec.c after the UUID-bound SystemOS/Data mount helpers.
 * First installation only, before PID 1 runs; uses the real guest APFS writer.
 */
#ifndef DITING_GPU_RUNTIME_INSTALL_H
#define DITING_GPU_RUNTIME_INSTALL_H
#include "DitingGpuRuntimeArchive.h"
#include <libkern/zlib.h>

extern const uint8_t DitingGpuRuntimeData[], DitingGpuRuntimeEnd[];
volatile int diting_gpu_runtime_stage, diting_gpu_runtime_error;
volatile unsigned diting_gpu_runtime_written;
#define DGR_EXT "/System/Library/Extensions/"
#define DGR_BUNDLE DGR_EXT "DitingAdrenoMetal.bundle"
#define DGR_TEMP DGR_BUNDLE ".diting-stage"
#define DGR_ID "/.diting-native-gpu-runtime-v1"
static const char *const dgr_names[DitingGpuRuntimeFiles] = {
    "Info.plist", "DitingAdrenoMetal", "DitingMetalBridge.dylib", "libvulkan_freedreno.dylib",
    DGR_EXT "DitingAdrenoMetal.clients", DGR_EXT "DitingAdrenoMetal.publish",
    "/diting/gpu-capture18", "/System/Library/LaunchDaemons/dev.unixnu.diting.gpu-capture18.plist"
};
static const uint32_t dgr_modes[DitingGpuRuntimeFiles] = {0644, 0755, 0755, 0755, 0644, 0644, 0755, 0644};

static int dgr_stat(const char *path, enum vtype type, uint64_t size, uint32_t mode)
{
    vnode_t vp = NULLVP;
    vfs_context_t ctx = vfs_context_current();
    int error = vnode_lookup(path, VNODE_LOOKUP_NOFOLLOW_ANY, &vp, ctx);
    if (error) return error;
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_uid); VATTR_WANTED(&va, va_gid); VATTR_WANTED(&va, va_data_size); VATTR_WANTED(&va, va_mode);
    error = vnode_getattr(vp, &va, ctx);
    if (!error && (vnode_vtype(vp) != type || vnode_mount(vp) != vnode_mount(rootvnode) ||
        va.va_uid != 0 || va.va_gid != 0 || (mode && (va.va_data_size != size || (va.va_mode & 07777) != mode)))) error = EINVAL;
    vnode_put(vp);
    return error;
}

static int dgr_id_equal(const char *path, const uint8_t *id)
{
    if (dgr_stat(path, VREG, 16, 0644)) return 0;
    vnode_t vp = NULLVP;
    vfs_context_t ctx = vfs_context_current();
    if (vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx)) return 0;
    uint8_t old[16]; int resid = 0;
    int error = vn_rdwr(UIO_READ, vp, (caddr_t)old, sizeof(old), 0, UIO_SYSSPACE,
        IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
    int close_error = vnode_close(vp, FREAD, ctx);
    return !error && !close_error && !resid && !memcmp(old, id, sizeof(old));
}

static voidpf dgr_zalloc(voidpf opaque __unused, uInt items, uInt size)
{
    if (size && items > (1u << 20) / size) return Z_NULL;
    size_t bytes = (size_t)items * size + 16;
    uint8_t *p = kalloc_data(bytes, Z_WAITOK | Z_ZERO);
    if (!p) return Z_NULL;
    memcpy(p, &bytes, sizeof(bytes));
    return p + 16;
}
static void dgr_zfree(voidpf opaque __unused, voidpf address)
{
    if (!address) return;
    uint8_t *p = (uint8_t *)address - 16;
    size_t bytes = 0; memcpy(&bytes, p, sizeof(bytes));
    kfree_data(p, bytes);
}
static int dgr_inflate(const struct DitingGpuRuntimeEntry *entry, uint8_t *data)
{
    z_stream stream; memset(&stream, 0, sizeof(stream));
    stream.zalloc = dgr_zalloc; stream.zfree = dgr_zfree;
    stream.next_in = (Bytef *)(uintptr_t)entry->packed; stream.avail_in = (uInt)entry->packed_bytes;
    stream.next_out = data; stream.avail_out = (uInt)entry->bytes;
    if (inflateInit(&stream) != Z_OK) return ENOMEM;
    int result = inflate(&stream, Z_FINISH);
    int complete = result == Z_STREAM_END && stream.total_out == entry->bytes && !stream.avail_in;
    inflateEnd(&stream);
    return complete ? 0 : EINVAL;
}

/* O_EXCL preserves an existing file. A previous interrupted staging write is
 * accepted only when its complete content and ownership match this candidate. */
static int dgr_unlink(proc_t p, user_addr_t scratch, const char *path)
{
    struct unlink_args request = {.path = scratch + 0xc00}; int32_t result = 0;
    int error = copyout(path, request.path, strlen(path) + 1);
    if (!error) error = unlink(p, &request, &result);
    return error;
}
static int dgr_write_new(proc_t p, user_addr_t scratch, const char *path,
    const uint8_t *data, uint64_t bytes, uint32_t mode, int repair_stage)
{
    vfs_context_t ctx = vfs_context_current(); vnode_t vp = NULLVP;
    int error = vnode_open(path, O_CREAT | O_EXCL | O_NOFOLLOW | FWRITE, mode, 0, &vp, ctx);
    if (error == EEXIST) {
        error = dgr_stat(path, VREG, 0, 0);
        if (error) return error;
        error = dgr_stat(path, VREG, bytes, mode);
        if (error) {
            if (!repair_stage) return error;
            error = dgr_unlink(p, scratch, path);
            return error ? error : dgr_write_new(p, scratch, path, data, bytes, mode, 0);
        }
        error = vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);
        if (error) return error;
        size_t chunk = bytes < 65536 ? (size_t)bytes : 65536;
        uint8_t *buffer = kalloc_data(chunk, Z_WAITOK);
        if (!buffer) error = ENOMEM;
        for (uint64_t at = 0; !error && at < bytes; at += chunk) {
            int amount = (int)(bytes - at < chunk ? bytes - at : chunk), resid = 0;
            error = vn_rdwr(UIO_READ, vp, (caddr_t)buffer, amount, (off_t)at, UIO_SYSSPACE,
                IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
            if (!error && (resid || memcmp(buffer, data + at, amount))) error = EEXIST;
        }
        if (buffer) kfree_data(buffer, chunk);
        int closed = vnode_close(vp, FREAD, ctx);
        if (error == EEXIST && !closed && repair_stage) {
            error = dgr_unlink(p, scratch, path);
            return error ? error : dgr_write_new(p, scratch, path, data, bytes, mode, 0);
        }
        return error ? error : closed;
    }
    if (error) return error;
    int resid = 0;
    error = vn_rdwr(UIO_WRITE, vp, (caddr_t)(uintptr_t)data, (int)bytes, 0, UIO_SYSSPACE,
        IO_NOAUTH | IO_UNIT, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
    if (!error && resid) error = EIO;
    if (!error) {
        struct vnode_attr va; VATTR_INIT(&va);
        VATTR_SET(&va, va_uid, 0); VATTR_SET(&va, va_gid, 0); VATTR_SET(&va, va_mode, mode);
        error = vnode_setattr(vp, &va, ctx);
    }
    if (!error) error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
    int closed = vnode_close(vp, FWRITE, ctx);
    if (error || closed) (void)dgr_unlink(p, scratch, path);
    if (!error && !closed) ++diting_gpu_runtime_written;
    return error ? error : closed;
}
static int dgr_mkdir(proc_t p, user_addr_t scratch, const char *path)
{
    int error = dgr_stat(path, VDIR, 0, 0);
    if (!error) return 0;
    if (error != ENOENT) return error;
    struct mkdir_args request = {.path = scratch + 0xc00, .mode = 0755}; int32_t result = 0;
    error = copyout(path, request.path, strlen(path) + 1);
    if (!error) error = mkdir(p, &request, &result);
    return error ? error : dgr_stat(path, VDIR, 0, 0);
}
static int dgr_rename(proc_t p, user_addr_t scratch, const char *from, const char *to)
{
    struct rename_args request = {.from = scratch + 0xc00, .to = scratch + 0xd00}; int32_t result = 0;
    int error = copyout(from, request.from, strlen(from) + 1);
    if (!error) error = copyout(to, request.to, strlen(to) + 1);
    if (!error) error = rename(p, &request, &result);
    if (!error) {
        char parent[256]; strlcpy(parent, to, sizeof(parent));
        char *slash = strrchr(parent, '/');
        if (!slash || slash == parent) return EINVAL;
        *slash = 0;
        vnode_t vp = NULLVP; vfs_context_t ctx = vfs_context_current();
        error = vnode_lookup(parent, VNODE_LOOKUP_NOFOLLOW_ANY, &vp, ctx);
        if (!error) { error = VNOP_FSYNC(vp, MNT_WAIT, ctx); vnode_put(vp); }
    }
    return error;
}
static int dgr_upgrade(proc_t p, user_addr_t scratch)
{
    mount_t mp = vnode_mount(rootvnode); dev_t selected = NODEV;
    char device[64];
    if (!native_diting_ufs_volume_device(XNU_OWNER_ROOT_VOLUME_UUID, 0, &selected, device, sizeof(device)) ||
        selected == NODEV || selected != native_owner_cache_bound_root_device) return EAUTH;
    if (!(vfs_flags(mp) & MNT_RDONLY)) return 0;
    uint8_t payload[0x150] = {0}; user64_addr_t spec = scratch + 0x900;
    uint16_t mode = 1; uint32_t anonymous_owner = 99;
    memcpy(payload, &spec, sizeof(spec)); memcpy(payload + 16, &mode, sizeof(mode));
    memcpy(payload + 0x144, &anonymous_owner, 4); memcpy(payload + 0x148, &anonymous_owner, 4);
    struct mount_args request = {.type = scratch + 0x800, .path = scratch + 0x820,
        .flags = MNT_UPDATE, .data = scratch + 0xa00}; int32_t result = 0;
    int error = copyout("apfs", request.type, 5);
    if (!error) error = copyout("/", request.path, 2);
    if (!error) error = copyout(device, spec, strlen(device) + 1);
    if (!error) error = copyout(payload, request.data, sizeof(payload));
    if (!error) error = mount(p, &request, &result);
    if (!error && (vnode_mount(rootvnode) != mp || (vfs_flags(mp) & MNT_RDONLY))) error = EROFS;
    return error;
}

static int dgr_install(proc_t p, user_addr_t scratch)
{
    const uint8_t *archive = DitingGpuRuntimeData;
    uintptr_t start = (uintptr_t)archive, end = (uintptr_t)DitingGpuRuntimeEnd;
    struct DitingGpuRuntimeEntry entries[DitingGpuRuntimeFiles];
    diting_gpu_runtime_stage = 1;
    if (end < start || !diting_gpu_runtime_parse(archive, end - start, entries)) return EINVAL;
    int rootful = 0; PE_parse_boot_argn("diting_rootful", &rootful, sizeof(rootful));
    if (rootful != 1 || !rootvnode || native_owner_data_status || native_owner_data_readonly ||
        native_owner_cache_status || !native_owner_cache_authorized ||
        !native_owner_cache_root_recheck || native_owner_cache_current_root != (uintptr_t)rootvnode ||
        strcmp(vfs_statfs(vnode_mount(rootvnode))->f_fstypename, "apfs")) return EAUTH;
    int error = dgr_stat("/System/Library/Extensions", VDIR, 0, 0);
    if (!error) error = dgr_stat("/diting", VDIR, 0, 0);
    if (!error) error = dgr_stat("/System/Library/LaunchDaemons", VDIR, 0, 0);
    if (error) return error;
    int bundle = dgr_stat(DGR_BUNDLE, VDIR, 0, 0);
    if (bundle && bundle != ENOENT) return bundle;
    int owned = !bundle && !dgr_stat(DGR_BUNDLE DGR_ID, VREG, 16, 0644);
    int installed = owned && dgr_id_equal(DGR_BUNDLE DGR_ID, archive + 32);
    if (!bundle && !owned) return EEXIST;
    if (installed) {
        unsigned present = 0; char path[256];
        for (unsigned i = 0; i < DitingGpuRuntimeFiles; ++i) {
            if (i < 4) snprintf(path, sizeof(path), DGR_BUNDLE "/%s", dgr_names[i]);
            else strlcpy(path, dgr_names[i], sizeof(path));
            int found = dgr_stat(path, VREG, entries[i].bytes, dgr_modes[i]);
            if (i < 4 && found) return found;
            if (!found) ++present;
        }
        if (present == DitingGpuRuntimeFiles) { diting_gpu_runtime_stage = 6; return 0; }
    }
    /* Validate every stream before requesting a write mount or creating files. */
    diting_gpu_runtime_stage = 2;
    uint8_t *raw[DitingGpuRuntimeFiles] = {0};
    for (unsigned i = 0; i < DitingGpuRuntimeFiles; ++i) {
        raw[i] = kalloc_data((size_t)entries[i].bytes, Z_WAITOK);
        if (!raw[i]) { error = ENOMEM; goto done; }
        error = dgr_inflate(entries + i, raw[i]);
        if (error) goto done;
    }
    if (entries[4].bytes != 2 || memcmp(raw[4], "*\n", 2) ||
        entries[5].bytes != 2 || memcmp(raw[5], "1\n", 2)) { error = EINVAL; goto done; }
    for (unsigned i = 1; i < 4; ++i) {
        if (entries[i].bytes < 32 || diting_gpu_runtime_u32(raw[i]) != 0xfeedfacf ||
            diting_gpu_runtime_u32(raw[i] + 4) != 0x0100000c) { error = EINVAL; goto done; }
    }
    diting_gpu_runtime_stage = 3;
    error = dgr_upgrade(p, scratch);
    if (error) goto done;
    if (bundle == ENOENT) {
        error = dgr_mkdir(p, scratch, DGR_TEMP);
        if (error) goto done;
        error = dgr_write_new(p, scratch, DGR_TEMP DGR_ID, archive + 32, 16, 0644, 0);
        if (error) goto done;
        for (unsigned i = 0; i < 4; ++i) {
            char path[256]; snprintf(path, sizeof(path), DGR_TEMP "/%s", dgr_names[i]);
            error = dgr_write_new(p, scratch, path, raw[i], entries[i].bytes, dgr_modes[i], 1);
            if (error) goto done;
        }
    } else if (!installed) {
        error = dgr_write_new(p, scratch, DGR_BUNDLE DGR_ID ".diting-gpu-stage", archive + 32, 16, 0644, 1);
        if (error) goto done;
        for (unsigned i = 0; i < 4; ++i) {
            char target[256], path[256];
            snprintf(target, sizeof(target), DGR_BUNDLE "/%s", dgr_names[i]);
            snprintf(path, sizeof(path), DGR_BUNDLE "/%s.diting-gpu-stage", dgr_names[i]);
            if (!dgr_stat(target, VREG, entries[i].bytes, dgr_modes[i]) &&
                !dgr_write_new(p, scratch, target, raw[i], entries[i].bytes, dgr_modes[i], 0))
                continue;
            error = dgr_write_new(p, scratch, path, raw[i], entries[i].bytes, dgr_modes[i], 1);
            if (error) goto done;
        }
    }
    diting_gpu_runtime_stage = 4;
    for (unsigned i = 4; i < DitingGpuRuntimeFiles; ++i) {
        char path[256]; snprintf(path, sizeof(path), "%s.diting-gpu-stage", dgr_names[i]);
        error = dgr_write_new(p, scratch, path, raw[i], entries[i].bytes, dgr_modes[i], 1);
        if (error) goto done;
    }
    if (bundle == ENOENT) {
        error = dgr_rename(p, scratch, DGR_TEMP, DGR_BUNDLE);
        if (error) goto done;
    } else if (!installed) {
        for (unsigned i = 0; i < 4; ++i) {
            char target[256], path[256];
            snprintf(target, sizeof(target), DGR_BUNDLE "/%s", dgr_names[i]);
            snprintf(path, sizeof(path), DGR_BUNDLE "/%s.diting-gpu-stage", dgr_names[i]);
            int staged = dgr_stat(path, VREG, entries[i].bytes, dgr_modes[i]);
            if (staged == ENOENT) continue;
            if (staged) { error = staged; goto done; }
            error = dgr_rename(p, scratch, path, target);
            if (error) goto done;
        }
    }
    diting_gpu_runtime_stage = 5;
    /* The publication flag is committed after the complete bundle and helpers. */
    static const unsigned order[] = {6, 7, 4, 5};
    for (unsigned n = 0; n < sizeof(order) / sizeof(order[0]); ++n) {
        unsigned i = order[n]; char path[256];
        snprintf(path, sizeof(path), "%s.diting-gpu-stage", dgr_names[i]);
        int existing = dgr_stat(dgr_names[i], VREG, entries[i].bytes, dgr_modes[i]);
        if (!existing) {
            error = dgr_write_new(p, scratch, dgr_names[i], raw[i], entries[i].bytes, dgr_modes[i], 0);
            if (!error) {
                error = dgr_unlink(p, scratch, path);
                if (error) goto done;
                continue;
            }
            if (error != EEXIST || !owned) goto done;
        } else if (existing != ENOENT && !(owned && existing == EINVAL && !dgr_stat(dgr_names[i], VREG, 0, 0))) {
            error = existing; goto done;
        }
        error = dgr_rename(p, scratch, path, dgr_names[i]);
        if (error) goto done;
    }
    if (bundle != ENOENT && !installed) {
        error = dgr_rename(p, scratch, DGR_BUNDLE DGR_ID ".diting-gpu-stage", DGR_BUNDLE DGR_ID);
        if (error) goto done;
    }
    diting_gpu_runtime_stage = 6;
done:
    for (unsigned i = 0; i < DitingGpuRuntimeFiles; ++i)
        if (raw[i]) kfree_data(raw[i], (size_t)entries[i].bytes);
    return error;
}
static void diting_gpu_runtime_prepare(proc_t p, user_addr_t scratch)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("diting_gpu_runtime_install", &enabled, sizeof(enabled)) || enabled != 1) return;
    int error = dgr_install(p, scratch);
    diting_gpu_runtime_error = error;
    printf("DITING GPU runtime: %s stage=%d error=%d files-written=%u; hardware/performance untested\n",
        error ? "SKIP" : "PASS", diting_gpu_runtime_stage, error, diting_gpu_runtime_written);
}
#undef DGR_EXT
#undef DGR_BUNDLE
#undef DGR_TEMP
#undef DGR_ID
#endif
