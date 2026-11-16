/* SPDX-License-Identifier: GPL-2.0-only */
/* SM8475 BWMON4/5 register/retirement contract, based on Xiaomi's
 * drivers/devfreq/bimc-bwmon.c at eff4b40407a1b6f1b1ab84554cbb540bcb495175.
 * A requested, bounded observation; it changes no interconnect or clock vote.
 * An enabled monitor, pending IRQ or retained measurement is never borrowed. */
#pragma once
#include <stdint.h>

namespace DitingBandwidth {
enum Region { Monitor, Global };
struct Io {
    void *context;
    bool (*read)(void *, Region, unsigned, uint32_t *);
    bool (*write)(void *, Region, unsigned, uint32_t);
    uint64_t (*timeUs)(void *);
    void (*sleepUs)(void *, unsigned);
};
struct Diagnostic {
    unsigned stage=0, error=0, writes=0, restored=0, kind=0;
    uint32_t count=0, timer=0, zones=0, maximum[4]{}, irq=0;
    uint32_t enabledBefore=0, windowBefore=0, window=19200, countUnit=65536;
    uint32_t rejectMask=0, configBefore[10]{}, guardsBefore[14]{};
    uint64_t startUs=0, endUs=0, peakBytesPerSecond=0;
};
class Probe {
    Io io_; Diagnostic &d_;
    unsigned enable_,clear_,window_,threshold_,actions_,limit_,count_,timer_,zones_,maximum_,irq_,irqClear_,irqEnable_;
    unsigned offsets_[10]{}; uint32_t before_[10]{};
    unsigned saved_=0;
    bool touched_=false;
    bool rd(Region region,unsigned off,uint32_t &value) {
        if(!io_.read(io_.context,region,off,&value)) {d_.error=2;return false;}
        return true;
    }
    bool wr(Region region,unsigned off,uint32_t value) {
        if(!io_.write(io_.context,region,off,value)) {d_.error=3;return false;}
        ++d_.writes;return true;
    }
    bool put(unsigned off,uint32_t value) {
        uint32_t actual=0;
        return wr(Monitor,off,value) && rd(Monitor,off,actual) && actual==value;
    }
    bool clear() {
        // MON3 clear registers are not self-clearing on every revision.
        bool ok=wr(Monitor,clear_,2);
        if(d_.kind==5)ok=wr(Monitor,clear_,0) && ok;
        return ok;
    }
    bool restore() {
        if(!touched_)return true;
        bool ok=put(enable_,0);
        // Clear only the disabled measurement's own local interrupt flags.
        uint32_t status=0;
        if(rd(Monitor,irq_,status)) {
            const uint32_t owned=status&(d_.kind==5?0x0fu:0xf0u);
            if(owned) {
                ok=wr(Monitor,irqClear_,owned) && ok;
                if(d_.kind==5)ok=wr(Monitor,irqClear_,0) && ok;
                else ok=wr(Global,0x108,1) && ok;
            }
        } else ok=false;
        ok=clear() && ok;
        for(unsigned i=saved_;i>1;--i)ok=put(offsets_[i-1],before_[i-1]) && ok;
        ok=put(enable_,before_[0]) && ok;
        for(unsigned i=0;i<saved_;++i) {
            uint32_t value=0;ok=rd(Monitor,offsets_[i],value) && value==before_[i] && ok;
        }
        return ok;
    }
public:
    Probe(Io io,Diagnostic &d):io_(io),d_(d) {}
    bool run(unsigned kind) {
        d_.stage=1;d_.kind=kind;
        if((kind!=4 && kind!=5) || !io_.read || !io_.write || !io_.timeUs || !io_.sleepUs) {
            d_.stage=4;d_.error=1;return false;
        }
        enable_=kind==5?0x10:0x2a0;clear_=enable_+4;
        window_=kind==5?0x20:0x2a8;threshold_=window_+4;actions_=window_+16;limit_=window_+20;
        count_=kind==5?0x38:0x2d0;timer_=count_+4;zones_=count_+8;maximum_=kind==5?0x44:0x2e0;
        irq_=kind==5?0:0x100;irqClear_=irq_+8;irqEnable_=irq_+12;
        const unsigned config[]={enable_,window_,threshold_,threshold_+4,threshold_+8,actions_,limit_,irqEnable_,0x18,0x1c};
        saved_=kind==5?10:8;
        for(unsigned i=0;i<saved_;++i) {
            offsets_[i]=config[i];
            if(!rd(Monitor,offsets_[i],before_[i])) {d_.stage=4;return false;}
            d_.configBefore[i]=before_[i];
        }
        d_.enabledBefore=before_[0];d_.windowBefore=before_[1];
        uint32_t status=0,count=0,zones=0,global=0;
        if(!rd(Monitor,irq_,status) || !rd(Monitor,count_,count) || !rd(Monitor,zones_,zones)) {
            d_.stage=4;return false;
        }
        // Disabled counters can still belong to firmware. Require an empty
        // counter set and no throttle/filter/IRQ owner before the first write.
        d_.guardsBefore[0]=before_[0];d_.guardsBefore[1]=before_[7];
        d_.guardsBefore[2]=status;d_.guardsBefore[3]=count;d_.guardsBefore[4]=zones;
        if(!rd(Monitor,timer_,d_.guardsBefore[13])) {d_.stage=4;return false;}
        for(unsigned i=0;i<4;++i) {
            uint32_t value=0;
            if(!rd(Monitor,maximum_+4*i,value)) {d_.stage=4;return false;}
            d_.guardsBefore[5+i]=value;
        }
        if(kind==4) {
            if(!rd(Global,0x100,global)) {d_.stage=4;return false;}
            d_.guardsBefore[9]=global;
            if(!rd(Global,0x10c,global)) {d_.stage=4;return false;}
            d_.guardsBefore[10]=global;
        } else {d_.guardsBefore[11]=before_[8];d_.guardsBefore[12]=before_[9];}
        for(unsigned i=0;i<13;++i) {
            const uint32_t owned=i==9 || i==10?d_.guardsBefore[i]&1u:d_.guardsBefore[i];
            if(owned)d_.rejectMask|=1u<<i;
        }
        if(d_.rejectMask) {d_.stage=4;d_.error=4;return false;}
        touched_=true;d_.stage=2;
        bool ok=clear() && put(window_,d_.window) && put(threshold_,512) &&
            put(threshold_+4,128) && put(threshold_+8,16) &&
            put(actions_,0x95250901u) && put(limit_,0xffffffffu);
        if(kind==5)ok=put(0x18,0) && put(0x1c,0) && ok;
        if(ok) {
            d_.startUs=io_.timeUs(io_.context);
            ok=put(enable_,1);
            if(ok)io_.sleepUs(io_.context,12000);
            ok=put(enable_,0) && ok;d_.endUs=io_.timeUs(io_.context);
            ok=rd(Monitor,count_,d_.count) && rd(Monitor,timer_,d_.timer) &&
                rd(Monitor,zones_,d_.zones) && rd(Monitor,irq_,d_.irq) && ok;
            uint32_t maximum=0;
            for(unsigned i=0;i<4;++i) {
                ok=rd(Monitor,maximum_+4*i,d_.maximum[i]) && ok;
                if(d_.maximum[i]>maximum)maximum=d_.maximum[i];
            }
            // Xiaomi's get_zone_count adds one to a nonzero MAX count. The
            // result is a quantized peak in a 1 ms hardware window; it is
            // neither sustained capacity nor a DDR clock measurement.
            if(ok && maximum && maximum<4095 && d_.zones && d_.endUs>d_.startUs &&
               d_.endUs-d_.startUs>=2000 && d_.endUs-d_.startUs<=200000)
                d_.peakBytesPerSecond=uint64_t(maximum+1)*d_.countUnit*19200000u/d_.window;
            else {ok=false;if(!d_.error)d_.error=5;}
        }
        if(!ok && !d_.error)d_.error=6;
        const unsigned error=d_.error;
        d_.restored=restore();
        d_.error=d_.restored?error:7;
        d_.stage=ok && d_.restored?3:5;
        return d_.stage==3;
    }
};
}
