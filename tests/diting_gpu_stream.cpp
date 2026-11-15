/* SPDX-License-Identifier: MIT */
#include "NativeGpuTraceStore.h"
#include "DitingUsbCommand.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static NativeGpuTraceRecord event(uint64_t value) {
    NativeGpuTraceRecord r{};r.us=value;r.event=10;
    for(unsigned i=0;i<12;++i)r.value[i]=value^(uint64_t(i)<<32);
    return r;
}
struct Buffer {
    NativeGpuTraceStreamHeader header;
    NativeGpuTraceRecord records[20];
    uint64_t canary=0x12345678abcdef00ULL;
};

int main(int argc,char **argv) {
    assert(argc==2);const std::string name=argv[1];
    NativeGpuTraceSlot slots[32]{};NativeGpuTraceStore store;
    store.slots=slots;store.capacity=4;Buffer out{};
    if(name=="wrap") {
        for(unsigned i=1;i<=9;++i)store.push(event(i));
        assert(store.range(0,2,&out,sizeof(out))==64+2*120);
        assert(out.header.first_available==6 && out.header.skipped==5 && out.header.next==7);
        assert(out.header.trace.count==2 && !out.header.missing);
        assert(out.records[0].sequence==6 && out.records[1].sequence==7);
        assert(store.range(7,20,&out,sizeof(out))==64+2*120 && out.header.next==9);
        assert(store.range(9,20,&out,sizeof(out))==64 && out.header.next==9);
    } else if(name=="busy") {
        for(unsigned i=1;i<=4;++i)store.push(event(i));
        slots[1].busy=1;
        assert(store.range(0,4,&out,sizeof(out))==64+3*120);
        assert(out.header.missing==1 && out.header.next==4 && out.records[1].sequence==3);
        slots[1].busy=0;
        assert(out.header.skipped+out.header.missing+out.header.trace.count==out.header.next);
    } else if(name=="bounds") {
        assert(!store.range(UINT64_MAX,1,&out,sizeof(out)));
        assert(!store.range(1,1,&out,sizeof(out)));
        assert(!store.range(0,0,&out,sizeof(out)));
        assert(!store.range(0,21,&out,sizeof(out)));
        assert(!store.range(0,20,&out,64+20*120-1));
        assert(!store.range(0,1,nullptr,sizeof(out)));
        store.slots=nullptr;assert(!store.range(0,1,&out,sizeof(out)));
    } else if(name=="concurrent") {
        store.capacity=32;std::atomic<bool> done{false};
        std::thread writer([&]{for(unsigned i=1;i<20000;++i)store.push(event(i));done.store(true);});
        uint64_t after=0;
        do {
            assert(store.range(after,20,&out,sizeof(out))>=64);
            assert(out.header.next>=after && out.header.next<=out.header.trace.issued);
            assert(out.header.skipped+out.header.missing+out.header.trace.count==out.header.next-after);
            for(unsigned i=0;i<out.header.trace.count;++i) {
                const auto &r=out.records[i];assert(r.sequence>after && r.sequence<=out.header.next && r.event==10);
                for(unsigned j=0;j<12;++j)assert(r.value[j]==(r.us^(uint64_t(j)<<32)));
                after=r.sequence;
            }
            after=out.header.next;
        } while(!done.load() || after<__atomic_load_n(&store.issued,__ATOMIC_ACQUIRE));
        writer.join();
    } else if(name=="commands") {
        uint64_t after=1;unsigned count=0;
        assert(diting_usb_parse_trace("trace 0 20",&after,&count) && !after && count==20);
        assert(diting_usb_parse_trace("trace 18446744073709551614 1",&after,&count));
        const char *bad[]={"trace ","trace -1 1","trace 18446744073709551615 1",
            "trace 18446744073709551616 1","trace 0 0","trace 0 21","trace 0 1 extra",
            "trace 0  1","trace 0 +1","trace 0 99999999999999999"};
        for(auto line:bad)assert(!diting_usb_parse_trace(line,&after,&count));
    } else if(name=="base64") {
        char text[4096];const unsigned char sample[]={0,1,2,253,254,255};
        assert(diting_usb_base64(sample,6,text,sizeof(text))==8 && !strcmp(text,"AAEC/f7/"));
        assert(diting_usb_base64(sample,1,text,sizeof(text))==4 && !strcmp(text,"AA=="));
        assert(diting_usb_base64(sample,2,text,sizeof(text))==4 && !strcmp(text,"AAE="));
        assert(!diting_usb_base64(sample,6,text,8));
        assert(!diting_usb_base64(sample,UINT32_MAX,text,sizeof(text)));
        unsigned char largest[64+20*120]{};
        const unsigned used=diting_usb_base64(largest,sizeof(largest),text,sizeof(text));
        assert(used==3288 && used+2*80+64+1<4096);
    } else assert(false);
    assert(out.canary==0x12345678abcdef00ULL);
    printf("PASS %s\n",argv[1]);
}
