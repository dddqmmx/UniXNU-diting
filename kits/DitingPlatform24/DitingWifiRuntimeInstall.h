/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Wi-Fi-only service installation before PID 1. Never writes GPU/UI files,
 * SystemOS, Data images, power preferences or a fabricated availability state.
 * The exact captured native cache is required before any installation write.
 */
#ifndef DITING_WIFI_RUNTIME_INSTALL_H
#define DITING_WIFI_RUNTIME_INSTALL_H
#include "DitingWifiRuntimeArchive.h"
#ifndef DITING_WIFI_RUNTIME_HOST_TEST
#include <libkern/zlib.h>
#include <sys/stdio.h>
#else
#include <zlib.h>
#endif

extern const uint8_t DitingWifiRuntimeData[], DitingWifiRuntimeEnd[];
volatile int diting_wifi_runtime_stage, diting_wifi_runtime_error;
volatile unsigned diting_wifi_runtime_written, diting_wifi_runtime_step;

#define DWR_ID "/diting/.diting-wifi-native-services-v1"
static const char *const dwr_files[4] = {
    "/diting/wifi-bootstrap",
    "/System/Library/LaunchDaemons/dev.unixnu.diting.wifi-bootstrap.plist",
    "/System/Library/LaunchDaemons/com.apple.wifid.plist",
    "/System/Library/xpc/launchd.plist"
};
static const uint32_t dwr_modes[4] = {0755,0644,0644,0644};

/* Native VFS helpers are kept independent of GPU feature selection. Their
 * ownership, mount/UUID binding and syscall ABI match the existing APFS
 * runtime writers. Host tests substitute the platform calls below and run
 * the actual archive and transaction code unchanged. */
#ifndef DITING_WIFI_RUNTIME_HOST_TEST
static int dwr_stat(const char *path, enum vtype type, uint64_t size, uint32_t mode)
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

static int dwr_read_id(const char *path, uint8_t id[16])
{
    int error = dwr_stat(path, VREG, 16, 0644);
    if (error) return error;
    vnode_t vp = NULLVP;
    vfs_context_t ctx = vfs_context_current();
    error = vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);
    if (error) return error;
    int resid = 0;
    error = vn_rdwr(UIO_READ, vp, (caddr_t)id, 16, 0, UIO_SYSSPACE,
        IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
    int close_error = vnode_close(vp, FREAD, ctx);
    return error ? error : close_error ? close_error : resid ? EIO : 0;
}
static inline int dwr_id_equal(const char *path, const uint8_t *id)
{
    uint8_t old[16];
    return !dwr_read_id(path, old) && !memcmp(old, id, sizeof(old));
}

/* Read-only comparison: preflight must not open a file for creation on a
 * read-only root. The small publication flag needs no allocation on reboot. */
static int dwr_equal(const char *path, const uint8_t *data, uint64_t bytes, uint32_t mode)
{
    int error = dwr_stat(path, VREG, bytes, mode);
    if (error) return error;
    vnode_t vp = NULLVP; vfs_context_t ctx = vfs_context_current();
    error = vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);
    if (error) return error;
    uint8_t small[16], *buffer = small;
    size_t chunk = bytes < 65536 ? (size_t)bytes : 65536;
    if (chunk > sizeof(small)) buffer = kalloc_data(chunk, Z_WAITOK);
    if (!buffer) error = ENOMEM;
    for (uint64_t at = 0; !error && at < bytes; at += chunk) {
        int amount = (int)(bytes - at < chunk ? bytes - at : chunk), resid = 0;
        error = vn_rdwr(UIO_READ, vp, (caddr_t)buffer, amount, (off_t)at, UIO_SYSSPACE,
            IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (!error && (resid || memcmp(buffer, data + at, amount))) error = EEXIST;
    }
    if (buffer && buffer != small) kfree_data(buffer, chunk);
    int closed = vnode_close(vp, FREAD, ctx);
    return error ? error : closed;
}
#endif

static voidpf dwr_zalloc(voidpf opaque __unused, uInt items, uInt size)
{
    if (size && items > (1u << 20) / size) return Z_NULL;
    size_t bytes = (size_t)items * size + 16;
    uint8_t *p = kalloc_data(bytes, Z_WAITOK | Z_ZERO);
    if (!p) return Z_NULL;
    memcpy(p, &bytes, sizeof(bytes));
    return p + 16;
}
static void dwr_zfree(voidpf opaque __unused, voidpf address)
{
    if (!address) return;
    uint8_t *p = (uint8_t *)address - 16;
    size_t bytes = 0; memcpy(&bytes, p, sizeof(bytes));
    kfree_data(p, bytes);
}
static int dwr_inflate(const struct DitingWifiRuntimeEntry *entry, uint8_t *data)
{
    z_stream stream; memset(&stream, 0, sizeof(stream));
    stream.zalloc = dwr_zalloc; stream.zfree = dwr_zfree;
    stream.next_in = (Bytef *)(uintptr_t)entry->packed; stream.avail_in = (uInt)entry->packed_bytes;
    stream.next_out = data; stream.avail_out = (uInt)entry->bytes;
    if (inflateInit(&stream) != Z_OK) return ENOMEM;
    int result = inflate(&stream, Z_FINISH);
    int complete = result == Z_STREAM_END && stream.total_out == entry->bytes && !stream.avail_in;
    inflateEnd(&stream);
    return complete ? 0 : EINVAL;
}

#ifndef DITING_WIFI_RUNTIME_HOST_TEST
/* O_EXCL preserves an existing file. A previous interrupted staging write is
 * accepted only when its complete content and ownership match this candidate. */
static int dwr_unlink(proc_t p, user_addr_t scratch, const char *path)
{
    struct unlink_args request = {.path = scratch + 0xc00}; int32_t result = 0;
    int error = copyout(path, request.path, strlen(path) + 1);
    if (!error) error = unlink(p, &request, &result);
    return error;
}
static int dwr_write_new(proc_t p, user_addr_t scratch, const char *path,
    const uint8_t *data, uint64_t bytes, uint32_t mode, int repair_stage)
{
    vfs_context_t ctx = vfs_context_current(); vnode_t vp = NULLVP;
    int error = vnode_open(path, O_CREAT | O_EXCL | O_NOFOLLOW | FWRITE, mode, 0, &vp, ctx);
    if (error == EEXIST) {
        error = dwr_stat(path, VREG, 0, 0);
        if (error) return error;
        error = dwr_stat(path, VREG, bytes, mode);
        if (error) {
            if (!repair_stage) return error;
            error = dwr_unlink(p, scratch, path);
            return error ? error : dwr_write_new(p, scratch, path, data, bytes, mode, 0);
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
            error = dwr_unlink(p, scratch, path);
            return error ? error : dwr_write_new(p, scratch, path, data, bytes, mode, 0);
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
    if (error || closed) (void)dwr_unlink(p, scratch, path);
    if (!error && !closed) ++diting_wifi_runtime_written;
    return error ? error : closed;
}
static int dwr_rename_flags(proc_t p, user_addr_t scratch, const char *from, const char *to, unsigned flags)
{
    struct renameatx_np_args request = {.fromfd = AT_FDCWD, .from = scratch + 0xc00,
        .tofd = AT_FDCWD, .to = scratch + 0xd00, .flags = flags | RENAME_NOFOLLOW_ANY};
    int32_t result = 0;
    int error = copyout(from, request.from, strlen(from) + 1);
    if (!error) error = copyout(to, request.to, strlen(to) + 1);
    if (!error) error = renameatx_np(p, &request, &result);
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
static int dwr_rename(proc_t p, user_addr_t scratch, const char *from, const char *to)
{
    return dwr_rename_flags(p, scratch, from, to, 0);
}
static int dwr_upgrade(proc_t p, user_addr_t scratch)
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
#endif

static int dwr_compare(const char *path, const uint8_t *data, uint64_t bytes, uint32_t mode)
{
    int error = dwr_stat(path,VREG,0,0);
    if (error) return error;
    if (dwr_stat(path,VREG,bytes,mode)) return EEXIST;
    return dwr_equal(path,data,bytes,mode);
}

/* Repair only this candidate's private staging file. Completed backups and
 * every destination supplied by callers remain intact on a mismatch. */
static int dwr_stage(proc_t p, user_addr_t scratch, const char *path,
    const uint8_t *data, uint64_t bytes, uint32_t mode)
{
    return dwr_write_new(p,scratch,path,data,bytes,mode,1);
}

static void dwr_path(const char *target, const uint8_t id[16],
    const char *suffix, char path[256])
{
    static const char hex[] = "0123456789abcdef";
    size_t n = strlcpy(path,target,256);
    n += strlcpy(path+n,suffix,256-n);
    for (unsigned i=0; i<16; ++i) {
        path[n++] = hex[id[i]>>4]; path[n++] = hex[id[i]&15];
    }
    path[n]=0;
}

#ifndef DITING_WIFI_RUNTIME_HOST_TEST
static int dwr_backup_job(proc_t p, user_addr_t scratch, const char *source,
    const char *backup, const char *temporary)
{
    int error = dwr_stat(backup,VREG,0,0);
    if (!error) {
        /* A backup is published only after its complete staging inode is
         * fsynced. Preserve that inode when resuming an interrupted install. */
        vnode_t vp = NULLVP; vfs_context_t ctx = vfs_context_current();
        error = vnode_lookup(backup,VNODE_LOOKUP_NOFOLLOW_ANY,&vp,ctx);
        if (!error) {
            struct vnode_attr va; VATTR_INIT(&va); VATTR_WANTED(&va,va_data_size); VATTR_WANTED(&va,va_mode);
            error = vnode_getattr(vp,&va,ctx);
            if (!error && (va.va_data_size<8 || va.va_data_size>65536 || (va.va_mode&07777)!=0644)) error=EINVAL;
            vnode_put(vp);
        }
        return error;
    }
    if (error!=ENOENT) return error;
    error = dwr_stat(source,VREG,0,0);
    if (error==ENOENT) return 0;
    if (error) return error;
    vnode_t vp = NULLVP; vfs_context_t ctx = vfs_context_current();
    error = vnode_open(source,FREAD|O_NOFOLLOW,0,0,&vp,ctx);
    if (error) return error;
    struct vnode_attr va; VATTR_INIT(&va); VATTR_WANTED(&va,va_data_size); VATTR_WANTED(&va,va_mode);
    error = vnode_getattr(vp,&va,ctx);
    if (!error && (va.va_data_size<8 || va.va_data_size>65536 || (va.va_mode&07777)!=0644)) error=EINVAL;
    size_t bytes = error ? 0 : (size_t)va.va_data_size;
    uint8_t *data = bytes ? kalloc_data(bytes,Z_WAITOK) : NULL;
    if (!error && !data) error=ENOMEM;
    int resid=0;
    if (!error) error=vn_rdwr(UIO_READ,vp,(caddr_t)data,(int)bytes,0,UIO_SYSSPACE,
        IO_NOAUTH,vfs_context_ucred(ctx),&resid,vfs_context_proc(ctx));
    if (!error && resid) error=EIO;
    int closed=vnode_close(vp,FREAD,ctx);
    if (!error) error=closed;
    if (!error) error=dwr_stage(p,scratch,temporary,data,bytes,0644);
    if (!error) error=dwr_rename_flags(p,scratch,temporary,backup,RENAME_EXCL);
    if (data) kfree_data(data,bytes);
    return error;
}
#endif

static int dwr_install_files(proc_t p, user_addr_t scratch,
    const uint8_t *raw[DitingWifiRuntimeFiles],
    const struct DitingWifiRuntimeEntry entries[DitingWifiRuntimeFiles], const uint8_t id[16])
{
    int error, current, present[4];
    char stages[4][256], backup[256], temporary[256], id_stage[256];
    diting_wifi_runtime_step=3;
    /* A changed cache indicates another task changed installed jobs. Never
     * overlay that task's native cache with our preserved older snapshot. */
    current=dwr_compare(dwr_files[3],raw[3],entries[3].bytes,0644);
    if (current && current!=EEXIST) return current;
    if (current) {
        error=dwr_compare(dwr_files[3],raw[4],entries[4].bytes,0644);
        if (error) return error==EEXIST ? ESTALE : error;
    }
    for (unsigned i=0; i<4; ++i) {
        present[i]=dwr_compare(dwr_files[i],raw[i],entries[i].bytes,dwr_modes[i]);
        if (present[i] && present[i]!=EEXIST && present[i]!=ENOENT) return present[i];
        /* The two new destinations are exclusive. wifid and the cache are
         * the only existing files authorized for replacement. */
        if (i<2 && present[i]==EEXIST) return EEXIST;
        if (!current && present[i]) return EEXIST;
        dwr_path(dwr_files[i],id,".wifi-stage-",stages[i]);
    }
    dwr_path(DWR_ID,id,".wifi-stage-",id_stage);
    if (!current && dwr_id_equal(DWR_ID,id)) {
        diting_wifi_runtime_stage=6; return 0;
    }
    error=dwr_stat(DWR_ID,VREG,0,0);
    if (error && error!=ENOENT) return error;
    if (!error && dwr_stat(DWR_ID,VREG,16,0644)) return EEXIST;
    diting_wifi_runtime_stage=3; diting_wifi_runtime_step=4;
    error=dwr_upgrade(p,scratch);
    if (error) return error;
    error=dwr_stage(p,scratch,id_stage,id,16,0644);
    if (error) return error;
    if (!current) {
        /* Power loss after cache commit: every prerequisite already matches.
         * Complete only the marker, without touching service definitions. */
        diting_wifi_runtime_stage=5;
        error=dwr_rename(p,scratch,id_stage,DWR_ID);
        if (!error) diting_wifi_runtime_stage=6;
        return error;
    }
    diting_wifi_runtime_step=5;
    for (unsigned i=0; i<4; ++i) if (present[i]) {
        error=dwr_stage(p,scratch,stages[i],raw[i],entries[i].bytes,dwr_modes[i]);
        if (error) return error;
    }
    diting_wifi_runtime_step=6;
    if (present[2]==EEXIST) {
        dwr_path(dwr_files[2],id,".before-wifi-",backup);
        dwr_path(dwr_files[2],id,".backup-stage-",temporary);
        error=dwr_backup_job(p,scratch,dwr_files[2],backup,temporary);
        if (error) return error;
    }
    dwr_path(dwr_files[3],id,".before-wifi-",backup);
    error=dwr_compare(backup,raw[4],entries[4].bytes,0644);
    if (error==ENOENT) {
        dwr_path(dwr_files[3],id,".backup-stage-",temporary);
        error=dwr_stage(p,scratch,temporary,raw[4],entries[4].bytes,0644);
        if (!error) error=dwr_rename_flags(p,scratch,temporary,backup,RENAME_EXCL);
    }
    if (error) return error;
    diting_wifi_runtime_stage=4; diting_wifi_runtime_step=7;
    for (unsigned i=0; i<3; ++i) if (present[i]) {
        error=dwr_rename_flags(p,scratch,stages[i],dwr_files[i],i<2 ? RENAME_EXCL : 0);
        if (error) return error;
    }
    /* Publish the native cache last, only after all declared executables and
     * plists exist. Recheck the exact preimage at this final commit boundary. */
    diting_wifi_runtime_step=8;
    error=dwr_compare(dwr_files[3],raw[4],entries[4].bytes,0644);
    if (error) return error==EEXIST ? ESTALE : error;
    error=dwr_rename(p,scratch,stages[3],dwr_files[3]);
    if (error) return error;
    diting_wifi_runtime_stage=5; diting_wifi_runtime_step=9;
    error=dwr_rename(p,scratch,id_stage,DWR_ID);
    if (!error) diting_wifi_runtime_stage=6;
    return error;
}

#ifndef DITING_WIFI_RUNTIME_HOST_TEST
static int dwr_install(proc_t p, user_addr_t scratch)
{
    struct DitingWifiRuntimeEntry entries[DitingWifiRuntimeFiles];
    const uint8_t *raw[DitingWifiRuntimeFiles]={0};
    int error=0,rootful=0;
    uintptr_t start=(uintptr_t)DitingWifiRuntimeData,end=(uintptr_t)DitingWifiRuntimeEnd;
    diting_wifi_runtime_stage=1; diting_wifi_runtime_step=1;
    if (end<start || !diting_wifi_runtime_parse(DitingWifiRuntimeData,end-start,entries)) return EINVAL;
    PE_parse_boot_argn("diting_rootful",&rootful,sizeof(rootful));
    diting_wifi_runtime_step=2;
    if (rootful!=1 || !rootvnode || native_owner_data_status || native_owner_data_readonly ||
        native_owner_cache_status || !native_owner_cache_authorized || !native_owner_cache_root_recheck ||
        native_owner_cache_current_root!=(uintptr_t)rootvnode ||
        strcmp(vfs_statfs(vnode_mount(rootvnode))->f_fstypename,"apfs")) return EAUTH;
    for (unsigned i=0; i<3; ++i) {
        static const char *const directories[]={"/diting","/System/Library/LaunchDaemons","/System/Library/xpc"};
        error=dwr_stat(directories[i],VDIR,0,0);
        if (error) return error;
    }
    diting_wifi_runtime_stage=2;
    for (unsigned i=0; i<DitingWifiRuntimeFiles; ++i) {
        raw[i]=kalloc_data((size_t)entries[i].bytes,Z_WAITOK);
        if (!raw[i]) {error=ENOMEM;goto done;}
        error=dwr_inflate(&entries[i],(uint8_t *)(uintptr_t)raw[i]);
        if (error) goto done;
        if (i && memcmp(raw[i],"bplist00",8)) {error=EINVAL;goto done;}
    }
    if (entries[0].bytes<32 || diting_wifi_runtime_u32(raw[0])!=0xfeedfacf ||
        diting_wifi_runtime_u32(raw[0]+4)!=0x0100000c ||
        diting_wifi_runtime_u32(raw[0]+12)!=2) {error=EINVAL;goto done;}
    error=dwr_install_files(p,scratch,raw,entries,DitingWifiRuntimeData+32);
done:
    for (unsigned i=0; i<DitingWifiRuntimeFiles; ++i) if (raw[i]) {
        uint8_t *buffer=(uint8_t *)(uintptr_t)raw[i];
        kfree_data(buffer,(size_t)entries[i].bytes);
    }
    return error;
}
static void diting_wifi_runtime_prepare(proc_t p, user_addr_t scratch)
{
    int enabled=0; PE_parse_boot_argn("diting_wifi",&enabled,sizeof(enabled));
    if (enabled!=1) return;
    diting_wifi_runtime_error=dwr_install(p,scratch);
    printf("DITING Wi-Fi services: %s stage=%d error=%d step=%u files-written=%u; switch/network untested\n",
        diting_wifi_runtime_error ? "SKIP" : "PASS",diting_wifi_runtime_stage,
        diting_wifi_runtime_error,diting_wifi_runtime_step,diting_wifi_runtime_written);
}
#endif
#undef DWR_ID
#endif
