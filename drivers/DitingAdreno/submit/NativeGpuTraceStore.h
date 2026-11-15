/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_GPU_TRACE_STORE_H
#define NATIVE_GPU_TRACE_STORE_H
#include "NativeGpuTrace.h"
#include "NativeGpuTraceStream.h"

/* Nonblocking writers and readers. A busy slot is dropped, never partly read.
 * Storage is allocated once by the diagnostic owner, outside submission locks. */
struct NativeGpuTraceSlot { uint32_t busy; NativeGpuTraceRecord record; };
struct NativeGpuTraceStore {
    NativeGpuTraceSlot *slots=nullptr;
    uint32_t capacity=0;
    uint64_t issued=0,dropped=0;
    uint64_t push(NativeGpuTraceRecord record) {
        if(!slots || !capacity)return false;
        const uint64_t seq=__atomic_add_fetch(&issued,1,__ATOMIC_RELAXED);
        auto &slot=slots[(seq-1)%capacity];uint32_t empty=0;
        if(!__atomic_compare_exchange_n(&slot.busy,&empty,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
            __atomic_add_fetch(&dropped,1,__ATOMIC_RELAXED);return false;
        }
        // A delayed writer must not overwrite a newer lap of this slot.
        if(slot.record.sequence<seq){record.sequence=seq;slot.record=record;}
        else {__atomic_add_fetch(&dropped,1,__ATOMIC_RELAXED);__atomic_store_n(&slot.busy,0,__ATOMIC_RELEASE);return 0;}
        __atomic_store_n(&slot.busy,0,__ATOMIC_RELEASE);return seq;
    }
    bool read(uint64_t seq,NativeGpuTraceRecord *out) {
        if(!slots || !capacity || !seq || !out)return false;
        auto &slot=slots[(seq-1)%capacity];uint32_t empty=0;
        if(!__atomic_compare_exchange_n(&slot.busy,&empty,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return false;
        const bool found=slot.record.sequence==seq;if(found)*out=slot.record;
        __atomic_store_n(&slot.busy,0,__ATOMIC_RELEASE);return found;
    }
    uint32_t range(uint64_t after,uint32_t count,void *out,uint32_t bytes) {
        if(!slots || !capacity || !out || !count || count>NativeGpuTraceStreamRecords ||
           bytes<sizeof(NativeGpuTraceStreamHeader)+count*sizeof(NativeGpuTraceRecord))return 0;
        const uint64_t last=__atomic_load_n(&issued,__ATOMIC_ACQUIRE);
        if(after>last || after==UINT64_MAX)return 0;
        auto *head=static_cast<NativeGpuTraceStreamHeader *>(out);
        const uint64_t first=last>=capacity?last-capacity+1:1;
        *head={{0x31545047,NativeGpuTraceVersion,sizeof(NativeGpuTraceRecord),0,last,
                __atomic_load_n(&dropped,__ATOMIC_RELAXED)},first,after,0,0};
        auto *records=reinterpret_cast<NativeGpuTraceRecord *>(head+1);
        uint64_t start=after+1;
        if(start<first){head->skipped=first-start;start=first;head->next=first-1;}
        /* Inspect at most count slots, even when writers dropped positions.
         * A reader must never wait on a submission or hide a sequence gap. */
        for(uint32_t n=0;n<count && start<=last;++n,++start) {
            if(read(start,records+head->trace.count))++head->trace.count;
            else ++head->missing;
            head->next=start;
            if(start==UINT64_MAX)break;
        }
        return sizeof(*head)+head->trace.count*sizeof(*records);
    }
};
#endif
