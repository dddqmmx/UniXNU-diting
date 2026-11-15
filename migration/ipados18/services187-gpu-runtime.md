# services187：GPU 运行时已安装，滑动性能未通过

2026-11-14，Linux / diting / SM8475 / iPadOS 18.7.10 / 22H374。
用户确认进入系统、触控可操作，随后确认滑动和动画仍明显卡顿。
**真实 GPU 执行与显示拷贝已有记录，界面性能为 FAIL，任务未完成。**

## 实现与安装

按 [官方 GPU 文档](https://unixnu.com/zh/porting/gpu) 使用 A730 `0x07030002`、
2 MiB GMEM、NativeAdreno ABI1 和匹配 22H374 的实际 Metal ABI。
Linux 构建 Turnip、Rust bridge 与 Metal plugin，实际签名后打包；保留真实 Vulkan
能力及既有 `TU_DEBUG=sysmem,nocb` 颜色策略。

新安装器在可写原生 OS/APFS 上、launchd 前安装八个 GPU 运行时、策略与采集文件。
完整安装后才发布 bundle；缺文件或失败不能用成功标记掩盖。相同 ID 已完整安装时
走检查后的快速路径，不重复解压。保留现有 Data/SystemOS，不覆盖旧 host APFS 元数据。
归档 12,821,480 字节，安装 ID：`0912509e-45dd-4e4b-8852-2683ee553258`。

保持八核、services186 ST61Y、services182 实测 RAM/FD/template/33,792 字节 loader。
BO 按需分配，`diting_gpu_pool_mib=0`。镜像 101,183,488 字节，启动参数 954 ASCII 字节，
含 rootful=1、GPU runtime install=1、GPU trace=1。跳过镜像哈希、审计和镜像回读。
源码基线 4d478a5，本轮 GPU 修改未提交；后续两个 HEAD 提交仅更新 186 文档。

本地入口与证据（产物和原始日志不入库）：

- `diting-work/build/services187/gpu-runtime/build.json`：三个构建阶段均 exit 0。
- `diting-work/build/services187/gpu-stage/build-report.json`：归档、清单和安装 ID。
- `diting-work/tmp/services187/{build-local.sh,package-local.sh,install-boot.py}`。
- `diting-work/logs/diting/services187/install-boot.{json,log}`：核对 diting/unlocked/b
  与 192 MiB boot_b，稀疏两块全 OKAY，4.859 秒；临时 boot OKAY，2.438 秒。
- 独立 `hardware-observation.json` 和 `candidate-observation.json` 保存后续观察，
  初始回执保留其生成时的未验收字段。

首次打包因缺本轮 DTB 被拦截，没有刷入失败候选；生成 DTB 后打包通过。失败日志保留。
六项启动安装器 host 测试通过，覆盖首次安装、重复启动、缺库、部分失败、畸形归档和错误根卷。

## 当前文件取证

`scripts/capture-diting-gpu-files.py` 在 root Recovery、未挂载 guest 卷上读取当前
SHNGROOT、APFS checkpoint 及指定文件块，stdout 精确长度、stderr 分离。
真实安装 ID 匹配，bundle 元数据和三份 trace 全可读，missing=0、unreadable=0；
1687 个 4 KiB 块、约 3.7 秒，未写分区、未下载库作镜像验证。

证据在 `diting-work/logs/diting/services187/gpu-files/`。
60/180/300 秒 trace 分别 1,997,672、1,995,032、2,020,832 字节，含 backboardd
真实提交、退役和 GPU 显示拷贝完成。minidump 只有 services184/185 等旧 epoch，
pstore 为空，不能冒充本轮启动日志。

| 真实窗口 | offset（字节） | size（字节） |
| --- | ---: | ---: |
| OS | 1,048,576 | 6,845,104,128 |
| SystemOS | 6,846,152,704 | 5,368,709,120 |
| Data | 12,231,639,040 | 8,589,934,592 |

窗口以设备 SHNGROOT 为准，不套用旧 Data 偏移。

## 性能与采样边界

`scripts/decode-gpu-trace.py` 的七项测试覆盖格式、时钟、跨 PID/queue/fence 关联、
ring 覆盖、显示 ID 命名空间、零 generation、缺失/失败时间戳和长间隔。
Record.us 是 uptime 微秒；退役 CP start/end 是 **19.2 MHz ticks**，
执行毫秒为 `(end-start)/19200`，不能当微秒或直接减 CPU 时钟。

| 快照 | 提交开始→CPU 退役 p95 | GPU 执行 p95 | offer→GPU 拷贝完成 p95 |
| --- | ---: | ---: | ---: |
| 60 | 6.706 ms | 6.593229 ms | 2.667 ms |
| 180 | 12.186 ms | 6.560937 ms | 11.044 ms |
| 300 | 8.341 ms | 0.780781 ms | 10.935 ms |

完整结果在 `diting-work/logs/diting/services187/gpu-performance.json`。
这些是至多 16384 条 live ring 的短窗口，旧 metadata 不扩展覆盖。
180 秒快照约 0.56 秒，拷贝完成间隔 p95 32.354 ms。
60/300 的长间隔可能是静止、睡眠、未捕获或卡顿，不能直接推算掉帧。
event20 phase7 GPU offer 与 phase1 普通 swap 的 ID 空间独立；event23 必须
status=0 且 generation 非零。以上没有物理 panel latch 时间，解码器不自动判 UI PASS。

详细前端追踪会产生大量 user-client 调用，同时 `SHENG_DIAG=1` 为大分配抓 backtrace。
[services188](services188-gpu-light-trace.md) 保持同一 GPU 库改轻量追踪；用户仍报告卡顿。
后续按新日志修复，不把编译、启动或事件减少当作性能通过。

原 services186 保留；因 OS 已装 GPU bundle，另有显式软件回退候选
`diting-work/build/services187/rollback/diting-services186-software.img`，使用 186 内核与触控、
显式关闭 Metal/scanout 并选择软件 UI。该候选尚未实机测试。
