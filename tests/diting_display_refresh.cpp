/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>
#include "DitingDisplayRefresh.h"
#include "DitingDisplayColour.h"
#include "DitingUsbReply.h"
using namespace DitingDisplay;
static const Board board{0xae00000,0xaf00000,0xae94000,0x84000,0x20000,0x400,1};
struct Hardware {
    uint32_t mdp[0x84000/4]{},clock[0x20000/4]{},dsi[0x400/4]{};
    unsigned calls=0,writeCalls=0,failWrite=0,us=0;
    bool stuck=false,busy=false,clockOff=false;
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    Hardware() {
        clock[0x9000/4]=0x80000000;clock[0x80a4/4]=clock[0x800c/4]=1;
        dsi[0]=0x20060000;dsi[1]=0x1f5;
        mdp[0]=mdp[0x1000/4]=0x80050000;
        mdp[0x36284/4]=1;mdp[0x36288/4]=0x180075;mdp[0x362b0/4]=100;
        mdp[0x362b4/4]=0x8000003c;mdp[0x160f4/4]=2;
        mdp[0x362a0/4]=2712;mdp[0x36290/4]=0xabcd0040;
        mdp[0x1418/4]=0x12345670;mdp[0x1348/4]=0x22222222;
        for(unsigned i=0;i<2;++i) {
            const auto p=Pipe[i]/4;
            mdp[p]=0x0a980262;mdp[p+0x14/4]=0xb8000000;
            mdp[p+0x24/4]=4880;mdp[p+0x30/4]=0x237ff;mdp[p+0x34/4]=0x03020001;
        }
    }
    static bool read(void *cookie,Region r,uint32_t off,uint32_t *out) {
        auto &h=*static_cast<Hardware *>(cookie);++h.calls;
        assert(!(off&3));
        if(r==Clocks) {assert(off<sizeof(h.clock));*out=h.clock[off/4];return true;}
        assert(r==Mdp || r==Dsi);
        assert(h.clock[0x9000/4]&0x80000000); // No dependent MMIO after power loss.
        if(r==Dsi) {assert(off<sizeof(h.dsi));*out=h.dsi[off/4];}
        else {
            assert(off<sizeof(h.mdp));
            if(off==0x134c) {
                assert(h.mdp[0x1418/4]==0x405 && h.mdp[0x1348/4]==0x7001);
                *out=h.busy?0x80:0;
            } else *out=h.mdp[off/4];
        }
        return true;
    }
    static bool write(void *cookie,Region r,uint32_t off,uint32_t value) {
        auto &h=*static_cast<Hardware *>(cookie);assert(r==Mdp);
        assert(h.clock[0x9000/4]&0x80000000);
        if(++h.writeCalls==h.failWrite)return false;
        if(off==0x362b4 && value==0x80000001) {
            assert(!(h.mdp[0x36288/4]&0x100000));
            assert(!(h.mdp[off/4]&0x80000000));
            assert(h.mdp[0x362b0/4]>=2712 && !h.busy);
        }
        if(off==0x36290) {
            assert(!(h.mdp[0x362b4/4]&0x80000000));
            assert(!(h.mdp[0x36288/4]&0x100000));
            assert(value==0xabcd0a98);h.busy=false;
        }
        assert(off==0x36288 || off==0x362b4 || off==0x36290 || off==0x1418 || off==0x1348);
        h.mdp[off/4]=value;h.writes.emplace_back(off,value);return true;
    }
    static void pause(void *cookie,unsigned n) {
        auto &h=*static_cast<Hardware *>(cookie);h.us+=n;
        if(!h.stuck && h.us>=1000)h.mdp[0x362b0/4]=2712;
        if(h.clockOff)h.clock[0x9000/4]=0;
    }
    Snapshot snapshot() {return capture(board,{this,read},true);}
    bool run(const Snapshot &s,RefreshDiagnostic &d) {return Refresh({{this,read},write,pause},d).run(board,s);}
    void original() {
        assert(mdp[0x362b4/4]==0x8000003c && mdp[0x36288/4]==0x180075);
        assert(mdp[0x1418/4]==0x12345670 && mdp[0x1348/4]==0x22222222);
    }
};
int main(int argc,char **argv) {
    assert(argc==2);Hardware hw;RefreshDiagnostic d;
    const auto s=hw.snapshot();assert(matchesConsole(board,s));
    if(!strcmp(argv[1],"transition")) {
        hw.busy=true;assert(hw.run(s,d));
        assert(d.stage==4 && !d.error && d.after==0x80000001 && d.teAfter==d.teBefore);
        assert(hw.writes[0]==std::make_pair(0x36288u,0x80075u));
        assert(hw.writes[1]==std::make_pair(0x362b4u,0x3cu));
        assert(hw.mdp[0x1418/4]==0x12345670 && hw.mdp[0x1348/4]==0x22222222);
        assert(hw.us<40000 && d.writes==hw.writes.size());
    } else if(!strcmp(argv[1],"write-failures")) {
        for(unsigned fail=1;fail<=8;++fail) {
            Hardware h;RefreshDiagnostic r;const auto snap=h.snapshot();h.failWrite=fail;
            assert(!h.run(snap,r));assert(r.stage==6 && r.error==3 && r.restored);
            h.original();assert(h.us<60000);
        }
    } else if(!strcmp(argv[1],"timeout")) {
        hw.stuck=true;assert(!hw.run(s,d));
        assert(d.stage==6 && d.error==4 && d.restored && hw.us<=40000);hw.original();
    } else if(!strcmp(argv[1],"power-loss")) {
        hw.clockOff=true;assert(!hw.run(s,d));
        assert(d.stage==6 && d.error==6 && !d.restored && hw.writes.size()==2);
    } else if(!strcmp(argv[1],"gates")) {
        const unsigned before=hw.calls;
        Snapshot wrong=s;wrong.pipe[1][5]=0x03010002;
        assert(!hw.run(wrong,d) && d.stage==5 && !d.writes && hw.calls==before);
        RefreshDiagnostic ready;Snapshot existing=s;existing.intf[0].reg[14]=0x80000001;
        assert(hw.run(existing,ready) && ready.stage==4 && !ready.writes && hw.calls==before);
        RefreshDiagnostic changed;hw.mdp[0x362b4/4]=0x80000002;
        assert(!hw.run(s,changed) && changed.error==5 && !changed.writes);
    } else if(!strcmp(argv[1],"colour")) {
        // Known register sentinels detect address/base and coefficient ordering
        // errors, independently of the decoder and rendered colour labels.
        hw.dsi[0x40/4]=0x123;hw.dsi[0x2a8/4]=0x456;
        hw.mdp[0x81f08/4]=7;hw.mdp[0x81f88/4]=8;
        hw.mdp[0x56704/4]=0x11;hw.mdp[0x58784/4]=0x22;
        const auto c=captureColour(board,{&hw,Hardware::read});
        assert(c.path.status==Ok && c.path.reads<=256);
        assert(c.dsi[0]==0x123 && c.dsi[4]==0x456 && c.dsc[0][3]==7 && c.dsc[1][3]==8);
        assert(c.pcc[0][0]==0x11 && c.pcc[1][32]==0x22 && hw.writes.empty());
        ColourSnapshot maximum=c;
        for(auto &bank:maximum.pcc)for(auto &word:bank)word=UINT32_MAX;
        char reply[4096];unsigned used=0;
        diting_usb_reply_add(reply,sizeof(reply)-96,&used,"DitingUSB BEGIN display color\n");
        used+=formatColour(reply+used,sizeof(reply)-96-used,maximum,UINT64_MAX,UINT64_MAX);
        assert(used<sizeof(reply)-97 && strstr(reply,"DSC index=1"));
        diting_usb_reply_add(reply,sizeof(reply),&used,"DitingUSB END display color\n");
        assert(strlen(reply)==used && strstr(reply,"\nDitingUSB END display color\n"));
        hw.clock[0x9000/4]=0;
        assert(captureColour(board,{&hw,Hardware::read}).path.status==PoweredOff && hw.writes.empty());
    } else assert(false);
    printf("PASS %s\n",argv[1]);
    return 0;
}
