/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "DitingDisplaySnapshot.h"

namespace DitingDisplay {
// Identify the observed Diting split BGRA console and active TE. The pinned
// Xiaomi encoder changes autorefresh only across a disabled/enabled transition,
// with TE disconnected and the previous transfer drained before disabling.
// An enabled count of 60 is an interval, not Hz. DitingDisplayRefresh performs
// the bounded disabled/enabled transition after a completed scanout copy.
static inline bool matchesConsole(const Board &b,const Snapshot &s) {
    if(!valid(b) || s.status!=Ok || s.mdss!=0x80050000 || s.mdp!=0x80050000 ||
       s.dsiVersion!=0x20060000 || !(s.dsiControl&1) ||
       !(s.intf[0].reg[9]&1) || (s.intf[0].reg[10]&0x180000)!=0x180000 ||
       s.intf[0].reg[13]>2712 || !(s.intf[0].reg[14]&0x80000000) ||
       (s.intf[1].reg[9]&1) || s.ctl[0][2]!=2)return false;
    for(unsigned i=0;i<2;++i)
        if(s.pipe[i][0]!=0x0a980262 || s.pipe[i][1]!=0xb8000000 ||
           s.pipe[i][2]!=4880 || s.pipe[i][3]!=0x237ff ||
           s.pipe[i][5]!=0x03020001)return false;
    return true;
}
struct CopyDiagnostic {
    uint32_t version=1,bytes=sizeof(CopyDiagnostic),sequence=0,stage=0;
    uint32_t error=0,before=0,after=0,console=0;
    uint64_t us=0,copy=0;
};
static_assert(sizeof(CopyDiagnostic)==48,"Diting display copy snapshot diagnostic ABI");
}
