/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mutex>
#include "../userland/gpu-test-oracle.h"
#include "../userland/gpu-test-clock.h"
using IOReturn=int;
enum {kIOReturnSuccess=0,kIOReturnBusy=1,kIOReturnBadArgument=2,kIOReturnNoSpace=3,
      kIOReturnNotPermitted=4,kIOReturnNotReady=5};
using IOLock=std::mutex;
static void IOLockLock(IOLock *lock){lock->lock();}
static void IOLockUnlock(IOLock *lock){lock->unlock();}
static uint64_t clock_us=1000000;
static unsigned pid=31,markers=0;
static uint64_t mach_absolute_time(){return clock_us*1000;}
static void absolutetime_to_nanoseconds(uint64_t value,uint64_t *out){*out=value;}
static int proc_selfpid(){return int(pid);}
template<class... Values> static void gpuTraceEvent(unsigned event,Values...){assert(event==64);++markers;}
#include "../drivers/DitingAdreno/submit/NativeGpuTestBroker.inc"
using State=DitingGpuTestState;
static DitingGpuTestUpdate update(State &state,unsigned bytes=0,unsigned next=DitingGpuTestRunning){
    DitingGpuTestUpdate out{};out.version=1;out.job=state.request.job;out.offset=state.used;out.bytes=bytes;out.state=next;
    for(unsigned i=0;i<bytes;++i)out.data[i]=uint8_t(i);
    return out;
}
static void ready(State &state,uint64_t time=1000000){DitingGpuTestJob job{};assert(state.poll(31,time,job)==State::Ok);}
static void protocol(){
    using Command=DitingGpuTestCommand;
    assert(Command::parse("gputest status").op==Command::Status);
    for(const char *profile:{"qualification","performance","all"})for(const char *policy:{"auto","sysmem","gmem"}){
        char line[64];snprintf(line,sizeof(line),"gputest run %s %s",profile,policy);assert(Command::parse(line).op==Command::Run);
    }
    assert(Command::parse("gputest run ui auto").op==Command::Run);
    assert(Command::parse("gputest cancel 4294967295").job==UINT32_MAX);
    assert(Command::parse("gputest result 1 131072 1024").op==Command::Result);
    const char *bad[]={nullptr,""," gputest status","gputest status ","gputest  status","gputest\tstatus",
        "gputest run ui sysmem","gputest run ui gmem","gputest run all shell","gputest run shell auto",
        "gputest cancel 0","gputest cancel -1","gputest cancel +1","gputest cancel 4294967296",
        "gputest result 1 131073 1","gputest result 1 0 1025","gputest result 1 0 0",
        "gputest result 1 0x1 1","gputest status;reboot","gputest status\nreboot", "gputest run all auto extra"};
    for(const auto *line:bad)assert(Command::parse(line).op==Command::Invalid);
    char too_long[65];memset(too_long,'1',64);too_long[64]=0;assert(Command::parse(too_long).op==Command::Invalid);
}
static void lifecycle(){
    State state{};DitingGpuTestJob job{};
    assert(state.start(3,0,1000000)==State::Unavailable);
    assert(state.poll(0,1000000,job)==State::Argument);ready(state);
    assert(state.start(4,1,1000000)==State::Argument);
    assert(state.start(3,0,1000001)==State::Ok);const auto id=state.request.job;
    assert(state.start(1,1,1000002)==State::Busy);
    assert(state.poll(32,1000002,job)==State::Busy);
    assert(state.poll(31,1000002,job)==State::Ok&&state.state==DitingGpuTestRunning);
    auto first=update(state,1024);assert(state.append(31,first,1000003)==State::Ok);
    assert(state.append(31,first,1000004)==State::Stale);
    auto finish=update(state,3,DitingGpuTestPassed);assert(state.append(32,finish,1000005)==State::Stale);
    assert(state.append(31,finish,1000005)==State::Ok&&state.used==1027&&state.state==DitingGpuTestPassed);
    const uint8_t *data=nullptr;uint32_t got=0;
    assert(state.read(id,1024,1024,&data,&got)==State::Ok&&got==3&&data[2]==2);
    assert(state.read(id,1027,1,&data,&got)==State::Ok&&got==0);
    assert(state.read(id,1028,1,&data,&got)==State::Argument);
    assert(state.start(1,2,1000006)==State::Ok&&state.used==0&&state.request.job!=id);
    assert(state.read(id,0,1,&data,&got)==State::Stale);
    state.request.job=UINT32_MAX;state.state=DitingGpuTestPassed;
    assert(state.start(1,0,1000007)==State::Ok&&state.request.job==1);
}
static void quiescence(){
    State state{};ready(state);assert(state.start(3,0,1000000)==State::Ok);ready(state);
    const auto id=state.request.job;assert(state.cancel(id,1000010)==State::Ok&&state.quiescing);
    assert(state.start(1,0,1000020)==State::Busy);
    auto data=update(state,1);assert(state.append(31,data,1000020)==State::Stale);
    auto finished=update(state,0,DitingGpuTestPassed);assert(state.append(31,finished,1000030)==State::Ok);
    assert(state.state==DitingGpuTestCancelled&&!state.quiescing);
    assert(state.start(2,1,1000040)==State::Ok);ready(state,1000040);
    state.expire(121000040);assert(state.state==DitingGpuTestTimedOut&&state.quiescing);
    ready(state,121000041);assert(state.start(1,0,121000042)==State::Busy);
    finished=update(state,0,DitingGpuTestCancelled);assert(state.append(31,finished,121000043)==State::Ok);
    assert(state.state==DitingGpuTestTimedOut&&!state.quiescing);
    assert(state.start(1,0,121000044)==State::Ok);
    // A pending job has not started a child and needs no exit acknowledgement.
    assert(state.cancel(state.request.job,121000045)==State::Ok&&!state.quiescing);
    assert(state.start(1,0,121000046)==State::Ok);
}
static void overflow(){
    State state{};ready(state);assert(state.start(3,0,1000000)==State::Ok);ready(state);
    for(unsigned i=0;i<128;++i){auto chunk=update(state,1024);assert(state.append(31,chunk,1000001+i)==State::Ok);}
    auto extra=update(state,1);assert(state.append(31,extra,1000200)==State::Full);
    assert(state.state==DitingGpuTestOverflow&&state.quiescing&&state.used==131072);
    assert(state.start(1,0,1000201)==State::Busy);
    auto finished=update(state,0,DitingGpuTestFailed);assert(state.append(31,finished,1000202)==State::Ok);
    assert(!state.quiescing&&state.state==DitingGpuTestOverflow);
}
static void replacement(){
    State state{};ready(state);assert(state.start(3,0,1000000)==State::Ok);ready(state);
    DitingGpuTestJob job{};assert(state.poll(32,5999999,job)==State::Busy);
    assert(state.poll(32,6000000,job)==State::Ok&&state.error==DitingGpuTestDaemonLost);
    assert(state.state==DitingGpuTestFailed&&!state.quiescing&&(job.flags&DitingGpuTestCancel));
    auto stale=update(state,0,DitingGpuTestFailed);assert(state.append(31,stale,6000001)==State::Stale);
    assert(state.start(1,0,6000002)==State::Ok);
    assert(State::age(1,2)==0);
}
static void broker(){
    IOLock lock;__atomic_store_n(&gpuTestLock,&lock,__ATOMIC_RELEASE);
    char out[4096];unsigned size=diting_gpu_test_text("gputest status",out,sizeof(out));
    assert(size&&strstr(out,"daemon=0")&&strstr(out,"error=0"));
    DitingGpuTestHello hello{1,0};DitingGpuTestJob job{};
    assert(gpuTestMethod(DitingGpuTestPoll,&hello,sizeof(hello),&job,sizeof(job))==0);
    hello.reserved=1;assert(gpuTestMethod(DitingGpuTestPoll,&hello,sizeof(hello),&job,sizeof(job))==kIOReturnBadArgument);hello.reserved=0;
    diting_gpu_test_text("gputest run qualification gmem",out,sizeof(out));assert(strstr(out,"error=0"));
    assert(gpuTestMethod(DitingGpuTestPoll,&hello,sizeof(hello),&job,sizeof(job))==0);
    DitingGpuTestMarker mark{1,job.job,1,1,1000,0};
    assert(gpuTestMethod(DitingGpuTestMark,&mark,sizeof(mark),nullptr,0)==0&&markers==1);
    mark.phase=2;mark.end_ns=999;assert(gpuTestMethod(DitingGpuTestMark,&mark,sizeof(mark),nullptr,0)==kIOReturnBadArgument);
    auto chunk=update(gpuTestState,3);chunk.data[0]=0;chunk.data[1]=255;chunk.data[2]=16;
    assert(gpuTestMethod(DitingGpuTestReport,&chunk,sizeof(chunk),nullptr,0)==0);
    diting_gpu_test_text("gputest result 1 0 1024",out,sizeof(out));assert(strstr(out,"bytes=3 total=3\nDATA AP8Q\n"));
    char tiny[5];memset(tiny,0xa5,sizeof(tiny));assert(diting_gpu_test_text("gputest status",tiny,4)<=3&&uint8_t(tiny[4])==0xa5);
    __atomic_store_n(&gpuTestLock,static_cast<IOLock *>(nullptr),__ATOMIC_RELEASE);
    diting_gpu_test_text("gputest status",out,sizeof(out));assert(strstr(out,"available=0"));
}
static void oracle(){
    using namespace DitingGpuOracle;uint8_t pixel[4];
    bytes({0.25,0.5,0.75,0.625},pixel,false,true);assert(pixel[0]==137&&pixel[1]==188&&pixel[2]==225&&pixel[3]==159);
    bytes({1,0.125,0.5,1},pixel,true,false);assert(pixel[0]==128&&pixel[1]==32&&pixel[2]==255&&pixel[3]==255);
    const Color result=over({1,0,0,0.25},{0.25,0.5,0.75,1});assert(result.r==0.4375&&result.g==0.375&&result.b==0.5625&&result.a==1);
    assert(half(0x3c00)==1&&half(0xbc00)==-1&&half(0x3800)==0.5&&half(1)==std::ldexp(1.0f,-24));
    assert(std::isinf(half(0x7c00))&&std::isnan(half(0x7e00)));
    assert(compute(0)==(1013904223u^0x5a5a5a5au));
    assert(bars(0,0).r==1&&bars(17,0).g==1&&bars(0,22).b==1);
}
static void cadence(){
    using namespace DitingGpuPacing;
    const uint64_t origin=1000000000ull;
    uint64_t previous=origin;
    for(unsigned step=1;step<=42;++step){
        const uint64_t deadline=next(origin,step,previous);
        assert(deadline-previous>=PeriodNs);
        // Reproduce long background sleeps plus small normal scheduling jitter.
        const uint64_t wake=deadline+(step==11||step==26?100000000ull:step%3*100000ull);
        previous=wake;
    }
    assert(previous-origin>=42*PeriodNs+200000000ull);
}
int main(int argc,char **argv){
    if(argc!=2)return 64;
    if(!strcmp(argv[1],"protocol"))protocol();else if(!strcmp(argv[1],"lifecycle"))lifecycle();
    else if(!strcmp(argv[1],"quiescence"))quiescence();else if(!strcmp(argv[1],"overflow"))overflow();
    else if(!strcmp(argv[1],"replacement"))replacement();else if(!strcmp(argv[1],"broker"))broker();
    else if(!strcmp(argv[1],"oracle"))oracle();else if(!strcmp(argv[1],"cadence"))cadence();else return 64;
    printf("PASS %s\n",argv[1]);
}
