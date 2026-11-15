/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Included by kern_exec.c after the UUID-bound Data mount helper.
 * Four small snapshots on an optional worker, never a launchd prerequisite.
 */
#ifndef DITING_USB_BOOT_CAPTURE_H
#define DITING_USB_BOOT_CAPTURE_H
#include "../../drivers/DitingUsb/DitingUsbDiagnostics.h"
#include <kern/thread.h>

unsigned diting_usb_read_kernel(uint64_t address, void *out, unsigned bytes)
{
    if (!out || !bytes || bytes > 512 || address < VM_MIN_KERNEL_ADDRESS ||
        address > UINT64_MAX - bytes) return 0;
    // Darwin24 rejects missing translations and non-managed physical pages.
    // No copyin, user-map fault, MMIO access, or memory write is performed.
    return (unsigned)ml_nofault_copy((vm_offset_t)address, (vm_offset_t)out, bytes);
}

#if XNU_OWNER_DATA_MOUNT && XNU_OWNER_DATA_UFS
static int duc_append(const char *data, unsigned bytes)
{
    static const char path[] = "/private/var/logs/diting/usb-boot.log";
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t parent = NULLVP, file = NULLVP;
    int error = vnode_lookup("/private/var", VNODE_LOOKUP_NOFOLLOW_ANY, &parent, ctx);
    if (error) return error;
    mount_t data_mount = vnode_mount(parent);
    if (vnode_vtype(parent) != VDIR || data_mount == vnode_mount(rootvnode) ||
        (vfs_flags(data_mount) & MNT_RDONLY) ||
        strcmp(vfs_statfs(data_mount)->f_fstypename, "apfs")) error = EROFS;
    if (!error) error = vnode_open(path, O_CREAT | O_NOFOLLOW | FWRITE, 0644, 0, &file, ctx);
    if (!error) {
        struct vnode_attr va; VATTR_INIT(&va);
        VATTR_WANTED(&va, va_uid); VATTR_WANTED(&va, va_gid);
        VATTR_WANTED(&va, va_mode); VATTR_WANTED(&va, va_data_size);
        error = vnode_getattr(file, &va, ctx);
        if (!error && (!VATTR_IS_SUPPORTED(&va, va_uid) || !VATTR_IS_SUPPORTED(&va, va_gid) ||
            !VATTR_IS_SUPPORTED(&va, va_mode) || !VATTR_IS_SUPPORTED(&va, va_data_size) ||
            vnode_vtype(file) != VREG || vnode_mount(file) != data_mount ||
            va.va_uid != 0 || va.va_gid != 0 || (va.va_mode & 07777) != 0644 ||
            va.va_data_size > 262144 - bytes)) error = EINVAL;
        int resid = 0;
        if (!error) error = vn_rdwr(UIO_WRITE, file, (caddr_t)data, bytes,
            (off_t)va.va_data_size, UIO_SYSSPACE, IO_NOAUTH | IO_UNIT,
            vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (!error && resid) error = EIO;
        if (!error) error = VNOP_FSYNC(file, MNT_WAIT, ctx);
        int closed = vnode_close(file, FWRITE, ctx);
        if (!error) error = closed;
    }
    vnode_put(parent);
    return error;
}

static void duc_worker(void *argument __unused, wait_result_t result __unused)
{
    // First after 1 s, then 10/30/70 s. Sleep this optional worker only.
    static const unsigned delays[] = {1, 9, 20, 40};
    static int sleep_event;
    char data[2048];
    for (unsigned i = 0; i < 4; ++i) {
        assert_wait_timeout((event_t)&sleep_event, THREAD_UNINT, delays[i], NSEC_PER_SEC);
        thread_block(THREAD_CONTINUE_NULL);
        int length = snprintf(data, sizeof(data), "\nUSB boot snapshot %u kernel=%s", i, version);
        if (length < 0 || length >= (int)sizeof(data)) break;
        length += (int)diting_usb_snapshot(data + length, (unsigned)(sizeof(data) - length));
        const int error = duc_append(data, (unsigned)length);
        printf("DITING USB capture: snapshot=%u error=%d bytes=%d\n", i, error, length);
        if (error) break;
    }
    thread_terminate(current_thread()); thread_block(THREAD_CONTINUE_NULL);
}

static void diting_usb_boot_capture_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("diting_usb", &enabled, sizeof(enabled)) || enabled != 1 ||
        native_owner_data_status || native_owner_data_readonly || !rootvnode) return;
    static unsigned started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    thread_t thread = THREAD_NULL;
    const kern_return_t error = kernel_thread_start(duc_worker, NULL, &thread);
    if (error == KERN_SUCCESS) thread_deallocate(thread);
    else printf("DITING USB capture: optional worker unavailable error=%d\n", error);
}
#else
static void diting_usb_boot_capture_start(void) {}
#endif
#endif
