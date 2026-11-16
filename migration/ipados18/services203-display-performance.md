# services203–205 显示与性能接续（2026-11-16）

最新状态见 [2026-11-16任务交接](交接-2026-11-16-services208-GPU显示与性能未达标.md)：手机最后确认临时运行services208，持久boot_b为services198，209尚未构建上机；正常颜色、闪烁和120fps仍未解决。用户要求停止试验并交接，且已取消低优先级；下文保留203–205历史时点，不作为当前运行状态或构建优先级要求。

services205时手机临时运行 **gpu-panel-colour-20261116-01**，持久boot_b保持services198。完整内核、强符号检查、八核打包、临时启动及匹配USB状态均通过，GPU/UI/Wi-Fi安装都是stage6/error0/written0。保留已工作的键盘资源、八核、触控、电池、ADSP和Data/SystemOS。该轮使用私有源码/缓存及当时的SCHED_IDLE/nice19/ionice idle/jobs1。

**用户最新反馈：“比如地图还是有问题。整个画面还是有很多错误甚至颜色闪烁”。此前“地图没问题”的判断已被撤回。地图、整体颜色与闪烁均未解决。** 不将少数图标的改善或 USB 缓冲区采样当作实屏颜色通过。

## services205 最新实机结果

```text
PANEL stage=6 error=0 panel=2 id=c5/10/00
target_hz=120 rollback_hz=60 before=0 after=2 batches=3
model=004c3132/00360200 selection_reason=0
address_mode=00 pixel_format=07 colour_error=0
colour_writes=0 colour_status=0
```

真实 DCS 与捕获 DT 选择匹配，面板接受120Hz，校准 panel timestamp 节奏 **121.039Hz**。颜色代码要求 pixel_format77，但实读07，因此没有颜色写入；address_mode00中BGR bit3已清零，不能据此认定切换MADCTL会修复错误。

USB自动UI功能4/4通过，29396条trace、missing/skipped/kernel drops均0，三场景动画均 **FAIL**：

| 场景 | PID43 GPU执行p95 | copy完成间隔p95 | offer至copy p95 |
| --- | ---: | ---: | ---: |
| 桌面滑动 | 6.2475ms | 24.299ms | 7.794ms |
| 设置 | 10.0025ms | 34.787ms | 11.994ms |
| 多任务 | 7.7927ms | 38.562ms | 12.116ms |

运动变化约63.1／23.3／63.2Hz；设置最大copy间隔431ms，有约200ms画面不变。`cp_execution`来自GPU写入的起止时间戳，按19200 ticks/ms换算。整体PID43 frontend完成p95约12ms、encode-age p95约9.5ms；提交、排队及呈现仍有成本。**120fps / 8.333ms**目标未通过，物理新合成帧仍UNQUALIFIED。

LLCC量化1ms峰值桌面19,070,976,000 B/s／设置42,795,008,000 B/s／多任务38,010,880,000 B/s，均writes19/restored1。DDR借用拒绝：mask802、enable0、irq_enable0f、filter_mask2、filter_match0、window90000000，stage4/error4/writes0，零峰值不是零带宽。MC3/SH5请求仍为DDR3,196,000kHz／LLCC1,066,000kHz、cache_ready1；实际DDR频率和持续容量未测定，不强借固件计数器。

当前帧采样changed0、16384字节，可见橙／黄色壁纸及部分蓝色图标，不能沿用历史Safari颜色结论。SSPP仍为b8000000／stride4880／FORMAT237ff／UNPACK03020001。DSC DATA_IN_SWAP0002c688是六分量identity；MAIN101202da的convert_rgb置位。CSC10、PCC、gamut、PA的enable均清零；IGC10000000的enable bit0也清零。DSI MDP_CTRL06100008高位含义未证实，不猜测写入。

证据：`diting-work/logs/diting/services205/gpu-panel-colour-20261116-01/{first-state-01,automatic-ui-01,current-pixels-01,user-colour-observation.json}`；驱动改动仍在私有源码，未合并主源码。当前无构建／打包／启动／采集进程，尚未准备services206。

## services203／204 历史证据

services203 的当前控制台 BGRA 采样中 Safari 图标为蓝色，用户在手机实屏仍看到红／橙色，颜色 **FAIL**，尚未闭环。DPU 两条活动 SSPP 的来源为 `b8000000`、stride4880、FORMAT237ff、UNPACK03020001；固定 Qualcomm 格式定义对应内存 BGRA。CSC10 enable bit0、PCC、gamut 与 PA enable bit20 均未启用。DSI MDP_CTRL 高位的含义仍未证实，不据此猜测写入。

services204 正确协商单字节最大回包长度后，实际 DCS DA/DB/DC 为 **c5/10/00**，但旧选择器错误地把 DT 型号名里的 42/02/0a、36/02/0b 当成硬件回复，结果 stage8/error5，时序 batches0。地址／像素模式查询尚未执行；不能把未读到的 ff 当成真实寄存器值。

固定 Xiaomi `mi_dsi_panel.c` 使用 DT 的 `mi,panel-id` 选择型号。保存的真实 DT `services169/live.dts` 中：

```text
/chosen/bootargs: msm_drm.dsi_display0=qcom,mdss_dsi_l12_36_02_0b_dsc_cmd:
/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_l12_36_02_0b_dsc_cmd
mi,panel-id = <0x4c3132 0x360200>
```

该节点要求 DSI0、1220×2712、RGB、DSC 命令模式。三个已核对的时序命令是 60Hz:2f00、90Hz:2f01、120Hz:2f02。面板 on 命令未发现 36／3a，不能据此编造默认地址模式。

services204 的一次 USB UI 自动评估：29979 条 trace，missing/skipped/kernel drops 全为0，worker功能检查4/4通过，三场景动画观察均 **FAIL**。校准 panel timestamp 节奏 **60.497Hz**；PID43 GPU执行 p95 为桌面5.643ms、设置9.831ms、多任务11.396ms。目标保持 **120fps / 8.333ms**，实际新合成帧的物理时序仍 UNQUALIFIED。

LLCC 1ms量化窗口峰值：桌面23,789,568,000 B/s、设置51,642,368,000 B/s；两次均 writes19/restored1。DDR计数器因借用前提失败而跳过（stage4/error4/writes0），**其零值不是零带宽**。MC3/SH5请求分别为 DDR3,196,000kHz、LLCC1,066,000kHz，GPU LLCC cache_ready1，但实际DDR频率和持续容量仍未测定。

## services205 实现与检查

- 从已保存 live DT 的 chosen 选择、mi,panel-id、几何和 DSI0/RGB配置生成独立 `diting-dsi-panel` ADT属性。L12_36还要求本机实际c5/10/00回复；L12_42保留时序计划，未有已验证DCS ID时不写入。
- 已退休的型号查询失败仍继续收集独立的GET_ADDRESS_MODE／GET_PIXEL_FORMAT。仅在完整资格、有效地址模式、像素模式77且BGR bit3确实置位时，保留所有方向／扫描位，清除BGR位，并回读确认；失败进行一次原值恢复。未退休命令继续保有同一FIFO。
- BWMON保留全部原有借用前提和恢复检查，新增拒绝位图及原始计数、IRQ、过滤器、窗口配置。不会强借固件拥有的计数器。
- UI采样挂到约200ms一次的既有worker状态回复，最多每场景一次，修正上一版错过短多任务场景的问题。观察仍是1ms量化峰值，不作为持续容量或实际频率证明。

14组面板模式／所有权／颜色故障场景、5组TE与像素读取场景、6组BWMON检查，以及显示、内存、USB回归均通过；六份固定DT时序字节流与保存输入一致。主机检查记录在 `diting-work/logs/diting/services205/gpu-panel-colour-20261116-01/host-checks.json`。GPU／UI／Wi-Fi用户态包未修改；三个安装ID分别为：

```text
GPU  2aa7404c-b1f6-40b0-8b1e-d3f3b3f72285
UI   4b012eac-071a-48c6-8ca0-6a5ecfa671f8
WiFi 58c90ec7-2665-43e6-8bda-722256e03a04
```

services205当时的后续方向为Turnip/metal2vulkan纹理格式、GMEM/sysmem传输、导入缓冲区同步及生命周期，以及显示模式广告与offer/copy调度。此后206–208已完成局部GMEM格式修正和刷新通知修正，正常颜色/闪烁仍失败；当前停止点及209取色顺序对照见页首交接。恢复开发后先分析已有证据、完成必要主机检查，再按正常优先级评估改变后的候选，不重测未变实现，不要求用户手动滑动测试。
