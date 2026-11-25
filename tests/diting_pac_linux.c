/* SPDX-License-Identifier: BSD-2-Clause
 * Bounded Linux EL0 PAC qualification for Diting. Run from RAM in Recovery.
 * Reset keys only for this process; never read keys or dereference PAC values.
 * This does not qualify XNU, firmware handoff, or direct EL1 key access.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef __aarch64__
#error This probe must be built as an AArch64 Linux ELF
#endif
#ifndef HWCAP_PACA
#define HWCAP_PACA (1UL << 30)
#endif
#ifndef HWCAP_PACG
#define HWCAP_PACG (1UL << 31)
#endif
#ifndef PR_PAC_RESET_KEYS
#define PR_PAC_RESET_KEYS 54
#endif

static sigjmp_buf recovery;
static volatile sig_atomic_t guarded;
static volatile sig_atomic_t trapped;

static void on_signal(int signo)
{
    if (guarded) {
        trapped = signo;
        siglongjmp(recovery, 1);
    }
    _exit(128 + signo);
}

/* Early-clobber outputs keep the discriminator separate from both values. */
#define ROUNDTRIP(sign, auth)                                                \
    __asm__ volatile(sign " %0, %2\n\tmov %1, %0\n\t" auth " %0, %2"       \
                     : "+&r"(value), "=&r"(signed_value) : "r"(modifier))

static int roundtrip(unsigned key, uint64_t raw, uint64_t modifier,
                     uint64_t *signed_out)
{
    uint64_t value = raw, signed_value = 0;
    if (sigsetjmp(recovery, 1)) {
        guarded = 0;
        return 0;
    }
    guarded = 1;
    switch (key) {
    case 0: ROUNDTRIP("pacia", "autia"); break;
    case 1: ROUNDTRIP("pacib", "autib"); break;
    case 2: ROUNDTRIP("pacda", "autda"); break;
    case 3: ROUNDTRIP("pacdb", "autdb"); break;
    default: guarded = 0; return 0;
    }
    guarded = 0;
    *signed_out = signed_value;
    return value == raw;
}

static int generic_pac(uint64_t raw, uint64_t modifier, uint64_t *result)
{
    if (sigsetjmp(recovery, 1)) {
        guarded = 0;
        return 0;
    }
    guarded = 1;
    __asm__ volatile("pacga %0, %1, %2"
                     : "=r"(*result) : "r"(raw), "r"(modifier));
    guarded = 0;
    return 1;
}

int main(void)
{
    const char *keys[] = {"IA", "IB", "DA", "DB"};
    unsigned long hwcap = getauxval(AT_HWCAP);
    struct sigaction action;
    cpu_set_t available;
    int failed = 0, cpu_count = 0;
    uint64_t storage = UINT64_C(0x27b156);
    uint64_t raw = (uint64_t)(uintptr_t)&storage;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("{\"scope\":\"Linux EL0 process\",\"hwcap\":\"0x%lx\","
           "\"paca\":%s,\"pacg\":%s,\"el1_qualified\":false,"
           "\"xnu_qualified\":false,", hwcap,
           hwcap & HWCAP_PACA ? "true" : "false",
           hwcap & HWCAP_PACG ? "true" : "false");
    if (!(hwcap & HWCAP_PACA)) {
        puts("\"result\":\"SKIP_NO_PACA\"}");
        return 77;
    }
    if (sched_getaffinity(0, sizeof(available), &available)) {
        printf("\"result\":\"FAIL_AFFINITY\",\"errno\":%d}\n", errno);
        return 1;
    }
    unsigned long reset_mask = 15UL | (hwcap & HWCAP_PACG ? 16UL : 0UL);
    if (prctl(PR_PAC_RESET_KEYS, reset_mask, 0UL, 0UL, 0UL)) {
        printf("\"result\":\"SKIP_KEY_RESET_DENIED\",\"errno\":%d}\n", errno);
        return 77;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGILL, &action, NULL) || sigaction(SIGSEGV, &action, NULL)) {
        printf("\"result\":\"FAIL_SIGNAL_SETUP\",\"errno\":%d}\n", errno);
        return 1;
    }
    printf("\"own_process_keys_reset\":true,\"cpus\":[");
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &available)) continue;
        cpu_set_t selected;
        CPU_ZERO(&selected);
        CPU_SET(cpu, &selected);
        printf("%s{\"cpu\":%d,", cpu_count++ ? "," : "", cpu);
        if (sched_setaffinity(0, sizeof(selected), &selected) || sched_getcpu() != cpu) {
            printf("\"result\":\"FAIL_CPU_SELECTION\",\"errno\":%d}", errno);
            failed = 1;
            continue;
        }
        printf("\"keys\":[");
        int cpu_failed = 0;
        for (unsigned key = 0; key < 4; ++key) {
            unsigned completed = 0, changed_null = 0, changed_live = 0;
            trapped = 0;
            for (unsigned sample = 1; sample <= 16; ++sample) {
                uint64_t modifier = UINT64_C(0xb156270000000000) | sample;
                uint64_t signed_value;
                if (!roundtrip(key, 0, modifier, &signed_value)) break;
                changed_null += signed_value != 0;
                ++completed;
                if (!roundtrip(key, raw, modifier, &signed_value)) break;
                changed_live += signed_value != raw;
                ++completed;
            }
            int ok = completed == 32 && changed_null && changed_live;
            cpu_failed |= !ok;
            printf("%s{\"key\":\"%s\",\"roundtrips\":%u,"
                   "\"signed_null_changed\":%u,\"signed_live_changed\":%u,"
                   "\"signal\":%d,\"result\":\"%s\"}", key ? "," : "",
                   keys[key], completed, changed_null, changed_live,
                   (int)trapped, ok ? "PASS" : "FAIL");
        }
        printf("],\"pacga\":");
        if (hwcap & HWCAP_PACG) {
            uint64_t first = 0, again = 0, other = 0;
            int ok = generic_pac(raw, 42, &first) &&
                     generic_pac(raw, 42, &again) &&
                     generic_pac(raw, 43, &other) && first == again && first != other;
            printf("\"%s\"", ok ? "PASS" : "FAIL");
            cpu_failed |= !ok;
        } else {
            printf("\"SKIP_NO_PACG\"");
        }
        printf(",\"result\":\"%s\"}", cpu_failed ? "FAIL" : "PASS");
        failed |= cpu_failed;
    }
    if (!cpu_count) failed = 1;
    printf("],\"cpu_count\":%d,\"result\":\"%s\"}\n",
           cpu_count, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
