/* SPDX-License-Identifier: MIT
 * Idle root daemon for fixed USB GPU diagnostics. Rendering runs only in an
 * exec'd child after a broker request. No shell, paths or shaders come from USB.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu-test-process.h"
#include "../drivers/DitingAdreno/submit/NativeGpuTestProtocol.h"

extern char **environ;
int diting_gpu_test_worker(int, char **);
static volatile sig_atomic_t quitting;
static void quit(int signal_number) { (void)signal_number; quitting = 1; }
struct Supervisor {
    io_connect_t connection;
    struct DitingGpuTestJob job;
    uint32_t offset, case_id, stage;
    char line[256]; unsigned line_bytes; int line_discard;
};
static kern_return_t poll_job(struct Supervisor *worker) {
    const struct DitingGpuTestHello hello = {DitingGpuTestVersion, 0};
    size_t bytes = sizeof(worker->job);
    kern_return_t result = IOConnectCallStructMethod(worker->connection, DitingGpuTestPoll,
        &hello, sizeof(hello), &worker->job, &bytes);
    return result ? result : bytes != sizeof(worker->job) ? kIOReturnBadArgument : 0;
}
static int update(struct Supervisor *worker, const uint8_t *data, unsigned bytes, uint32_t state, uint32_t error) {
    struct DitingGpuTestUpdate output = {0};
    if (bytes > sizeof(output.data)) return 0;
    output.version = DitingGpuTestVersion; output.job = worker->job.job;
    output.offset = worker->offset; output.bytes = bytes; output.state = state;
    output.error = error; output.case_id = worker->case_id; output.stage = worker->stage;
    output.worker_us = diting_gpu_monotonic_ms() * 1000;
    if (bytes) memcpy(output.data, data, bytes);
    kern_return_t result = IOConnectCallStructMethod(worker->connection, DitingGpuTestReport,
        &output, sizeof(output), NULL, NULL);
    if (result) { fprintf(stderr, "Diting GPU test: report job=%u offset=%u status=%x\n", output.job, output.offset, result); return 0; }
    worker->offset += bytes; return 1;
}
static int tick(void *context) {
    struct Supervisor *worker = context;
    const uint32_t job = worker->job.job;
    if (quitting || poll_job(worker) || worker->job.job != job ||
        (worker->job.flags & DitingGpuTestCancel)) return 0;
    return 1;
}
static void marker(struct Supervisor *worker) {
    uint32_t id = 0, phase = 0;
    unsigned long long start = 0, end = 0;
    if (sscanf(worker->line, "{\"kind\":\"begin\",\"id\":%u,\"start_ns\":%llu}", &id, &start) == 2) phase = 1;
    else if (sscanf(worker->line, "{\"kind\":\"end\",\"id\":%u,\"start_ns\":%llu,\"end_ns\":%llu}", &id, &start, &end) == 3) phase = 2;
    if (!phase || !id || id > 64 || end < (phase == 2 ? start : 0)) return;
    worker->case_id = id; worker->stage = phase;
    const struct DitingGpuTestMarker record = {DitingGpuTestVersion, worker->job.job, id, phase, start, end};
    const kern_return_t result = IOConnectCallStructMethod(worker->connection, DitingGpuTestMark,
        &record, sizeof(record), NULL, NULL);
    if (result) fprintf(stderr, "Diting GPU test: marker job=%u case=%u status=%x\n", worker->job.job, id, result);
}
static int received(void *context, const uint8_t *data, unsigned bytes) {
    struct Supervisor *worker = context;
    if (!update(worker, data, bytes, DitingGpuTestRunning, 0)) return 0;
    for (unsigned i = 0; i < bytes; ++i) {
        if (data[i] == '\n') {
            if (!worker->line_discard) { worker->line[worker->line_bytes] = 0; marker(worker); }
            worker->line_bytes = 0; worker->line_discard = 0;
        } else if (worker->line_bytes + 1 < sizeof(worker->line)) worker->line[worker->line_bytes++] = (char)data[i];
        else worker->line_discard = 1;
    }
    return 1;
}
int diting_gpu_test_daemon(void) {
    if (geteuid()) return 77;
    signal(SIGTERM, quit); signal(SIGINT, quit); signal(SIGPIPE, SIG_IGN);
    int lease = open("/private/var/run/diting-gpu-test.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat info;
    if (lease < 0 || fstat(lease, &info) || !S_ISREG(info.st_mode) || info.st_uid ||
        (info.st_mode & 07777) != 0600 || flock(lease, LOCK_EX | LOCK_NB)) {
        if (lease >= 0) close(lease);
        fprintf(stderr, "Diting GPU test: another supervisor/worker owns the lease\n"); return 75;
    }
    struct Supervisor worker = {0};
    uint32_t finished_job = 0, finished_state = DitingGpuTestFailed, finished_error = 0;
    int finished_ack = 1;
    while (!quitting) {
        if (!worker.connection) {
            io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NativeAdrenoGpuPublished"));
            if (!service) { sleep(1); continue; }
            kern_return_t result = IOServiceOpen(service, mach_task_self(), DitingGpuTestOpenType, &worker.connection);
            IOObjectRelease(service);
            if (result) { worker.connection = 0; sleep(1); continue; }
            fprintf(stderr, "Diting GPU test: USB broker ready pid=%d schema=1\n", getpid());
        }
        if (poll_job(&worker)) { IOServiceClose(worker.connection); worker.connection = 0; sleep(1); continue; }
        if (!worker.job.job) { usleep(200000); continue; }
        const uint32_t job = worker.job.job;
        if (job == finished_job) {
            // A transient report failure cannot authorize executing an old
            // job again. Retry its terminal acknowledgement only.
            if (!finished_ack) finished_ack = update(&worker, NULL, 0, finished_state, finished_error);
            usleep(200000); continue;
        }
        if (worker.job.flags & DitingGpuTestCancel) { usleep(200000); continue; }
        if (worker.job.version != DitingGpuTestVersion || worker.job.profile < 1 || worker.job.profile > DitingGpuTestKeyboard ||
            worker.job.policy > 2 || worker.job.width != 1220 || worker.job.height != 2712 ||
            worker.job.frames != 240 || worker.job.warmup != 24 || worker.job.timeout_ms != 120000) {
            worker.offset = 0; update(&worker, NULL, 0, DitingGpuTestFailed, EINVAL); usleep(200000); continue;
        }
        char values[8][24];
        const uint32_t numbers[] = {worker.job.profile, worker.job.policy, job, worker.job.width,
            worker.job.height, worker.job.frames, worker.job.warmup, worker.job.timeout_ms};
        char *arguments[11] = {"/diting/gpu-capture18", "--worker"};
        for (unsigned i = 0; i < 8; ++i) { snprintf(values[i], sizeof(values[i]), "%u", numbers[i]); arguments[i + 2] = values[i]; }
        arguments[10] = NULL;
        worker.offset = worker.case_id = worker.stage = worker.line_bytes = 0; worker.line_discard = 0;
        fprintf(stderr, "Diting GPU test: start job=%u profile=%u policy=%u\n", job, worker.job.profile, worker.job.policy);
        struct DitingGpuProcessResult result = diting_gpu_process("/diting/gpu-capture18", arguments,
            environ, lease, worker.job.timeout_ms, DitingGpuTestResultBytes, &worker, received, tick);
        if (result.unreaped) {
            // Keep the lease and broker cancellation until waitpid confirms
            // exit. A signal request is not proof that DMA/userspace stopped.
            fprintf(stderr, "Diting GPU test: job=%u child=%d has not exited; new work refused\n", job, result.pid);
            for (;;) {
                int status = 0; pid_t reaped = waitpid(result.pid, &status, WNOHANG);
                if (reaped == result.pid) {
                    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
                    if (WIFSIGNALED(status)) result.signal = WTERMSIG(status);
                    break;
                }
                if (reaped < 0 && errno == ECHILD) break;
                if (reaped < 0 && errno != EINTR) fprintf(stderr, "Diting GPU test: waitpid error=%d\n", errno);
                kill(-result.pid, SIGKILL); poll_job(&worker); sleep(1);
            }
        }
        worker.stage = 9;
        uint32_t state = result.cancelled ? DitingGpuTestCancelled : result.error == ETIMEDOUT ? DitingGpuTestTimedOut :
            result.error == EOVERFLOW ? DitingGpuTestOverflow :
            !result.error && !result.signal && !result.exit_code ? DitingGpuTestPassed : DitingGpuTestFailed;
        // poll may have observed a cancelled/expired job; append acknowledges
        // actual child exit and preserves the broker's original terminal state.
        worker.job.job = job;
        finished_job = job; finished_state = state;
        finished_error = result.error ? (uint32_t)result.error :
            result.signal ? 0x20000u + (uint32_t)result.signal : (uint32_t)(result.exit_code < 0 ? EIO : result.exit_code);
        finished_ack = update(&worker, NULL, 0, state, finished_error);
        fprintf(stderr, "Diting GPU test: finish job=%u state=%u bytes=%u exit=%d signal=%d error=%d\n",
            job, state, worker.offset, result.exit_code, result.signal, result.error);
        // Terminal jobs remain readable; the poll result carries CANCEL until
        // the USB host starts a new job.
        usleep(200000);
    }
    if (worker.connection) IOServiceClose(worker.connection);
    close(lease); return 0;
}
