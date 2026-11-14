# UniXNU-diting

Redmi K50 至尊版（`diting` / Qualcomm SM8475）的 UniXNU 格式 2 设备仓库。目标系统为
**iPadOS 18.7.10（22H374）/ Darwin 24.6.0**，系统输入为 iPad7,11 / J171AP。

- 核心构建工具：[UniXNU](https://github.com/UniXNU/UniXNU)
- 固件：仓库内 `platform-firmware/ditingPkg` 的 FDF 内嵌 `sm8475-xiaomi-diting.dtb`；外部 `uefi-diting` 开发树只作参考，其旧 FDF 可能仍内嵌 Nothing Phone (2) 的 DTB
- 官方说明：[仓库迁移](https://unixnu.com/zh/porting/migration)、[设备仓库](https://unixnu.com/zh/porting/device-repository)、
  [驱动适配](https://unixnu.com/zh/porting/drivers)、[启动验证顺序](https://unixnu.com/zh/porting/bring-up)、
  [日志抓取](https://unixnu.com/zh/porting/log-capture)
- 本设备：[格式 2 配置](migration/format2/README.zh.md)、[系统迁移](migration/ipados18/README.zh.md)、
  [结构化移植记录](porting.toml)、[AGENTS.md](AGENTS.md)

## 当前状态（2026-11-14）

**services186 已进入系统，用户确认触屏点按、滑动及熄屏后亮屏触控恢复正常。** 当前持久 boot_b 和最近临时启动镜像
均为 services186，保持八核；本轮没有更新 userdata。services185 镜像保留作回退。新固件与 DarwinBoot24 使用本机
实测 RAM banks，WLAN 普通供电、BDF 功放选择及共享 RPMh/AOP 交易按厂商契约修正。
长期稳定性、Wi-Fi 联网、音频和 Metal 功能仍需本轮实机验收。

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
- 当前停点：准备用户态的 `launchd_cache_loader` 在 restore 模式下以
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

## 源码边界

仓库只包含板级源码、补丁、配置和脚本。IPSW、Apple 私有二进制、固件、认证数据、userdata 和构建
产物不进入仓库，由脚本在本机获取或生成。设备级代码采用 BSD-2-Clause-Patent；导入组件保留原始
许可证及 SPDX 标记，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 免责声明与商标

- 本项目是非官方、独立的硬件适配研究，与 Apple Inc.、小米、高通无隶属、授权或背书关系；相关商标归各自所有者。
- 操作涉及 userdata 分区 LBA 划窗、自建 UEFI 引导与内核替换，可能造成数据丢失或设备无法启动；请先备份，风险由使用者自行承担。
- 仓库不包含、也不分发 Apple 还原镜像、厂商固件或提取后的二进制；这些内容由使用者在自有设备或本地环境获取。
