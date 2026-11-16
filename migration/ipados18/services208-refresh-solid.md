# services208 刷新通知与 DPU 纯色对照（2026-11-16）

手机最后确认临时运行services208，持久boot_b保持services198。27项主机检查、完整内核、链接、八核打包、临时启动及匹配USB均通过，GPU/UI/Wi-Fi安装均stage6/error0/written0。用户确认纯色对照是“稳定的纯蓝色”，恢复也通过，但正常画面最新反馈仍是“颜色和闪烁仍异常”；地图、卡顿和120fps未解决。Wi-Fi暂停，用户现要求停止试验并整理交接。services209仅完成取色顺序对照的私有源码/缓存与主机检查，尚未构建上机，接手入口见 [完整任务交接](交接-2026-11-16-services208-GPU显示与性能未达标.md)。用户已取消低优先级，恢复开发后使用正常优先级/jobs8和私有缓存。

services207 一次矩阵执行 auto 43/43、sysmem 39/39、gmem 39/39，共 121 项像素/路径检查通过；64,704 条 trace 无 missing/skipped/kernel drops/recovery。真实 UI 动画三场景仍 FAIL，物理新合成帧 UNQUALIFIED。面板 timestamp 校准约 120.986 Hz，不是已证明的 120 fps。

## 本轮改动

`MiniIOMobileFramebuffer` 原有 thread call 默认已是 HIGH 优先级。本轮保留优先级，增加 ONCE 防止自重排的并发回调，并通过 XNU 内部 `THREAD_CALL_DELAY_SYS_CRITICAL` 关闭此通知的默认 timer coalescing。迟到时保持相位，选择最新已到期的周期，跳过旧周期并重新安排未来期限；不报告过时或未来的刷新时间。

事件 25 保留原字段，补充计划期限、实际回调时间、周期 ticks、跳过周期数和下一期限。解码器保留软件通知与物理显示的证据边界，检查时间戳/相位一致性，并统计到期至回调的延迟。

新增 USB 显式触发的 `display solid blue|green|red MS`，MS 范围 1000–60000。默认不运行，不写 framebuffer。固定色源来自当前两个 VIG 的 `SRC_CONSTANT_COLOR`，按 Xiaomi sm8450 modules `3b30ba7df6608c97c0ba2d55d679a6b5987c4c66` 的 ABGR8888 契约设置 FORMAT/UNPACK，并 flush CTL0 的 VIG0/1 位。命令仅排队；独立普通内核优先级回调每次执行有界寄存器操作，不持锁等待观察时长。

控制器保存并恢复两个 VIG 的 FORMAT、UNPACK、CONSTANT_COLOR 六个原值；只有 flush 退休且硬件帧计数前进才报告 restored=1。电源关闭时不访问 DPU，恢复重试有上限；配置所有权改变时报告失败，不覆盖新配置。`display solid status` 返回实际阶段与 error/restored；`display solid cancel` 提前恢复或重试待恢复状态。这是输出隔离工具，寄存器通过不能证明手机肉眼颜色正确。

驱动 SDK 没有导出刷新通知所用的内部 urgency/API。纯色对照使用公开的普通 delayed thread call；系统刷新通知的明确 critical urgency 留在 XNU 内部。恢复实际时间由诊断记录，不能假称精确毫秒到期。

## 主机与构建状态

27 项主机检查通过，覆盖实际增量 XNU patch 重放、刷新相位/迟到/提前调用/溢出、纯色三颜色/取消/部分写失败/电源失去与恢复/配置改变/flush 超时、显示 reader、trace 解码和既有 USB 传输边界。

首次 native 编译发现 driver SDK 不导出 `THREAD_CALL_DELAY_SYS_CRITICAL`，已将纯色对照改用公开 delayed API；内核 trace 的 IOReturn 窄化转换也已修正。完整内核、链接、八核打包和临时启动最终均通过；匹配运行内核及安装状态已由独立USB读取确认。

```text
diting-work/build/services208/gpu-refresh-solid-20261116-01/
  work-state.json
  runtime-source/
  tree/.cache/xnu-work/
diting-work/tmp/services208/gpu-refresh-solid-20261116-01/
  check-host.py
  run-kernel-build.py
  boot-candidate.py
diting-work/logs/diting/services208/gpu-refresh-solid-20261116-01/
  host-checks.json
  first-state-01/state.json
  solid-restore-01/report.json
  solid-blue-observation-01/report.json
  automatic-ui-01/
```

实际源码保存在上述私有候选；相对于完整 services207 的 XNU 增量为 `zzzzzzzzzzzzzzzzzzzzzzzz-diting-refresh-deadlines.patch`。保留 GPU/Mesa 运行时与安装 ID、键盘资源、八核、触控、电池、ADSP、UI/Wi-Fi 包及 Data/SystemOS。已完成的内核继承此前 SCHED_IDLE、nice19、ionice idle、jobs1；用户随后取消低优先级，后续正常优先级/jobs8，不写共享缓存，不改持久 boot_b，不做镜像哈希/审计/回读。

## 设备结果

1秒蓝色恢复检查与60秒观察均成功，writes14/error0/restored1。用户确认“稳定的纯蓝色”；最终恢复两个VIG的FORMAT/UNPACK/CONSTANT_COLOR全部原值且flush退休。常量源对照FORMAT为004237ff、UNPACK为03010002、蓝色CONSTANT_COLOR为ffff0000。

常量源绕过GPU/DDR正常framebuffer读取，并同时改变UNPACK。此结果只证明该配置下的后段输出稳定正确，不能排除正常fetch格式、缓存一致性或扫描同步问题。自动恢复后正常UI仍颜色异常、闪烁。

自动UI功能4/4 PASS，31,545条trace的missing/skipped/kernel drops/recovery全为0；三场景动画均FAIL。桌面、设置、多任务内容变化观察分别为76.5/20.0/56.5Hz，最长运动内容不变为33.438/175.240/58.492ms。这些不是物理fps；panel timestamp校准120.989Hz，物理新合成帧仍UNQUALIFIED。

软件通知间隔p95从207的75.046ms降到208的8.348ms；1626次通知全部发送成功，回调迟到p95为0.023542ms、最大0.150991ms，时钟invalid及跳过周期均0。通知流仍有408.343ms长间隔，尚未归因，不能声称全部停顿消失。PID43 CP执行p95为8.117760ms、最大16.410156ms；提交开始到CPU退休p95为10.158ms，copy offer到完成p95为8.327ms，copy完成间隔p95为30.380ms。

## 停止点与未完成

208的链接、打包、匹配启动、纯色恢复及一次自动USB UI评估已完成。正常颜色、闪烁、明显卡顿及物理120fps均未通过；DDR实际频率和持续带宽仍未测定。209准备了保留正常FORMAT、仅临时改变UNPACK的`display fetch-swap MS`对照，主机检查PASS，但内核/链接/打包/上机均NOT_STARTED。本次交接没有继续构建或设备操作；完整源码、记录路径和恢复开发后的命令见 [任务交接](交接-2026-11-16-services208-GPU显示与性能未达标.md)。
