# services197/198：USB GPU 自动化测试

更新日期：2026-11-15（Asia/Shanghai）。目标保持 **120 fps / 8.333 ms**。

本体系通过 USB 启动固定 Vulkan/Metal 负载、注入系统手势、采集真实 framebuffer 和内核 trace，自动生成评估结果，无需人工滑动或开关应用。services197 已完成首轮 97 项设备检查；services198 修正输入/观察时序与显示计数解释，已完成构建、部署和完整自动矩阵。**自动测试体系已实机运行，GPU 120 fps 目标仍为 FAIL；services195 用户原始反馈“动画卡顿，颜色错误”保留。**

## 执行入口

在设备仓库根目录执行，输出目录须为新目录：

```bash
python3 scripts/run-diting-gpu-tests.py \
  --profile all --policies auto sysmem gmem \
  --output-dir /home/dddqmmx/UniXNU/diting-work/logs/diting/services198/automatic-next-01
```

主机自动选择唯一 Diting CDC 端口，必要时加 `--port /dev/ttyACM0`。一次完整矩阵为 auto 35 项、sysmem 31 项、gmem 31 项，共 **97 项**。三个策略分别启动新 worker；UI 场景只在 auto 执行，使用运行中的 compositor 策略。

`--profile qualification` 只运行功能检查，`performance` 运行 GPU 负载，`ui --policies auto` 运行系统手势与画面观察。退出码 0 表示所选检查完整通过，2 表示检查失败或证据不完整，3 表示基础设施失败。完整矩阵尚无物理合成帧认证时会返回 2，不能因此无故重测。

USB 管理入口为 `python3 scripts/diting-usb-command.py gputest status`，以及 `gputest run all auto`、`gputest result JOB OFFSET COUNT`、`gputest cancel JOB`。常规使用完整 runner，它负责唯一会话、结果长度、场景对应、超时取消和报告，不需要手动拼接命令。

## 覆盖与证据

| 检查 | 自动执行与判定 |
| --- | --- |
| 后端身份 | 真实 A730v3 Vulkan 与 NativeBridgeMetalDevice；核对芯片、ABI、策略和渲染器身份 |
| 颜色（每策略 20 项） | RGBA/BGRA、sRGB、FP16、图案/stride、混合、load/store/scissor、采样、深度、4× MSAA、AIR 与 IOSurface；每项 257×129 像素与独立 CPU 参考比较 |
| 同步与生命周期 | compute 后真实 CPU acquire；64 次 BO 映射、退役、解除映射和释放 |
| GPU 负载（每策略 6 项） | 1220×2712 clear、8/32 层混合；预热 24 次后保留全部 240 个完成样本，按 8.333 ms 检查 p95 |
| UI | 桌面八次连续滑动、两次设置打开/滚动/关闭、双击 Home 后多任务手势；设置前台身份单独核实 |
| 动画时序 | 每次手势 43 个观察点，固定前六步预热；保留实际观察时间、读取耗时、内容 hash 和独立输入时间 |
| 内核对应 | event64 记录 job/case/phase 和 Mach 时间；非 UI 工作按实际 worker PID 对应 submit/retire；UI 保留系统客户端的交织工作 |

输出包括 `report.json`、`report.md`、`junit.xml`、原始 USB、每任务 NDJSON、`trace.bin`、`decoded-trace.json`、显示/时钟原始回复，以及每 UI 场景的 `ui-frames-JOB-CASE.csv` 和 `ui-inputs-JOB-CASE.csv`。

GPU readback 的颜色正确只覆盖这些固定渲染路径，不能排除其他应用着色器、compositor 转换或物理面板颜色问题。32×32 framebuffer 内容采样是只读、非原子、DPU 之前的观察；颜色检查、连续内容变化、GPU 完成与物理呈现分别记录。

## 时序修正

services197 首轮通过实际 USB 完成 97 项设备检查，采集 28,057 条 trace，缺失、跳过、kernel drops 和 recovery 均为 0。但原输入线程使用普通后台睡眠，实际间隔出现 80–107 ms，醒来后连续补发过期坐标。三个 UI 动画评估均为 INCOMPLETE，原始结果完整保留。

services198 的输入线程和独立 framebuffer 观察线程使用 Mach 单调绝对等待与测试 worker 内的 time-constraint policy：周期 8.333 ms、computation 0.5 ms、constraint 1 ms，读取并核对实际 policy。每次延迟后保留完整周期，禁止补发突发手势；主机同时核对输入间隔、观察间隔、读取成本和起终点时钟对应。观察跟不上时仍为 INCOMPLETE，不伪装成动画 FAIL 或 120 fps PASS。

固定 NothingOSS `sde_hw_intf_get_vsync_info` 证实 `INT_COUNT_VAL` 高/低 16 位是读指针帧/行计数，会在 kickoff/autorefresh 中复位。原 services197 用三个短窗口相减得到的“TE 60.146 Hz”已撤回；`AUTOREFRESH=0x8000003c` 的低位是 frame_count=60，不能读成 60 Hz 配置。

更新后的主机从已使能的 panel VSYNC raw timestamp 取证：先用至少两秒、32 个样本的稳定 CPU 窗口校准 raw 单位，检查两个半窗口的漂移与残差；仅相邻 CPU 读取不超过一个 120 Hz 周期的时间戳变化用于周期评估。稀疏采样、复位或校准失败保持 UNAVAILABLE。重放首轮 1,154 个时间戳样本后，raw clock 约 19.200 MHz，130 个紧邻变化的中位周期 16.541 ms，约 **60.455 Hz**；这依然是 VSYNC 时间戳节奏，不是新合成帧的物理 fps。

## 主机与构建

services198 的 25 项主机回归通过，覆盖 ASan/UBSan、真实子进程、lease、崩溃/取消/恢复、碎片 PTY、超时、原始记录保留、手势突发、独立输入、60 Hz/终态动画误判和显示校准。复用未改变的 17 项安装器恢复检查与 7 组完整 AIR 管线编译结果。更新 helper 的 iOS 严格编译、签名和归档通过。

候选使用私有源码与 XNU 缓存，GPU 图形 runtime 复用 services197；UI 第六版归档 ID `3d4f894a-5231-44f3-8ed9-b552dc3cb1d1` 保持配对。八核、services182 的 33,792 字节 loader、原电池/ADSP、Data/SystemOS 保留；未重写 userdata 镜像。继续跳过镜像哈希、镜像审计和设备镜像回读。

services198 GPU 安装 ID：`544c1397-c1d5-4b57-bad8-6642003bf1e4`。源码 HEAD 为 `e6c7a9f7f74f8d303022ce0d1683f2e47b17e7ad`，实现还包含未提交差异，不能 reset/clean。

## 最终设备结果

services198 于 `2026-11-15T06:08:24.773525Z`（北京时间 14:08）完成稀疏写 boot_b 和同镜像临时启动；两块发送/写入均 OKAY，刷入 4.530 秒、启动 2.543 秒。完整内核链接、八核打包通过，镜像 101,183,488 字节，实际 boot args 为 1022 字节。原始安装回执中的 `UNTESTED` / `hardware_boot_confirmed=false` 保留其生成时点，后续实机证据独立记录。

USB 在 `06:09:34Z` 核实运行内核包含匹配 GPU/UI 归档，GPU 安装 stage6/error0/written8/step17，UI stage6/error0，实际请求刷新率120。`06:13:09Z` 只读复核得到相同结果；随后 `gputest status` 显示 daemon16、error0、末任务3/state3、worker_error0、quiescing0，无活动 worker。手机保持 services198 正常运行。

完整矩阵在 `06:09:34.805259Z` 至 `06:10:07.421201Z` 自动执行，耗时32.616秒，使用4,325条USB命令。auto/all执行35项，sysmem/all与gmem/all各31项；97项结果完整，worker均为PASSED。主机另行评估动画与显示证据，最终 **assessment=FAIL，execution=COMPLETE，manual_interaction_required=false**。worker PASSED只表示结构化设备检查完成，不等于最终性能通过。

| 自动评估项 | services198 实机结果 |
| --- | --- |
| 固定颜色路径 | 三策略共60项通过；每项33,153像素，mismatches0、guard_errors0；系统其他着色器、compositor与面板颜色尚未认证 |
| 同步/生命周期/后端 | CPU acquire、64次BO生命周期、真实GPU身份与策略检查通过 |
| 固定全屏GPU负载 | 三策略18组通过，保留4,320个完成样本；最接近预算的Vulkan 32层混合CPU完成p95为8.271ms |
| 桌面连续滑动 | 动画FAIL；最低观察内容变化66.536Hz，最长静止91.834ms |
| 设置打开/滚动/关闭 | 动画FAIL；最低观察内容变化13.311Hz，最长静止192.052ms；前台身份验证通过 |
| 多任务切换 | 动画FAIL；观察内容变化56.536Hz，最长静止33.432ms |
| 输入/观察时序 | 11次手势全部独立观察，input p95≤8.359ms、observer p95≤8.367ms、读取p95≤0.290ms；无采样迟到判定 |
| 显示时间戳节奏 | 1,109点校准，94个紧邻边沿，raw clock约19.199MHz；周期p50=16.532ms，约60.489Hz，BELOW_TARGET |
| 内核取证 | 27,085条事件，missing0、skipped0、kernel drops0、GPU recovery0；全部case markers对应通过 |
| 物理新合成帧 | UNQUALIFIED，hardware_performance_pass=false |

UI场景中主要客户端PID43的CP p95分别为7.098ms、9.646ms、12.514ms；提交至CPU退役p95分别为7.445ms、10.462ms、14.725ms。固定离屏负载通过不能排除系统复杂合成的瓶颈。copy间隔包含场景等待和静止，不能直接计算物理掉帧；完整样本与各PID分组保留在 `report.json`。

本轮只读接续检查没有重启、刷机或重跑矩阵。后续先重放这轮原始NDJSON/trace定位问题，并在下一候选的全部可预见修复、适当主机检查、完整构建和打包完成后进行一次USB自动矩阵；不要为相同实现重复实机测试。显示时间戳约60.489Hz只证明对应硬件时间戳节奏，不能由此直接修改面板或时钟寄存器。

本机原始记录位于 `/home/dddqmmx/UniXNU/diting-work`：

- `logs/diting/services197/automatic-01`：首轮原始矩阵、NDJSON、trace 和报告。
- `logs/diting/services198/baseline-reanalysis.json`：首轮显示时间戳的离线重放与原 TE 统计更正。
- `logs/diting/services198/gpu-automation-host.log`、`automation-build.log`、`host-checks.json`：本轮主机与 helper 构建记录。
- `logs/diting/services198/automatic-01/report.md`、`report.json`、`junit.xml`：已完成的最终自动评估及原始NDJSON/USB/trace/CSV；最终报告返回2。
- `logs/diting/services198/first-state/state.json`、`resume-state-01/state.json`、`resume-state-01/gputest-status.raw.log`：匹配运行内核、安装状态与终态worker的独立只读证据。
- `logs/diting/services198/final-assessment.json`：引用原始部署、安装与自动矩阵的接续摘要；不修改旧回执。
- `build/services198/work-state.json`、`tmp/services198`：私有候选状态与构建/部署脚本。

物理合成帧呈现、input-to-photon 和其他应用颜色仍需对应层的实际证据，`hardware_performance_pass` 保持 false。后续修复沿 USB 自动测试取证，不再要求用户连续滑动或开关应用。
