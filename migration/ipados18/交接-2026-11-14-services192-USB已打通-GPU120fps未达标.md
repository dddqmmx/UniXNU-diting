# UniXNU-diting 交接：services192 USB 已打通，GPU 120 fps 未达标

2026-11-16 最新交接：[services208 GPU显示与性能未达标](交接-2026-11-16-services208-GPU显示与性能未达标.md)。手机最后确认临时运行services208，持久boot_b保持services198；刷新通知p95改善到8.348ms，纯色蓝色对照稳定且已恢复，但正常颜色、闪烁和120fps仍未解决。services209仅完成源码/缓存与主机检查，尚未构建上机。用户要求停止试验，当前仅整理文档。以下services198和services192段落均保留其历史时点，不是当前部署。

最新接续日期：2026-11-15（Asia/Shanghai）。当前入口为
[services197/198 USB GPU自动化与评估](services197-gpu-automation.md)。

**当前持久boot_b与匹配运行内核为services198，已完成完整构建、部署和一次97项USB自动矩阵。**
GPU安装stage6/error0，UI第六版保持stage6/error0。固定颜色60项、GPU负载18组通过；桌面、设置与
多任务动画评估均FAIL，校准panel VSYNC时间戳节奏60.489Hz，120fps/8.333ms目标未通过。
27,085条trace的missing/skipped/kernel drops/recovery均0；物理新合成帧尚未认证。

接续入口为 `python3 scripts/run-diting-gpu-tests.py --profile all --policies auto sysmem gmem --output-dir <新目录>`。
报告保存在 `diting-work/logs/diting/services198/automatic-01`。用户现要求通过USB自动执行和评估，
不再依赖手动滑动或开关应用；相同实现先分析已有原始记录，整批可预见修复与主机检查完成后才测试下一候选。
06:13Z只读复核匹配services198，GPU/UI安装6/0、daemon16/error0、末job3/state3/quiescing0，无活动worker。
保持八核、Data/SystemOS、UI第六版与独立电池/ADSP修改，继续跳过镜像哈希、审计及设备镜像回读。

以下为 **2026-11-14 services192历史交接**，其设备、构建、缓存与下一步描述仅代表当时，不用于当前部署。
[services186 原交接](交接-2026-11-14-services186-启动与触控正常.md) 保留启动、触控及更早故障的历史证据。

**当前持久 boot_b 与最近临时启动均为 services192。系统内 USB 日志、命令及有界 RAM 诊断链路已打通；
GPU 性能仍为 FAIL，目标是 120 fps，每帧预算 8.333 ms。** services191 最新用户反馈为“还是极其卡顿”；
services192 已取得真实 USB/status/GPU 时钟回复，尚无新的界面、触控和流畅度验收。

用户现要求整理文档交接，本轮不再实施新的 GPU 实验。交接时没有运行中的构建、打包、刷机或采集任务，
没有活动 goal 或代理任务。手机保留在 services192 XNU 系统，下一轮恢复实现时先使用已打通的 USB 链路取证。

## 当前设备与部署

| 项目 | 已记录状态 |
| --- | --- |
| 设备 | Redmi K50 Ultra / diting / SM8475，序列号 `1d4eb066` |
| 系统 | iPadOS 18.7.10 / 22H374 / Darwin 24.6.0，iPad7,11 / J171AP |
| 最近部署检查 | product=diting、unlocked=yes、current-slot=b |
| boot_b 容量 | `0xC000000`，201,326,592 字节（192 MiB） |
| 持久 boot_b / 最近启动 | services192 / 同镜像 `fastboot boot` 临时启动 |
| 部署完成 | `2026-11-14T14:05:21.403438Z`，北京时间 22:05 |
| 传输 | 稀疏 boot_b 两块均 OKAY，共 4.467 秒；临时启动 OKAY，共 2.554 秒 |
| 启动镜像 | `diting-work/build/services192/boot/diting-xnu-initial.img`，101,183,488 字节 |
| 内核 / CPU / loader | 原生内核 63,678,600 字节；八核；DarwinBoot24 33,792 字节 |
| 实际启动参数 | 1006 ASCII 字节，含 `fb_hz=120`、`fb_beat=0`、`diting_gpu_trace=2` |
| 最新 USB 证据 | `ffff:0128 Diting XNU console`，`/dev/ttyACM0`；完整 status 与两份 gpu 回复 |
| USB 取证时间 | `2026-11-14T14:15:49Z` 至 `14:15:51Z`，北京时间 22:15 |
| 实机 ADSP / PMIC | ADSP stage13/error0、SCM0:0、PMIC_online1 |
| 电池 | stage4、percent100、flags0x7；界面电量与插拔尚未验收 |
| 用户态 | 复用 services187 GPU runtime；本轮未替换 Data/SystemOS 或重写 userdata 镜像 |

services192 原始安装回执的 `hardware_boot_confirmed=false`、USB 等 `UNTESTED` 字段记录的是回执生成时点，
必须保留。后续 USB 实测来自独立的 `handoff-usb-check.json`、原始回复及 `clock-observation.json`，
不能覆盖原回执，也不能将后续 USB 成功扩展为 services192 UI 或性能成功。

services186 用户已确认进入系统、点按、滑动及熄屏后亮屏触控恢复正常，services187/188/191 也有可操作界面观察。
基本触控的历史 PASS 与当前 GPU 性能 FAIL 分开记录；双指手势、长期稳定、当前 Wi-Fi 联网、PCM 播放、
Mux/Finder 及 USB 插拔仍缺本轮验收。

## services186–192 已完成工作

| 轮次 | 实现与设备证据 | 验收边界 |
| --- | --- | --- |
| services186 | ST61Y 协议、供电、GENI 与共享 GCC 投票修复；用户确认基本触控和亮屏恢复 | 触控历史 PASS，外围与长期稳定未全面验收 |
| services187 | Linux 构建并签名真实 Turnip/Metal runtime，guest APFS 在 launchd 前安装；实际 submit/retire/GPU copy 可见 | 用户确认界面可操作，滑动和动画仍卡顿 |
| services188 | trace=2，保留内核关键事件，取消前端详细追踪与 SHENG_DIAG 开销 | 用户确认进入系统、触控正常，仍明显卡顿 |
| services189 | 取消额外软件节拍等待，保留真实完成同步 | 用户描述动画瞬间到终态，没有连贯动画 |
| services190 | DWC3/PHY 独立接管、CDC 分帧与主机握手；ADSP 实际签名固件内嵌及真实 PMIC 通道 | 实时日志、help/status、64 字节 RAM、坏地址拒绝及命令回 Fastboot PASS；用户仍见动画跳到终态 |
| services191 | 补齐 22H374 六项刷新属性；增加有界游标 GPU trace 与单 CDC 会话连续采集 | 构建、USB/RAM/trace PASS；用户反馈“还是极其卡顿”，性能 FAIL |
| services192 | 独立 gpu 命令，增加只读 GPUCC、GMU 与已有 RBBM 计数器快照 | 构建、部署、USB/status/两份 gpu 回复 PASS；UI 未重新验收，120 fps 未达标 |

详细历史入口：[services186 触控](services186-st61y-touch.md)、
[services187 runtime](services187-gpu-runtime.md)、[services188/189 trace](services188-gpu-light-trace.md)、
[services190 USB](services190-usb-diagnostics.md)、[独立电池任务](services188-battery-recognition.md)。

**先前把 services190 描述为“60 Hz 对照”的说法已过时。** 匹配运行内核的 `gRefreshHz` 经真实 USB RAM 读取为
`78 00 00 00`，即 120；services191 同样已核实请求值为 120。请求值、软件通知、GPU 拷贝完成与物理面板刷新率
是不同层次的证据，不能由 `fb_hz=120` 宣称实测 120 fps。

## services191 性能证据与限制

主机连续采集 25.116 秒，发送 2,377 条有界命令，取得 16,792 条事件。
采集显式跳过 23,626 个早于保留窗口的序号；已检查范围缺失位置为 0，内核报告 drops 为 0。
保留事件覆盖系统 uptime 125.666–224.604 秒，包含采集开始前的 ring 数据；这不是完整五分钟日志，
也不能把“范围内无缺失”写成所有历史事件无损。

| 保留事件统计 | 实测值 | 说明 |
| --- | --- | --- |
| 提交开始 → CPU 观察到退役 p95 | 17.576 ms | 已超过 8.333 ms 帧预算；提交不是呈现帧 |
| CP 执行 p95 | 15.471562 ms | 按实际 19.2 MHz CP timestamp ticks 换算 |
| GPU offer → copy 完成 p95 | 16.458 ms | 是显示拷贝链路，不是物理 panel latch |
| 相邻 copy 完成间隔 p95 | 47.962 ms | 只统计间隔不超过 250 ms 的样本 |
| 确认 GPU copy 完成 | 1,211 次 | 未确认/不成功 latch 记录为 0；此字段不代表物理 latch |
| GPU recovery 事件 | 0 | 只覆盖保留事件，不能证明全程无故障 |
| 软件刷新通知 | 3,051 次，全部 send 成功，目标120 | send 成功不等于客户端已处理 |
| 软件通知 CPU 间隔 | p50 8.331 ms、p95 9.174 ms | 此处为全部相邻通知统计；不是屏幕帧间隔 |

长间隔可能属于静止、睡眠、停顿或未捕获阶段，不直接计为掉帧。物理面板 latch/fps、完整输入到显示延迟以及
前端详细耗时仍未测得。用户的真实界面反馈为 FAIL；解码器的 `hardware_performance_pass=false` 应继续保留。

按同轮 submit 的 handle 字段匹配退役样本：74-handle 组353个样本的 CP p95为16.457 ms，
1-handle 组1180个样本的 CP p95为0.807 ms。两组属于不同实际负载，提示继续定位复杂提交的成本，
不能将这项分组直接当成受控策略对比。

当前渲染 runtime 仍保留已用于颜色行为的 `TU_DEBUG=sysmem,nocb`。
GMEM 策略调整仅讨论过，尚未实现、构建或部署；没有 services193 候选。下一轮应比较复杂提交的 CP 成本与
sysmem 路径，结合实际时钟证据定位瓶颈，不能只靠修改目标 Hz 解决卡顿。

## services192 实现及真实时钟读数

新增 [DitingAdrenoClockSnapshot.h](../../drivers/DitingAdreno/DitingAdrenoClockSnapshot.h)，在
[DitingAdrenoPlatform.cpp](../../drivers/DitingAdreno/DitingAdrenoPlatform.cpp) 实现 `diting_gpu_snapshot_text`。
reader 仅在 CP 成功启动后通过 release/acquire 发布；GX 未实际供电时跳过依赖读取。
GPUCC 捕获最多 38 次寄存器读取，PLL/divider 在两次采样间改变则报告 `CHANGED`；`0x90a8` 是 branch，
不能当成 RCG divider。GMU 及既有 RBBM 计数器另作固定、有界读取，计数器使用最多三次 high/low/high 尝试。

[DitingUsbDevice.h](../../drivers/DitingUsb/DitingUsbDevice.h) 与
[diting-usb-command.py](../../scripts/diting-usb-command.py) 增加独立 `gpu` 命令；`status` 保持缓存快照。
新 reader 不写时钟、不改变频率投票、不选择新计数器、不改渲染策略。
[decode-gpu-trace.py](../../scripts/decode-gpu-trace.py) 默认比较目标改为120，并统计 event25 软件刷新通知。

此前验证：8 个时钟 guard 场景通过 ASan/UBSan，9 项 trace decoder 测试通过；完整内核与八核打包通过，
`_diting_gpu_snapshot_text` 为强符号，原生内核未解析符号为0。services192 复用 services187 归档和安装 ID
`0912509e-45dd-4e4b-8852-2683ee553258`，不需要更换用户态 runtime。

两份完整 `gpu` 回复的配置一致，关键原值如下：

```text
requested_hz=900000000 requested_DDR=11
GX=f8222800 GFX_CBCR=00000221 RDVM_CBCR=80000000
GMU_CFG=00100602 HUB_CFG=00100603
PLL0 MODE=e0000005 OPMODE=1 L=0044005d ALPHA=0000c000 USER=7
PLL1 MODE=00000104 OPMODE=0 L=44440034 ALPHA=00001555 USER=1
GMU PERF=0 BW=0 RETURN=0
RBBM_CONTROL=1 RBBM_SELECT=0
sample1 uptime_us=640344800 RBBM0=6fd72656
sample2 uptime_us=640549675 RBBM0=7ab75e88
```

按固定 NothingOSS SM8475 GPUCC/PLL/RCG 源码解码，PLL0 programmed output 为 1.8 GHz，
GMU RCG 为200 MHz，HUB RCG 为150 MHz。PLL 输出仍需经过 GX 路径，不能将1.8 GHz写成实际GX频率。
两次 RBBM 增量182,466,610，header uptime 增量204,875 µs，对应约0.891 GHz计数速率。
header 与 counter 并非同一瞬间采样，且计数器受 gate 行为影响；这只能用于范围判断，不能标为精确物理900 MHz。
`requested_hz=900000000` 本身只是频率投票，以上读数均不证明120 fps。

参考版本固定为 NothingOSS `c402fcdb3a1523d10b15ef1123bced45456f7592`。
本机 `diting-work/tmp/services191/reference/` 保留 `gpucc-waipio.c`、`clk-alpha-pll.c`、`clk-rcg2.c`、
`adreno_gen7_perfcounter.c` 与来源 JSON。下一轮核对厂商 GMU/HUB consumer 时钟策略时使用这些固定输入。

## 已打通的 USB 操作入口

以下命令在 `/home/dddqmmx/UniXNU/UniXNU-diting` 执行。
主机脚本自动选择唯一 Diting CDC 端口；需要时显式加 `--port /dev/ttyACM0`。
同一时间仅保留一个占用串口的采集/命令进程，避免破坏 raw-mode/DTR/队列握手。

```bash
python3 scripts/diting-usb-command.py status
python3 scripts/diting-usb-command.py gpu
python3 scripts/capture-diting-gpu-live.py --seconds 25 --output-dir /home/dddqmmx/UniXNU/diting-work/logs/diting/services192/live-next-01
python3 scripts/decode-gpu-trace.py /home/dddqmmx/UniXNU/diting-work/logs/diting/services192/live-next-01/trace.bin --stream --refresh-hz 120 --output /home/dddqmmx/UniXNU/diting-work/logs/diting/services192/live-next-01/decoded-120fps.json
```

采集输出目录必须是新目录；已存在时换编号，保留旧证据。`trace AFTER COUNT` 每次最多返回20条事件，
捕获报告同时记录 cursor、skipped、missing 与 kernel drops。验收前检查这些字段及命令 BEGIN/END 的完整性。

`read HEX-KVA BYTES` 提供1–512字节 managed kernel RAM 的有界 nofault 读取，拒绝低地址、越界与不可用范围。
只能用当前镜像的匹配符号定位 KVA；services190/191 的绝对地址不直接沿用到192。
需要硬件寄存器时通过板级受限 `gpu` reader，不能把 RAM API 当成任意物理 MMIO 入口。

下一轮恢复部署或 Recovery 取证时，已有命令回 Fastboot：

```bash
python3 scripts/diting-usb-command.py fastboot
fastboot -s 1d4eb066 devices
```

主机先收齐 accepted/END，再核对序列号、product、unlocked、slot和目标分区容量。
命令回 Fastboot 已在 services190 与 services191 实测通过；192 本次只复测 status/gpu，未为文档整理重复重启。
已有只读 Recovery 候选为 `diting-work/build/services171/recovery-log-reader.img`；USB CDC 为当前常规取证入口。
历史普通重启 pstore 保留仍不可靠，空 pstore 不证明没有 panic。Mux/Finder 验收不包含在本次 CDC PASS 内。

## 工作区、源码与本机证据

工作区为 `/home/dddqmmx/UniXNU`，设备仓库为其下 `UniXNU-diting`，核心工具仓库为其下 `UniXNU`。
设备仓库 HEAD 为 `bd21ea788120fc48037e4f5981f3893e8dbaa4c3`，有大量未提交改动；
核心 HEAD 为 `066c8d8315f83749d28bcae2faf3595718646be5`。
HEAD 不包含全部当前实现，必须连同工作区差异接续，不要 reset/clean，也不要覆盖独立电池/ADSP 修改。

以下路径均相对 `/home/dddqmmx/UniXNU`，原始设备日志、固件、restore输入和生成产物只留本机：

| 用途 | 本机入口 |
| --- | --- |
| 当前构建/设备摘要 | [services192 work-state](../../../diting-work/build/services192/work-state.json) |
| 原始部署回执 | [install-boot.json](../../../diting-work/logs/diting/services192/install-boot.json) |
| 主机检查与链接 | [host-checks.json](../../../diting-work/logs/diting/services192/host-checks.json)、[link-check.json](../../../diting-work/logs/diting/services192/link-check.json) |
| USB 后续实测 | [handoff-usb-check.json](../../../diting-work/logs/diting/services192/handoff-usb-check.json) |
| USB 原始回复 | [status](../../../diting-work/logs/diting/services192/handoff-status.raw.log)、[gpu1](../../../diting-work/logs/diting/services192/handoff-gpu-01.raw.log)、[gpu2](../../../diting-work/logs/diting/services192/handoff-gpu-02.raw.log) |
| 时钟解码与限制 | [clock-observation.json](../../../diting-work/logs/diting/services192/clock-observation.json) |
| 191 用户观察 | [hardware-observation.json](../../../diting-work/logs/diting/services191/hardware-observation.json) |
| 191 连续采集 | [capture.json](../../../diting-work/logs/diting/services191/live-01/capture.json)、[trace.bin](../../../diting-work/logs/diting/services191/live-01/trace.bin)、[decoded-120fps-with-refresh.json](../../../diting-work/logs/diting/services191/live-01/decoded-120fps-with-refresh.json) |
| 191 managed RAM | [snapshot.json](../../../diting-work/logs/diting/services191/gpu-memory-01/snapshot.json) |
| 192 完整构建/打包日志 | [build-20261114T135732Z.log](../../../diting-work/build/services192/build-20261114T135732Z.log)、[package-20261114T140128Z.log](../../../diting-work/build/services192/package-20261114T140128Z.log) |

构建入口保存在 `diting-work/tmp/services192/`：`build-local.sh`、`run-kernel-build.py`、
`package-local.sh`、`run-package.py`、`install-boot.py`。Linux 工具链为 `/home/dddqmmx/.unixnu/run.sh`。
当前完整 XNU 缓存仍位于 `diting-work/build/services169/tree/.cache/xnu-work`，**内容现在对应 services192，
不是 services169 原始对象**；192树也复用了既有cache/build及191输入。
`runtime-source` 是本轮构建 overlay，新候选必须同步实际被构建的源码、recipe和merged入口并独立编号。

`diting-work/tmp/services192/source-before/` 保存本轮实现前的相关191源码；
`handoff-source-before/` 保存本轮交接编辑前的四份文档。已有各轮产物和显式软件回退
`diting-work/build/services187/rollback/diting-services186-software.img` 均保留；回退候选本身未实机验收。
services182 的实测 RAM/FD/template/33,792字节loader、services186 ST61Y与services187实际runtime继续保持配对。

## 下一轮实现顺序与验收要求

1. 保持120 fps目标，先在192运行系统用USB取得新的界面观察及连续滑动、开关应用、多任务的有界trace。
   保存场景、采集窗口、原始回复与丢失统计；把用户视觉反馈与GPU/显示/软件通知各阶段数据对应。
2. 使用已保存的GPUCC/GMU/RBBM读数核对厂商GMU/HUB consumer策略，比较复杂提交与sysmem路径成本。
   当前尚未证实完整卡顿根因；任何时钟或GMEM策略变更需有匹配硬件契约和测量依据。
3. 每个新候选独立编号、更新实际构建overlay；同步源码与merged构建入口，完成适当主机检查、完整链接和打包，
   通过已授权的稀疏boot_b/临时boot部署，再经USB自主取证。保留Data/SystemOS和独立电池/ADSP工作。
4. 以8.333 ms预算检查各阶段尾延迟，并补物理呈现/面板时序证据及真实连续动画验收。
   构建PASS、120Hz请求、send成功、GPU copy完成或没有记录fault均不构成性能达标。

用户已授权构建、稀疏写boot_b、临时启动、Recovery取证和有界USB/RAM诊断，无需重复询问常规许可。
当前仅有Linux主机，不能假定macOS APFS写入环境可用；若需替换runtime，可接续187已验证的guest安装链。
继续使用 `--skip-image-verification`，跳过镜像哈希、镜像审计和设备镜像回读；
诊断RAM读取与镜像回读是不同用途。保持八核，不猜测性关闭硬件、改电压或放宽真实成功条件。
先读 [AGENTS.md](../../AGENTS.md)、相关驱动约束、官方文档及固定上游实现；
官方站点此前返回403，本机核心仓库 `UniXNU/docs/` 有已读取的对应文档可作参考。
