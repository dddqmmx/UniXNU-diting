// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../drivers/ArmDTPlatform/DitingPlatformIdentity.h"
#include <assert.h>
#include <string.h>

int main()
{
  const char valid[] = "a1234567-b123-5123-8123-c123456789ab";
  char output[37] = {};
  assert(diting_platform_identity_copy(valid,sizeof(valid),output));
  assert(!memcmp(valid,output,sizeof(valid)));
  const char upper[] = "A1234567-B123-5123-8123-C123456789AB";
  assert(diting_platform_identity_copy(upper,sizeof(upper),output));
  for (unsigned bytes = 0; bytes < 80; ++bytes) {
    if (bytes != sizeof(valid)) assert(!diting_platform_identity_copy(valid,bytes,output));
  }
  assert(!diting_platform_identity_copy(nullptr,37,output));
  assert(!diting_platform_identity_copy(valid,37,nullptr));
  const char nullUUID[] = "00000000-0000-0000-0000-000000000000";
  assert(!diting_platform_identity_copy(nullUUID,sizeof(nullUUID),output));
  // Validate every byte and the exact extent used by the actual kernel path.
  for (unsigned i = 0; i < 37; ++i) {
    char changed[37];memcpy(changed,valid,sizeof(changed));changed[i] = char(0xff);
    assert(!diting_platform_identity_copy(changed,sizeof(changed),output));
    if (i < 36) {
      changed[i] = '\0';
      assert(!diting_platform_identity_copy(changed,sizeof(changed),output));
    }
  }
  // Missing terminators and extra bytes are refused without reading them.
  char unterminated[36];memcpy(unterminated,valid,sizeof(unterminated));
  assert(!diting_platform_identity_copy(unterminated,sizeof(unterminated),output));
}
