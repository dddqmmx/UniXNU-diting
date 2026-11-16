/* SPDX-License-Identifier: MIT */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The production supervisor and host fixtures use this same bounded process
 * runner. The child gets a separate process group and a retained lease fd.
 * A cancelled job cannot be replaced before its child has actually exited. */
struct DitingGpuProcessResult { int error, exit_code, signal, cancelled, unreaped; pid_t pid; uint32_t bytes; };
typedef int (*DitingGpuProcessData)(void *, const uint8_t *, unsigned);
typedef int (*DitingGpuProcessTick)(void *);
static inline uint64_t diting_gpu_monotonic_ms(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) return 0;
    return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}
static inline struct DitingGpuProcessResult diting_gpu_process(
    const char *path, char *const argv[], char *const env[], int lease,
    uint32_t timeout_ms, uint32_t limit, void *context,
    DitingGpuProcessData data, DitingGpuProcessTick tick)
{
    struct DitingGpuProcessResult result = {0, -1, 0, 0, 0, 0, 0};
    if (!path || !argv || !data || !tick || !timeout_ms || !limit) { result.error = EINVAL; return result; }
    int pipefd[2];
    if (pipe(pipefd)) { result.error = errno; return result; }
    for (unsigned i = 0; i < 2; ++i) {
        // launchd normally supplies all standard descriptors. Also handle a
        // closed stdout without closing the child's dup2 destination.
        if (pipefd[i] < 3) {
            const int replacement = fcntl(pipefd[i], F_DUPFD_CLOEXEC, 3);
            if (replacement < 0) { result.error = errno; close(pipefd[0]); close(pipefd[1]); return result; }
            close(pipefd[i]); pipefd[i] = replacement;
        }
        if (fcntl(pipefd[i], F_SETFD, FD_CLOEXEC)) {
            result.error = errno; close(pipefd[0]); close(pipefd[1]); return result;
        }
    }
    int flags = fcntl(pipefd[0], F_GETFL);
    if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK)) {
        result.error = errno; close(pipefd[0]); close(pipefd[1]); return result;
    }
    posix_spawn_file_actions_t actions; posix_spawnattr_t attributes;
    int failure = posix_spawn_file_actions_init(&actions);
    if (failure) { result.error = failure; close(pipefd[0]); close(pipefd[1]); return result; }
    failure = posix_spawnattr_init(&attributes);
    if (failure) {
        result.error = failure; posix_spawn_file_actions_destroy(&actions);
        close(pipefd[0]); close(pipefd[1]); return result;
    }
    if (!failure) failure = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    if (!failure) failure = posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    if (!failure) failure = posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    if (!failure && lease >= 0) failure = posix_spawn_file_actions_adddup2(&actions, lease, 198);
    sigset_t empty, defaults;
    sigemptyset(&empty); sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM); sigaddset(&defaults, SIGINT); sigaddset(&defaults, SIGALRM);
    if (!failure) failure = posix_spawnattr_setsigmask(&attributes, &empty);
    if (!failure) failure = posix_spawnattr_setsigdefault(&attributes, &defaults);
    if (!failure) failure = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    if (!failure) failure = posix_spawnattr_setpgroup(&attributes, 0);
    if (!failure) failure = posix_spawn(&result.pid, path, &actions, &attributes, argv, env);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
    close(pipefd[1]);
    if (failure) { result.error = failure; close(pipefd[0]); return result; }
    const uint64_t begin = diting_gpu_monotonic_ms();
    uint64_t stopping = 0, exited_at = 0;
    int exited = 0, eof = 0, status = 0, have_status = 0;
    if (!begin) result.error = EIO;
    while (!exited || !eof) {
        uint8_t block[1024];
        for (unsigned reads = 0; reads < 16; ++reads) {
            ssize_t n = read(pipefd[0], block, sizeof(block));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                if (!n) eof = 1;
                else if (errno != EAGAIN && errno != EWOULDBLOCK) { result.error = errno; eof = 1; }
                break;
            }
            if ((uint32_t)n > limit - result.bytes) result.error = EOVERFLOW;
            else if (!result.error) {
                result.bytes += (uint32_t)n;
                if (!data(context, block, (unsigned)n)) result.error = EIO;
            }
            if (result.error) break;
        }
        if (!exited) {
            pid_t got = waitpid(result.pid, &status, WNOHANG);
            if (got == result.pid) { exited = have_status = 1; exited_at = diting_gpu_monotonic_ms(); }
            else if (got < 0 && errno != EINTR) { result.error = errno; exited = 1; }
        }
        const uint64_t now = diting_gpu_monotonic_ms();
        const int keep_running = tick(context);
        if (!stopping && !exited && (result.error || !keep_running || now - begin >= timeout_ms)) {
            result.cancelled = !keep_running;
            if (!result.error && !result.cancelled) result.error = ETIMEDOUT;
            stopping = now ? now : 1;
            kill(-result.pid, SIGTERM);
        }
        if (stopping && !exited && now - stopping >= 500) kill(-result.pid, SIGKILL);
        if (stopping && !exited && now - stopping >= 3000) { result.unreaped = 1; break; }
        if (exited && !eof && exited_at && now - exited_at >= 500) {
            // A fixed worker never launches descendants. Refuse malformed
            // fixtures that leave a descendant holding the result pipe.
            kill(-result.pid, SIGKILL);
            if (now - exited_at >= 1000) { result.error = EPIPE; break; }
        }
        if (!exited || !eof) {
            struct pollfd ready = {pipefd[0], POLLIN, 0};
            if (poll(&ready, 1, 20) < 0 && errno != EINTR) result.error = errno;
            // EOF may arrive before a sleeping child exits. POLLHUP would
            // otherwise make this loop spin instead of keeping a bounded tick.
            if (eof && !exited) { struct timespec delay = {0, 20000000}; nanosleep(&delay, NULL); }
        }
    }
    close(pipefd[0]);
    if (have_status && !result.error) {
        if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
        if (WIFSIGNALED(status)) result.signal = WTERMSIG(status);
    } else if (have_status && WIFSIGNALED(status)) result.signal = WTERMSIG(status);
    return result;
}
