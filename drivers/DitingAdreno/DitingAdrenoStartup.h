/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_ADRENO_STARTUP_H
#define DITING_ADRENO_STARTUP_H
#include <stdint.h>

namespace DitingAdrenoStartup {
enum { Capacity=32, PhaseBytes=96 };
struct Entry {unsigned ready,status;uint64_t us,a,b;char phase[PhaseBytes];};
struct Log {unsigned count;Entry entries[Capacity];};
// Entries are published once and never overwritten. Readers do not allocate,
// take registry locks, access hardware, or inspect a writer's partial entry.
inline bool put(Log &log,const char *phase,unsigned status,uint64_t a,uint64_t b,uint64_t us)
{
    unsigned index=__atomic_load_n(&log.count,__ATOMIC_RELAXED);
    do {
        if(index>=Capacity)return false;
    } while(!__atomic_compare_exchange_n(&log.count,&index,index+1,false,
        __ATOMIC_RELAXED,__ATOMIC_RELAXED));
    Entry &entry=log.entries[index];entry.status=status;entry.us=us;entry.a=a;entry.b=b;
    unsigned n=0;
    for(;phase && phase[n] && n+1<PhaseBytes;++n)entry.phase[n]=phase[n];
    entry.phase[n]=0;
    __atomic_store_n(&entry.ready,1,__ATOMIC_RELEASE);
    return true;
}
inline bool get(const Log &log,unsigned index,Entry &entry)
{
    if(index>=Capacity || !__atomic_load_n(&log.entries[index].ready,__ATOMIC_ACQUIRE))return false;
    entry=log.entries[index];return true;
}
}
#endif
