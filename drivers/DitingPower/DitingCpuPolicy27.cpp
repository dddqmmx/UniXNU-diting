// SPDX-License-Identifier: BSD-2-Clause-Patent
// iOS 27 (24B156) EPSS CPU policy driver unit. Upstream component:ArmCpu does
// not include DitingCpuPolicy.h, so this translation unit hosts it and exports
// diting_cpu_policy_start() and diting_cpu_snapshot_text().
#include "../ArmCpu/DitingCpuPolicy.h"

extern "C" void diting_cpu_policy_start(void)
{
    DitingCpuPolicy::start();
}
