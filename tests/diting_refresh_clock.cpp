/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "MiniDisplayRefreshClock.h"
using namespace DitingRefreshClock;
int main() {
    const auto early=select(1000,999,100);
    assert(!early.ready && early.next==1000);
    const auto late=select(1000,1450,100);
    assert(late.ready && late.reported==1400 && late.next==1500 && late.skipped==4);
    assert(nextAfter(1400,1510,100)==1600);
    assert(nextAfter(0,1450,100)==1550);
    assert(!select(1000,1450,0).ready);
    assert(nextAfter(UINT64_MAX-10,UINT64_MAX-5,100)==UINT64_MAX);
    // Delayed callbacks keep a single phase, select only a due period and
    // rearm in the future; there is no backlog of historical notifications.
    for(uint64_t period: {1u,7u,100u,160000u}) {
        uint64_t due=1000;
        for(unsigned n=0;n<300;++n) {
            const auto fired=due+(uint64_t(n)*n*127)%(period*30);
            const auto tick=select(due,fired,period);
            assert(tick.ready && tick.reported<=fired && fired-tick.reported<period);
            assert((tick.reported-1000)%period==0 && tick.next>fired);
            assert(tick.skipped==(tick.reported-due)/period);
            const auto armTime=fired+period*2+3;
            due=nextAfter(tick.reported,armTime,period);
            assert(due>armTime && (due-1000)%period==0);
        }
    }
    puts("PASS refresh phase, late callbacks, early calls and overflow");
}
