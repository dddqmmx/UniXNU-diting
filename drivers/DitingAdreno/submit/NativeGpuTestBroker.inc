/* SPDX-License-Identifier: MIT */
/* Included once by the native publisher. The diagnostics gate is independent
 * of the GPU gate; USB never waits for GPU work or executes userspace code. */
#include "NativeGpuTestState.h"
#include "NativeGpuTestCommand.h"
#include "../../DitingUsb/DitingUsbCommand.h"
#include "../../DitingUsb/DitingUsbReply.h"
#ifdef DITING_GUEST_24B156
/* On iOS 27 MH_FILESET, avoid putting a 1 MiB static BSS buffer in native __DATA. */
static IOLock *gpuTestLock;
static IOLock *gpuTestLockAcquire() { return nullptr; }
static IOReturn gpuTestMethod(uint32_t, const void *, size_t, void *, size_t) { return kIOReturnUnsupported; }
extern "C" unsigned diting_gpu_test_text(const char *command, char *out, unsigned capacity) {
    unsigned used = 0;
    if (!command || !out || !capacity) return 0;
    diting_usb_reply_add(out, capacity, &used, "GPUTEST schema=1 available=0 reason=ios27-minimal\n");
    return used;
}
#else
static DitingGpuTestState gpuTestState{};
static IOLock *gpuTestLock;
static IOLock *gpuTestLockAcquire() { return __atomic_load_n(&gpuTestLock, __ATOMIC_ACQUIRE); }
static uint64_t gpuTestNow() {
    uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000;
}
static IOReturn gpuTestReturn(DitingGpuTestState::Error error) {
    switch (error) {
    case DitingGpuTestState::Ok: return kIOReturnSuccess;
    case DitingGpuTestState::Busy: return kIOReturnBusy;
    case DitingGpuTestState::Argument: return kIOReturnBadArgument;
    case DitingGpuTestState::Full: return kIOReturnNoSpace;
    case DitingGpuTestState::Stale: return kIOReturnNotPermitted;
    default: return kIOReturnNotReady;
    }
}
static IOReturn gpuTestMethod(uint32_t method, const void *input, size_t inbytes, void *output, size_t outbytes) {
    IOLock *lock = gpuTestLockAcquire();
    if (!lock) return kIOReturnNotReady;
    if (!input) return kIOReturnBadArgument;
    IOLockLock(lock);
    IOReturn answer = kIOReturnBadArgument;
    if (method == DitingGpuTestPoll && inbytes == sizeof(DitingGpuTestHello) && output && outbytes == sizeof(DitingGpuTestJob)) {
        const auto &hello = *static_cast<const DitingGpuTestHello *>(input);
        if (hello.version == DitingGpuTestVersion && !hello.reserved)
            answer = gpuTestReturn(gpuTestState.poll(uint32_t(proc_selfpid()), gpuTestNow(), *static_cast<DitingGpuTestJob *>(output)));
    } else if (method == DitingGpuTestReport && inbytes == sizeof(DitingGpuTestUpdate) && !outbytes) {
        answer = gpuTestReturn(gpuTestState.append(uint32_t(proc_selfpid()), *static_cast<const DitingGpuTestUpdate *>(input), gpuTestNow()));
    } else if (method == DitingGpuTestMark && inbytes == sizeof(DitingGpuTestMarker) && !outbytes) {
        const auto &mark = *static_cast<const DitingGpuTestMarker *>(input);
        gpuTestState.expire(gpuTestNow());
        if (uint32_t(proc_selfpid()) == gpuTestState.daemon &&
            mark.version == DitingGpuTestVersion && mark.job && mark.job == gpuTestState.request.job &&
            gpuTestState.active() && mark.case_id && mark.case_id <= 64 && mark.phase >= 1 && mark.phase <= 2 &&
            (!mark.end_ns || mark.end_ns >= mark.start_ns)) {
            gpuTraceEvent(DitingGpuTestTraceEvent, proc_selfpid(), mark.job, mark.case_id, mark.phase, mark.start_ns, mark.end_ns);
            answer = kIOReturnSuccess;
        }
    }
    IOLockUnlock(lock); return answer;
}
extern "C" unsigned diting_gpu_test_text(const char *command, char *out, unsigned capacity) {
    unsigned used = 0;
    if (!command || !out || !capacity) return 0;
    IOLock *lock = gpuTestLockAcquire();
    if (!lock) {
        diting_usb_reply_add(out, capacity, &used, "GPUTEST schema=1 available=0 reason=broker-unavailable\n"); return used;
    }
    const auto parsed = DitingGpuTestCommand::parse(command);
    IOLockLock(lock);
    const uint64_t now = gpuTestNow(); gpuTestState.expire(now);
    DitingGpuTestState::Error status = DitingGpuTestState::Argument;
    if (parsed.op == DitingGpuTestCommand::Status) status = DitingGpuTestState::Ok;
    if (parsed.op == DitingGpuTestCommand::Run) status = gpuTestState.start(parsed.profile, parsed.policy, now);
    if (parsed.op == DitingGpuTestCommand::Cancel) status = gpuTestState.cancel(parsed.job, now);
    if (parsed.op == DitingGpuTestCommand::Result) {
        const uint8_t *buffer = nullptr; uint32_t got = 0;
        status = gpuTestState.read(parsed.job, parsed.offset, parsed.bytes, &buffer, &got);
        diting_usb_reply_add(out, capacity, &used, "GPUTEST schema=1 error=%u job=%u offset=%u bytes=%u total=%u\nDATA ",
            unsigned(status), parsed.job, parsed.offset, got, gpuTestState.used);
        if (status == DitingGpuTestState::Ok) used += diting_usb_base64(buffer, got, out + used, capacity - used);
        diting_usb_reply_add(out, capacity, &used, "\n");
    } else {
        diting_usb_reply_add(out, capacity, &used,
            "GPUTEST schema=1 error=%u available=1 daemon=%u heartbeat_age_us=%llu job=%u state=%u profile=%u policy=%u bytes=%u case=%u stage=%u worker_error=%u created_us=%llu completed_us=%llu timeout_ms=%u quiescing=%u\n",
            unsigned(status), gpuTestState.daemon, (unsigned long long)(gpuTestState.daemon ? DitingGpuTestState::age(now, gpuTestState.heartbeat_us) : 0),
            gpuTestState.request.job, gpuTestState.state, gpuTestState.request.profile, gpuTestState.request.policy,
            gpuTestState.used, gpuTestState.case_id, gpuTestState.stage, gpuTestState.error,
            (unsigned long long)gpuTestState.request.created_us, (unsigned long long)gpuTestState.completed_us,
            gpuTestState.request.timeout_ms, gpuTestState.quiescing);
    }
    IOLockUnlock(lock); return used;
}
#endif
