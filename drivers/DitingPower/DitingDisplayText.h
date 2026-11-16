/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdarg.h>
#include "DitingDisplaySnapshot.h"

// The caller provides vsnprintf (IOLib in XNU, stdio on the host).
namespace DitingDisplay {
struct Text {
    char *data; unsigned capacity, used = 0;
    void add(const char *format, ...) {
        if (!data || capacity <= used + 1) return;
        va_list args; va_start(args, format);
        const int n = vsnprintf(data + used, capacity - used, format, args); va_end(args);
        if (n > 0) used += unsigned(n) < capacity - used ? unsigned(n) : capacity - used - 1;
    }
};
static inline unsigned formatSnapshot(char *out, unsigned bytes, const Board &board,
    const Snapshot &s, uint64_t start, uint64_t end) {
    Text text{out, bytes};
    const char *status = s.status == Ok ? "PASS" : s.status == Changed ? "CHANGED" : "SKIP";
    text.add("DISPLAY schema=1 status=%s reason=%u reads=%u cpu_start_us=%llu cpu_end_us=%llu physical_present=UNQUALIFIED timestamp_units=RAW\n",
        status, unsigned(s.status), s.reads, (unsigned long long)start, (unsigned long long)end);
    text.add("DPU base=%llx bytes=%x mdss_raw=%08x mdp_raw=%08x DSI0_VERSION=%08x CONTROL=%08x STATUS=%08x PATTERN=%08x\n",
        (unsigned long long)board.mdp, board.mdpBytes, s.mdss, s.mdp, s.dsiVersion, s.dsiControl, s.dsiStatus, s.dsiPattern);
    text.add("CLOCK GDSC=%08x AHB=%08x MDP=%08x INT2_GDSC=%08x BYTE0=%08x BYTE0_INTF=%08x PCLK0=%08x\n",
        s.clock[0], s.clock[1], s.clock[2], s.clock[3], s.clock[4], s.clock[5], s.clock[6]);
    if (s.status != Ok && s.status != Changed) return text.used;
    for (unsigned i = 0; i < 2; ++i) {
        const auto &v = s.intf[i]; const auto *r = v.reg;
        text.add("INTF index=%u off=%x TIMING=%08x CONFIG=%08x CONFIG2=%08x COUNT_EN=%08x FRAME=%u LINE=%u PERIOD=%u STATUS=%08x TS_CTRL=%08x PANEL_TS=%llu MDP_TS=%llu TS_STABLE=%u TE_EN=%08x TE_CFG=%08x TE_HEIGHT=%u TE_OUT=%u TE_LINE=%u AUTOREFRESH=%08x TE_DETECT=%08x TE_COUNT=%08x TE_INIT=%08x\n",
            i + 1, Intf[i], r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8],
            (unsigned long long)v.panelTimestamp, (unsigned long long)v.mdpTimestamp, v.stable,
            r[9], r[10], r[11], r[12], r[13], r[14], r[15], r[16], r[17]);
    }
    for (unsigned i = 0; i < 8; ++i)
        text.add("SSPP index=%u off=%x SIZE=%08x SRC0=%08x STRIDE=%08x FORMAT=%08x OP=%08x UNPACK=%08x\n",
            i, Pipe[i], s.pipe[i][0], s.pipe[i][1], s.pipe[i][2], s.pipe[i][3], s.pipe[i][4], s.pipe[i][5]);
    for (unsigned i = 0; i < 6; ++i)
        text.add("CTL index=%u off=%x TOP=%08x FLUSH=%08x INTF_ACTIVE=%08x\n", i, Ctl[i], s.ctl[i][0], s.ctl[i][1], s.ctl[i][2]);
    return text.used;
}
}
