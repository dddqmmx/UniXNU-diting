/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "DitingDisplayPresentation.h"

namespace DitingDisplay {
// SM8475 live DT + Xiaomi sm8450-modules 3b30ba7: the count is a
// frame interval, not Hz. Change it across disabled/enabled states only.
struct RefreshDiagnostic {
    uint32_t version=1,bytes=sizeof(RefreshDiagnostic),sequence=0,stage=0;
    uint32_t error=0,writes=0,before=0,after=0,teBefore=0,teAfter=0;
    uint32_t line=0,start=0,scheduler=0,polls=0,restored=0,reserved=0;
    uint64_t us=0,endUs=0,copy=0;
};
static_assert(sizeof(RefreshDiagnostic)==88,"Diting refresh diagnostic ABI");
struct RefreshIo {
    Io reader;
    bool (*write)(void *,Region,uint32_t,uint32_t);
    void (*pause)(void *,unsigned);
};
class Refresh {
    RefreshIo io_; RefreshDiagnostic &d_;
    bool changed_=false, debug_=false;
    uint32_t debugPeripheral_=0,debugDspp_=0;
    static constexpr uint32_t Te=0x36288,Auto=0x362b4,Line=0x362b0;
    static constexpr uint32_t DebugPeripheral=0x1418,DebugDspp=0x1348,DebugStatus=0x134c;
    bool power() {
        uint32_t c[3]{};
        for(unsigned i=0;i<3;++i)
            if(!io_.reader.read(io_.reader.cookie,Clocks,ClockOffsets[i],&c[i])) {
                d_.error=2;return false;
            }
        if(c[0]==~0u || !(c[0]&0x80000000u) ||
           (c[1]&0x80000001u)!=1 || (c[2]&0x80000001u)!=1) {
            d_.error=6;return false;
        }
        return true;
    }
    bool rd(uint32_t off,uint32_t &value) {
        if(!power())return false;
        if(!io_.reader.read(io_.reader.cookie,Mdp,off,&value)) {d_.error=2;return false;}
        return true;
    }
    bool wr(uint32_t off,uint32_t value) {
        if(!power())return false;
        if(!io_.write(io_.reader.cookie,Mdp,off,value)) {d_.error=3;return false;}
        ++d_.writes;return true;
    }
    bool drain() {
        for(unsigned i=0;i<400;++i) {
            io_.pause(io_.reader.cookie,50);++d_.polls;
            if(!rd(Line,d_.line))return false;
            // sde_encoder_phys_cmd_is_ongoing_pptx, vdisplay=2712.
            if(d_.line==0 || d_.line>=2712)return true;
        }
        d_.error=4;return false;
    }
    bool debugRestore() {
        if(!debug_)return true;
        const bool a=wr(DebugPeripheral,debugPeripheral_);
        const bool b=wr(DebugDspp,debugDspp_);
        if(a && b)debug_=false;
        return a && b;
    }
    bool restore() {
        bool ok=debugRestore();
        if(changed_) {
            // Keep TE disconnected while restoring a changed interval.
            ok=wr(Te,d_.teBefore&~0x100000u) && ok;
            uint32_t current=0;
            if(!rd(Auto,current))return false;
            ok=wr(Auto,current&~0x80000000u) && ok;
            if((current&0xffffu)!=(d_.before&0xffffu) && !drain()) {
                // An unretired transfer does not allow another count change.
                // Restore the current count's enable/TE state and report that
                // the original configuration could not be restored.
                wr(Auto,current);wr(Te,d_.teBefore);
                rd(Auto,d_.after);rd(Te,d_.teAfter);return false;
            }
            ok=wr(Auto,d_.before) && ok;
            ok=wr(Te,d_.teBefore) && ok;
            ok=rd(Auto,d_.after) && ok;ok=rd(Te,d_.teAfter) && ok;
            ok=ok && d_.after==d_.before && d_.teAfter==d_.teBefore;
        }
        return ok;
    }
public:
    Refresh(RefreshIo io,RefreshDiagnostic &d):io_(io),d_(d) {}
    bool run(const Board &b,const Snapshot &s) {
        d_.stage=1;d_.before=s.intf[0].reg[14];d_.teBefore=s.intf[0].reg[10];
        if(!matchesConsole(b,s) || !io_.reader.read || !io_.write || !io_.pause ||
           !(d_.before&0xffffu)) {d_.stage=5;d_.error=1;return false;}
        if(d_.before==0x80000001u) {d_.stage=4;d_.after=d_.before;d_.teAfter=d_.teBefore;return true;}
        uint32_t live=0;
        if(!rd(Auto,live) || live!=d_.before || !rd(Te,live) || live!=d_.teBefore) {
            d_.stage=6;if(!d_.error)d_.error=5;return false;
        }
        changed_=true;
        if(!wr(Te,d_.teBefore&~0x100000u) || !wr(Auto,d_.before&~0x80000000u))goto failed;
        d_.stage=2;
        if(!drain())goto failed;
        // The vendor's second disable sequence reads the autorefresh FSM
        // through the debug mux. Save both selectors and restore both.
        if(!rd(DebugPeripheral,debugPeripheral_) || !rd(DebugDspp,debugDspp_))goto failed;
        debug_=true;
        if(!wr(DebugPeripheral,0x405) || !wr(DebugDspp,0x7001))goto failed;
        for(unsigned i=0;i<100;++i) {
            io_.pause(io_.reader.cookie,200);++d_.polls;
            if(!rd(DebugStatus,d_.scheduler))goto failed;
            if(!(d_.scheduler&0x80u))break;
            uint32_t count=0;
            if(!rd(0x362a0,d_.start) || !rd(0x36290,count) ||
               !wr(0x36290,(count&0xffff0000u)|(d_.start&0xffffu)))goto failed;
            if(i==99) {d_.error=4;goto failed;}
        }
        if(!debugRestore())goto failed;
        d_.stage=3;
        if(!rd(Auto,live) || (live&0x80000000u)) {d_.error=5;goto failed;}
        if(!wr(Auto,0x80000001u) || !rd(Auto,d_.after) || d_.after!=0x80000001u ||
           !wr(Te,d_.teBefore) || !rd(Te,d_.teAfter) || d_.teAfter!=d_.teBefore) {
            if(!d_.error)d_.error=5;goto failed;
        }
        d_.stage=4;return true;
    failed:
        const uint32_t error=d_.error;
        d_.restored=restore();d_.stage=6;d_.error=error;
        return false;
    }
};
}
