/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "DitingBacklightProtocol.h"
#include "DitingPanelStreams.h"

namespace DitingPanel {
enum class Panel : unsigned { Unknown, L12_42, L12_36 };
// Xiaomi selects a panel from mi,panel-id in the chosen DT node. The
// 36/02/0b and 42/02/0a model names are not standard DA/DB/DC replies.
// This ADT property is emitted only after validating the retained live DT.
struct Selection {
    uint32_t version=0,modelHigh=0,modelLow=0,width=0,height=0,rgb=0;
};
static_assert(sizeof(Selection)==24,"Diting panel selection ADT ABI");
inline Panel model(const Selection &s) {
    if(s.version!=1 || s.modelHigh!=0x004c3132 || s.width!=1220 || s.height!=2712 || s.rgb!=1)
        return Panel::Unknown;
    if(s.modelLow==0x00420200)return Panel::L12_42;
    if(s.modelLow==0x00360200)return Panel::L12_36;
    return Panel::Unknown;
}
inline Panel identify(const Selection &s,unsigned da,unsigned db,unsigned dc) {
    // c5/10/00 was read from this L12_36 device in services204. It is a
    // hardware guard for the DT-selected model, not a universal model ID.
    // L12_42 streams remain available, but no verified DCS ID exists yet.
    if(model(s)==Panel::L12_36 && da==0xc5 && db==0x10 && dc==0)return Panel::L12_36;
    return Panel::Unknown;
}
inline unsigned modeValue(Panel panel,unsigned hz) {
    if(panel==Panel::L12_42)return hz==60?2:hz==90?4:hz==120?3:255;
    if(panel==Panel::L12_36)return hz==60?0:hz==90?1:hz==120?2:255;
    return 255;
}
inline unsigned modeRate(Panel panel,unsigned value) {
    const unsigned rates[]={60,90,120};
    for(unsigned hz:rates)if(modeValue(panel,hz)==value)return hz;
    return 0;
}
struct Batch {
    uint32_t words[DitingBrightness::Command::MaximumWords]{};
    unsigned count=0,waitMs=0;
};
struct Plan {Batch batch[4]{};unsigned count=0,expected=255;};

// Same embedded header and 0xff payload padding as Xiaomi dsi_ctrl.c.
// The pinned controller supports 64 bytes of TPG storage. Force a LAST
// boundary before that limit, retaining packet order and every DT delay,
// as the vendor does at its embedded-memory batch limit. No physical DMA.
inline bool decode(const uint8_t *blob,unsigned bytes,Plan &out,unsigned expected) {
    if(!blob || !bytes || bytes>128 || expected>255)return false;
    out={};out.expected=expected;
    unsigned at=0,lastHeader=0;
    while(at<bytes) {
        if(bytes-at<7 || blob[at+1]>1 || blob[at+2] || (blob[at+3]&~0x40u))return false;
        const unsigned type=blob[at],flags=blob[at+3],wait=blob[at+4];
        const unsigned size=(unsigned(blob[at+5])<<8)|blob[at+6];
        at+=7;
        if(!size || size>bytes-at || wait>100 ||
           (type!=0x15 && type!=0x39) || (type==0x15 && size!=2))return false;
        const unsigned need=type==0x15?1:1+(size+3)/4;
        if(need>DitingBrightness::Command::MaximumWords || out.count>=4)return false;
        auto *b=&out.batch[out.count];
        if(b->count+need>DitingBrightness::Command::MaximumWords) {
            b->words[lastHeader]|=0x80000000u;
            if(++out.count>=4)return false;
            b=&out.batch[out.count];
        }
        lastHeader=b->count;
        if(type==0x15)b->words[b->count++]=uint32_t(blob[at])|(uint32_t(blob[at+1])<<8)|0x00150000u;
        else {
            b->words[b->count++]=size|0x40390000u;
            for(unsigned i=0;i<size;i+=4) {
                uint32_t word=0;
                for(unsigned n=0;n<4;++n)word|=uint32_t(i+n<size?blob[at+i+n]:255)<<(8*n);
                b->words[b->count++]=word;
            }
        }
        at+=size;
        if(!(flags&0x40) || wait || at==bytes) {
            b->words[lastHeader]|=0x80000000u;b->waitMs=wait;
            ++out.count;
        }
    }
    return out.count>0 && out.count<=4;
}
inline bool plan(Panel panel,unsigned hz,Plan &out) {
    const uint8_t *blob=nullptr;unsigned bytes=0;
    if(panel==Panel::L12_42) {
        bytes=80;
        blob=hz==60?DtStreams::L12_42_60Hz:hz==90?DtStreams::L12_42_90Hz:hz==120?DtStreams::L12_42_120Hz:nullptr;
    } else if(panel==Panel::L12_36) {
        bytes=98;
        blob=hz==60?DtStreams::L12_36_60Hz:hz==90?DtStreams::L12_36_90Hz:hz==120?DtStreams::L12_36_120Hz:nullptr;
    }
    return blob && decode(blob,bytes,out,modeValue(panel,hz));
}
inline bool readByte(unsigned count,uint32_t d0,uint32_t d1,uint8_t &value) {
    if((count==4 || count==8) && (d0>>24)==0x21) {value=(d0>>16)&255;return true;}
    if(count==8 && (d1>>24)==0x1c && ((d1>>16)&255)==1 && !(d1&0xff00)) {
        value=d0>>24;return true;
    }
    return false;
}
}
