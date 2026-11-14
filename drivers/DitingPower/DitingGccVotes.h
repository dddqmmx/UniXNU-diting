/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_GCC_VOTES_H
#define DITING_GCC_VOTES_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    void *context;
    uint32_t (*read)(void *);
    void (*write)(void *, uint32_t);
} DitingGccVoteIo;

/* The caller owns the shared lock for this complete RMW/readback. */
static inline bool diting_gcc_vote_io(const DitingGccVoteIo *io,
                                     uint32_t set_mask, uint32_t clear_mask) {
    if (!io || !io->read || !io->write || (set_mask & clear_mask)) return false;
    const uint32_t before = io->read(io->context);
    const uint32_t next = (before & ~clear_mask) | set_mask;
    if (next != before) io->write(io->context, next);
    return io->read(io->context) == next;
}

/* clk_branch_wait skips HALT when the particular branch has HWCG enabled.
 * The separate APSS vote readback remains mandatory. */
static inline bool diting_gcc_branch_ready(uint32_t status, bool has_hwcg) {
    return !(status & (1u << 31)) || (has_hwcg && (status & 2u));
}
#endif
