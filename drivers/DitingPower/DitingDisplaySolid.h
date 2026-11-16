/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "DitingDisplayPresentation.h"

namespace DitingDisplay {
// Xiaomi sm8450 display modules 3b30ba7, sde_plane.c: solid fill uses
// ABGR8888, with red in bits 7..0 and blue in bits 23..16. No DDR reads.
enum SolidStage { SolidIdle, SolidQueued, SolidApplying, SolidVisible,
    SolidRestoring, SolidDone, SolidRejected, SolidPowerWait, SolidFailed };
struct SolidDiagnostic {
    uint32_t version=1,bytes=sizeof(SolidDiagnostic),sequence=0,stage=SolidIdle;
    uint32_t error=0,colour=0,durationMs=0,writes=0,restored=0,cancelled=0;
    uint32_t before[2][3]{},after[2][3]{},flush=0,frame=0;
    uint64_t startUs=0,visibleUs=0,expiryUs=0,endUs=0;
};
struct SolidState {
    Snapshot original;
    uint32_t owned=0;
    uint64_t transitionUntil=0;
};
struct SolidIo { Io reader; bool (*write)(void *,Region,uint32_t,uint32_t); };
struct SolidRequest { unsigned action=3,colour=0,durationMs=0; };
static inline bool solidEquals(const char *a,const char *b) {
    while(*a && *a==*b) {++a;++b;} return *a==*b;
}
static inline const char *solidPrefix(const char *command,const char *prefix) {
    while(*prefix) {if(*command++!=*prefix++)return nullptr;}return command;
}
static inline unsigned solidDuration(const char *p) {
    unsigned value=0,digits=0;
    while(*p>='0' && *p<='9' && digits<5) {value=value*10+unsigned(*p++-'0');++digits;}
    return *p || !digits || value<1000 || value>60000?0:value;
}
static inline SolidRequest parseSolid(const char *command) {
    if(const char *p=solidPrefix(command,"display fetch-swap ")) {
        const auto ms=solidDuration(p);return ms?SolidRequest{2,4,ms}:SolidRequest{};
    }
    const char *prefix="display solid ";
    while(*prefix) {if(*command++!=*prefix++)return {};}
    if(solidEquals(command,"status"))return {0,0,0};
    if(solidEquals(command,"cancel"))return {1,0,0};
    const char *names[]={"blue ","green ","red "};
    for(unsigned n=0;n<3;++n) {
        const char *word=names[n],*p=command;
        while(*word && *p==*word) {++p;++word;}
        if(*word)continue;
        const unsigned value=solidDuration(p);
        if(!value)return {};
        return {2,n+1,value};
    }
    return {};
}
class SolidFill {
    SolidIo io_; SolidState &s_; SolidDiagnostic &d_;
    static constexpr uint32_t Flush=0x16018,Mask=3,Unpack=0x03010002;
    static constexpr uint32_t Fields[3]={0x30,0x34,0x3c};
    static constexpr unsigned StablePipe[4]={0,1,2,4},StableIntf[6]={0,1,2,9,10,14},Order[3]={2,1,0};
    uint32_t desired(unsigned pipe,unsigned field) const {
        // A bounded normal-memory fetch comparison: keep solid fill off,
        // preserve alpha/constant words, change only red/blue unpack order.
        if(d_.colour==4)return field==1?Unpack:d_.before[pipe][field];
        if(field==0)return 0x4237ff;
        if(field==1)return Unpack;
        return d_.colour==1?0xffff0000u:d_.colour==2?0xff00ff00u:0xff0000ffu;
    }
    bool power() {
        uint32_t c[3]{};
        for(unsigned n=0;n<3;++n)
            if(!io_.reader.read(io_.reader.cookie,Clocks,ClockOffsets[n],&c[n]))return false;
        return c[0]!=~0u && (c[0]&0x80000000u) &&
            (c[1]&0x80000001u)==1 && (c[2]&0x80000001u)==1;
    }
    bool rd(uint32_t off,uint32_t &value) {
        return power() && io_.reader.read(io_.reader.cookie,Mdp,off,&value);
    }
    bool wr(uint32_t off,uint32_t value) {
        if(!power() || !io_.write(io_.reader.cookie,Mdp,off,value))return false;
        ++d_.writes;return true;
    }
    bool profile(const Snapshot &live) const {
        const auto &original=s_.original;
        if(live.status!=Ok || live.mdss!=original.mdss || live.mdp!=original.mdp ||
           live.dsiVersion!=original.dsiVersion || live.dsiControl!=original.dsiControl)return false;
        for(unsigned n=0;n<6;++n)
            if(live.ctl[n][0]!=original.ctl[n][0] || live.ctl[n][2]!=original.ctl[n][2])return false;
        for(unsigned n=0;n<2;++n) {
            for(unsigned field: StablePipe)
                if(live.pipe[n][field]!=original.pipe[n][field])return false;
            for(unsigned field: StableIntf)
                if(live.intf[n].reg[field]!=original.intf[n].reg[field])return false;
        }
        for(unsigned n=2;n<8;++n)
            for(unsigned field=0;field<6;++field)
                if(live.pipe[n][field]!=original.pipe[n][field])return false;
        return true;
    }
    bool values(bool restored) {
        for(unsigned n=0;n<2;++n)for(unsigned f=0;f<3;++f) {
            uint32_t &v=d_.after[n][f];
            if(!rd(Pipe[n]+Fields[f],v))return false;
            if(v!=(restored?d_.before[n][f]:desired(n,f)))return false;
        }
        return true;
    }
    void finish(unsigned stage,unsigned error,uint64_t now) {
        d_.stage=stage;d_.error=error;d_.endUs=now;
    }
    void restore(const Snapshot &live,uint64_t now) {
        if(!s_.owned) {d_.restored=1;finish(SolidDone,d_.error,now);return;}
        if(!profile(live)) {finish(SolidFailed,5,now);return;}
        if(!rd(Flush,d_.flush)) {d_.stage=SolidPowerWait;return;}
        // Do not replace another pending flush. Retry from the callback.
        if(d_.flush) {d_.stage=SolidPowerWait;return;}
        // Refuse to overwrite a value replaced by another configuration owner.
        for(unsigned n=0;n<2;++n)for(unsigned f=0;f<3;++f) {
            if(!(s_.owned&(1u<<(n*3+f))))continue;
            uint32_t value=0;
            if(!rd(Pipe[n]+Fields[f],value)) {d_.stage=SolidPowerWait;return;}
            if(value!=desired(n,f) && value!=d_.before[n][f]) {finish(SolidFailed,5,now);return;}
        }
        for(unsigned n=0;n<2;++n)for(unsigned f=0;f<3;++f) {
            if(!(s_.owned&(1u<<(n*3+f))))continue;
            if(!wr(Pipe[n]+Fields[f],d_.before[n][f])) {d_.stage=SolidPowerWait;return;}
        }
        if(!wr(Flush,Mask)) {d_.stage=SolidPowerWait;return;}
        d_.frame=live.intf[0].reg[4];d_.stage=SolidRestoring;
        s_.transitionUntil=now+200000;
    }
public:
    SolidFill(SolidIo io,SolidState &s,SolidDiagnostic &d):io_(io),s_(s),d_(d) {}
    bool active() const {
        return d_.stage==SolidQueued || d_.stage==SolidApplying || d_.stage==SolidVisible ||
            d_.stage==SolidRestoring || d_.stage==SolidPowerWait;
    }
    bool request(unsigned colour,unsigned ms,uint64_t now) {
        if(active() || s_.owned || colour<1 || colour>4 || ms<1000 || ms>60000 ||
           !io_.reader.read || !io_.write)return false;
        const uint32_t sequence=d_.sequence;
        d_={};d_.sequence=sequence;d_.stage=SolidQueued;d_.colour=colour;d_.durationMs=ms;
        d_.startUs=now;d_.expiryUs=now+uint64_t(ms)*1000;
        s_={};s_.transitionUntil=now+200000;return true;
    }
    void cancel(uint64_t now) {
        if(active() || s_.owned) {
            d_.cancelled=1;d_.expiryUs=now;
            if(d_.stage==SolidFailed) {d_.stage=SolidPowerWait;s_.transitionUntil=now+5000000;}
        }
    }
    // Each step is bounded register work. No sleeps or polling loops under a lock.
    void step(const Board &board,uint64_t now) {
        if(!active())return;
        const Snapshot live=capture(board,io_.reader,true);
        if(d_.stage==SolidQueued) {
            if(d_.cancelled || now>=d_.expiryUs) {d_.restored=1;finish(SolidDone,0,now);return;}
            if(!matchesConsole(board,live) || live.ctl[0][0]!=0xf0020000u) {finish(SolidRejected,1,now);return;}
            for(unsigned n=1;n<6;++n)
                if(live.ctl[n][2]) {finish(SolidRejected,1,now);return;}
            for(unsigned n=2;n<8;++n)
                if(live.pipe[n][0] || live.pipe[n][1]) {finish(SolidRejected,1,now);return;}
            if(live.ctl[0][1]) {
                if(now>=s_.transitionUntil)finish(SolidRejected,4,now);
                return;
            }
            s_.original=live;
            for(unsigned n=0;n<2;++n)for(unsigned f=0;f<3;++f)
                if(!rd(Pipe[n]+Fields[f],d_.before[n][f])) {finish(SolidRejected,2,now);return;}
            d_.frame=live.intf[0].reg[4];d_.stage=SolidApplying;
            // Constant/unpack first, format last; latch both pipes in one flush.
            for(unsigned n=0;n<2;++n)for(unsigned f: Order) {
                s_.owned|=1u<<(n*3+f);
                if(!wr(Pipe[n]+Fields[f],desired(n,f))) {
                    d_.error=3;d_.stage=SolidPowerWait;s_.transitionUntil=now+5000000;return;
                }
            }
            if(!wr(Flush,Mask)) {d_.error=3;d_.stage=SolidPowerWait;s_.transitionUntil=now+5000000;}
            return;
        }
        if(live.status!=Ok) {
            if(d_.stage!=SolidPowerWait) {
                d_.error=2;d_.stage=SolidPowerWait;s_.transitionUntil=now+5000000;
            } else if(now>=s_.transitionUntil)finish(SolidFailed,2,now);
            return;
        }
        if(!profile(live)) {finish(SolidFailed,5,now);return;}
        if(d_.stage==SolidPowerWait) {
            if(now>=s_.transitionUntil) {finish(SolidFailed,4,now);return;}
            restore(live,now);return;
        }
        if(d_.stage==SolidRestoring) {
            d_.flush=live.ctl[0][1];
            if(!d_.flush && live.intf[0].reg[4]!=d_.frame && values(true)) {
                d_.restored=1;s_.owned=0;finish(SolidDone,d_.error,now);
            } else if(now>=s_.transitionUntil)finish(SolidFailed,4,now);
            return;
        }
        if(d_.cancelled || now>=d_.expiryUs ||
           (d_.stage==SolidApplying && now>=s_.transitionUntil)) {
            if(d_.stage==SolidApplying && !d_.cancelled)d_.error=4;
            d_.stage=SolidPowerWait;s_.transitionUntil=now+5000000;restore(live,now);return;
        }
        if(d_.stage==SolidApplying) {
            d_.flush=live.ctl[0][1];
            if(!d_.flush && live.intf[0].reg[4]!=d_.frame && values(false)) {
                d_.stage=SolidVisible;d_.visibleUs=now;
            }
        } else if(!values(false)) {
            d_.error=5;d_.stage=SolidPowerWait;s_.transitionUntil=now+5000000;restore(live,now);
        }
    }
};
}
