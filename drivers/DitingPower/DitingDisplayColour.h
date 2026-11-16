/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "DitingDisplayText.h"
namespace DitingDisplay {
// Read ordinary configuration words only. LUT data ports, interrupt-clear
// registers, panel commands and debug-selector writes are excluded.
struct ColourSnapshot {
    Snapshot path;
    uint32_t dsi[7]{},format[2]{},pipe[2][7]{},dspp[2][6]{},pcc[2][33]{},dsc[2][8]{};
};
static inline ColourSnapshot captureColour(const Board &b,Io io) {
    ColourSnapshot out;
    Reader r(b,io,out.path);
    out.path.status=r.capture(false);
    if(out.path.status!=Ok)return out;
    auto &s=out.path;
    if(!r.read(Mdp,0,s.mdss) || !r.read(Mdp,0x1000,s.mdp)) {s.status=Unavailable;return out;}
    if(s.mdss!=0x80050000u || s.mdp!=0x80050000u || s.dsiVersion!=0x20060000u) {
        s.status=Unavailable;return out;
    }
    // Xiaomi dsi_ctrl_reg.h and live sde-dspp/dsc sub-block offsets.
    const uint32_t dsiOff[]={0x40,0x1b8,0x44,0x20,0x2a8,0x2ac,0x2b0};
    const uint32_t pipeOff[]={0xa00,0x1a00,0x1a04,0x1a08,0x1a0c,0x1a10,0x1a14};
    const uint32_t dsppOff[]={0,0x800,0x1260,0x1700,0x1000,0x17c0};
    const uint32_t dsppBase[]={0x55000,0x57000};
    for(unsigned i=0;i<7;++i)if(!r.read(Dsi,dsiOff[i],out.dsi[i]))goto failed;
    for(unsigned i=0;i<2;++i) {
        if(!r.read(Mdp,Intf[i]+0x90,out.format[i]))goto failed;
        for(unsigned n=0;n<7;++n)if(!r.read(Mdp,Pipe[i]+pipeOff[n],out.pipe[i][n]))goto failed;
        for(unsigned n=0;n<6;++n)if(!r.read(Mdp,dsppBase[i]+dsppOff[n],out.dspp[i][n]))goto failed;
        for(unsigned n=0;n<33;++n)if(!r.read(Mdp,dsppBase[i]+0x1704+4*n,out.pcc[i][n]))goto failed;
        const uint32_t offsets[]={0x81000,0x81f00+0x80*i,0x81f04+0x80*i,0x81f08+0x80*i,
            0x81100+0x100*i,0x81130+0x100*i,0x81134+0x100*i,0x81138+0x100*i};
        for(unsigned n=0;n<8;++n)if(!r.read(Mdp,offsets[n],out.dsc[i][n]))goto failed;
    }
    for(unsigned i=0;i<3;++i) {
        uint32_t clock=0;
        if(!r.read(Clocks,ClockOffsets[i],clock))goto failed;
        if(clock!=s.clock[i]) {s.status=Changed;return out;}
    }
    return out;
failed:
    s.status=Unavailable;return out;
}
static inline unsigned formatColour(char *out,unsigned bytes,const ColourSnapshot &c,uint64_t start,uint64_t end) {
    Text t{out,bytes};const auto &s=c.path;
    t.add("COLOUR schema=1 status=%s reason=%u reads=%u cpu_start_us=%llu cpu_end_us=%llu register_writes=0 physical_colour=UNQUALIFIED\n",
        s.status==Ok?"PASS":s.status==Changed?"CHANGED":"SKIP",unsigned(s.status),s.reads,
        (unsigned long long)start,(unsigned long long)end);
    if(s.status!=Ok && s.status!=Changed)return t.used;
    t.add("DSI MDP_CTRL=%08x MDP_CTRL2=%08x DCS_CMD=%08x VIDEO_DATA=%08x COMPRESSION=%08x COMP2=%08x COMP3=%08x\n",
        c.dsi[0],c.dsi[1],c.dsi[2],c.dsi[3],c.dsi[4],c.dsi[5],c.dsi[6]);
    for(unsigned i=0;i<2;++i) {
        t.add("COLOURPIPE index=%u FORMAT=%08x VIG_OP=%08x CSC10_OP=%08x MATRIX=",i,c.format[i],c.pipe[i][0],c.pipe[i][1]);
        for(unsigned n=2;n<7;++n)t.add("%s%08x",n==2?"":",",c.pipe[i][n]);t.add("\n");
        t.add("DSPP index=%u OP=%08x PA=%08x IGC=%08x PCC=%08x GAMUT=%08x GC=%08x\n",
            i,c.dspp[i][0],c.dspp[i][1],c.dspp[i][2],c.dspp[i][3],c.dspp[i][4],c.dspp[i][5]);
        t.add("PCC index=%u COEFFICIENTS=",i);
        for(unsigned n=0;n<33;++n)t.add("%s%08x",n?",":"",c.pcc[i][n]);t.add("\n");
        t.add("DSC index=%u COMMON=%08x CTL=%08x CFG=%08x SWAP=%08x DF=%08x MAIN=%08x PICTURE=%08x SLICE=%08x\n",
            i,c.dsc[i][0],c.dsc[i][1],c.dsc[i][2],c.dsc[i][3],c.dsc[i][4],c.dsc[i][5],c.dsc[i][6],c.dsc[i][7]);
    }
    return t.used;
}
}
