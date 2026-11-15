# services188：轻量 GPU 追踪，卡顿仍未修复

2026-11-14，Linux / diting / SM8475 / iPadOS 18.7.10 / 22H374。
用户确认“已进系统，触控正常，仍明显卡顿”。**性能 FAIL，GPU 任务继续。**

## 改动与构建

[services187](services187-gpu-runtime.md) 已证明 GPU 提交、退役和显示拷贝在运行。
本轮复用同一 GPU 库，仅将 `diting_gpu_trace` 从 1 改为 2。
`NativeGpuTraceDetailed=1`、`NativeGpuTraceKernelOnly=2` 共用原 trace ABI；
内核模式 2 保留 submit/retire/display，现有前端仅模式 1 设置
`SHENG_GPU_TRACE` 和 `SHENG_DIAG`。真实 fence、DMA 所有权与颜色策略保留。

修改位于两份 `NativeGpuTrace.h`、DitingAdreno `NativeGpuTrace.inc` 与实际 overlay
使用的 merged `build-ipados18-prelinked-boot.py`。CP 时间单位注释同时修正。
根 scripts 打包器仍有独立历史差异，不能盲目覆盖；本轮使用 services188/runtime-source。

- 完整内核 exit 0，52,109,336 字节，日志
  `diting-work/build/services169/build-20261114T035252Z.log`。
- native build-report 显示从 services188 overlay 编译 `NativeAdrenoPublish.cpp`，
  包含模式 2 的 include，对象时间晚于源码。
- 本轮 DTB、八核；services186 ST61Y 和 services182 实测 RAM/FD/template/33792 字节 loader。
- 镜像 101,183,488 字节，启动参数 954 ASCII 字节，rootful=1、trace=2、GPU pool=0。
- 03:59:39 UTC（北京时间 11:59）部署完成；diting/unlocked/b 与 192 MiB boot_b
  核对后稀疏两块全 OKAY，4.272 秒；临时 boot OKAY，2.448 秒。
- 未替换 Data/SystemOS；复用安装 ID `0912509e-45dd-4e4b-8852-2683ee553258`。

本地 scripts 在 `diting-work/tmp/services188/`，kernel/boot 在
`diting-work/build/services188/`，回执和独立观察在 `diting-work/logs/diting/services188/`。
初始回执不覆盖；后续确认写 `hardware-observation.json`。
XNU 缓存 services169/tree/.cache/xnu-work 当前对应 services188。
GPU 实现仍未提交，HEAD 的两个 services186 文档提交不包含本轮源码。

## 验收与继续

轻量追踪没有解决卡顿，性能仍为 FAIL。用户已确认操作约 5 分钟后回到 Fastboot；
临时只读取证 Recovery 正在收取新 trace，尚待确认轮次与解码。
不能沿用 services187 文件冒充 services188。

对比活动片段 p95/p99、CP 执行、CPU 观察退役和 GPU 拷贝耗时；60 Hz 对应
16.667 ms 比较预算，不代表测得 panel 刷新率。静止/睡眠/未捕获间隔独立保留，
物理 panel latch 未观测时不自动判 PASS。GPU 执行、用户滑动动画和长期稳定分别验收。

保持八核、触控与现有硬件，不猜测性提压、关外围或直接切 GMEM。
保留 services187、原 services186 和尚未测试的显式软件回退候选。
新尝试独立编号，继续跳过镜像哈希、镜像审计和镜像回读。
