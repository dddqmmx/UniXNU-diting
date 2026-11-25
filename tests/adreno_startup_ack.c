/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Exercise the real AArch64 HFI ring and GMU power waiter under QEMU. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "DitingAdrenoGmu.h"
#include "DitingAdrenoRegisters.h"

typedef struct {
    DitingGpuGmuDevice device;
    uint32_t shared[4096] __attribute__((aligned(4096)));
    uint64_t time;
    unsigned mode,waits,commands,interrupt,clear_count,reset,delayed;
} Fixture;
static Fixture f;
static size_t reg_offset(uint32_t reg) {return (size_t)reg*4-0x68000;}
static int ready(void *cookie) {(void)cookie;return 0;}
static int barrier(void *cookie) {(void)cookie;__asm__ volatile("dsb sy":::"memory");return 0;}
static int now(void *cookie,uint64_t *out) {Fixture *s=cookie;*out=s->time;return 0;}
static int read32(void *cookie,unsigned window,size_t at,uint32_t *out)
{
    Fixture *s=cookie;assert(window==DitingGpuGmu);
    if(at==reg_offset(GEN7_GMU_CM3_FW_INIT_RESULT))*out=s->reset?0xe00:0x100;
    else if(at==reg_offset(GEN7_GMU_HFI_CTRL_STATUS))*out=1;
    else if(at==reg_offset(GEN7_GMU_GMU2HOST_INTR_INFO))*out=s->interrupt;
    else {assert(!"unexpected register read");return 1;}
    return 0;
}
static int write32(void *cookie,unsigned window,size_t at,uint32_t value)
{
    Fixture *s=cookie;assert(window==DitingGpuGmu);
    if(at==reg_offset(GEN7_GMU_GMU2HOST_INTR_CLR)) {
        assert(value==1 || value==(1u<<31));
        if(s->mode==9 && value==1)return 1;
        s->interrupt&=~value;++s->clear_count;
    } else if(at==reg_offset(GEN7_GMU_HOST2GMU_INTR_SET)) {
        if(value==(1u<<30))s->interrupt|=1u<<31;
        else assert(value==(1u<<31));
    } else {assert(!"unexpected register write");return 1;}
    return 0;
}
static void reply(Fixture *s,unsigned error,unsigned stale)
{
    volatile uint32_t *v=s->shared+18,*data=s->shared+212+1024;
    uint32_t request=s->device.hfi.pending_header;
    unsigned at=v[11];
    data[at]=0x10300|(request&255);
    data[(at+1)&1023]=stale?((request&0xfffff)|(1u<<20)):request;
    data[(at+2)&1023]=error;data[(at+3)&1023]=0;
    __asm__ volatile("dsb oshst":::"memory");
    v[11]=(at+4)&1023;s->interrupt|=1;
}
static int notify(void *cookie)
{
    Fixture *s=cookie;++s->commands;
    s->shared[16]=s->shared[17]; // the firmware consumed this actual command
    /* A new response needs the previous MSGQ notification acknowledged. */
    if(s->interrupt&1)return 0;
    if(s->commands==2) {
        if(s->mode==2 || s->mode==3 || s->mode==6) {s->delayed=1;return 0;}
        if(s->mode==4) {reply(s,0x55,0);return 0;}
        if(s->mode==5) {reply(s,0,0);s->reset=1;return 0;}
        if(s->mode==7) {reply(s,0,1);return 0;}
        if(s->mode==8)return 1;
        if(s->mode==11) {s->time=99;return 0;}
        if(s->mode==14) {s->shared[4]=4;return 0;}
    }
    if(s->mode==16 && s->commands==5) {reply(s,0x77,0);return 0;}
    if(s->mode==12)return 0;
    reply(s,0,0);return 0;
}
static int wait_us(void *cookie,unsigned us)
{
    Fixture *s=cookie;assert(us==100);++s->waits;
    if(s->mode==12)return 1;
    if(s->mode==6)return 0; // stalled clock still reaches the iteration bound
    if(s->delayed) {
        s->time+=1000001;s->delayed=0;
        if(s->mode==2)reply(s,0,0);
    } else s->time+=us;
    return 0;
}
/* Power serialization is unchanged; give the real waiter fixed test inputs. */
DitingPowerStatus DitingAdrenoBuildPowerTables(const DitingPowerInputs *in,unsigned level,DitingPowerTables *out)
{
    assert(in && level==0);memset(out,0,sizeof *out);out->gpu_index=12;out->bus_index=11;
    return DitingPowerSuccess;
}
int main(int argc,char **argv)
{
    assert(argc==2);f.mode=(unsigned)strtoul(argv[1],0,10);f.time=100;f.interrupt=0x80;
    DitingGpuGmuDevice *d=&f.device;
    d->io=(DitingGpuOps){&f,ready,read32,write32,barrier,0,now,wait_us};
    assert(DitingAdrenoHfiInitializeStopped(&d->hfi,f.shared,sizeof f.shared,0x60002000,&f,notify)==DitingAdrenoHfiSuccess);
    d->attempted=d->fw_ready=d->hfi_ready=1;
    DitingPowerInputs input={0};DitingGpuPowerResult r;
    DitingGpuStatus result=DitingAdrenoGmuStartPower(d,&input,0,&r);
    if(f.mode==1 || f.mode==2 || f.mode==10 || f.mode==13 || f.mode==15 || f.mode==16) {
        assert(result==DitingGpuSuccess && d->power_started && !d->poisoned);
        assert(r.phase==5 && r.published==4 && r.acknowledged==4 && r.interrupt_clears==4);
        assert(!d->hfi.pending_header && (f.interrupt&0x80));
        if(f.mode==2)assert(r.boundary_polls==1 && r.max_poll_gap_us==1000001 && f.waits==1);
        if(f.mode==13) {
            assert(DitingAdrenoGmuHoldGpu(d,&r)==DitingGpuSuccess && d->gpu_held);
            assert(r.command==10 && r.acknowledged==1 && r.interrupt_clears==1);
            assert(!(f.interrupt&(1u<<31)) && (f.interrupt&0x80));
        }
        if(f.mode==15) {
            /* Invalid bounds rejected without poisoning. */
            assert(DitingAdrenoGmuSetPowerLevel(d,0,2,&r)==DitingGpuArgument && !d->poisoned);
            assert(DitingAdrenoGmuSetPowerLevel(d,14,2,&r)==DitingGpuArgument && !d->poisoned);
            assert(DitingAdrenoGmuSetPowerLevel(d,3,12,&r)==DitingGpuArgument && !d->poisoned);
            /* Valid runtime DCVS transitions (cmd 30): 324MHz (idx=3, bus=2) -> 900MHz (idx=13, bus=11). */
            assert(DitingAdrenoGmuSetPowerLevel(d,3,2,&r)==DitingGpuSuccess && r.command==30 && r.acknowledged==1 && !d->poisoned);
            assert(DitingAdrenoGmuSetPowerLevel(d,13,11,&r)==DitingGpuSuccess && r.command==30 && r.acknowledged==1 && !d->poisoned);
        }
        if(f.mode==16) {
            /* Firmware error on runtime DCVS vote poisons device and rejects further votes. */
            assert(DitingAdrenoGmuSetPowerLevel(d,1,2,&r)==DitingGpuIo && r.firmware_error==0x77 && d->poisoned);
            assert(DitingAdrenoGmuSetPowerLevel(d,3,2,&r)==DitingGpuPoisoned);
        }
    } else {
        assert(result!=DitingGpuSuccess && d->poisoned && d->hfi.poisoned && !d->power_started);
        assert(r.resources_must_retain && r.published<=2 && r.acknowledged<=1);
        if(f.mode==3 || f.mode==6 || f.mode==7)assert(result==DitingGpuTimeout && r.boundary_polls==1);
        if(f.mode==4)assert(result==DitingGpuIo && r.firmware_error==0x55 && r.transport==DitingAdrenoHfiFirmwareError);
        if(f.mode==5)assert(result==DitingGpuPrerequisite && r.fw_status==0xe00);
        if(f.mode==6)assert(f.waits==10000 && r.polls==10002);
        if(f.mode==7)assert(d->hfi.skipped==1);
        if(f.mode==8)assert(r.transport==DitingAdrenoHfiNotifyError);
        if(f.mode==9)assert(r.acknowledged==0 && f.shared[28]==0);
        if(f.mode==11 || f.mode==12)assert(result==DitingGpuIo);
        if(f.mode==14)assert(r.transport==DitingAdrenoHfiCorrupt);
    }
    printf("PASS real GMU/HFI scenario %u\n",f.mode);return 0;
}
