/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "DitingAdrenoStartup.h"
#include <cassert>
#include <cstring>
#include <thread>
#include <cstdio>
int main()
{
    using namespace DitingAdrenoStartup;
    Log log{};Entry copy{};
    assert(!get(log,0,copy) && !get(log,Capacity,copy));
    char long_phase[200];memset(long_phase,'x',sizeof long_phase);long_phase[199]=0;
    assert(put(log,long_phase,7,11,13,17));assert(get(log,0,copy));
    assert(strlen(copy.phase)==PhaseBytes-1 && copy.status==7 && copy.a==11 && copy.b==13 && copy.us==17);
    std::thread writers[4];
    for(unsigned w=0;w<4;++w)writers[w]=std::thread([&log,w]{
        for(unsigned i=0;i<16;++i)put(log,"immutable",w,i,16-i,100+i);
    });
    for(unsigned i=0;i<1000;++i)for(unsigned n=1;n<Capacity;++n)if(get(log,n,copy)) {
        assert(copy.ready==1 && !strcmp(copy.phase,"immutable") && copy.a+copy.b==16);
    }
    for(auto &thread:writers)thread.join();
    assert(log.count==Capacity && !put(log,"overflow",0,0,0,0));
    for(unsigned n=0;n<Capacity;++n)assert(get(log,n,copy));
    assert(get(log,0,copy) && copy.status==7 && copy.us==17);
    puts("PASS bounded startup history and concurrent publication");
}
