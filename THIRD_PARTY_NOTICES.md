# Third-party source and asset notices

This repository (`UniXNU-diting`) is maintained as an unofficial, independent hardware port for Redmi K50 Ultra (`diting` / Qualcomm SM8475). It is not affiliated with, endorsed by, or sponsored by Apple Inc., Xiaomi Inc., Qualcomm Incorporated, or any other vendor. Trademarks belong to their respective owners.

## Upstream provenance and credits

Although maintained independently from upstream repositories, this port builds directly upon and gratefully acknowledges the following projects and their contributors (full Git history, copyright headers, and per-component license files are preserved):

- **UniXNU** ([UniXNU/UniXNU](https://github.com/UniXNU/UniXNU)): core UEFI/XNU build framework, ARM64 support, OS profiles, reusable components, and `SoftwareAES` reference (`BSD-2-Clause-Patent`).
- **Community SM8475 / Pong port** ([Chengdong-Yu/UniXNU-pong](https://github.com/Chengdong-Yu/UniXNU-pong), `Chengdong-Yu/pong-*`, and [Chengdong-Yu/uefi-pong](https://github.com/Chengdong-Yu/uefi-pong)): original SM8475 board/driver foundation from which the vendored `DitingPlatform24` drivers and SM8475 SoC seed were derived.
- **UniXNU-sirius** ([57UU/UniXNU-sirius](https://github.com/57UU/UniXNU-sirius)): Linux cross-compilation toolchain workflow reference.

## Source components

- UniXNU core, DarwinBoot, XnuPrelink, NativeUfs, NativeRamBlock, and the Adreno modules: retain their per-file SPDX identifiers, copyright notices, and upstream repository `LICENSE` files under `drivers/`.
- `DitingPlatform24` and the vendored SM8475 device drivers under `drivers/` (originally derived from [Chengdong-Yu/UniXNU-pong](https://github.com/Chengdong-Yu/UniXNU-pong) and `Chengdong-Yu/pong-*`): original sources, copyright notices, and per-directory `LICENSE` files are retained; the sources are adapted for Diting and provenance is documented here and in `README.md` / `README_EN.md`. They are not upstream A740/WCN7850 implementations.
- `drivers/DitingCrypto` (`NativeAES27.cpp`, `DitingSoftwareAes27.cpp`, `OriginalAESABI27.h`): derived from UniXNU `drivers/SoftwareAES/NativeAES.cpp` under `BSD-2-Clause-Patent`.
- `drivers/DitingTouch`: implements the ST61Y wire protocol independently without Linux dependencies, under `BSD-2-Clause-Patent`. The protocol/register reference revisions are recorded in its README; the XNU producer retains its original APSL notice. No vendor touch firmware is distributed.
- `platform-firmware/ditingPkg` (Diting UEFI firmware package): derived from EDK2, Project Mu, and Silicium-ACPI (`BSD-2-Clause-Patent` / upstream EDK2 licenses apply). The EDK II build tree is not vendored here.
- Linux GPL-2.0 components: the `ath11k`/MHI/QMI bodies compiled from the pinned Linux source tree are `GPL-2.0-only`; GPL-2.0-licensed driver subcomponents retain their `LICENSE-GPL-2.0-only` files. `metal2vulkan`, Mesa, and Turnip components carry their own GPL/LGPL/MIT licenses.
- Apple Open Source dependencies (including XNU 12377.121.6 / Darwin 25.5.0 for iOS 27.2.1 and XNU 11417.140.69 / Darwin 24.6.0 for iPadOS 18.7.10) are governed by the Apple Public Source License (APSL) and fetched according to pinned revisions at build time.
- Project Mu, EDK2, and other build-time dependencies keep their upstream licenses.

## Apple restore assets

- The default target restore is **iOS 27.2.1 (24B156)** (`iPhone18,2` / `V54AP`), and the legacy target restore is **iPadOS 18.7.10 (22H374)** (`iPad7,11` / `J171AP`). The OS, SystemOS, AppOS, trust caches, and related components are obtained from Apple's public restore service by local scripts at build time. No Apple restore binaries, caches, or IPSW contents are stored in this repository, its git history, or its public CI artifacts.

## Vendor / device firmware

- The board firmware blobs listed in `proprietary-files.txt` (Qualcomm ADSP/modem/QCA6490 `amss`, `amss20`, `m3`, `regdb`; Adreno 730 GMU/ZAP/SQE; NXP TFA9873 `tfa98xx.cnt`; Goodix touch firmware; Xiaomi device firmware) are the property of their respective vendors. They are extracted by the user from their own device or local firmware, are not stored or redistributed by this repository, and must not be committed or attached to public artifacts.
- Qualcomm, NXP, STMicroelectronics, Goodix, and Xiaomi firmware is used only on the user's own hardware under the vendors' terms.

## No warranty

The device-level LICENSE does not replace a component's file-level license. This repository is provided as-is for research; see the README disclaimer.
