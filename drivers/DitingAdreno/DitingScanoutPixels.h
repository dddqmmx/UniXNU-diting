/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "../DitingPower/DitingDisplayText.h"

namespace DitingScanoutPixels {
constexpr unsigned Side=64,Count=Side*Side,Chunk=64;
struct Request {bool capture=false;unsigned first=0,count=0;};
inline bool decimal(const char *&p,unsigned &value) {
    value=0;
    if(*p<'0' || *p>'9')return false;
    do {const unsigned digit=unsigned(*p-'0');if(value>(Count-digit)/10)return false;
        value=value*10+digit;++p;
    } while(*p>='0' && *p<='9');
    return true;
}
inline bool parse(const char *p,Request &out) {
    if(!p)return false;
    const char prefix[]="display pixels";
    for(unsigned i=0;i<sizeof(prefix)-1;++i)if(*p++!=prefix[i])return false;
    out={};
    if(!*p) {out.capture=true;return true;}
    if(*p++!=' ' || !decimal(p,out.first) || *p++!=' ' || !decimal(p,out.count) || *p)return false;
    return out.count && out.count<=Chunk && out.first<Count && out.count<=Count-out.first;
}
struct Snapshot {
    uint64_t capture=0,copy=0,startUs=0,endUs=0;
    uint32_t changed=0;
};
inline unsigned format(char *out,unsigned bytes,const Snapshot &s,const uint32_t *pixels,Request request) {
    DitingDisplay::Text t{out,bytes};
    t.add("PIXELS schema=1 status=PASS capture=%llu copy=%llu start_us=%llu end_us=%llu changed=%u width=1220 height=2712 stride=4880 grid_width=64 grid_height=64 format=80 first=%u count=%u physical=b8000000 physical_colour=UNQUALIFIED\n",
        (unsigned long long)s.capture,(unsigned long long)s.copy,(unsigned long long)s.startUs,
        (unsigned long long)s.endUs,s.changed,request.first,request.count);
    if(request.count && pixels) {
        t.add("DATA ");
        for(unsigned i=0;i<request.count;++i)t.add("%s%08x",i?",":"",pixels[request.first+i]);
        t.add("\n");
    }
    return t.used;
}
}
