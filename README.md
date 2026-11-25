# UniXNU-diting

**简体中文** | [English](README_EN.md)

Redmi K50 至尊版（`diting` / Qualcomm SM8475）的独立 UniXNU 格式 2 设备移植仓库（最初衍生自 [UniXNU](https://github.com/UniXNU/UniXNU) 与社区 SM8475 Pong 工作 [Chengdong-Yu/UniXNU-pong](https://github.com/Chengdong-Yu/UniXNU-pong)）。默认目标系统为
**iOS 27.2.1（24B156）/ arm64e**（系统输入为 iPhone18,2 / V54AP，公开源码基线 XNU 12377.121.6 / Darwin 25.5.0）。
精确还原包选择、公开源码基线、隔离工具链与构建入口见 [iOS 27.2.1 适配记录](migration/ios27/README.zh.md)。

历史 **iPadOS 18.7.10（22H374）/ Darwin 24.6.0**（iPad7,11 / J171AP）目标继续保留在 `board.toml` 的
`[os_overrides."ipados-18.7.10-22H374"]` 中，可通过 `--os ipados-18.7.10-22H374` 选择。

- 核心构建工具：[UniXNU](https://github.com/UniXNU/UniXNU)
- 固件：仓库内 `platform-firmware/ditingPkg` 的 FDF 内嵌 `sm8475-xiaomi-diting.dtb`；外部 `uefi-diting` 开发树只作参考，其旧 FDF 可能仍内嵌 Nothing Phone (2) 的 DTB
- 官方说明：[仓库迁移](https://unixnu.com/zh/porting/migration)、[设备仓库](https://unixnu.com/zh/porting/device-repository)、
  [驱动适配](https://unixnu.com/zh/porting/drivers)、[启动验证顺序](https://unixnu.com/zh/porting/bring-up)、
  [日志抓取](https://unixnu.com/zh/porting/log-capture)
- 本设备：[格式 2 配置](migration/format2/README.zh.md)、[系统迁移](migration/ipados18/README.zh.md)、
  [结构化移植记录](porting.toml)、[AGENTS.md](AGENTS.md)

2026-11-17 停止状态：用户认为当前性能“已经很不错”，要求暂停GPU优化并提交、推送。
手机保留临时运行services213，完整八核构建、链接、打包、GMU/CP与匹配USB均PASS，持久boot_b仍为198。
212的142项颜色矩阵通过，用户确认正常界面“闪烁已消失”；地图和全部颜色尚未完整验收。
213增加实际render-mode计数并使用gmem,nocb，一次自动UI功能3/4 PASS、设置前台前提FAIL，
物理120fps仍UNQUALIFIED。无活动测试或后台构建/采集。最新源码、证据和暂停约束见
[services213任务交接](migration/ipados18/交接-2026-11-17-services213-GPU优化暂停.md)。

## USB GPU自动化历史状态（2026-11-15）

services198已完成完整内核、八核打包和部署，GPU/UI安装均stage6/error0。通过USB自动运行
Vulkan/Metal固定负载、桌面滑动、应用打开/关闭及多任务手势，采集framebuffer和内核trace，
生成JSON、Markdown与JUnit评估；无需人工操作手机。

一次完整矩阵已执行97项，固定颜色60项、GPU负载18组通过；三类系统动画仍FAIL，校准的
panel VSYNC时间戳节奏约60.489Hz。目标保持 **120 fps / 8.333 ms**，物理新合成帧和其他系统颜色
仍未认证。27,085条trace的missing/skipped/kernel drops/recovery均0。自动化体系的成功执行
不能作为GPU性能达标的证据。

```bash
python3 scripts/run-diting-gpu-tests.py --profile all --policies auto sysmem gmem \
  --output-dir /home/dddqmmx/UniXNU/diting-work/logs/diting/services198/automatic-next-01
```

输出目录必须为新目录；相同实现已有完整证据时先离线分析，不重复实机测试。主机先完成整批可预见修复与检查，
下一候选再运行一次矩阵。入口和验收边界见 [USB GPU自动化](migration/ipados18/services197-gpu-automation.md)。

## 亮度与图标历史状态（2026-11-15）

当时持久 boot_b 与运行内核为 services193 的 `ui-brightness-20261115-06`，保持八核、120 Hz请求和
services182的33,792字节loader。第四版用户确认“亮度正常图标不显示”；第六版实际设置BKS事务的
低、中、高亮度及恢复原值均得到正确DCS回读，failures=0。亮度通过；图标第四版视觉失败，新的缓存读取修复
已刷入第六版，仍待打开设置后的图标日志和可见画面验收。熄屏恢复本轮未验收。

UI安装stage6/error0，设备已从只读Recovery回到正常系统。当时GPU安装stage1/error17/files-written0，
后续已由services198的6/0安装状态替代，120 fps性能仍未通过。此低优先级任务使用私有源码/缓存、nice19/ionice3和单编译任务，没有改动GPU工作源码或
共享XNU缓存。实现、设备证据和未完成项见 [services193亮度与设置图标](migration/ipados18/services193-ui-brightness.md)。

## USB与GPU历史状态（截至2026-11-14）

**services192已打通系统内USB，GPU120 fps性能仍FAIL。** services192完整内核、八核打包、
稀疏写 boot_b 与临时启动完成，实机枚举 `ffff:0128 Diting XNU console`（`/dev/ttyACM0`），完整
`status` 与两份 `gpu` 时钟快照通过。services190/191 已实测实时日志、有界 RAM 读取与命令回 Fastboot；
192 本次未重复重启。原生 Mux/Finder、USB 插拔及长期稳定尚未验收。

目标为 **120 fps / 8.333 ms**。services191 用户反馈“还是极其卡顿”，连续 USB trace 中提交至退役 p95
为17.576 ms、CP 执行 p95 为15.472 ms；192 增加只读时钟诊断，尚无新的 UI、触控和流畅度验收。
120 Hz 请求与软件通知已核实，物理面板帧率仍未测得，不能将正常启动、频率投票或 GPU 拷贝完成视为性能通过。

保持 services186 ST61Y、services182 实测 RAM/FD/33,792 字节 loader，以及 services187 实际 GPU runtime；
Data/SystemOS 保留。最新 ADSP stage13/error0、SCM0:0、PMIC_online1，电池 stage4/percent100/flags0x7；
界面电量、插拔和 PCM 播放尚未验收。源码含大量未提交改动，独立电池/ADSP 工作必须保留。
完整证据、USB 命令、构建入口和下一步见
[services192 历史交接](migration/ipados18/交接-2026-11-14-services192-USB已打通-GPU120fps未达标.md)。

触控历史基线：services186 已进入系统，用户确认点按、滑动及熄屏后亮屏触控恢复正常。
本机 Recovery 已确认触控器为 **ST61Y**；旧驱动使用 Goodix 协议。
[services186](migration/ipados18/services186-st61y-touch.md) 接入真实协议、供电与共享时钟投票，
主机测试、完整内核链接与八核打包通过，基本触控及亮屏恢复已通过实机观察；双指手势尚未测试。

见 [services186 修复与验收记录](migration/ipados18/services186-st61y-touch.md) 和
[任务交接入口](migration/ipados18/交接-2026-11-14-services186-启动与触控正常.md)。

### 历史状态（2026-11-11）

- bring-up 第 6 阶段：UFS 三窗口（OS / SystemOS / Data）已发布，根卷按 UUID 匹配并挂载，
  `launchd` 已实机启动。
- 已验证的设备布局（userdata `0x2F8A00..0x3ADF3FA`，4 KiB LBA）：
  OS `0x100000`、SystemOS `0x198100000`、Data `0x285000000`，SHNGROOT v1 清单；
  三个卷 UUID 与内核内嵌的 `XNU_OWNER_*_VOLUME_UUID` 一致。
- 早期停点：准备用户态的 `launchd_cache_loader` 在 restore 模式下以
  `Failed to verify code directory` 退出。按 `migration/ipados18/services.md`（services9）的既有结论，
  启动参数需要 `launchd_unsecure_cache=1`；该参数已写入 `board.toml` 的 `kernel_args`。
- 固件从 `0xA7000000` 读取 console，services182 另补 pmsg 导出，再用 Advanced Logger
  扫传。services183/184 已从 OEM minidump 取得真实 XNU 日志；普通重启的 pstore 保留
  对照仍失败，空 pstore 或缺失记录不能证明没有 panic。

## 组件与来源

`drivers/` 保留社区 Diting SM8475 实现（A730 / QCA6490 / SM8475 UFS 等）与来源声明，板级
源码已收编进本仓库。组件 ID 保持 `DitingPlatform24` 以保留来源、ABI 与验证记录；不要把它标成上游
A740/WCN7850 实现。Diting 的板级配置在 `board.toml`、`dts/diting.dts` 和 `drivers/`。

## 构建

需要完整 Xcode（含 iPhoneOS SDK）、Python 3.12、dtc、匹配的 22H374 restore/ABI 输入和板级私有
固件。源版本由 `feeds.toml`、`unixnu.lock` 和迁移记录固定；克隆时加 `--recurse-submodules`。

CI 使用 `.github/workflows/package.yml`（仅 `workflow_dispatch`），在 macOS runner 上构建
kernel / payload / boot image，产物为 `diting-package` artifact。三个卷 UUID 是内核常量，换 userdata
必须重编内核；占位 UUID 的构建只能用于编译验证，不能上机。

Linux 也可以复用 Sirius 使用的 UniXNU Linux 工具链，本轮已在 Arch Linux 本地编译完整
Darwin 24 内核、板级驱动、DarwinBoot24，并完成预链接和 boot image 打包。
AEA 解密、APFS 用户态卷写入仍使用现有 macOS `diting-userdata.yml` workflow。
命令及输入要求见 [services169 本地构建记录](migration/ipados18/services169-linux-build.md)。

## 来源与致谢

本仓库作为独立设备移植项目维护，完整保留 Git 提交历史、原始版权声明与各组件许可证。项目建立在以下上游与社区工作之上，特此致谢：

- [UniXNU](https://github.com/UniXNU/UniXNU)：核心 UEFI/XNU 构建框架、ARM64 支持、OS 配置与通用驱动组件（`BSD-2-Clause-Patent`）。
- [Chengdong-Yu/UniXNU-pong](https://github.com/Chengdong-Yu/UniXNU-pong)（`Chengdong-Yu/pong-*`、[Chengdong-Yu/uefi-pong](https://github.com/Chengdong-Yu/uefi-pong)）：社区 SM8475 / Pong 移植基础，本仓库收编的 `DitingPlatform24` 驱动与 SM8475 SoC 种子最初衍生自此。
- [57UU/UniXNU-sirius](https://github.com/57UU/UniXNU-sirius)：Linux 交叉编译工具链流程参考。

## 源码边界与许可证

仓库只包含板级源码、补丁、配置和脚本。IPSW、Apple 私有二进制、固件、认证数据、userdata 和构建
产物不进入仓库，由脚本在本机获取或生成。设备级代码采用 [BSD-2-Clause-Patent](LICENSE)；导入组件保留原始
许可证、版权声明及 SPDX 标记，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 免责声明与商标

- 本项目是非官方、独立的硬件适配研究，与 Apple Inc.、小米、高通无隶属、授权或背书关系；相关商标归各自所有者。
- 操作涉及 userdata 分区 LBA 划窗、自建 UEFI 引导与内核替换，可能造成数据丢失或设备无法启动；请先备份，风险由使用者自行承担。
- 仓库不包含、也不分发 Apple 还原镜像、厂商固件或提取后的二进制；这些内容由使用者在自有设备或本地环境获取。
