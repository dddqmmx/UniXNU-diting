/* SPDX-License-Identifier: GPL-2.0-only */
#include "DitingBandwidthProbe.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>
using namespace DitingBandwidth;
struct Hardware {
    std::array<uint32_t,192> monitor{},global{};
    std::vector<std::tuple<Region,unsigned,uint32_t>> writes;
    unsigned kind=5;
    uint64_t us=100;
    bool data=true,saturated=false,late=false,failSetup=false,failRestore=false,irq=false;
    unsigned enable() const {return kind==5?0x10:0x2a0;}
    unsigned window() const {return kind==5?0x20:0x2a8;}
    unsigned count() const {return kind==5?0x38:0x2d0;}
    unsigned maximum() const {return kind==5?0x44:0x2e0;}
    unsigned status() const {return kind==5?0:0x100;}
    Hardware(unsigned type=5):kind(type) {monitor[window()/4]=0x90000000;}
    static bool read(void *cookie,Region region,unsigned off,uint32_t *value) {
        auto &h=*static_cast<Hardware *>(cookie);assert(!(off&3) && off<0x300);
        *value=(region==Global?h.global:h.monitor)[off/4];return true;
    }
    static bool write(void *cookie,Region region,unsigned off,uint32_t value) {
        auto &h=*static_cast<Hardware *>(cookie);assert(!(off&3) && off<0x300);
        h.writes.emplace_back(region,off,value);
        if(region==Global) {
            assert(off==0x108 && value==1);h.global[0x100/4]&=~value;return true;
        }
        if(h.failSetup && off==h.window()+4 && value==512) {h.failSetup=false;return false;}
        if(h.failRestore && off==h.window() && value==0x90000000)return false;
        if(off==h.enable()+4 && value) {
            h.monitor[h.count()/4]=h.monitor[(h.count()+4)/4]=h.monitor[(h.count()+8)/4]=0;
            for(unsigned i=0;i<4;++i)h.monitor[(h.maximum()+4*i)/4]=0;
        }
        if(off==h.status()+8)h.monitor[h.status()/4]&=~value;
        else h.monitor[off/4]=value;
        return true;
    }
    static uint64_t time(void *cookie) {return static_cast<Hardware *>(cookie)->us;}
    static void sleep(void *cookie,unsigned delay) {
        auto &h=*static_cast<Hardware *>(cookie);assert(delay==12000 && h.monitor[h.enable()/4]==1);
        assert(h.monitor[(h.status()+12)/4]==0); // interrupts remain disabled
        h.us+=h.late?300000:delay;
        if(h.data) {
            h.monitor[h.count()/4]=3;h.monitor[(h.count()+4)/4]=5000;
            h.monitor[(h.count()+8)/4]=8u<<24;
            h.monitor[(h.maximum()+12)/4]=h.saturated?4095:199;
            if(h.irq) {h.monitor[h.status()/4]=h.kind==5?8:0x80;if(h.kind==4)h.global[0x100/4]|=1;}
        }
    }
    Io io() {return {this,read,write,time,sleep};}
};
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"measured-restored")) {
        for(unsigned type:{4u,5u}) {
            Hardware h(type);h.irq=true;const auto old=h.monitor;Diagnostic d;
            assert(Probe(h.io(),d).run(type) && d.stage==3 && !d.error && d.restored);
            assert(d.peakBytesPerSecond==13107200000ULL && d.startUs==100 && d.endUs==12100);
            assert(h.monitor[h.window()/4]==old[h.window()/4] && h.monitor[h.enable()/4]==0);
            assert(h.monitor[h.status()/4]==0 && h.monitor[(h.count()+8)/4]==0 && h.global[0x100/4]==0);
            for(const auto &w:h.writes)if(std::get<0>(w)==Monitor && std::get<1>(w)==h.status()+12)assert(!std::get<2>(w));
        }
    } else if(!strcmp(argv[1],"owned-prerequisites")) {
        for(unsigned type:{4u,5u})for(unsigned reason=0;reason<6;++reason) {
            Hardware h(type);
            if(reason==0)h.monitor[h.enable()/4]=1;
            if(reason==1)h.monitor[h.status()/4]=type==5?8:0x80;
            if(reason==2)h.monitor[(h.status()+12)/4]=type==5?8:0x80;
            if(reason==3)h.monitor[h.count()/4]=1;
            if(reason==4)h.monitor[(h.count()+8)/4]=1;
            if(reason==5)h.monitor[h.maximum()/4]=1;
            const auto old=h.monitor;Diagnostic d;
            assert(!Probe(h.io(),d).run(type) && d.error==4 && h.writes.empty() && h.monitor==old);
            const unsigned bits[]={0,2,1,3,4,5};
            assert(d.rejectMask==(1u<<bits[reason]) && d.guardsBefore[bits[reason]]!=0);
        }
        Hardware h(4);h.global[0x100/4]=1;Diagnostic d;
        assert(!Probe(h.io(),d).run(4) && h.writes.empty());
        Hardware filter;filter.monitor[0x18/4]=1;Diagnostic f;
        assert(!Probe(filter.io(),f).run(5) && filter.writes.empty());
        assert(f.rejectMask==(1u<<11) && f.guardsBefore[11]==1 && f.configBefore[8]==1);
    } else if(!strcmp(argv[1],"owned-diagnostics")) {
        Hardware h;h.monitor[0x18/4]=0x77;h.monitor[0x1c/4]=0x22;
        h.monitor[h.count()/4]=3;h.monitor[(h.count()+4)/4]=456;
        h.monitor[(h.count()+8)/4]=0x100;h.monitor[(h.maximum()+8)/4]=12;
        const auto old=h.monitor;Diagnostic d;
        assert(!Probe(h.io(),d).run(5) && h.writes.empty() && h.monitor==old);
        assert(d.rejectMask==((1u<<11)|(1u<<12)|(1u<<3)|(1u<<4)|(1u<<7)));
        assert(d.guardsBefore[11]==0x77 && d.guardsBefore[12]==0x22 && d.guardsBefore[13]==456);
        assert(d.guardsBefore[3]==3 && d.guardsBefore[4]==0x100 && d.guardsBefore[7]==12);
        assert(d.windowBefore==0x90000000 && !d.startUs && !d.endUs && !d.peakBytesPerSecond);
        Hardware global(4);global.global[0x10c/4]=1;Diagnostic g;
        assert(!Probe(global.io(),g).run(4) && global.writes.empty());
        assert(g.rejectMask==(1u<<10) && g.guardsBefore[10]==1);
    } else if(!strcmp(argv[1],"no-data")) {
        for(unsigned type:{4u,5u})for(unsigned fault=0;fault<3;++fault) {
            Hardware h(type);h.data=fault!=0;h.saturated=fault==1;h.late=fault==2;Diagnostic d;
            assert(!Probe(h.io(),d).run(type) && d.error==5 && d.restored && !d.peakBytesPerSecond);
            assert(h.monitor[h.window()/4]==0x90000000 && !h.monitor[h.enable()/4]);
        }
    } else if(!strcmp(argv[1],"failed-restore")) {
        Hardware h;h.failRestore=true;Diagnostic d;
        assert(!Probe(h.io(),d).run(5) && d.error==7 && !d.restored && !h.monitor[h.enable()/4]);
    } else if(!strcmp(argv[1],"failed-setup")) {
        Hardware h;h.failSetup=true;Diagnostic d;
        assert(!Probe(h.io(),d).run(5) && d.error==3 && d.restored && !d.startUs && !d.peakBytesPerSecond);
        assert(h.monitor[h.window()/4]==0x90000000 && !h.monitor[h.enable()/4]);
    } else assert(false);
    printf("PASS %s\n",argv[1]);
}
