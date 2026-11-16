# services206–207：GMEM 事件传输颜色错误（2026-11-16）

最新状态见 [2026-11-16任务交接](交接-2026-11-16-services208-GPU显示与性能未达标.md)：手机最后确认临时运行services208，持久boot_b保持services198；209仅完成源码/缓存与主机检查，尚未构建上机。用户要求停止试验、整理交接，Wi-Fi暂停；恢复开发后使用正常优先级/jobs8。

本页保留services206–207历史证据。services207 / gpu-gmem-transfer-20261116-01 完整内核、链接、八核打包与临时启动均通过。一次矩阵auto43/43、sysmem39/39、gmem39/39，共121项通过；64,704条trace无missing/skipped/kernel drops/recovery。但用户在207及208仍确认颜色异常和闪烁，真实UI三场景动画均FAIL，物理新合成帧未认证。局部GMEM修正不能代表整屏显示已修复；208继承该GPU包，刷新与纯色证据见 [services208记录](services208-refresh-solid.md)。

## services206 实机证据

完整内核、链接、八核打包及临时启动通过；USB 匹配运行内核，GPU／UI／Wi-Fi 安装均 stage6/error0。GPU 安装 ID 为 `e90ec2e6-b141-4e55-bba5-5c8014eb631a`。

一次 USB qualification 矩阵执行 99 项：auto 33/33、sysmem 33/33、gmem 27/33。三个 Vulkan 线性 BGRA 用例分别有 18,604/33,153 个像素错误，首批错误直接显示红蓝交换；sysmem 对照没有像素错误。

- `vk-linear-bgra-load-store-draw`
- `vk-linear-bgra-srgb-load-store-draw`
- `vk-mutable-linear-bgra-load-store-draw`

另外三个 Metal IOSurface 重用用例四轮 CPU 像素检查全部通过，但路径证明失败：旧计数器没有记录 GMEM 事件传输，且 IOSurface 可由运行时的 shadow image 渲染，不能假定它直接使用线性 render target。这三项是取证缺口，不是复现了 Metal 像素错误。

2,375 条 trace，missing/skipped/kernel drops/recovery 均为 0。整屏颜色、闪烁及物理 120 fps 均未解决。原始证据保存在：

```text
diting-work/logs/diting/services206/gpu-format-sync-20261116-01/
  first-state-01/
  linear-contract-01/
  linear-contract-findings.json
```

## services207 修复与检查

旧适配在 `event_blit_run` 中强制把 `RB_RESOLVE_SYSTEM_BUFFER_INFO.COLOR_SWAP` 改成 WZYX，覆盖线性 BGRA 所需的 WXYZ。GMEM 事件加载／保存会转换颜色表示，系统缓冲区必须保留自己的格式交换；这与逐 texel 的 draw 传输不同。已对照固定 Mesa `85fda3b36405bcaea8bffc64aa4f2651dc90770d` 的原始实现，恢复事件描述符的格式设置。

新增独立事件传输计数，分别保留 load/store 次数、非恒等交换、MUTABLEEN 和线性图像次数。原有 16 项 draw/input 计数 ABI 保持；新 `tu_gmem_event_stats` 为 8 项独立计数。Vulkan 线性用例增加 store-only 首帧的独立 CPU 像素检查。Metal IOSurface 用例验证实际 GMEM 执行与每轮退休后的 CPU 像素，明确 GPU target tiling 未知，允许 shadow image，避免误报线性路径已认证。

27 项自动化检查、17 项安装器恢复检查、4 项 runtime 检查通过，固定源准备脚本生成的 Mesa 文件与本轮编译输入一致。新 Turnip 对象及库、配套测试、签名和 GPU boot archive 已完成；GPU 新安装 ID 为 `e65581df-e63d-4b94-bfa4-91bc7d901f08`。随后完整内核、链接、打包、临时启动及一次121项USB矩阵全部完成，像素/路径检查通过，正常实屏颜色与闪烁仍失败。

候选保存在 `diting-work/{build,tmp,logs/diting}/services207/gpu-gmem-transfer-20261116-01`。该历史轮次编译继承SCHED_IDLE、nice19、ionice idle、jobs1；用户后来已取消低优先级，不能用于后续构建。共享Mesa和XNU缓存未写入。只重编译GMEM传输对象，保留Metal bridge与发布策略、键盘资源、八核、触控、电池、ADSP、UI、Wi-Fi和Data/SystemOS。镜像哈希、审计和镜像回读按用户要求跳过。

DDR 实际频率和持续带宽仍未测定。GMEM 颜色错误的复现不等于已排除带宽瓶颈；后续性能判定继续使用真实 UI 的 CP 执行、提交、copy 与独立动画观察，目标保持 120 fps / 8.333 ms。
