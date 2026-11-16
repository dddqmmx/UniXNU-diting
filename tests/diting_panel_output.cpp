/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define main diting_refresh_contract_tests
#include "diting_display_refresh.cpp"
#undef main
#include "DitingPanelTear.h"
#include "DitingScanoutPixels.h"
int main(int argc,char **argv) {
    assert(argc==2);Hardware h;h.mdp[0x362b4/4]=0x80000001;
    PanelTearDiagnostic d;const auto s=h.snapshot();
    auto run=[&](Hardware &hw,PanelTearDiagnostic &diag,const Snapshot &snap) {
        return PanelTear({{&hw,Hardware::read},Hardware::write,Hardware::pause},diag).run(board,snap,120);
    };
    if(!strcmp(argv[1],"te-transition")) {
        assert(run(h,d,s) && d.stage==3 && !d.error && d.writes==3 && d.polls==20);
        assert(d.before==0x180075 && d.after==0x18003a && h.mdp[0x362b4/4]==0x80000001);
        assert(h.writes[0]==std::make_pair(0x36288u,0x80075u));
        assert(h.writes[1]==std::make_pair(0x36288u,0x8003au));
        assert(h.writes[2]==std::make_pair(0x36288u,0x18003au));
    } else if(!strcmp(argv[1],"te-failures")) {
        for(unsigned fail=1;fail<=3;++fail) {
            Hardware hw;hw.mdp[0x362b4/4]=0x80000001;hw.failWrite=fail;PanelTearDiagnostic diag;
            assert(!run(hw,diag,hw.snapshot()) && diag.error==3 && diag.stage==4);
            assert(hw.mdp[0x36288/4]==0x180075 && hw.mdp[0x362b4/4]==0x80000001);
        }
        h.stuck=true;assert(!run(h,d,s) && d.error==4 && d.restored && h.us==20000);
        Hardware off;off.mdp[0x362b4/4]=0x80000001;off.clockOff=true;PanelTearDiagnostic lost;
        assert(!run(off,lost,off.snapshot()) && lost.error==6 && !lost.restored && lost.writes==1);
    } else if(!strcmp(argv[1],"te-prerequisites")) {
        auto wrong=s;wrong.pipe[0][1]+=4096;assert(!run(h,d,wrong) && !d.writes && !h.writeCalls);
        wrong=s;wrong.intf[0].reg[14]=0x8000003c;assert(!run(h,d,wrong) && !h.writeCalls);
        assert(panelVsyncCount(60)==116 && panelVsyncCount(90)==77 && panelVsyncCount(120)==58 && !panelVsyncCount(0));
    } else if(!strcmp(argv[1],"pixel-ranges")) {
        DitingScanoutPixels::Request q;
        assert(DitingScanoutPixels::parse("display pixels",q) && q.capture);
        assert(DitingScanoutPixels::parse("display pixels 4032 64",q) && q.first==4032 && q.count==64);
        for(const char *bad:{"display","display pixelsx","display pixels ","display pixels 0","display pixels 0 0",
            "display pixels 4096 1","display pixels 4095 2","display pixels 0 65","display pixels 0 1 extra",
            "display pixels -1 2","display pixels 18446744073709551615 1"})assert(!DitingScanoutPixels::parse(bad,q));
    } else if(!strcmp(argv[1],"pixel-reply")) {
        DitingScanoutPixels::Snapshot snap{UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT32_MAX};
        uint32_t pixels[4096];for(auto &p:pixels)p=UINT32_MAX;
        char reply[4096];unsigned used=0;
        diting_usb_reply_add(reply,sizeof(reply)-96,&used,"DitingUSB BEGIN display pixels 4032 64\n");
        used+=DitingScanoutPixels::format(reply+used,sizeof(reply)-96-used,snap,pixels,{false,4032,64});
        diting_usb_reply_add(reply,sizeof(reply),&used,"DitingUSB END display pixels 4032 64\n");
        assert(used<sizeof(reply)-1 && strlen(reply)==used && strstr(reply,"count=64") && strstr(reply,"\nDitingUSB END"));
        char small[32];memset(small,0x5a,sizeof(small));
        assert(DitingScanoutPixels::format(small,17,snap,pixels,{true,0,0})==16 && !small[16] && small[17]==0x5a);
    } else assert(false);
    printf("PASS %s\n",argv[1]);
}
