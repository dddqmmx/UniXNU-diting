/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * UI-only update before PID 1. Reuses the verified guest APFS stat, inflate,
 * mount and rename helpers; GPU files, identity and counters are untouched.
 */
#ifndef DITING_UI_RUNTIME_INSTALL_H
#define DITING_UI_RUNTIME_INSTALL_H
#include "DitingUiRuntimeArchive.h"

extern const uint8_t DitingUiRuntimeData[], DitingUiRuntimeEnd[];
volatile int diting_ui_runtime_stage, diting_ui_runtime_error;
volatile unsigned diting_ui_runtime_written;
#define DUI_LIBRARY "/diting/libditing-ui18.dylib"
#define DUI_STAGE DUI_LIBRARY ".ui-brightness-stage"
#define DUI_BACKUP_PREFIX DUI_LIBRARY ".before-ui-"
#define DUI_ID "/diting/.diting-ui-brightness-20261115"
#define DUI_ID_STAGE DUI_ID ".new"

static int dui_equal(const char *path, const uint8_t *data, uint64_t bytes, uint32_t mode)
{
    int error = dgr_stat(path, VREG, 0, 0);
    if (error) return error;
    if (dgr_stat(path, VREG, bytes, mode)) return EEXIST;
    vnode_t vp = NULLVP; vfs_context_t ctx = vfs_context_current();
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
    return error ? error : closed;
}

/* Only this candidate's staging paths can be repaired. The original backup
 * and any destination supplied by callers are preserved on a mismatch. */
static int dui_write_new(proc_t p, user_addr_t scratch, const char *path,
    const uint8_t *data, uint64_t bytes, uint32_t mode, int repair_stage)
{
    vfs_context_t ctx = vfs_context_current(); vnode_t vp = NULLVP;
    int error = vnode_open(path, O_CREAT | O_EXCL | O_NOFOLLOW | FWRITE, mode, 0, &vp, ctx);
    if (error == EEXIST) {
        error = dui_equal(path, data, bytes, mode);
        if (!error || !repair_stage || error != EEXIST) return error;
        error = dgr_unlink(p, scratch, path);
        return error ? error : dui_write_new(p, scratch, path, data, bytes, mode, 0);
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
    if (!error && !closed) ++diting_ui_runtime_written;
    return error ? error : closed;
}
static int dui_backup(proc_t p, user_addr_t scratch, const uint8_t id[16])
{
    char path[256];
    size_t length = strlcpy(path, DUI_BACKUP_PREFIX, sizeof(path));
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) {
        path[length++] = digits[id[i] >> 4]; path[length++] = digits[id[i] & 15];
    }
    path[length] = 0;
    int error = dgr_stat(DUI_LIBRARY, VREG, 0, 0);
    if (error == ENOENT) return 0;
    if (error) return error;
    vnode_t vp = NULLVP; vfs_context_t ctx = vfs_context_current();
    error = vnode_open(DUI_LIBRARY, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);
    if (error) return error;
    struct vnode_attr va; VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size); VATTR_WANTED(&va, va_mode);
    error = vnode_getattr(vp, &va, ctx);
    uint32_t mode = error ? 0 : va.va_mode & 07777;
    if (!error && (va.va_data_size < 32 || va.va_data_size > DitingUiRuntimeRawMax ||
        (mode != 0755 && mode != 0644))) error = EINVAL;
    size_t bytes = error ? 0 : (size_t)va.va_data_size;
    uint8_t *data = bytes ? kalloc_data(bytes, Z_WAITOK) : NULL;
    if (!error && !data) error = ENOMEM;
    int resid = 0;
    if (!error) error = vn_rdwr(UIO_READ, vp, (caddr_t)data, (int)bytes, 0, UIO_SYSSPACE,
        IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
    if (!error && resid) error = EIO;
    int closed = vnode_close(vp, FREAD, ctx);
    if (!error) error = closed;
    if (!error) error = dui_write_new(p, scratch, path, data, bytes, mode, 0);
    if (data) kfree_data(data, bytes);
    return error;
}
static int dui_install_archive(proc_t p, user_addr_t scratch, const uint8_t *archive, size_t archive_bytes)
{
    struct DitingUiRuntimeEntry entry;
    diting_ui_runtime_stage = 1;
    if (!diting_ui_runtime_parse(archive, archive_bytes, &entry)) return EINVAL;
    int rootful = 0; PE_parse_boot_argn("diting_rootful", &rootful, sizeof(rootful));
    if (rootful != 1 || !rootvnode || native_owner_data_status || native_owner_data_readonly ||
        native_owner_cache_status || !native_owner_cache_authorized || !native_owner_cache_root_recheck ||
        native_owner_cache_current_root != (uintptr_t)rootvnode ||
        strcmp(vfs_statfs(vnode_mount(rootvnode))->f_fstypename, "apfs")) return EAUTH;
    int error = dgr_stat("/diting", VDIR, 0, 0);
    if (error) return error;
    diting_ui_runtime_stage = 2;
    uint8_t *raw = kalloc_data((size_t)entry.bytes, Z_WAITOK);
    if (!raw) return ENOMEM;
    const struct DitingGpuRuntimeEntry compressed = {entry.packed, entry.bytes, entry.packed_bytes};
    error = dgr_inflate(&compressed, raw);
    if (!error && (diting_ui_runtime_u32(raw) != 0xfeedfacf ||
        diting_ui_runtime_u32(raw + 4) != 0x0100000c || diting_ui_runtime_u32(raw + 12) != 6)) error = EINVAL;
    if (error) goto done;
    int present = dui_equal(DUI_LIBRARY, raw, entry.bytes, 0755);
    if (dgr_id_equal(DUI_ID, archive + 32)) {
        if (present) { error = EEXIST; goto done; }
        diting_ui_runtime_stage = 6; goto done;
    }
    if (present && present != EEXIST && present != ENOENT) { error = present; goto done; }
    diting_ui_runtime_stage = 3;
    error = dgr_upgrade(p, scratch);
    if (error) goto done;
    error = dui_write_new(p, scratch, DUI_ID_STAGE, archive + 32, 16, 0644, 1);
    if (error) goto done;
    if (present) {
        error = dui_write_new(p, scratch, DUI_STAGE, raw, entry.bytes, 0755, 1);
        if (error) goto done;
        diting_ui_runtime_stage = 4;
        error = dui_backup(p, scratch, archive + 32);
        if (error) goto done;
        error = dgr_rename(p, scratch, DUI_STAGE, DUI_LIBRARY);
        if (error) goto done;
    }
    diting_ui_runtime_stage = 5;
    error = dgr_rename(p, scratch, DUI_ID_STAGE, DUI_ID);
    if (!error) diting_ui_runtime_stage = 6;
done:
    kfree_data(raw, (size_t)entry.bytes);
    return error;
}
static void diting_ui_runtime_prepare(proc_t p, user_addr_t scratch)
{
    uintptr_t start = (uintptr_t)DitingUiRuntimeData, end = (uintptr_t)DitingUiRuntimeEnd;
    int error = end < start ? EINVAL : dui_install_archive(p, scratch, DitingUiRuntimeData, end - start);
    diting_ui_runtime_error = error;
    printf("DITING UI runtime: %s stage=%d error=%d files-written=%u; icons/brightness untested\n",
        error ? "SKIP" : "PASS", diting_ui_runtime_stage, error, diting_ui_runtime_written);
}
#undef DUI_LIBRARY
#undef DUI_STAGE
#undef DUI_BACKUP_PREFIX
#undef DUI_ID
#undef DUI_ID_STAGE
#endif
