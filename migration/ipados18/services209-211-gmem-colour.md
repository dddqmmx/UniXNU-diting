# services209–211：红蓝闪烁与 GMEM 载入接续

> 本页为失败与修复的历史证据。后续212正常启动并通过142项颜色矩阵，用户确认正常界面“闪烁已消失”；当前临时运行213，GPU优化已按用户要求暂停。最新入口：[services213交接](交接-2026-11-17-services213-GPU优化暂停.md)。

更新时间：2026-11-17，Asia/Shanghai。用户已恢复开发，要求必须控制蓝色／橙色闪烁。当前显示正确性和120fps／8.333ms均未通过。

## 当前设备与约束

手机已临时启动services211，USB确认匹配内核，但GPU硬件启动失败；211不是可验收颜色的正常GPU运行状态。持久boot_b保持services198。序列号1d4eb066，product diting、unlocked yes、slot b、boot_b 0xc000000。下表保留210历史安装状态；211 GPU ID为`fd6f236c-a2fa-4aec-9c1a-b06416d62f28`，安装6/0/8，UI与Wi-Fi ID不变且6/0/0。

| 210运行时 | 安装ID | 设备状态 |
| --- | --- | --- |
| GPU | `478c29df-1b1a-4c7f-a5ae-7b190f2ac33d` | stage6/error0/written8 |
| UI | `4b012eac-071a-48c6-8ca0-6a5ecfa671f8` | stage6/error0/written0 |
| Wi-Fi | `58c90ec7-2665-43e6-8bda-722256e03a04` | stage6/error0/written0 |

继续正常优先级／nice0／jobs8、私有源码／缓存。Wi-Fi暂停；保留键盘、八核、触控、电池、ADSP、Data/SystemOS。不reset/clean、不覆盖独立修改、不写共享缓存。常规构建、匹配安装、临时Fastboot boot和有界USB诊断已有授权，不重复询问。跳过镜像哈希、审计与回读，使用`--skip-image-verification`。用USB自动测试，不要求人工滑动或开关应用，不拍屏取日志。

## 209：取色交换未控制闪烁

209完整内核、链接、打包、临时启动及匹配USB均PASS；GPU/UI/Wi-Fi安装均stage6/error0/written0。

1秒和60秒`display fetch-swap`恢复均PASS，最终stage5/error0/restored1/writes14。用户确认蓝色／橙色闪烁仍存在，不能把UNPACK交换永久化作为修复。

16次64×64预DPU framebuffer采样均在GPU拷贝完成后读取，`changed=0`，alpha全为255。多数相邻帧约374点发生精确红蓝互换，例如`0xff9aa7ff ↔ 0xffffa79a`，主要集中于Dock。**错误已存在于扫描前的内存内容中。** 这不排除另有扫描竞争。

证据根目录：`diting-work/logs/diting/services209/gpu-fetch-order-20261116-01/`。

- `fetch-observation-01/report.json`：恢复及用户反馈。
- `temporal-pixels-01/report.json`、16份`.bgra`、`mosaic.png`：连续像素。
- `rbbm-cadence-01/report.json`：只读时钟计数；selector/source未解码，实际GPU频率仍UNQUALIFIED。

## 210：保存路径修正及载入覆盖缺口

210保留207事件传输修复，移除`r2d_dst_impl`、`r3d_dst_impl`、`r3d_dst_gmem`中的强制WZYX，保留view自身格式。新增12项destination计数及Vulkan用例34–37，每项执行6轮不对齐renderArea重绘，线性图像另做7次原始mapped存储检查。

固定Mesa revision：`85fda3b36405bcaea8bffc64aa4f2651dc90770d`。固定源码重放、49项主机检查、运行库编译／链接／签名、完整八核内核、链接、打包及临时启动均PASS。

| 矩阵 | 结果 | 说明 |
| --- | --- | --- |
| auto/all | 46/47 | Vulkan32层负载CPU完成p95超预算 |
| sysmem/all | 43/43 | 该矩阵通过 |
| gmem/all | 39/43 | 四项新增回归缺少draw载入证据，保持FAIL |

34–37的seed、6轮像素和线性7次原始存储检查均通过。但强制GMEM时事件载入6次、draw保存6次、draw载入0次。普通图像的不对齐renderArea只保证draw保存，**不能保证draw载入**。保留210原始失败，不改写为通过。

73418条trace的missing/skipped/kernel drops/recovery均0。真实UI动画三场景仍FAIL，最低观察内容变化率为桌面83.1、设置13.3、多任务59.9Hz；这些值不是物理fps。面板timestamp节奏121.009Hz，物理新合成帧仍UNQUALIFIED。

210静止桌面16帧对应16个不同完成copy，15个相邻转换都有精确红蓝互换，最多963点。涉及区域比209更大，闪烁未解决，不能由离屏像素PASS推断正常UI正确。

证据根目录：`diting-work/logs/diting/services210/gmem-draw-format-20261117-01/`。

- `first-state-01/state.json`：匹配内核、安装ID及DPU／内存。
- `automatic-01/report.json`、`report.md`、三份原始`.ndjson`、`decoded-trace.json`：矩阵及路径证据。
- `temporal-pixels-01/report.json`、16份`.bgra`、`mosaic.png`：持续红蓝交替。

## 211：线性源格式与真实draw载入回归

211从完整210私有源码、缓存、Mesa和包克隆。线性BGRA内存保持BGRA字节序；`r3d_src_load`原先无条件将GMEM载入源的texture swap清为identity。211在线性内存源上保留图像格式swap，tiled源保持既有规则。site1计数读取实际发出的system-memory源descriptor，不再记录假定的identity。

新增46–48三项external host pointer线性BGRA/sRGB/mutable回归，使用真实`VK_EXT_external_memory_host`、owner原始指针及6轮局部重绘。奇数尺寸外部线性图像触发现有unpadded路径；强制GMEM时必须有至少6次draw载入和6次draw保存，7次原始owner存储及独立transfer-buffer oracle均须通过。34–37保留普通部分重绘的事件载入／draw保存覆盖，不冒充外部载入证据。

51项主机检查PASS（automation30、boot stage17、runtime4），固定源码补丁重放与实际编译源一致。qualification auto40、performance auto8、all auto50、all sysmem/gmem各46。GPU运行库编译、链接、签名及重新打包、完整八核内核、链接、打包与临时启动均PASS；USB匹配211。首次设备状态检查FAIL：GPU clock SKIP，broker-unavailable；不能执行颜色矩阵或验收修复。

### 211实际启动失败（11月17日接续）

只读软件状态确认CX clocks为attempted1/ready1/poisoned0、GMU SMMU为ready1；GMU固件与HFI均ready1，但power_attempted1/poisoned1/power_started0/gpu_held0，未进入CP发布。库安装6/0/8不代表硬件启动成功。

失败后保留的HFI pending header为`0x0020a003`（sequence2，带宽表ID3），命令队列read/write均220，响应队列read4/write8。未消费回复为`0x00110303, 0x0020a003, 0`，实际对应返回成功的ACK。它说明回复曾到达，无法单独判定是等待超时、硬件健康检查还是队列观察失败；此前缺少持久的具体power result。reserved pmsg不属于managed RAM，通用nofault reader拒绝；msgbuf读取成功但启动日志已被覆盖，未扩大RAM读取权限。

证据：`diting-work/logs/diting/services211/linear-gmem-load-20261117-01/`下的`first-state-01/state.json`、`gpu-startup-read-02/report.json`与`hfi-failure-read-01/report.json`及原始数据。保留本轮FAIL，未重试同一211实现。

212保留211完整GPU运行库，按固定原厂协议补MSGQ bit0确认、一次期限边界最终ACK检查及最多32条不可覆盖启动记录。15项真实AArch64 HFI／GMU与并发记录主机测试通过，正常优先级／jobs8完整内核构建中。下一接续见[services212](services212-gmu-ack-colour.md)。

私有目录：

```text
diting-work/build/services211/linear-gmem-load-20261117-01/
  runtime-source/patches/mesa/parity211/0001-linear-gmem-load-source-format.patch
  runtime-source/userland/gpu-test18.mm
  runtime-source/scripts/diting_gpu_automation.py
  mesa/src/freedreno/vulkan/tu_clear_blit.cc
  work-state.json
diting-work/tmp/services211/linear-gmem-load-20261117-01/
```

运行库构建必须通过`/home/dddqmmx/.unixnu/run.sh python3 .../build-runtime.py`进入工具链。后续按序运行`run-kernel-build.py`、`check-link.py`、`prepare-package-kernel.py`、`run-package.py`、`boot-candidate.py`。boot脚本检查当前210匹配内核、空闲worker和运行时ID，正常同步文件系统回Fastboot后临时启动211；不写boot_b。

启动后`read-state.py`核对安装，`run-gpu-assessment.py --profile all`执行一次改变后完整矩阵，`capture-temporal-pixels.py`读取16帧连续像素。输出目录必须是新目录。颜色、闪烁、地图和性能分别验收。

当前presenter仍复制到正在扫描的单个`b8000000` framebuffer；扫描竞争／双缓冲仍为独立后续方向，尚未修复或排除。GPU／DDR频率请求、软件通知、CPU内容变化和面板时间戳均不能代替物理新合成帧fps。
