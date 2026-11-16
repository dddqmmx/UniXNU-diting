/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "DitingDisplayText.h"
#include "DitingDisplayPresentation.h"
#include "DitingUsbReply.h"
using namespace DitingDisplay;
static const Board board{0xae00000,0xaf00000,0xae94000,0x84000,0x20000,0x400,1};
struct Hardware {
    uint32_t mdp[0x84000/4]{}, clock[0x20000/4]{}, dsi[0x400/4]{};
    unsigned calls=0, dependent=0, fail=0, gdsc=0, high=0;
    bool change=false, torn=false;
    Hardware() {
        clock[0x9000/4]=1u<<31; clock[0x80a4/4]=clock[0x800c/4]=1;
        dsi[0]=0x20060000; dsi[1]=1;
        for(auto base:Intf) {
            mdp[(base+0xa8)/4]=1; mdp[(base+0xac)/4]=42;
            mdp[(base+0x210)/4]=1; mdp[(base+0x214)/4]=0x1234; mdp[(base+0x218)/4]=2;
            mdp[(base+0x21c)/4]=0x5678; mdp[(base+0x220)/4]=3;
            mdp[(base+0x288)/4]=(1u<<19)|(1u<<20); mdp[(base+0x284)/4]=1;
            mdp[(base+0x298)/4]=17u<<16;
        }
    }
    static bool read(void *cookie, Region region, uint32_t off, uint32_t *out) {
        auto &h=*static_cast<Hardware *>(cookie); ++h.calls;
        if(h.fail==h.calls)return false;
        if(region==Clocks) {
            assert(off<sizeof(h.clock));
            if(off==0x9000 && ++h.gdsc==2 && h.change)h.clock[off/4]=0;
            *out=h.clock[off/4];return true;
        }
        ++h.dependent;
        assert((h.clock[0x9000/4]&(1u<<31)) && h.clock[0x80a4/4]==1 && h.clock[0x800c/4]==1);
        if(region==Dsi) {assert(off<sizeof(h.dsi));*out=h.dsi[off/4];return true;}
        assert(off<sizeof(h.mdp));
        if(h.torn && ((off&0xfff)==0x218 || (off&0xfff)==0x220)) {*out=++h.high;return true;}
        *out=h.mdp[off/4];return true;
    }
    Snapshot capture(bool full=true) {return DitingDisplay::capture(board,{this,read},full);}
};
int main(int argc,char **argv) {
    assert(argc>=2); const std::string test=argv[1]; Hardware hw;
    if(test=="board") {
        assert(valid(board));
        for(unsigned i=0;i<7;++i) {
            Board wrong=board;
            if(i==0)++wrong.version; if(i==1)++wrong.mdp; if(i==2)--wrong.mdpBytes;
            if(i==3)++wrong.clocks; if(i==4)--wrong.clockBytes; if(i==5)++wrong.dsi; if(i==6)--wrong.dsiBytes;
            assert(DitingDisplay::capture(wrong,{&hw,Hardware::read},true).status==Unavailable);
        }
        assert(!hw.calls);
    } else if(test=="board-file") {
        assert(argc==3); Board input{};auto *file=fopen(argv[2],"rb");assert(file);
        assert(fread(&input,1,sizeof(input),file)==40 && fgetc(file)==EOF);fclose(file);assert(valid(input));
    } else if(test=="copy-snapshot") {
        Snapshot s; s.status=Ok;s.mdss=s.mdp=0x80050000;s.dsiVersion=0x20060000;s.dsiControl=0x1f5;
        s.intf[0].reg[9]=1;s.intf[0].reg[10]=0x180075;s.intf[0].reg[13]=2712;
        s.intf[0].reg[14]=0x8000003c;s.ctl[0][2]=2;
        for(unsigned i=0;i<2;++i) {
            s.pipe[i][0]=0x0a980262;s.pipe[i][1]=0xb8000000;s.pipe[i][2]=4880;
            s.pipe[i][3]=0x237ff;s.pipe[i][5]=0x03020001;
        }
        assert(matchesConsole(board,s));
        Snapshot count=s;count.intf[0].reg[14]=0x80000001;assert(matchesConsole(board,count));
        for(unsigned test=0;test<14;++test) {
            Snapshot bad=s;
            if(test==0)bad.status=Changed;
            if(test==1)bad.dsiControl=0;
            if(test==2)bad.mdss=0;
            if(test==3)bad.dsiVersion=0;
            if(test==4)bad.intf[0].reg[9]=0;
            if(test==5)bad.intf[0].reg[10]&=~0x100000u;
            if(test==6)bad.mdp=0;
            if(test==7)bad.intf[0].reg[14]=60;
            if(test==8)bad.intf[1].reg[9]=1;
            if(test==9)bad.pipe[0][1]+=4096;
            if(test==10)bad.pipe[1][3]=0;
            if(test==11)bad.pipe[1][5]=0x03010002;
            if(test==12)bad.ctl[0][2]=0;
            if(test==13)bad.intf[0].reg[13]=2713;
            assert(!matchesConsole(board,bad));
        }
        Board wrong=board;++wrong.version;assert(!matchesConsole(wrong,s));
    } else if(test=="clock-off") {
        hw.clock[0x9000/4]=0;assert(hw.capture().status==PoweredOff);assert(!hw.dependent);
        hw.clock[0x9000/4]=1u<<31;hw.clock[0x80a4/4]=0x80000001;
        assert(hw.capture().status==PoweredOff);assert(!hw.dependent);
    } else if(test=="dsi-off") {
        hw.dsi[1]=0;assert(hw.capture().status==PoweredOff);assert(hw.dependent==2);
    } else if(test=="stable" || test=="fast") {
        const auto s=hw.capture(test=="stable");assert(s.status==Ok && s.reads==hw.calls && s.reads<=256);
        assert(s.intf[0].reg[4]==42 && s.intf[0].reg[16]==17u<<16);
        assert(s.intf[0].stable==3 && s.intf[0].panelTimestamp==0x200001234ULL);
        assert(s.intf[0].mdpTimestamp==0x300005678ULL);
        if(test=="fast")assert(s.reads<50);
    } else if(test=="torn") {
        hw.torn=true;const auto s=hw.capture();assert(s.status==Ok && !s.intf[0].stable && !s.intf[1].stable);
        assert(!s.intf[0].panelTimestamp && !s.intf[0].mdpTimestamp && s.reads<=256);
    } else if(test=="disabled") {
        for(auto base:Intf)hw.mdp[(base+0x210)/4]=0;
        const auto s=hw.capture();assert(s.status==Ok && !s.intf[0].stable && !s.intf[1].stable);
    } else if(test=="changed") {
        hw.change=true;assert(hw.capture().status==Changed);
    } else if(test=="io") {
        hw.fail=20;const auto s=hw.capture();assert(s.status==Unavailable && s.reads==20 && hw.calls==20);
    } else if(test=="bounds") {
        Snapshot s;Reader reader(board,{&hw,Hardware::read},s);uint32_t value=0;
        assert(!reader.read(Mdp,board.mdpBytes,value));assert(!reader.read(Dsi,1,value));
        assert(!reader.read(Region(3),0,value));assert(!hw.calls);
        s.reads=255;assert(reader.read(Clocks,0x9000,value));assert(!reader.read(Clocks,0x9000,value));assert(hw.calls==1);
    } else if(test=="format" || test=="reply") {
        Snapshot s=hw.capture();s.status=Changed;
        for(auto &v:s.intf) {for(auto &r:v.reg)r=UINT32_MAX;v.panelTimestamp=v.mdpTimestamp=UINT64_MAX;v.stable=3;}
        for(auto &pipe:s.pipe)for(auto &r:pipe)r=UINT32_MAX;
        for(auto &ctl:s.ctl)for(auto &r:ctl)r=UINT32_MAX;
        char reply[4096];unsigned used=0;constexpr unsigned limit=sizeof(reply)-96;
        diting_usb_reply_add(reply,limit,&used,"DitingUSB BEGIN display\n");
        used+=formatSnapshot(reply+used,limit-used,board,s,UINT64_MAX,UINT64_MAX);
        assert(used<limit-1 && strstr(reply,"CTL index=5") && strstr(reply,"timestamp_units=RAW"));
        diting_usb_reply_add(reply,sizeof(reply),&used,"DitingUSB END display\n");
        assert(strlen(reply)==used && std::string(reply).find("\nDitingUSB END display\n")!=std::string::npos);
        if(test=="reply") {printf("%s",reply);return 0;}
        char bounded[32];memset(bounded,0x5a,sizeof(bounded));
        assert(formatSnapshot(bounded,17,board,s,0,1)==16);
        assert(!bounded[16] && bounded[17]==0x5a && bounded[31]==0x5a);
        assert(formatSnapshot(nullptr,0,board,s,0,1)==0);
    } else if(test=="trailer") {
        char reply[4096];unsigned used=0;const std::string huge(8192,'x'),command(63,'a');
        diting_usb_reply_add(reply,sizeof(reply)-96,&used,"DitingUSB BEGIN %s\n",command.c_str());
        diting_usb_reply_add(reply,sizeof(reply)-96,&used,"%s",huge.c_str());
        assert(used==sizeof(reply)-97);
        diting_usb_reply_add(reply,sizeof(reply)-96,&used,"%s",huge.c_str());
        if(used && reply[used-1]!='\n')diting_usb_reply_add(reply,sizeof(reply),&used,"\n");
        diting_usb_reply_add(reply,sizeof(reply),&used,"DitingUSB END %s\n",command.c_str());
        assert(used<sizeof(reply) && strlen(reply)==used);
        assert(std::string(reply).find("\nDitingUSB END "+command+"\n")!=std::string::npos);
    } else assert(false);
    printf("PASS %s\n",argv[1]);
}
