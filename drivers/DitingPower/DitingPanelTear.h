/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "DitingDisplayRefresh.h"

namespace DitingDisplay {
struct PanelTearDiagnostic {
    uint32_t version=1,bytes=sizeof(PanelTearDiagnostic),sequence=0,stage=0;
    uint32_t error=0,writes=0,before=0,after=0,polls=0,restored=0,hz=0,reserved=0;
    uint64_t startUs=0,endUs=0;
};
static_assert(sizeof(PanelTearDiagnostic)==64,"Diting panel tear diagnostic ABI");
// Both L12 DT modes have 2712 + 20 + 20 + 4 lines. The pinned Xiaomi
// sde_encoder_phys_cmd/sde_hw_intf contract uses XO / (vtotal * refresh).
inline uint32_t panelVsyncCount(unsigned hz) {
    return hz==60 || hz==90 || hz==120 ? uint32_t(19200000ULL/(2756ULL*hz)) : 0;
}
class PanelTear {
    RefreshIo io_;PanelTearDiagnostic &d_;
    static constexpr uint32_t Te=0x36288,Line=0x362b0;
    bool power() {
        uint32_t values[3]{};
        for(unsigned i=0;i<3;++i)
            if(!io_.reader.read(io_.reader.cookie,Clocks,ClockOffsets[i],&values[i])) {d_.error=2;return false;}
        if(values[0]==~0u || !(values[0]&0x80000000u) ||
           (values[1]&0x80000001u)!=1 || (values[2]&0x80000001u)!=1) {d_.error=6;return false;}
        return true;
    }
    bool read(uint32_t offset,uint32_t &value) {
        if(!power())return false;
        if(!io_.reader.read(io_.reader.cookie,Mdp,offset,&value)) {d_.error=2;return false;}
        return true;
    }
    bool write(uint32_t value) {
        if(!power())return false;
        if(!io_.write(io_.reader.cookie,Mdp,Te,value)) {d_.error=3;return false;}
        ++d_.writes;return true;
    }
public:
    PanelTear(RefreshIo io,PanelTearDiagnostic &diagnostic):io_(io),d_(diagnostic) {}
    bool run(const Board &b,const Snapshot &s,unsigned hz) {
        d_.stage=1;d_.hz=hz;d_.before=s.intf[0].reg[10];
        const uint32_t count=panelVsyncCount(hz);
        if(!count || !matchesConsole(b,s) || s.intf[0].reg[14]!=0x80000001u ||
           !io_.reader.read || !io_.write || !io_.pause) {d_.error=1;d_.stage=4;return false;}
        const uint32_t wanted=(d_.before&~0xffffu)|count;
        uint32_t current=0;
        if(!read(Te,current) || current!=d_.before) {if(!d_.error)d_.error=5;d_.stage=4;return false;}
        if(current==wanted) {d_.after=current;d_.stage=3;return true;}
        // Disconnect external TE while the previous transfer drains. Preserve
        // the interval, source, PLL, TE height and every unrelated control bit.
        if(!write(d_.before&~0x100000u)) {d_.stage=4;return false;}
        d_.stage=2;
        for(unsigned i=0;i<400;++i) {
            io_.pause(io_.reader.cookie,50);++d_.polls;
            if(!read(Line,current))goto failed;
            if(!current || current>=2712)break;
            if(i==399) {d_.error=4;goto failed;}
        }
        if(!write(wanted&~0x100000u) || !read(Te,current) || current!=(wanted&~0x100000u) ||
           !write(wanted) || !read(Te,d_.after) || d_.after!=wanted) {
            if(!d_.error)d_.error=5;goto failed;
        }
        d_.stage=3;return true;
    failed:
        const uint32_t error=d_.error;
        d_.restored=write(d_.before) && read(Te,d_.after) && d_.after==d_.before;
        d_.error=error;d_.stage=4;return false;
    }
};
}
