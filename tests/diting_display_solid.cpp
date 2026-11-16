/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>
#include "DitingDisplaySolid.h"
using namespace DitingDisplay;
static const Board board{0xae00000,0xaf00000,0xae94000,0x84000,0x20000,0x400,1};
struct Hardware {
    uint32_t mdp[0x84000/4]{},clock[0x20000/4]{},dsi[0x400/4]{};
    unsigned writeCalls=0,failWrite=0,dependentReads=0;
    SolidState state;SolidDiagnostic diagnostic;
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    Hardware() {
        clock[0x9000/4]=0x80000000;clock[0x80a4/4]=clock[0x800c/4]=1;
        dsi[0]=0x20060000;dsi[1]=0x1f5;
        mdp[0]=mdp[0x1000/4]=0x80050000;
        mdp[0x36284/4]=1;mdp[0x36288/4]=0x18003a;mdp[0x362b0/4]=2712;
        mdp[0x362b4/4]=0x80000001;mdp[0x160f4/4]=2;mdp[0x16014/4]=0xf0020000;
        for(unsigned n=0;n<2;++n) {
            const auto p=Pipe[n]/4;
            mdp[p]=0x0a980262;mdp[p+0x14/4]=0xb8000000;
            mdp[p+0x24/4]=4880;mdp[p+0x30/4]=0x237ff;mdp[p+0x34/4]=0x03020001;
            mdp[p+0x3c/4]=0x10203040+n;
        }
    }
    static bool read(void *cookie,Region r,uint32_t off,uint32_t *out) {
        auto &h=*static_cast<Hardware *>(cookie);assert(!(off&3));
        if(r==Clocks) {assert(off<sizeof(h.clock));*out=h.clock[off/4];return true;}
        ++h.dependentReads;assert(h.clock[0x9000/4]&0x80000000);
        if(r==Dsi) {assert(off<sizeof(h.dsi));*out=h.dsi[off/4];}
        else {assert(r==Mdp && off<sizeof(h.mdp));*out=h.mdp[off/4];}
        return true;
    }
    static bool write(void *cookie,Region r,uint32_t off,uint32_t value) {
        auto &h=*static_cast<Hardware *>(cookie);
        assert(r==Mdp && (h.clock[0x9000/4]&0x80000000));
        assert(off==0x5030 || off==0x5034 || off==0x503c ||
               off==0x7030 || off==0x7034 || off==0x703c || off==0x16018);
        if(++h.writeCalls==h.failWrite)return false;
        if(off==0x16018) {assert(!h.mdp[off/4] && value==3);}
        h.mdp[off/4]=value;h.writes.emplace_back(off,value);return true;
    }
    SolidFill controller() {return {{{this,read},write},state,diagnostic};}
    void step(uint64_t now) {controller().step(board,now);}
    void retire() {mdp[0x16018/4]=0;++mdp[0x360ac/4];}
    void visible(unsigned colour=1) {
        assert(controller().request(colour,1000,100));step(100);
        assert(diagnostic.stage==SolidApplying && writes.size()==7);
        retire();step(2100);assert(diagnostic.stage==SolidVisible);
    }
    void original() const {
        for(unsigned n=0;n<2;++n) {
            assert(mdp[(Pipe[n]+0x30)/4]==0x237ff);
            assert(mdp[(Pipe[n]+0x34)/4]==0x03020001);
            assert(mdp[(Pipe[n]+0x3c)/4]==0x10203040+n);
        }
    }
};
int main(int argc,char **argv) {
    assert(argc==2);Hardware h;
    if(!strcmp(argv[1],"normal")) {
        for(unsigned colour=1;colour<=3;++colour) {
            Hardware v;v.visible(colour);
            assert(v.mdp[0x5030/4]==0x4237ff && v.mdp[0x5034/4]==0x03010002);
            assert(v.mdp[0x503c/4]==(colour==1?0xffff0000u:colour==2?0xff00ff00u:0xff0000ffu));
            v.step(1000099);assert(v.diagnostic.stage==SolidVisible && v.writes.size()==7);
            v.step(1000100);assert(v.diagnostic.stage==SolidRestoring);v.original();
            // Readback alone cannot certify the latched hardware state.
            v.step(1001100);assert(v.diagnostic.stage==SolidRestoring && !v.diagnostic.restored);
            v.retire();v.step(1002100);
            assert(v.diagnostic.stage==SolidDone && v.diagnostic.restored && !v.diagnostic.error);
            assert(!v.controller().active() && v.state.owned==0 && v.writes.size()==14);
        }
    } else if(!strcmp(argv[1],"fetch-order")) {
        h.visible(4);
        for(unsigned n=0;n<2;++n) {
            assert(h.mdp[(Pipe[n]+0x30)/4]==0x237ff); // Normal memory fetch, no solid fill.
            assert(h.mdp[(Pipe[n]+0x34)/4]==0x03010002);
            assert(h.mdp[(Pipe[n]+0x3c)/4]==0x10203040+n);
        }
        h.step(1000100);h.retire();h.step(1002100);h.original();
        assert(h.diagnostic.restored && !h.diagnostic.error);
        assert(parseSolid("display fetch-swap 1000").colour==4);
        assert(parseSolid("display fetch-swap 60000").durationMs==60000);
        for(const char *bad: {"display fetch-swap 999","display fetch-swap 60001","display fetch-swap 1000 extra"})
            assert(parseSolid(bad).action==3);
    } else if(!strcmp(argv[1],"write-failures")) {
        for(unsigned fail=1;fail<=7;++fail) {
            Hardware v;v.failWrite=fail;assert(v.controller().request(1,1000,100));v.step(100);
            assert(v.diagnostic.stage==SolidPowerWait && v.diagnostic.error==3);
            v.step(2100);assert(v.diagnostic.stage==SolidRestoring);
            v.retire();v.step(4100);v.original();
            assert(v.diagnostic.stage==SolidDone && v.diagnostic.restored && v.diagnostic.error==3);
        }
    } else if(!strcmp(argv[1],"cancel-busy")) {
        h.visible();const auto writes=h.writes.size();
        assert(!h.controller().request(3,1000,3000) && h.diagnostic.colour==1 && h.writes.size()==writes);
        h.controller().cancel(3000);h.step(3000);assert(h.diagnostic.stage==SolidRestoring);
        h.retire();h.step(5000);h.original();assert(h.diagnostic.restored && h.diagnostic.cancelled);
        Hardware v;assert(v.controller().request(1,1000,100));v.controller().cancel(101);v.step(102);
        assert(v.diagnostic.stage==SolidDone && v.diagnostic.restored && v.writes.empty());
    } else if(!strcmp(argv[1],"power-loss")) {
        h.visible();h.clock[0x9000/4]=0;const auto reads=h.dependentReads;
        h.step(3000);assert(h.diagnostic.stage==SolidPowerWait && h.dependentReads==reads);
        h.clock[0x9000/4]=0x80000000;h.step(23000);assert(h.diagnostic.stage==SolidRestoring);
        h.retire();h.step(25000);h.original();assert(h.diagnostic.restored);
        Hardware v;v.visible();v.clock[0x9000/4]=0;v.step(3000);v.step(5003000);
        assert(v.diagnostic.stage==SolidFailed && !v.controller().active() && !v.diagnostic.restored);
        v.clock[0x9000/4]=0x80000000;v.controller().cancel(5004000);v.step(5004000);
        v.retire();v.step(5006000);v.original();assert(v.diagnostic.restored);
    } else if(!strcmp(argv[1],"profile-gates")) {
        h.mdp[0x160f4/4]=4;assert(h.controller().request(1,1000,100));h.step(100);
        assert(h.diagnostic.stage==SolidRejected && h.writes.empty());
        Hardware v;v.visible();const auto writes=v.writes.size();v.mdp[0x160f4/4]=4;v.step(3000);
        assert(v.diagnostic.stage==SolidFailed && v.diagnostic.error==5 && !v.diagnostic.restored && v.writes.size()==writes);
        Hardware b;b.mdp[0x16018/4]=0x400;assert(b.controller().request(1,1000,100));b.step(100);b.step(200100);
        assert(b.diagnostic.stage==SolidRejected && b.diagnostic.error==4 && b.writes.empty());
    } else if(!strcmp(argv[1],"flush-timeout")) {
        assert(h.controller().request(1,1000,100));h.step(100);h.step(200100);
        assert(h.diagnostic.stage==SolidPowerWait && h.diagnostic.error==4);
        const auto writes=h.writes.size();h.step(5200100);
        assert(h.diagnostic.stage==SolidFailed && !h.diagnostic.restored && h.writes.size()==writes);
        h.retire();h.controller().cancel(5200200);h.step(5200200);h.retire();h.step(5202200);
        h.original();assert(h.diagnostic.restored);
    } else if(!strcmp(argv[1],"parser")) {
        assert(parseSolid("display solid blue 1000").colour==1);
        assert(parseSolid("display solid green 60000").durationMs==60000);
        assert(parseSolid("display solid red 12000").colour==3);
        assert(parseSolid("display solid status").action==0 && parseSolid("display solid cancel").action==1);
        for(const char *bad: {"","display solid blue 999","display solid blue 60001","display solid blue 600000",
            "display solid blue -1000","display solid blue 1000 extra","display solid unknown 1000","display solid cancel extra"})
            assert(parseSolid(bad).action==3);
    } else assert(false);
    printf("PASS %s\n",argv[1]);
}
