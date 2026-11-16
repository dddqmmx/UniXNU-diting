/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "DitingPanelMode.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>
using namespace DitingBrightness;
struct Hardware {
    std::array<uint32_t,256> regs{};
    std::vector<uint32_t> fifo;
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    unsigned id[3]={0xc5,0x10,0},value=0,queries=0,batches=0,tears=0,lastHz=0;
    DitingPanel::Selection selected{1,0x004c3132,0x00360200,1220,2712,1};
    unsigned address=0,pixel=0x77,colourWrites=0;
    unsigned returnLimit=2,negotiations=0;
    bool complete=true,badReturn=false,badReadback=false,failTear=false,unsupportedColour=false;
    bool unsupportedPixel=false,rejectColour=false,badColourRestore=false,stallColour=false;
    Hardware() {
        regs[Control/4]=3;regs[DmaControl/4]=0xa9000100;
        regs[DmaLength/4]=0x1234;regs[TriggerControl/4]=0x18105;regs[PatternControl/4]=0x1200;
        regs[InterruptControl/4]=EnableMask|(1u<<16)|(1u<<28);
    }
    static uint32_t read(void *cookie,uint32_t off) {
        assert(off<0x400 && !(off&3));return static_cast<Hardware *>(cookie)->regs[off/4];
    }
    static void write(void *cookie,uint32_t off,uint32_t v) {
        auto &h=*static_cast<Hardware *>(cookie);assert(off<0x400 && !(off&3));
        h.writes.emplace_back(off,v);
        if(off==InterruptControl) {
            const uint32_t status=DmaDone|BtaDone|ErrorPending|(1u<<16)|(1u<<28);
            assert(!(v&status&~(DmaDone|BtaDone|ErrorPending)));
            h.regs[off/4]=(h.regs[off/4]&~(v&status)&~EnableMask)|(v&EnableMask);
        } else if(off==AckError)h.regs[off/4]&=~v;
        else h.regs[off/4]=v;
        if(off==PatternData)h.fifo.push_back(v);
        if(off==ReadControl && v)h.regs[ReadControl/4]=h.regs[ReadData0/4]=h.regs[ReadData1/4]=0;
        if(off==FifoReset && v)h.fifo.clear();
        if(off!=DmaTrigger)return;
        h.regs[Status/4]|=DmaBusy;
        const unsigned words=h.regs[DmaLength/4]/4;assert(words && words<=16 && h.fifo.size()==((words+1)&~1u));
        const bool query=((h.fifo[0]>>16)&0x3f)==6;
        if(((h.fifo[0]>>16)&0x3f)==0x37) {
            assert(words==1 && h.fifo[0]==0x80370001u);
            h.returnLimit=1;++h.negotiations;
        } else if(query) {
            ++h.queries;const unsigned reg=h.fifo[0]&255;
            const unsigned value=reg==0xda?h.id[0]:reg==0xdb?h.id[1]:reg==0xdc?h.id[2]:
                reg==0x0b?h.address:reg==0x0c?h.pixel:
                h.badReadback && h.batches && h.value==2?0:h.value;
            assert(reg==0xda || reg==0xdb || reg==0xdc || reg==0x0b || reg==0x0c || reg==0x2f);
            // The preceding brightness operation left a two-byte limit.
            // Verify a fresh negotiation for every single-byte query.
            assert(h.returnLimit==1 && h.negotiations==h.queries);
            h.regs[ReadControl/4]=4u<<16;
            h.regs[ReadData0/4]=((h.badReturn && reg==0xda) || (h.unsupportedColour && reg==0x0b) ||
                (h.unsupportedPixel && reg==0x0c)?0x02u:0x21u)<<24 | value<<16;
            h.returnLimit=2; // model another client changing its return length
        } else if((h.fifo[0]&0x003f00ffu)==0x00150036u) {
            assert(words==1);++h.colourWrites;
            if(!(h.rejectColour && h.colourWrites==1))h.address=(h.fifo[0]>>8)&255;
            if(h.badColourRestore && h.colourWrites==2)h.address=0x20;
            if(h.stallColour && h.colourWrites==1)return;
        } else {
            ++h.batches;
            for(unsigned at=0;at<words;) {
                const uint32_t header=h.fifo[at];const unsigned type=(header>>16)&0x3f;
                if(type==0x15) {if((header&255)==0x2f)h.value=(header>>8)&255;++at;}
                else {assert(type==0x39);at+=1+((header&0xffff)+3)/4;}
                assert(at<=words);if(at==words)assert(header&0x80000000u);
            }
        }
        if(h.complete) {h.regs[Status/4]&=~DmaBusy;h.regs[InterruptControl/4]|=DmaDone|(query?BtaDone:0);}
    }
    static void pause(void *,unsigned us) {assert(us==1);}
    Io io() {return {this,read,write,pause};}
    static bool tear(void *cookie,unsigned hz) {
        auto &h=*static_cast<Hardware *>(cookie);++h.tears;h.lastHz=hz;
        if(h.failTear && h.tears==1)return false;
        return true;
    }
    void restoredControls() {
        assert(regs[Control/4]==3 && regs[DmaControl/4]==0xa9000100 && regs[DmaLength/4]==0x1234);
        assert(regs[TriggerControl/4]==0x18105 && regs[PatternControl/4]==0x1200);
        assert(regs[InterruptControl/4]==(EnableMask|(1u<<16)|(1u<<28)));
    }
};
static void run(Hardware &h,DitingPanel::Mode &mode,Command &command,unsigned hz) {
    command.initialize(h.io());mode.initialize(command,h.io(),{&h,Hardware::tear},h.selected);
    assert(mode.start(hz,0));
    uint64_t now=0;
    for(unsigned step=0;step<1000 && mode.active();++step) {mode.step(now);now+=1000000ULL;}
    assert(!mode.active());assert(!command.inFlight());h.restoredControls();
}
int main(int argc,char **argv) {
    assert(argc==2);Hardware h;DitingPanel::Mode mode;Command command;
    if(!strcmp(argv[1],"all-plans")) {
        for(unsigned panel=1;panel<=2;++panel)for(unsigned hz:std::array<unsigned,3>{60,90,120}) {
            DitingPanel::Plan plan;
            const auto kind=static_cast<DitingPanel::Panel>(panel);
            assert(DitingPanel::plan(kind,hz,plan) && plan.count && plan.count<=4);
            assert(plan.expected==DitingPanel::modeValue(kind,hz));
            unsigned writes=0;
            for(unsigned i=0;i<plan.count;++i) {
                const auto &b=plan.batch[i];assert(b.count && b.count<=16);
                for(unsigned at=0;at<b.count;) {
                    const uint32_t header=b.words[at];const unsigned type=(header>>16)&0x3f;
                    if(type==0x15) {
                        if((header&255)==0x2f) {assert(((header>>8)&255)==plan.expected);++writes;}
                        ++at;
                    } else {assert(type==0x39);at+=1+((header&0xffff)+3)/4;}
                    assert(at<=b.count);if(at==b.count)assert(header&0x80000000u);
                }
            }
            assert(writes==1 && DitingPanel::modeRate(kind,plan.expected)==hz);
        }
        for(unsigned hz:std::array<unsigned,3>{60,90,120}) {
            Hardware hw;DitingPanel::Mode m;Command c;run(hw,m,c,hz);const auto &d=m.diagnostic();
            assert(d.stage==6 && !d.error && d.panel==2 && hw.lastHz==hz);
            assert(d.after==DitingPanel::modeValue(DitingPanel::Panel::L12_36,hz));
            assert(d.batches<=4 && hw.queries==(hz==60?6u:7u) && hw.tears==1);
            assert(d.returnSizeCommands==hw.queries && d.addressMode==0 && d.pixelFormat==0x77 && !d.colourError);
            assert(d.colourStatus==1 && !d.colourWrites && d.addressAfter==0);
        }
    } else if(!strcmp(argv[1],"unknown")) {
        h.id[0]=0x99;h.address=8;run(h,mode,command,120);const auto &d=mode.diagnostic();
        assert(d.stage==8 && d.error==5 && !h.batches && !h.tears && h.queries==5);
        assert(d.selectionReason==2 && d.addressMode==8 && d.pixelFormat==0x77 && !h.colourWrites);
    } else if(!strcmp(argv[1],"metadata")) {
        for(unsigned fault=0;fault<7;++fault) {
            Hardware hw;
            if(fault==0)hw.selected={};
            if(fault==1)hw.selected.modelHigh=0x4c3133;
            if(fault==2)hw.selected.modelLow=0x420200; // No verified DCS ID for L12_42.
            if(fault==3)hw.selected.width=1080;
            if(fault==4)hw.selected.rgb=0;
            if(fault==5)hw.selected.version=2;
            if(fault==6) {hw.id[0]=0x36;hw.id[1]=2;hw.id[2]=0x0b;} // Model name is not a DCS reply.
            hw.address=8;DitingPanel::Mode m;Command c;run(hw,m,c,120);const auto &d=m.diagnostic();
            assert(d.stage==8 && d.error==5 && !hw.batches && !hw.tears && !hw.colourWrites);
            assert(hw.queries==5 && d.addressMode==8 && d.pixelFormat==0x77);
        }
    } else if(!strcmp(argv[1],"malformed")) {
        h.badReturn=true;run(h,mode,command,120);const auto &d=mode.diagnostic();
        assert(d.error==4 && !h.batches && !h.tears && h.queries==5 && !h.colourWrites);
        assert(d.responses[0][0]==0xda && d.responses[0][1]==0x40000);
        assert(d.responses[0][2]==0x02c50000 && d.returnSizeCommands==5);
        assert(d.addressMode==0 && d.pixelFormat==0x77); // Independent evidence survives the ID failure.
    } else if(!strcmp(argv[1],"optional-colour")) {
        for(unsigned fault=0;fault<2;++fault) {
            Hardware hw;hw.address=8;hw.unsupportedColour=fault==0;hw.unsupportedPixel=fault==1;
            DitingPanel::Mode m;Command c;run(hw,m,c,120);const auto &d=m.diagnostic();
            assert(d.stage==6 && !d.error && d.colourError==(fault==0?1u:2u));
            assert(d.returnSizeCommands==7 && hw.tears==1 && !hw.colourWrites);
            assert(d.colourStatus==0 && hw.address==8);
        }
    } else if(!strcmp(argv[1],"colour-guards")) {
        for(unsigned fault=0;fault<3;++fault) {
            Hardware hw;hw.address=fault==0?0x4b:0x48;hw.pixel=fault==1?0x66:fault==2?0xff:0x77;
            DitingPanel::Mode m;Command c;run(hw,m,c,120);
            assert(m.diagnostic().stage==6 && !m.diagnostic().colourWrites && !hw.colourWrites);
            assert(m.diagnostic().colourStatus==0);
        }
    } else if(!strcmp(argv[1],"colour-correction")) {
        h.address=0x48;run(h,mode,command,120);const auto &d=mode.diagnostic();
        assert(d.stage==6 && !d.error && !d.colourError && d.colourStatus==2);
        assert(d.addressMode==0x48 && d.addressAfter==0x40 && h.address==0x40);
        assert(h.colourWrites==1 && d.colourWrites==1 && !d.colourRestored && h.queries==8);
        assert(d.responses[7][0]==0x0b && (d.responses[7][2]>>16&255)==0x40);
    } else if(!strcmp(argv[1],"colour-rollback") || !strcmp(argv[1],"colour-restore-failed")) {
        h.address=0x48;h.rejectColour=true;h.badColourRestore=!strcmp(argv[1],"colour-restore-failed");
        run(h,mode,command,120);const auto &d=mode.diagnostic();
        assert(d.addressMode==0x48 && h.colourWrites==2 && d.colourWrites==2 && (d.colourError&8));
        if(h.badColourRestore) {
            assert(d.stage==8 && d.error==14 && d.colourStatus==4 && !d.colourRestored);
            assert(!h.batches && !h.tears && (d.colourError&16));
        } else {
            assert(d.stage==6 && !d.error && d.colourStatus==3 && d.colourRestored && h.address==0x48);
            assert(h.queries==9 && d.returnSizeCommands==9 && d.addressAfter==0x48);
        }
    } else if(!strcmp(argv[1],"readback-rollback") || !strcmp(argv[1],"tear-rollback")) {
        h.badReadback=!strcmp(argv[1],"readback-rollback");h.failTear=!h.badReadback;
        run(h,mode,command,120);const auto &d=mode.diagnostic();
        assert(d.stage==8 && d.error==(h.badReadback?8u:9u) && d.restored && d.rollbackHz==60);
        assert(h.value==0 && h.lastHz==60 && d.batches<=8 && h.queries==8 && d.returnSizeCommands==8);
    } else if(!strcmp(argv[1],"retained")) {
        h.complete=false;command.initialize(h.io());mode.initialize(command,h.io(),{&h,Hardware::tear},h.selected);
        assert(mode.start(120,0));assert(mode.step(0)==1 && command.inFlight());
        const auto writes=h.writes.size();assert(mode.step(60000000)==0 && mode.retained());
        assert(command.inFlight() && h.writes.size()==writes && !command.finish());
        h.regs[Status/4]=0;assert(mode.step(61000000)==0 && h.writes.size()==writes);
        h.regs[InterruptControl/4]|=DmaDone|BtaDone;mode.step(62000000);
        assert(!mode.active() && !command.inFlight() && mode.diagnostic().error==10 && !h.batches);h.restoredControls();
    } else if(!strcmp(argv[1],"colour-retained")) {
        h.address=0x48;h.stallColour=true;command.initialize(h.io());
        mode.initialize(command,h.io(),{&h,Hardware::tear},h.selected);assert(mode.start(120,0));
        uint64_t now=0;
        while(!h.colourWrites && now<100000000ULL) {mode.step(now);now+=1000000ULL;}
        assert(h.colourWrites==1 && command.inFlight());
        now+=60000000ULL;assert(mode.step(now)==0 && mode.retained());
        const auto writes=h.writes.size();h.regs[Status/4]=0;
        assert(mode.step(now+1000000ULL)==0 && h.writes.size()==writes && command.inFlight());
        h.regs[InterruptControl/4]|=DmaDone;h.stallColour=false;
        for(unsigned step=0;step<500 && mode.active();++step) {now+=1000000ULL;mode.step(now);}
        assert(!mode.active() && !command.inFlight());h.restoredControls();
        const auto &d=mode.diagnostic();
        assert(d.stage==8 && d.error==10 && d.colourRestored && d.colourStatus==3);
        assert(h.colourWrites==2 && h.address==0x48 && !h.batches && !h.tears);
    } else if(!strcmp(argv[1],"busy")) {
        h.regs[Status/4]=MdpBusy;command.initialize(h.io());mode.initialize(command,h.io(),{&h,Hardware::tear},h.selected);
        assert(mode.start(120,0));assert(mode.step(0)==1 && h.writes.empty());mode.step(201000000);
        assert(!mode.active() && mode.diagnostic().error==2 && h.writes.empty());
    } else assert(false);
    printf("PASS %s\n",argv[1]);
}
