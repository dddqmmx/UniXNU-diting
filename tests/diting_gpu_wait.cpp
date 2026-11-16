/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoFencePoll.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <mutex>
struct State {
    std::mutex gate;
    uint64_t now=0;
    unsigned polls=0, pauses=0;
    bool complete=false, close=false, progress=false, closing=false;
    static uint64_t clock(void *cookie) {return static_cast<State *>(cookie)->now;}
    static int idle(void *cookie) {
        auto &s=*static_cast<State *>(cookie);
        // Another client can acquire the same gate while this request waits.
        assert(s.gate.try_lock()); ++s.pauses; s.now+=50;
        if(s.progress && s.pauses==3)s.complete=true;
        if(s.closing)s.close=true;
        s.gate.unlock();return 0;
    }
    IOReturn poll() {
        std::lock_guard<std::mutex> guard(gate); ++polls;
        return close?kIOReturnNotReady:complete?kIOReturnSuccess:kIOReturnTimeout;
    }
    IOReturn wait(uint32_t timeout) {
        const NativeAdrenoRingClock time{this,clock,idle};
        return native_adreno_poll_fence(time,timeout,[this](){return poll();});
    }
};
int main(int argc,char **argv) {
    assert(argc==2);State state;
    if(!strcmp(argv[1],"poll")) {assert(state.wait(0)==kIOReturnTimeout && state.polls==1 && !state.pauses);}
    else if(!strcmp(argv[1],"progress")) {
        state.progress=true;assert(state.wait(200)==kIOReturnSuccess && state.polls==4 && state.pauses==3);
    } else if(!strcmp(argv[1],"timeout")) {assert(state.wait(200)==kIOReturnTimeout && state.now==200 && state.polls==5);}
    else if(!strcmp(argv[1],"close")) {
        state.closing=true;assert(state.wait(UINT32_MAX)==kIOReturnNotReady && state.polls==2 && state.pauses==1);
    } else if(!strcmp(argv[1],"units")) {
        assert(native_adreno_wait_us(0)==0 && native_adreno_wait_us(1)==1 && native_adreno_wait_us(999)==1);
        assert(native_adreno_wait_us(1000)==1 && native_adreno_wait_us(1001)==2);
        assert(native_adreno_wait_us(UINT64_MAX)==UINT32_MAX);
        NativeAdrenoRingClock absent{};
        assert(native_adreno_poll_fence(absent,1,[&](){++state.polls;return kIOReturnSuccess;})==kIOReturnBadArgument);
        assert(!state.polls);
    } else assert(false);
    printf("PASS %s\n",argv[1]);
}
