# services212：恢复GPU启动后继续颜色验收

> 本页保留212历史结果。当前手机临时运行213，用户认为性能已很好并要求暂停GPU优化；最新停止点见[services213交接](交接-2026-11-17-services213-GPU优化暂停.md)。下方接续命令在用户恢复前不执行。

2026-11-17，Asia/Shanghai。212运行时GMU／CP正常启动、匹配GPU／UI／Wi-Fi库均6/0/0。用户在212正常界面明确确认“闪烁已消失”；地图和整体颜色尚未完整验收，120fps目标仍未达到。持久boot_b保持198。211失败取证见[209–211记录](services209-211-gmem-colour.md)。

212私有源码及缓存从完整211克隆，GPU库与测试worker、UI、Wi-Fi、键盘原样保留，GPU ID `fd6f236c-a2fa-4aec-9c1a-b06416d62f28`。未修改触控、电池、ADSP、USB传输或Data/SystemOS；正常优先级、nice0、jobs8。默认临时Fastboot启动，不写boot_b或userdata，不拍屏，不请求手动滑动。

修改源为`DitingAdrenoGmu.c/.h`、`DitingAdrenoPlatform.cpp`与新`DitingAdrenoStartup.h`，另有README与三个测试文件。固定原厂参考：NothingOSS SM8475 revision `c402fcdb3a1523d10b15ef1123bced45456f7592`的`adreno_gen7_hfi.c`，已有本地副本在`diting-work/tmp/services195/reference/`；官方文档网站403，本次阅读本地UniXNU的GPU／显示／组件／patch与UEFI文档副本。

响应等待只清除实际观察到的MSGQ bit0，保留GPU OOB与其他中断；一秒／四秒期限或迭代上限到达时，健康检查和真实匹配ACK最多再观察一次，没有重发、延长循环或合成成功。固件错误、NMI、坏队列、旧sequence、通知或时钟失败仍拒绝启动并保留DMA backing。保存实际command/header、transport/error、ACK数、边界检查、IRQ确认、等待间隔及健康读数；`gpu`即使SKIP也可输出已完成的软件启动记录。最多32个历史记录发布后不可覆盖，panic snapshot使用slot9。

15项主机测试通过，实际AArch64 GMU waiter及HFI ring由交叉GCC静态编译并通过QEMU执行；并发记录由真实C++ header执行。覆盖迟到成功ACK、缺失ACK／冻结时钟有界超时、固件错误／NMI、旧sequence、队列损坏、通知／IRQ写／等待失败、回退时钟、OOB与MSGQ分离及记录容量。211已完成的51项运行库／矩阵主机检查保留，不重跑相同设备实现。

完整八核内核、链接、打包、临时启动和匹配USB均PASS；原始内核／boot及GPU库保持保留。不能将库安装或临时启动回执当作显示／性能通过。

```text
diting-work/build/services212/gmu-ack-observe-20261117-01/
  work-state.json
  runtime-source/
  tree/.cache/xnu-work/
diting-work/tmp/services212/gmu-ack-observe-20261117-01/
  check-host.py
  run-kernel-build.py
  check-link.py
  prepare-package-kernel.py
  run-package.py
  boot-candidate.py
  read-state.py
  run-gpu-assessment.py
  capture-temporal-pixels.py
diting-work/logs/diting/services212/gmu-ack-observe-20261117-01/
  host-checks.json
```

构建后按脚本顺序链接、打包及临时启动。正常GPU worker必须空闲；若仍是211的broker-unavailable，boot脚本仅接受匹配211内核、明确的GMU power poison／power_started0／gpu_held0和CP diagnostics_ready0，随后正常同步文件系统回Fastboot。不是对任意不可用broker的放行。

匹配启动后一次完整auto/sysmem/gmem矩阵须覆盖211外部host pointer的真实draw载入与保存，另读16个不同完成copy的连续像素，并做自动UI评估。若GPU启动仍失败，先读新保留结果，不重启碰运气。颜色、闪烁、地图、软件节奏与物理120fps分别记证据。

## 212 已完成设备结果（2026-11-17 接续）

GMU启动保留22条结果，published4/acknowledged4、transport0/error0；CP与正常GPU broker可用。最新只读`continuation-state-01/state.json`确认匹配212、worker末job3/state3、quiescing0、无活动测试任务。GPU/UI/Wi-Fi均stage6/error0/written0，继续复用匹配211运行库。

一次完整矩阵auto50/50、sysmem46/46、gmem46/46均PASS，覆盖外部host pointer的真实draw载入和保存。66,993条trace的missing/skipped/kernel drops/recovery均0。静止正常桌面16个不同完成copy未再观察到精确红蓝互换，alpha全255、采样changed0；此前209／210失败证据继续保留。用户对当前212正常界面回答 **“闪烁已消失”**。这将当前界面的蓝橙闪烁记为视觉PASS，不能扩大到地图、所有颜色或物理fps。

| UI场景 | 120Hz动画观察 | 最低观察内容变化率 | 运动期间最长内容不变 |
| --- | --- | ---: | ---: |
| 桌面连续滑动 | FAIL | 59.8Hz | 58.438ms |
| 设置打开／滚动／关闭 | FAIL | 23.3Hz | 100.170ms |
| 多任务 | FAIL | 56.5Hz | 33.469ms |

校准panel timestamp节奏120.960Hz；这是面板时钟证据，物理新合成帧仍UNQUALIFIED。完整矩阵中PID43的CP执行p95为7.450208ms，提交到CPU退休p95为9.592ms，frontend scheduled p95为2.483ms、callback p95为11.154ms。离屏颜色与负载检查通过，真实UI卡顿继续FAIL。下一步保留212颜色／GMU修复，优先分析现有逐场景提交、执行、退休、刷新、呈现及CPU调度证据，再做改变后的候选。

新增原始证据目录为`diting-work/logs/diting/services212/gmu-ack-observe-20261117-01/`：`first-state-01/`、`continuation-state-01/`、`automatic-01/report.json`与`report.md`／`decoded-trace.json`、`temporal-pixels-01/report.json`和16份像素、`visual-flicker-01.json`。
