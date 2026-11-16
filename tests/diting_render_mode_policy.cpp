/* SPDX-License-Identifier: MIT */
#include "../drivers/DitingMetal/DitingRenderModePolicy.h"
#include <assert.h>
using namespace DitingRenderModePolicy;
int main() {
    assert(parse(nullptr).mode==Auto);
    assert(parse("nocb").mode==Auto && parse("nocb").flags==NoConcurrentBinning);
    assert(parse("gmem,nocb").mode==Gmem && parse("gmem,nocb").flags==NoConcurrentBinning);
    assert(parse("nocb,sysmem").mode==Sysmem);
    assert(parse("sysmem,gmem,nocb").mode==Ambiguous);
    assert(parse("nogmem,nosysmem,nocb-extra").mode==Auto);
    assert(parse("gmem-extra").flags==0);
    assert(parse(",,gmem,,nocb,").mode==Gmem);
}
