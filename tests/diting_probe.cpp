/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "DitingProbe.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
static std::vector<std::string> lines;
static void emit(const char *line,unsigned bytes) {assert(bytes<240);lines.emplace_back(line,bytes);}
int main() {
    diting_probe_record(16,"invalid",1,"invalid",0);
    diting_probe_panic_snapshot(emit);assert(lines.empty());
    const std::string longText(512,'x');
    DitingProbeRecord(7,longText.c_str(),DitingProbeSkip,longText.c_str(),UINT64_MAX);
    diting_probe_panic_snapshot(emit);assert(lines.size()==1 && lines[0].find("SKIP")!=std::string::npos);
    std::thread writer([](){
        for(unsigned i=1;i<=5000;++i) {
            char reason[128];snprintf(reason,sizeof(reason),"epoch=%u",i);
            DitingProbeRecord(7,"epoch",DitingProbePass,reason,i);
        }
    });
    for(unsigned i=0;i<5000;++i) {
        lines.clear();diting_probe_panic_snapshot(emit);
        for(const auto &line:lines)if(line.find(" epoch:")!=std::string::npos) {
            unsigned epoch=0;unsigned long long detail=0;
            assert(sscanf(line.c_str(),"DITING PROBE 7 epoch: PASS epoch=%u detail=%llu",&epoch,&detail)==2);
            assert(epoch==detail);
        }
    }
    writer.join();lines.clear();diting_probe_panic_snapshot(emit);
    assert(lines.size()==1 && lines[0].find("epoch=5000 detail=5000")!=std::string::npos);
    puts("PASS bounded coherent panic snapshots");
}
