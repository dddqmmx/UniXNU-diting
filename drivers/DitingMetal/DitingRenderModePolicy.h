/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace DitingRenderModePolicy {
enum : uint32_t { Auto=0, Sysmem=1, Gmem=2, Ambiguous=3, NoConcurrentBinning=1 };
struct Policy { uint32_t mode, flags; };
// Read the selected debug tokens; observing a policy never changes it.
inline Policy parse(const char *text) {
    bool sysmem=false, gmem=false, nocb=false;
    if(text)for(const char *at=text;*at;) {
        const char *end=strchr(at,',');
        const size_t bytes=end?size_t(end-at):strlen(at);
        sysmem|=bytes==6 && !memcmp(at,"sysmem",6);
        gmem|=bytes==4 && !memcmp(at,"gmem",4);
        nocb|=bytes==4 && !memcmp(at,"nocb",4);
        if(!end)break;
        at=end+1;
    }
    return {sysmem&&gmem?Ambiguous:sysmem?Sysmem:gmem?Gmem:Auto,
            nocb?NoConcurrentBinning:0u};
}
}
