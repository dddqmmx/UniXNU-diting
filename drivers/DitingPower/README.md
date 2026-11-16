# DitingPower

> 当前临时运行213，保留已验证的面板识别/120Hz请求、DDR/LLCC请求与有界显示诊断。212正常界面闪烁已由用户确认消失；用户认为当前性能已很好并要求暂停GPU优化。物理120fps、实际DDR频率及持续带宽仍未认证，见[最新交接](../../migration/ipados18/交接-2026-11-17-services213-GPU优化暂停.md)。下方编号段落保留各候选原始时点。

services209 增加 `display fetch-swap MS`：沿用 services208 已实测恢复的控制器，在正常
framebuffer fetch 下临时使用相反的红蓝 UNPACK 顺序。保持 FORMAT、alpha 与 CONSTANT_COLOR
原值，不启用 solid fill、不写 framebuffer；范围1000–60000 ms，到期恢复并核对 flush。
此对照用于区分正常取色与渲染/扫描问题，默认不运行，不作为颜色修复结论。

services208 增加 USB 显式触发的 SSPP 纯色对照。默认不运行；只接受 blue/green/red 与
1000–60000 ms。控制器核对当前双 VIG/CTL0/TE 配置后，保存 FORMAT、UNPACK 和 CONSTANT_COLOR，
按固定 Xiaomi ABGR8888 契约设置常量色并一次 flush 两个 VIG。到期或 cancel 恢复六个原值，
只有 flush 退休且硬件帧计数前进后才报告 restored=1。独立回调每次做有界寄存器操作，
不在 USB 线程或 displayGate 内等待观察时长。电源关闭时不读写 DPU；恢复重试有上限，
配置所有权改变时报告失败并保留诊断，不覆盖新配置。原面板时序、背光与 GPU copy 保持。
主机恢复/超时/所有权检查通过；实机颜色与闪烁仍待确认。

services204 私有候选增加 `gpu bandwidth`：仅在空闲、无未消费中断及保留计数的 BWMON4/5 上，
各采样 12 ms，读取 1 ms 硬件窗口峰值，并核对原配置恢复。保持中断关闭及现有 CPU/GPU/DPU
投票，恢复失败后拒绝再次借用。64 KiB 计数单位来自本机 DT；峰值按固定 Xiaomi
`bimc-bwmon.c` 的非零 MAX 加一规则换算。它不能证明持续带宽上限或实际 DDR 频率。
主机协议、所有权、无数据和恢复故障检查通过；尚未进行本候选实机测量。

Diting 的电源、ADSP 与音频：PAS1 加载签名 ADSP 固件，GENI I2C 驱动功放，MI2S 输出到双功放，另有电池/充电器与面板背光。

## 适配硬件

- SM8475 APSS：SMEM `80900000/200000`、TCSR mutex3 `1f43000`、IPCC `ed18000` / INTID261、AOSS `c300000`；DRV2 active TCS0–2、sleep TCS3–4、wake TCS5–6、fast TCS7
- LPASS ADSP：PAS1、49 MiB 本机预留内存、SMP2P 与 GLINK
- 音频：GENI I2C `a94000`（`qupv3_se13_i2c_active`，GPIO48/49），TFA9873 双功放地址 `0x34/0x35`，复位 GPIO1/89，WCD938x，tertiary MI2S SD1
- 电池/充电器：按 Xiaomi SM8450 `qti_battery_charger.{h,c}` 的 battery 0 属性解码，通过 `IOPMPowerSource` 上报，不写充电限值
- 背光：Diting L12 OLED（1220×2712，42/02/0a 或 36/02/0b），面板 DCS `0x51` 写、`0x52` 回读，SM8475 DSI v2.6 @ `0x0ae94000`

services203 在同一亮度工作循环中加入经 DDIC 身份和档位回读确认的 L12 120 Hz 时序切换，失败有界恢复，未退休命令保留 FIFO 所有权。候选尚未上机；接口、固定参考和硬件结果边界见 [亮度与面板时序](README.backlight.md)。

## 仓库关系

Diting 专属实现。协议与服务结构参照 UniXNU-sheng 的 Qualcomm 实现（保留原 BSD-2-Clause-Patent 声明），板级参数来自 NothingOSS `android_kernel_msm-5.10_nothing_sm8475` 与 Diting 实机 DT。

## 说明

- `libDitingPower.a` 由 `scripts/build-diting-power-native.py` 生成，由主构建 force-load；`ArmDTPlatform` 在 CPU nubs 发布后调用电源入口。
- 背光细节另见 `README.backlight.md`。固件（`adsp.mbn`）不进 Git，用 `refresh_runtime --power-firmware DIR` 或 `diting_adsp-fw=/abs/dir` 提供。

构建、接口与组件目录见 [UniXNU 组件目录](https://unixnu.com/reference/components)，板级输入与编译入口在 [UniXNU-diting](https://github.com/Chengdong-Yu/UniXNU-pong)。

## 已接入的代码

- `DitingAdsp.cpp`：手机自身 signed ADSP 固件、PAS1、49 MiB 本机预留内存、SMP2P、GLINK、ready/fatal 通知和有界等待。
- 实际 SMEM `80900000/200000`、TCSR mutex3 `1f43000`、IPCC `ed18000` / INTID261、AOSS `c300000`。不使用 Sheng 的 SM8550 IPCC、固件或 PAS36 DTB。
- 通过 `gPhysBase/gPhysSize` 检查 DarwinBoot 实际 RAM 所有权；若覆盖 ADSP carveout，停止加载，不覆盖 XNU RAM。
- 电源、WLAN 与 GPU 通过 `DitingPlatformResources` 共用 APSS active TCS0–2 的完整交易锁。按真实命令完成和 idle 状态回收；超时保留所有权，不覆盖 sleep/wake/fast TCS。只保留启动 minima，不在 handover 时降掉共享电源投票。
- ADSP 与 WLAN 共用 AOSS QMP owner，分别发送补零的 100/64 字节消息。超时不清空未消费邮箱，AOP 真实清零后才处理迟到完成与后续请求。
- 触屏加入共享RPMh owner，ldoc2按本机provider请求1.8V/mode7；touch/WLAN/USB/audio共用
  GCC62008投票锁与回读。只有带HWCG的AHB分支允许idle halt；core/core2x/SE必须真实运行。
- `DitingBattery.cpp` / `ShengBattmgr.c`：保留 UniXNU 协议 API 和诊断字段编号，按本机 Xiaomi 属性表、单位和通知消息读取 battery 0；不读取双电量计扩展、不发送充电属性写入。
- `DitingAudio.cpp`：`adsp_apps`、APM speaker/playback graph、TFA9873 配置、独立 SID1801 DMA 和输出环；只有真实初始化完成才发布 `DitingAudio`。尚无真机播放成功记录。

## 板级音频约束

Diting 是 TFA98xx 双功放（I2C `a94000`，地址 `34/35`）及 WCD938x，不能加载 Sheng CS35L43 功放配置。实际 `pinctrl-0` 是 qupv3_se13_i2c_active，GPIO48/49、qup13 mux1、pull-up / 2mA；GPIO52/53 不属于这条总线。复位为 GPIO1/89；本机 DT 未设置 `reset-polarity`，Nothing 驱动默认高电平有效，必须 HIGH 10ms 后 LOW 10ms 释放。此前反向序列将功放一直保持在复位状态。PCM 为 tertiary MI2S SD1（本机 DT rx-lines=0x2）；SD0（tx-lines=0x1）属于反馈，不能用于播放输出。只应用本机容器中的易失性 speaker 配置，不运行校准或写工厂 MTP。仍需真机确认 LPASS 时钟、DMA 周期完成及匹配 Darwin24 的 HAL。CPU 调频/系统睡眠回调尚未接入；保留当前固件策略。

## 构建与输入

`scripts/build-diting-power-native.py` 产生 `libDitingPower.a`，由主构建脚本 force-load，`ArmDTPlatform` 在 CPU nubs 已发布后调用。电池 metaclass 延后注册，避免 prelink 静态初始化顺序依赖。

`CONFIG_XNU_DITING_POWER`独立编译和启动电源/ADSP/音频。打包器只在该开关启用时添加 `diting_adsp=2`、`diting_audio=2`。HAL由 `refresh_runtime --enable-audio-hal` 或 `refresh_ui --enable-audio-hal` 安装为现代 `.driver`；工厂提供真实接口，实际端点Ready后才枚举设备。内核仅提供control/output两个内存类型和输出start/stop，不创建capture环。本机 `adsp.mbn` 通过 `refresh_runtime --power-firmware DIR` 放入 `/diting/firmware/adsp.mbn`，也可用 `diting_adsp-fw=/absolute/directory` 选择显式目录；固件不放入Git。

RPMh和APPSSMMU lease由独立 `DitingPlatformResources` 提供，不再定义在无线对象内。GPU/Wi-Fi/音频共享唯一 `libDitingSmmu4K.a`；SID1801使用与其他客户端相同的禁用context编程、回读和TLBSYNC顺序。公开来源及私有输入路径见 `configs/diting-native-sources.toml` 和 `local.toml.example`。详见[源码审计](https://github.com/Chengdong-Yu/UniXNU-pong/blob/main/migration/ipados18/services74-source-audit.md)；HAL加载和服务发布仍不是实际PCM发声证据。

## 电量识别（2026-11-14，代码与主机验证完成）

电池协议来自 [Xiaomi SM8450 的公开头文件](https://github.com/LineageOS/android_kernel_xiaomi_sm8450/blob/eff4b40407a1b6f1b1ab84554cbb540bcb495175/drivers/power/supply/qti_battery_charger.h)
和[属性转换实现](https://github.com/LineageOS/android_kernel_xiaomi_sm8450/blob/eff4b40407a1b6f1b1ab84554cbb540bcb495175/drivers/power/supply/qti_battery_charger.c)，
固定 revision `eff4b40407a1b6f1b1ab84554cbb540bcb495175`，与 services186 触屏参考同一源码版本。
原 Nothing 属性表在 `BATT_CONSTANT_CURRENT=12` 后与本机错位；温度、计数、容量和剩余时间已修正。

| 读取项 | opcode / property | 原始值 → 发布值 |
| --- | --- | --- |
| 电量 | `0x30 / 4` | 0.01% → 四舍五入百分比；`CurrentCapacity` / `MaxCapacity=100` |
| 电压、电流 | `0x30 / 7,9` | µV → mV；放电为正的 µA → 充电为正的 mA |
| 温度 | `0x30 / 13` | 0.01°C，`2750` 表示 27.50°C，不再误读充电电流 |
| 电荷计数 | `0x30 / 15` | mAh → `AppleRawCurrentCapacity`，保留真实零值 |
| 满充、设计容量、循环 | `0x30 / 18,17,16` | µAh → mAh；循环数不换算 |
| 预计充满、放空时间 | `0x30 / 20,21` | 分钟、秒 → 分钟 |
| 电源类型 | `0x32 / 7` | USB adapter enum，支持 PD/PPS、Apple 充电器与 Xiaomi HVDCP |

八项基础读数完成后先发布电量，再读取可选信息。5秒周期刷新、2秒请求超时、10分钟慢速刷新；
失败的可选字段延后重试，缺失适配器信息不会阻止百分比更新。owner、type、opcode、property、长度和数值范围全部核对。
未读到容量时不发布默认 0%；后续容量失败或通道断开保留上次读数并标记 `DitingBatteryStale`。
`DitingBatteryValidFields`、`FreshFields`、`MissingFields` 与 `TransportEpoch` 区分保留值、失败字段和通道代次。

物理 VBUS 与充电状态分开处理，100% 不自行认定满充。电源变化需要两个独立采样周期确认；
同一周期的基础和可选信息更新只计一次，低电量策略在确认后计算。通道 down/up 即使合并到一次工作循环，
也会重置请求和订阅、丢弃旧接收消息及 PMIC 发送队列，保留上次真实状态。
首次 `updateStatus()` 在 `registerService()` 之前完成，匹配者读取到的是已准备好的属性。

两套构建入口均支持 `--battery-only`，在独立、已准备头文件的 `--tree` 内生成 `libDitingBattery.a`；
它用于编译检查，不替代完整 `libDitingPower.a`。主机验证命令：

```sh
python3 -m unittest discover -s tests -p test_diting_battery.py -v
python3 scripts/build-diting-power-native.py --tree <独立的已准备树> --battery-only
```

11项 ASan/UBSan 测试及 Darwin24 ARM64 电池对象、完整电源归档编译通过；生产 merged recipe 也已独立编译。
本轮没有编译共享内核缓存、打包、刷机或操作手机。真实 PMIC 响应、iPadOS 电量显示和插拔更新尚待实机验收。
需要本机 `/diting/firmware/adsp.mbn`、真实 ADSP ready 与 PMIC 通道，失败时正常启动继续，不能以编译成功代替硬件读数。
详见[电量识别实现与验证记录](../../migration/ipados18/services188-battery-recognition.md)。

参考：UniXNU 官方 `docs/zh/porting/power.md`；UniXNU-sheng `0e1f6e5` 的 Qualcomm 协议/服务结构（保留原 BSD-2-Clause-Patent 声明）；NothingOSS `android_kernel_msm-5.10_nothing_sm8475` `sm8475/t` 的 PAS 与电池驱动。

构建成功只说明接口完成链接；需下一轮 REC 日志确认 PAS、通道响应及真实电池服务发布，不能据此声称已经出声或硬件已可用。

services45 已实际读出 0x34/0x35 两个功放 revision=0xb73，并完成
SID1801 的 DMA 映射，尚未完成 graph。services46 合并官方 power.md
所引用的 UniXNU-sheng 552b674 的 GLINK 大 intent 保留修复；已占满的
小空闲 intent 不再挤掉 graph 命令申请的大 intent。同时内存映射命令
等待真实 MEM_MAP_DONE/handle，不能把普通 ACK 当成映射完成。HAL 继续
在 staged 目录，未据此宣称系统播放已接通。

## 当前实机状态（2026-11-14）

services190 已完整链接并实机启动：使用本机0x9fd00000/0x3100000 reservation和显式本机签名固件，实时USB status读到ADSP stage13/error0、SCM0:0、PMIC_online1与增长的RX/TX。电池诊断stage4/percent100/flags0xb；界面电量、插拔、PCM尚未验收。文件优先；仅ENOENT且diting_adsp_builtin=1使用受限zlib解压的本地builtin，保持真实PAS并在关闭时释放缓冲。固件与产物不进Git。见 [services190](../../migration/ipados18/services190-usb-diagnostics.md)。以下保留services186历史记录。

services186 已由用户确认进入系统，触屏点按、滑动及熄屏后亮屏触控恢复正常。此前共享 RPMh 的10个host场景、
QMP 的 8 个 host 场景和完整原生内核构建通过；本轮 PAS、PCM 播放、充电状态与背光
操作尚未验收。见 [services185](../../migration/ipados18/services185-bdf-power-boot.md)。

services186的共享GCC并发、完整内核链接和resources-only/ramoops关闭编译检查通过，
POWER/BACKLIGHT关闭时仍链接唯一资源owner。实机结果见
[services186](../../migration/ipados18/services186-st61y-touch.md)。

## 许可

`LICENSE` 为 BSD-2-Clause-Patent。每个源文件的 SPDX 声明为准。
