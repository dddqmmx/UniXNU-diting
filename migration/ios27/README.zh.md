# Diting iOS 27.2.1（24B156）适配

本项目已将 `board.toml` 的默认目标正式切换为 **iOS 27.2.1（24B156）**（`os_profile = "ios-27.2.1-24B156"`、`diting_ios27_defconfig`、`arm64e`）；原 iPadOS 18.7.10（22H374）配置保留在 `[os_overrides."ipados-18.7.10-22H374"]`（通过 `--os ipados-18.7.10-22H374` 选择）。已完成真机（`1d4eb066`）**iOS 27.2.1 完整用户态启动、Adreno 730 60 FPS GPU 硬件加速与 1220×2712 实机锁屏显示、ST61Y SPI 触控接入、`FromEdgeTip` 边缘手势钩子安装、运行态 USB 返回 fastboot 及 `boot_b` 持久化刷入**。

## 已确认的输入

| 项目 | 实际输入 |
| --- | --- |
| 设备 | Redmi K50 至尊版 / diting / SM8475，八核，4 KiB 页 |
| 还原包 | iPhone18,2 / V54AP，27.2.1 / 24B156 |
| BuildIdentity | `Customer Erase Install (IPSW)`，ApChipID 0x8150、ApBoardID 0x0e |
| 原始内核 | `kernelcache.release.v54`，78,512,128 字节，arm64e MH_FILESET，301 个条目 |
| 原始内核版本 | Darwin 27.2.0，XNU 13432.42.3，RELEASE_ARM64_T8150 |
| 公开源码基线 | XNU 12377.121.6 / Darwin 25.5.0，提交 ac9718fb1af618d5ce8678d0dc6e8a58f252216f |
| 原始 OS | `141-10103-223.dmg.aea`，解密后 10,580,131,840 字节，UUID 2913A3C9-CACD-44DB-A47E-90F19C3610A5 |
| OS 内容 | `/System/Library/CoreServices/SystemVersion.plist` 实际为 iOS 27.2.1 / 24B156 |
| SystemOS / AppOS | `141-10617-235.dmg.aea` / `141-08824-252.dmg` |
| 原始 SystemOS | 解密后 6,322,913,280 字节，UUID C78313FC-73C7-461D-A8A7-D3273E1B2B7B |
| 内核 family | APFS 3288.42.1、IOGPUFamily 162.16.1、IOSurface 403.1、IOStorageFamily 2.1 |

Apple 尚未发布此还原包内核的源码。上游当前的 arm64e 路径以公开的 XNU 12377 加兼容补丁构建；
编译成功不能证明它与 24B156 的全部调用和二进制类型布局兼容。配置分别记录源码内核与原始内核版本。

同一还原包还有 `Recovery Customer Install` 的 Erase 身份，其 OS 是另一个镜像。
`diting_ios27.restore_identity()` 严格选取正式系统身份，避免误用恢复环境。
旧的 22H374 UUID、静态预链接 slide、24A446 函数地址和匹配缓存不用于此目标。

## 可复现的主机入口

UniXNU 固定在 519d80242a5028ee123447b01bc3b809baed565f。
组件版本见 [diting-ios27-inputs.toml](../../configs/diting-ios27-inputs.toml)。
公开源、组件、设备源码快照、构建缓存和 Apple 输入各自在新输出目录中保存，现有设备 checkout 与共享缓存保留。

```sh
python3 scripts/fetch-diting-ios27-inputs.py \
  --unixnu /path/to/pinned-UniXNU --out /path/to/private/ipsw \
  --lzfse /path/to/lzfse --jobs 8

python3 scripts/fetch-diting-ios27-inputs.py \
  --unixnu /path/to/pinned-UniXNU --out /path/to/private/ipsw \
  --userland --decrypt-with /path/to/ipsw --jobs 8

python3 scripts/build-diting-ios27.py \
  --unixnu /path/to/pinned-UniXNU --out /path/to/private/build \
  --stage prepare --jobs 8
```

支持原生 AEA 解密的 `ipsw` 3.1.749 已在本机 Linux 解密正式 OS；密钥由工具内部处理。
提取不生成独立镜像哈希、审计或设备回读。ZIP/AEA 解码与文件系统元数据检查仍保留其格式约束。

Linux clang 需要 XNU 类型内建函数及四参数 `__ptrauth`。原工具链缺少
`"authenticates-null-values"` 的解析，但 LLVM 后端已有相应签名与认证行为。
[编译器补丁](../../patches/toolchain/clang-ptrauth-auth-null.patch)将该参数传给已有 codegen。
生产 XNU 头文件及其 PAC 语义保持原样。

```sh
# 在已有 UniXNU Linux 隔离环境中执行，base-toolchain 默认为 /opt/unixnu-linux。
python3 scripts/build-diting-ios27-toolchain.py \
  --unixnu /path/to/pinned-UniXNU --out /path/to/private/compiler --jobs 8

env UNIXNU_TOOLCHAIN=/path/to/private/compiler/toolchain \
  python3 scripts/build-diting-ios27.py \
  --unixnu /path/to/pinned-UniXNU --out /path/to/private/build \
  --stage kernel --jobs 8

env UNIXNU_TOOLCHAIN=/path/to/private/compiler/toolchain \
  python3 scripts/build-diting-ios27.py \
  --unixnu /path/to/pinned-UniXNU --out /path/to/private/build \
  --stage prelink --kernelcache /path/to/private/ipsw/kernelcache.decompressed --jobs 8
```

准备阶段固定 SM8475 DTS、0x000–0x700 的 CPU affinity 和八核参数。
内核阶段构建实际 arm64e 源码内核；预链接阶段另外构建 RELEASE-LTO 比较内核，并对新的 fileset 建立映射。
缺失或有歧义的引用必须解决，默认不允许 `--allow-missing`。
阶段结果分别写入输出目录的 `diting-ios27-state.json`，不将 native-only 内核标成可运行 iOS 的成品。

## 验证与尚未完成项

最新阶段交接详情见 [2026-11-24 Wi-Fi 深度诊断、软件 AES 与自动测试进度交接文档](交接-2026-11-24-ios27-24B156-WiFi诊断与自动测试进度.md)（历史记录见 [2026-11-23 严格预链接清零与 8 核/存储/APFS 实机验证交接文档](交接-2026-11-23-ios27-24B156-严格预链接清零与8核存储APFS实机验证.md)、[2026-11-22 交接文档](交接-2026-11-22-ios27-24B156-进入bsd_init与自动日志链路.md)、[2026-11-20 启动推进交接](交接-2026-11-20-ios27-24B156-启动推进.md)）。

`python3 -m unittest discover -s tests -p test_diting_ios27.py -v` 已通过 **12** 项检查，
覆盖正式系统/恢复环境选择、错误版本与芯片拒绝、重复身份、私有补丁复用、严格预链接检查以及坏 fileset 范围。
真实 BuildManifest 与原始内核也通过同一身份检查。

编译器门禁检查实际 LLVM IR：四参数字段写零仍执行签名，读取始终认证；三参数字段保留 nullable 行为，
无效选项必须拒绝，同时检查 XNU 类型内建函数。其结果写入编译器输出的 `checks/compiler-check.json`
（已还原 `llvm-ios27` 中未完成的 `__auth_nulls` 实验性修改并备份补丁，消除 C++ 全局变量 `TrackingVH` 崩溃，
保持编译器与 `patches/toolchain/clang-ptrauth-auth-null.patch` 一致）。
`patches/tooling/linux-arm64e-fp-type-discrimination.patch` 为 `arm64e` 构建统一启用
`-fptrauth-function-pointer-type-discrimination`，`patches/xnu-ios27/iig-ptrauth-member-cast.patch`
修正 DriverKit `iig` 生成代码中缺失 `__builtin_load_member_function_pointer` 时的成员指针 PAC 重签。

`XnuPrelink`（`patches/components-ios27/xnuprelink-conservative-reference.patch`，`MATCHER_VERSION=14`，**91** 项单测 PASS）
已达成 **24B156 严格预链接全清零**：
- `fileset_shape.py` 归一化 ARMv8.5 `BTI` 落地指令与 `MSR TCO`（保留的 24B156 kext `__TEXT_EXEC` 全量扫描已确认含 0 条 `MSR TCO` / 0 条 MTE tag 指令）；
- `fileset_match.py` 新增 `OSMetaClass` 构造调用提取、`struct vnodeop_desc` `vnop_*` 识别、`OSSymbol::withCString*` 全局符号提取、
  只读 `Class::metaClass` 槽位消歧、`pair_vtable_ctors`、arm64e `MOVK ..., LSL #48` PAC diversity 指纹（`pair_pac_signatures`）、
  `anchor_reference_verified`、`pair_empty_destructors`，以及经反汇编与 XNU 源码逐条核验的 **420** 个函数 / **18** 个数据条目的 24B156 审查锚点（`fixtures/fileset-24B156-symbols.json`）；
- `fileset_segmented.py` 对齐 `com.apple.kernel` 与根 `MH_FILESET` 的 `cpusubtype`（`0xc0000002`）；
- `os-profiles/ios-27.2.1-24B156/profile.toml` 与 `board.toml` 固定 24B156 AMFI 早期安全初始化入口
  `early_security_init = "com.apple.driver.AppleMobileFileIntegrity=0xfffffe000927b430"`，保持默认严格预链接（无 `--allow-missing`）；
- `build-diting-ios27.py --stage prelink` 已在严格模式下返回 0，产出 `output/diting/ios-27.2.1-24B156/diting_ios27/kernel/kernel.macho`（`93,601,792` 字节，**0** 未解析引用、**0** 存根函数、`allow_missing: false`、`mapping_complete: true`、`kernel_collection_linked: true`）。

SM8475 UFS、平台资源与 USB 诊断驱动（`scripts/build-diting-ios27-drivers.py`）已编译为 `libDitingUfs27.a`（`pending_imports: []`），
并已接入 `arm64e` 内核（`drivers_linked: true`），配合：
- `patches/xnu-ios27/sm8475-no-cntp-ctl.patch`、`sm8475-no-iortc-wait.patch`（解除 `bsd_init()` 30s `IORTC` 等待）；
- `patches/xnu-ios27/sm8475-secondary-cpu-bringup.patch` 与 `board.toml` 中 `armcpu-self-ipi=1`（解决 SM8475 `arm64e` 8 核副核页表 PoC 清理、`CPACR_EL1` `isb sy`、`VBAR_EL1` 切换时序与核间 IPI）；
- `drivers/NativeUfs/DitingUfsTransport.cpp`（兼容 iPadOS 18 直接绑定与 iOS 27 延迟子类 `UniXNUBlockStorageDevice`）；
- `patches/xnu-ios27/source-storage-runtime.patch`、`diting-ramoops-console.patch`（12 份相位偏移 `D27L`）与 `diting-early-fb-trace.patch`。

实机 `1d4eb066` 已完成整机验证并持久化刷入 `boot_b`（`boot-diag-46a` / `boot-diag-46b` -> `boot-ios27-final`）：
- **严格预链接与 AMFI 修复**：`kernel.macho`（`93,716,480` 字节，0 未解析引用、0 存根函数）将 AMFI GOT 槽位 `0xfffffe0008143ee0` 绑定到 `_unixnu_kernel_stack_shift`，彻底消除 `OSEntitlements.cpp:863` panic；`lcks=0x10` 消除 RELEASE APFS 与 DEVELOPMENT XNU 之间的 `RWLock` 调试跟踪误报；
- **8 核 SMP、UFS 与三卷挂载**：8 核全上线；SM8475 UFS 与 `SHNGROOT` 挂载 `24B156` `OS`（`2913A3C9-...`）、`SystemOS`（`C78313FC-...`，union over `/`）、`staged_system_apps`（union over `/Applications`）与可写 `Data`（`E5FD1797-...`，8 GiB RW on `/private/var`）；
- **跳过 4K 全量重算加速启动**：`patches/xnu-ios27/ios27-fast-cs-debug.patch`（`diting_fast_cs=1`）使内核到达 `Early boot complete` 耗时从 ~130 秒缩短至 **~9 秒**；
- **一次性 24B156 Data 种子与 Keychain 清理**：`patches/xnu-ios27/ios27-data-seed-skip-migration.patch` 自动写入 `com.apple.migration.plist` / `com.apple.DataMigration.plist`（`24B156`）、`com.apple.purplebuddy*.plist`、`com.apple.springboard.plist` 以及 `{mobile,root}/Library/Preferences/{backboardd,.GlobalPreferences}.plist`（禁用 10-bit 深度缓冲与压缩以适配 32-bit BGRA GOP），并清理旧版 Keychain，解除 `backboardd` 阻塞并彻底消除 `securityd` 崩溃循环；
- **实机画面点亮与持续输出（`boot-diag-46b`，160 秒全程零 panic）**：修复 `NativeLegacyDisplay::announcePoweredByFirmware()` 为 `OriginalLegacyStateRequest{0, 1, 0, 1, 1, {}}`（同时置位 `requested=1` 与 `powered=1`）后，`IOMobileFramebufferLegacy` 持续向 `1220×2712` GOP（`0xb8000000`）提交帧：**100/100 swaps accepted，0 rejected**；`frame #0..#4` 绘制居中苹果启动徽标（`lit=29457, box=(513,1181)-(706,1417)`），`frame #5..#15+` 过渡至完整 iOS 27 锁屏画面（`lit=21997..22012, box=(393,116)-(1073,2687)`），261 个 iOS 27.2.1 进程（含 `SpringBoard`、`backboardd`、`PosterBoard`、`InputUI`、`AccessibilityUIServer`、`securityd`、`lsd`、`iconservicesagent`）稳定运行；
- **持久化刷入**：`boot-ios27-final/diting-ios27-boot.img`（`117,960,704` 字节，不含自动定时重启参数，保留 `diting_fast_cs=1` 与 `lcks=0x10`）已刷入当前活动槽位 `boot_b` 并冷启动进入 iOS 27.2.1 持续运行。

### 2026-11-24 Adreno 730 GPU 硬件加速、触控坐标修复、边缘解锁钩子与 USB Fastboot 验证

在真机 `1d4eb066` 上继续完成并验证以下能力（构建产物 `boot-ios27-gpu-unlock3-112/diting-ios27-boot.img` / `.sparse.img`，`--fat-mib 112` = `117,960,704` 字节，`kernel.macho` `93,880,320` 字节，0 未解析引用 / 0 存根函数）：
- **Adreno 730 GPU + arm64e Turnip/Metal 60 FPS 全屏加速**：
  - 在 `drivers/DitingMetal/DitingSurfaceHandoff.h` 中针对 `DITING_GUEST_24B156` 绕过 iOS 27 不存在的 `IOCoreSurfaceRoot` selector `0x504f4701`，保留 `syncActiveImports`；
  - 升级 `drivers/DitingMetal/NativeBridgeMetalFrontend.mm`（`MTLCompiler.framework/Versions/32023`、`newLibraryWithSource:`/`newLibraryWithStitchedDescriptor:`、`b3a8` 双平面布局回退）与 `drivers/DitingMetal/DitingMetalPlugin.mm`（沙盒客户端 `propertyFalse` 与 `METAL2VULKAN_SHADER_CACHE_OPTIONAL=1`）；
  - 实机确认 `backboardd`、`SpringBoard`、`MercuryPosterExtension`、`WidgetRenderer_Default` 均获得真实 Adreno 730 Vulkan/Metal 设备（`status=0x0`），持续 60 FPS GPU 渲染与 `scanout_convert`（实测 `offers=26185 comp=26185 fail=0 acc=26187 rej=0 lit=1024/1024`）。
- **ST61Y 触控线性标定与底部边缘上滑解锁修复**：
  - 修复 `drivers/DitingTouch/NativeHidInput27.cpp` 对 `DitingSt61yProtocol.h` 已归一化 `0..65535` 坐标再次按 `12200×27120` 缩放导致 `x > 18.6%`、`y > 41.4%` 全被截断的问题，改为按 `65536` 线性映射至 `0..32767`；
  - 在 `drivers/DitingMetal/DitingMetalPlugin.mm` 为 `backboardd` 内的 `BKHIDDirectTouchEventProcessor processEvent:sender:dispatcher:` 安装 PAC 安全 ObjC swizzle 钩子（实机日志确认 `edge-touch FromEdgeTip hook installed on BKHIDDirectTouchEventProcessor status=0x0`），对边缘落点（含底部 Home Bar `y > 0.921`）在子触点与父集合事件置位 `kIOHIDDigitizerEventFromEdgeTip`（`0x800`）。实机物理上滑进入桌面待用户手测确认。
- **运行态与冷启动 USB 控制台 + 一键返回 fastboot（已实机验证）**：
  - 通过冷启动落盘日志 `D27U` 定位冷启动 USB 失败根因：冷启动时 `PM8350c GPIO7` 与 NXP Repeater/eUSB2 PHY 已就绪（`revision=a1 id=22:32:1`），但 `DSTS=0x420004`、`QGEN=0x4`、`GUSB2=0x102500` 导致 `DWC3_DCTL_CSFTRST` 在 `QSCRATCH` 未切至 UTMI-as-PIPE 且 PHY `SUSPHY`/`ENBLSLPM` 未清零时超时；
  - 在 `drivers/DitingUsb/DitingUsbDevice.h` 中在 `DWC3_DCTL_CSFTRST` 之前强制置位 `GCTL.DSBLCLKGTNG`、清零 USB2/USB3 `SUSPHY`/`ENBLSLPM` 并在 `QSCRATCH + 0x08` 切换 `PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW`（`QGEN=0x4 -> 0xd`，并保留 `GCTL.CORESOFTRESET` + `PHYSOFTRST` 回退路径）；
  - 已将永久镜像 `boot-ios27-gpu-coldusb1-112/diting-ios27-boot.sparse.img`（`--fat-mib 112` = `117,960,704` 字节，`kernel.macho` `93,880,320` 字节，0 未解析 / 0 存根）稀疏刷入 `boot_b` 并经两次真机纯冷启动验证：**纯冷启动 19 秒自动枚举 `05ac:1881` USB 控制台，系统 60 FPS GPU 锁屏启动（`lit=1024/1024`）后保持在线，实测冷启动状态下执行 `scripts/diting-usb-libusb.py fastboot` 23~24 秒自动同步文件系统并返回 `1d4eb066 fastboot`**；内核 panic 时继续由 `diting_panic_fastboot=1` 自动回退 fastboot。
  - 实机冷启动触控日志已记录底部边缘落点命中 `edge-touch down idx=0 mask=0x7 x=0.595 y=0.962 edge=1` 且画面随即离开锁屏进入浅色系统界面（`offers=924 comp=924 fail=0`）。

- **电池识别与 QCA6490 Wi-Fi 上线（`boot-ios27-gpu-battwifi3-112`，已刷入 `boot_b` 并通过纯冷启动验证）**：
  - **UFS 固件包持久化**：将真机签名 `adsp.mbn`（23 MiB）与全部 222 个 QCA6490 Wi-Fi 固件文件（`amss20.bin`/`m3.bin`/`regdb.bin`/`bdwlan.*`，25 MiB）打包为 4K 对齐 `PWF2` 包（47 MiB），永久写入 `sda37` 64 GiB 偏移（`seek=16777216`），并在 `drivers/NativeUfs/DitingUfsTransport.cpp`、`drivers/DitingPower/DitingFirmware.c`、`drivers/DitingWifi/DitingWifiPlatform.cpp` 中接入 `DITING_GUEST_24B156` 按需读取；
  - **修复 `arm64e` XNU `IOWorkLoop` 主动事件源误判（电池与中断事件源根因）**：`arm64e` 启用 `ptrauth_member_function_pointer_type_discrimination` 时，`OSMetaClassBase::_ptmf2ptf` 返回共享 `_vfpthunk_` 而非解析虚表项，导致 `IOWorkLoop::eventSourcePerformsWork()` 比较 `inEventSource` 与 `controlG` 的 `checkForWork` 恒等并把所有 `IOInterruptEventSource`（含 `DitingBattery::inboxEvent`）误归类为 passive；新增 `patches/xnu-ios27/ios27-ioworkloop-arm64e-active-eventsource.patch` 后，`DitingAdsp`（`stage=13`）与 `DitingBattery`（`stage=4`，实测发布 `74%–76%`、`4080–4115 mV`、`VBUS=4466–4471 mV`、`external=1`、`charging=1`）立即上线并持续刷新；
  - **QCA6490 PCIe0 + arm64e `ath11k` + 原生 iOS 27 `wifid` 接入**：将全部 37 个 Linux 7.0.14 `ath11k`/MHI/QMI 源文件以 `arm64e-apple-ios27.2 -fptrauth-function-pointer-type-discrimination` 编译链接为 `DitingAth11k-linked.o`（0 函数指针类型不兼容警告、strict prelink `kernel.macho` `94,060,544` 字节、0 未解析 / 0 存根）；在 `drivers/DitingWifi/DitingWifiNet.cpp` 中将 `kCardCaps` 扩展至 32 字节以匹配 iOS 27 `24B156` 的 23 字节请求，并在 `scripts/build-diting-ios27-bootimg.py` 的 `/arm-io` 下补充真实 `wlan`（`qcom,qca6490-wlan`）ADT 节点以解除原生 `com.apple.wifid` 的 `_LimitLoadToDeviceTree` 门控；
  - **实机纯冷启动验证（`boot_b`，`test_diting_ios27.py` 12/12 PASS）**：纯冷启动 10 秒枚举 `05ac:1881` USB（`QGEN=d`），`DitingAdsp stage=13`、`DitingBattery stage=4`（`74%`, `4080 mV`, `VBUS=4471 mV`, `PMIC_online=1`），`QCA6490` 挂起 `en0` 并扫描到 **25 个真实 Wi-Fi 网络**，原生 `/usr/sbin/wifid` 自动启动并打开 `Diting80211UserClient` 完成 `en0` 绑定（`native client type 43/12/96/97/19 -> 0`），同时保持 60 FPS GPU UI（`lit=1024/1024, fail=0`）与 `edge-touch` 边缘上滑钩子（已修复 `mask=0x7` 移动帧重复触发 `down`）；设置内 Wi-Fi 列表/联网与电池图标 UI 表现待用户屏幕实测确认。

- **物理电源/音量键、背光控制、色彩管理与 120 Hz 模式（`boot-ios27-gpu-btnbl120-3-112`，已稀疏刷入 `boot_b` 并通过纯冷启动验证）**：
  - **电源键与音量键（`drivers/DitingTouch/DitingButtons27.cpp`）**：基于 SM8475 SPMI arbiter v7（`core=0xc400000`、`cnfg=0xc42d000`、`obsrvr=0xc440000`）读取 PMK8350 PON（PPID `0x013` bit 7=电源、bit 6=音量-）与 PM8350 GPIO6（PPID `0x18d` 低有效=音量+），采用 10 ms 常规 / 5 ms 去抖只读 observer 轮询（规避 PDC v2.7 `0x20700` 共享中断风暴），通过 `NativeHidInput27` Consumer Report 2（bit 1=`0x30` 电源、bit 2=`0xe9` 音量+、bit 3=`0xea` 音量-）上报；纯冷启动 USB `cmd buttons` 实测：`BUTTONS stage=3 irq_mode=0 pon=407 gpio6=434 pdc=0x20700 err=0 usable=1/1/1 sink=1`。
  - **背光控制与 `CoreBrightness` 物理控制器接入（`drivers/DitingPower/DitingBacklight.cpp` + `drivers/DitingMetal/DitingMetalPlugin.mm`）**：
    - 复用内核中真实 `com.apple.driver.AppleARMPlatform` 的 `AppleARMBacklight` metaclass（避免重复注册类导致 panic），通过 SM8475 DSI0（`0x0ae94000`）DCS `0x51`/`0x52`（`0..4095`、`bl_inverted_dbv=1`、回读比例 4×）发布 iOS 27 `CoreBrightness` 所需的 `IODisplayParameters.BrightnessMilliNits`（`2000..500000`）、`backlight-marketing-table`、`LminProduct`/`LmidProduct`/`LmaxProduct`、`backlight-calibration-parameters`、`NativeSandboxGrantProcess="backboardd"`，支持 `brightness-nits`（有符号 16.16 nits）与 `brightness`/`FadePeriod`；
    - 在 `DitingMetalPlugin.mm` 的 `backboardd` 早期构造函数中安装 PAC 安全 `-[BrightnessSystemInternal initBLControl:]` 钩子，将受 ADT `vmm-present` 影响的 `mode=3`（`VMBLControl`）改写为 `mode=1`（物理 `BLControl`）；
    - 纯冷启动实测：`DitingBacklight: L12 DCS service ready, level=307 max=4095, readback PASS`（`raw=1228`, `reads=1`, `err=none`），`CoreBrightness initBLControl mode=3->1 ok=1`，且 `CoreBrightness` 成功调用 `DitingBacklight::setProperties()`（`DisplayBrightnessNitsNVRAM`、`CompensationTweak`）并打开 `IOMobileFramebuffer` 色彩/背光客户端（`#5/#6/#7`），无沙盒拒绝。
  - **显示色彩管理与 GPU 线性 BGRA 红蓝修复**：
    - 将 iPadOS 18 `services210`+`services211` 的 Mesa Turnip `tu_clear_blit.cc` 线性 BGRA GMEM 载入/保存色彩修复合入 iOS 27 `arm64e` `libvulkan_freedreno.dylib`（导出 `tu_gmem_destination_stats`/`tu_gmem_event_stats`，不改写 `services187` 共享源码），并同步更新 `trustcache-24B156-gpu.bundle` 与 `kernel.macho.gpuaddon`；实机 `display pixels` 采样证实帧缓冲主色由红蓝互换（`ff7f8d9f`/`ff5c6d82`）恢复为与 `SSPP UNPACK=03020001` 一致的真实暖色 RGB（`ff9e8d7f`/`ff735f4e`/`ffece7e4`）；
    - 面板 DDIC `display panel` 实测 `address_mode=00`（RGB 顺序）、`pixel_format=07`（24-bit RGB888）、`colour_error=0`，`display color` `status=PASS`，ADT 保持 `artwork-display-gamut="sRGB"`，OS 层由 `CoreBrightness`（`CBDisplayContaineriOS`/`CBColorModuleiOS`/`libEDR`）与 QuartzCore GPU 色彩管线协同接管。
  - **120 Hz 刷新率模式（面板 + DPU TE + IOMFB）**：
    - `scripts/build-diting-ios27-bootimg.py` 通过 `--live-dtb` 写入校验后的 24 字节 `/chosen/diting-dsi-panel`；`board.toml` 启用 `vsync=120 vsync-fixed=1 diting_panel_hz=120 diting_backlight=1 diting_buttons=1`；`build-diting-ios27-drivers.py` 将 `NativeLegacyDisplay.cpp` 的 `sheng-vsync-fixed` 同步适配为 `vsync-fixed`；
    - 纯冷启动实测：`DITING_FB: vsync event=5 hz=120` 与 `vsync event=4 hz=120`（发布 1/120 s 固定间隔 `IOMFBDisplayRefresh`）；`display panel` 实测 `status=PASS stage=6 error=0 panel=2 id=c5/10/00 target_hz=120 before=0 after=2 batches=3`；`display` 实测 `INTF_1 TE_CFG=0018003a`、`AUTOREFRESH=80000001`，19.2 MHz `PANEL_TS` 实测硬件扫描节拍为 **121.79 Hz**（`FRAME 3028 -> 3168`, `Δt=1.1495 s`）。
  - **整机回归与主机测试**：严格预链接 `kernel.macho`（`94,076,928` 字节，0 未解析 / 0 存根）；主机单元测试 `test_diting_backlight.py`、`test_diting_panel.py`、`test_diting_display.py`、`test_diting_display_refresh.py`、`test_diting_ios27.py` **31/31 PASS**；纯冷启动 10 秒枚举 `05ac:1881` USB，`ADSP stage=13`、`Battery stage=4`（`66%`, `4006 mV`）、`QCA6490` `en0` 扫描 29 个网络且 `wifid` 绑定正常、`GPU status=PASS`、`offers=135 comp=135 fail=0 lit=1024/1024`。物理按键手感与锁屏/亮屏、亮度滑块视觉变化、夜览/屏幕观感与 120fps 视觉流畅度待用户实机交互确认。
- **桌面过曝修复、软件 `IOAESAccelerator` 与 Wi-Fi 深度诊断（进行中，临时镜像 `boot-ios27-gpu-wifi27-5-112`，详见[2026-11-24 交接文档](交接-2026-11-24-ios27-24B156-WiFi诊断与自动测试进度.md)）**：
  - `board.toml` 启用 `sheng-fb-half=2` 修复 16-bit half-float framebuffer 桌面过曝发白；
  - 新增软件 `IOAESAccelerator`（`drivers/DitingCrypto/`、`configs/software-aes-24B156.json`，预链接加入 `com.apple.iokit.IOCryptoAcceleratorFamily`），实机确认 `WIFI_AES state=1 kat=1 reg=1 done=3` 且 `WIFI_IOKIT busy=0 leaves=none`；
  - 补齐 iOS 27 Wi-Fi 540 字节关联事件与协议结构体（`patches/xnu-ios27/ios27-wifi-association-event.patch`、`drivers/DitingWifi/DitingWifiProtocol27.h`、`tests/test_diting_wifi_ios27.py`），并建立 USB HID 自动操控（`hid tap`/`hid swipe`）、实机截屏（`scripts/diting-screen-capture.py`）与 Wi-Fi 自动校验脚本（`scripts/test-diting-ios27-wifi.py`）；
  - 实机 `wifi27-5` 已确认 QCA6490 扫网（48 个真实热点）、`CoreWiFi` 只读查询与 `PF_SYSTEM` 事件唤醒均正常且零沙盒拒绝，同时定位到设置页无列表/无法开关的直接原因为用户态 `wifid` 内两处线程阻塞：（1）主管理线程 `th#1` 在 `SCPreferencesLock` 写入 PID 锁文件时阻塞于内核 `VNOP_WRITE`；（2）`th#3` 在 `libsystem_trace` 同步发送 Mach 消息（ID `11601`）时遇到 5/5 满端口队列并持锁阻塞 `th#4`/`th#5`/`th#6`。`boot_b` 仍保留已验证的 `btnbl120-3`，待上述阻塞修复并完成全量回归后再持久化刷写。

本机工作目录：`diting-work/build/services227/ios27-24B156`；编译器：`diting-work/tools/llvm-ios27`。
原始还原包、解密镜像、ABI 数据和构建产物只保存在本地。
