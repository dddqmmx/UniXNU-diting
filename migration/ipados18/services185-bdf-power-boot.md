# services185：进入系统，触屏不工作

日期：2026-11-14（Asia/Shanghai）。设备 Redmi K50 Ultra / diting / SM8475；
iPadOS 18.7.10 / 22H374 / Darwin 24.6.0，八核。

**用户确认“进系统了”，随后报告触屏不工作，并确认画面仍更新、实体按键有反应。**
本轮部署时持久 boot_b 与临时启动镜像均为 services185，后已被 services186 替换。进入系统已通过实机观察，
触屏未通过；长期稳定性、Wi-Fi 联网、PCM 音频和 Metal 功能尚未完成本轮验收。
用户要求提交并推送本轮更改和文档状态。

本轮源码归档提交：`97bcaf9aeb87d805c068e95b7128c05120aa8005`。

后续触屏修正见 [services186](services186-st61y-touch.md)：本机 Recovery 确认 ST61Y，
旧 Goodix 协议不匹配。services186 现已由用户确认进系统、点按与滑动正常，熄屏后亮屏触控恢复正常；双指手势尚未测试。services185 历史观察保留。

## 供电根因与修正

[services184 取证](services184-rpmh-minidump.md) 的三条同轮 pmsg 均停在提前直接使能
smpe3 后。Nothing CNSS2 的普通启动 regulator list **不包含 vdd-wlan-ipa / smpe3**；
它在 BDF 下载完成后，根据可选 `host_bdf_data` 决定功放路径。

参考：`NothingOSS/android_kernel_msm-5.10_nothing_sm8475`，commit
`c402fcdb3a1523d10b15ef1123bced45456f7592`，CNSS2 `power.c/qmi.c` 与
`drivers/net/wireless/cnss_utils/wlan_firmware_service_v01.{c,h}`。

- 普通电源按厂商 consumer list 排序，从所选 live DT 重新生成电压范围和 provider。
  smpe3 由 BDF/AOP 单独管理；不再枚举所有 supply 后提前直接使能。
- 未请求模式的电源不再统一写 mode=7；仅显式 `qcom,init-mode` 按实际 PMIC 类型映射。
- BDF 响应加入可选 TLV `0x10`（u64 `host_bdf_data`），最大响应长度7→18字节，
  每段响应清零，在最后成功的 **ELF/BIN** BDF 响应后配置功放。
- bit0=1 为外置功放，不配置 S3E；bit0=0 为内置功放，经 AOP 发送
  `{class: wlan_pdc, ss: rf, res: s3e.e, upval: 1}`，固定64字节、补零。
  TLV 缺失保留默认状态，不猜功放类型。
- WLAN 和 ADSP 共用 AOSS QMP owner；未消费邮箱超时后保留所有权，AOP 真实清零
  才处理迟到完成与下一消息，不伪造 ACK。
- 保留 services184 的共享 active TCS0–2 交易、完成判断和超时所有权修正。
- 两套构建入口均修正 POWER/BACKLIGHT 独立归档选择与链接条件；关键 pmsg 上限
  增为有界512条，计数采用原子操作。

本轮没有新增 XNU patch，复用 services182 新 FD/template、33,792字节 DarwinBoot24
与实测 RAM DTB，保持八核、Tools 入口和既有功能参数。不更改电压、不关闭硬件，
不放宽 CPU、DMA、供电或服务的真实成功条件。

## 验证与部署

- QMP host 测试8个场景通过：握手、64/100字节补零、超时所有权、迟到完成、
  继承占用、无效参数和变化 descriptor。
- BDF policy C 测试通过：缺失、外置、内置及无关高位。
- 本机 live DT power contract 生成通过；两套 Python/bash 语法与差异格式检查通过。
- 提交前补验5类异常供电输入（未知 supply、非零 load、越界电压、其他 PCIe owner、
  不支持的模式）均拒绝；DBGC 两种 cursor、环绕、截短与损坏 header 验证通过；
  capture 的完整读取、短读取、远端非零退出3种场景验证完整文件与 `.partial` 状态。
- 工作流预检0问题；38个变更 Python 文件、两套 shell 入口和当前文档链接检查通过。
  核心 porting checker 默认目录仍不认识社区 DitingPlatform24；使用其既有
  `allow_custom_components=True` 校验同份记录通过，未改核心 catalog。
- 完整原生驱动编译、内核链接、八核预链接与打包通过；实际生成的 ath11k QMI
  包含 TLV 和最终 ELF/BIN 回调，外部导入满足归档链接约束。
- 07:42（部署结束 `2026-11-13T23:42:37.659688+00:00`）：diting / unlocked=yes /
  current-slot=b / boot_b=192 MiB 检查通过。稀疏两块发送写入全 OKAY，4.670秒；
  临时启动 OKAY，2.459秒。镜像101,183,488字节。
- 用户实机观察：进入系统；触屏不工作；画面仍更新、实体按键有反应。

进入系统支持本批修正有效，不能单凭该结果证明每一项都是独立重启根因。
尚未取回 services185 的 BDF 功放响应或网络功能日志，不记录虚构的 Wi-Fi 成功。
用户要求跳过镜像哈希、镜像审计和设备镜像回读，本轮保留 `--skip-image-verification`。

## 触屏问题与后续（历史时点）

services185 的 Goodix 生产者由 `bsd/dev/diting_goodix.c` 经 `/dev/diting-touch` 向用户态
`userland/touch.c` 送真实事件。旧轮 kmsg 已记录时钟未打开、固件版本全零和初始化
失败，优先核对本机 QUP0 SE4 时钟、GPIO/SPI、供电共享所有权及实际报告状态。
这些旧日志不等于已取得 services185 的触屏根因；实体按键有效也不证明触屏服务已发布。

当时先保持系统运行并做主机侧源码定位与可用的 USB 读取。后续 services186 已完成
ST61Y 修正和基本触控实测；当前入口见其独立记录，保留本轮历史观察。

## 本地入口

- 构建：`diting-work/tmp/services185/build-local.sh`；本轮源码基线 `source-before/`。
- 源码 overlay：`diting-work/build/services185/runtime-source/`。
- 内核/打包：`diting-work/build/services185/{kernel,boot}/`；日志 `build-local.log/package.log`。
- 镜像：`diting-work/build/services185/boot/diting-xnu-initial.img`。
- 部署：`diting-work/logs/diting/services185/install-boot.{json,log}`。
- 用户观察：`diting-work/logs/diting/services185/hardware-observation.json`，独立于部署回执。

安装回执是在用户观察前生成，`hardware_boot_confirmed=false` 保留其原始含义；
后续独立观察记录补充确认，不回写原始传输证据。构建缓存仍在
`diting-work/build/services169/tree/.cache/xnu-work`，本轮完成时对应 services185，**现在对应 services186**。
本轮未写 userdata，取证 Recovery 未挂载 data/metadata/cache；所有私有输入、
原始设备日志及产物留在本地。
