# services195：GPU 完整候选与集中验收

更新时间：2026-11-15（Asia/Shanghai）。目标保持 **120 fps / 8.333 ms**。

本轮接续 services192 交接及 services193 第六版亮度修复。完整内核、runtime、主机检查和八核打包已经通过；services195 已稀疏写入 boot_b 并通过同镜像临时启动命令。系统内安装状态、连续动画和性能正在集中取证，**尚未达成 120 fps 验收**。Fastboot OKAY 仅表示传输与启动请求完成。

## 已定位并集中处理的问题

- services193 的真实 RAM 记录为 GPU 安装 stage1/error17/files-written0。只读 Recovery 确认 OS 中保留 services187 的合法 Diting 安装 ID `0912509e-45dd-4e4b-8852-2683ee553258`；运行内核携带另一安装 ID。旧安装器把版本差异当成外来 bundle，提前返回 EEXIST。因此先前候选的 runtime 改动未实际安装。
- 安装器区分所有权和版本，先解压验证全部候选，再以持久 intent、独立 stage 和 APFS 原子目录交换完成升级。发布标志最后提交；断电、辅助文件提交失败及发布 rename/fsync 失败保留真实错误并支持下一次启动续装。
- submit 的依赖 fence 在共享 gate 内只做零超时观察，等待在 gate 外进行，提交前重新检查连接和 fence，完成前不持有 BO 引用。
- 单 ring 的准入根据实际任务优先级决定，移除按连接分配顺序把后续客户端降为低优先级的规则；移除用户态后台进程排除名单。
- 已固定的 Turnip BGRA GMEM 传输修复保留，默认 `TU_DEBUG=nocb` 由 Turnip 按 pass 选择 GMEM/sysmem。`nocb` 表示禁止 concurrent binning。`diting_tu=1` 选择这一策略，不改请求刷新率或 GPU 电压。
- 增加按 Diting live DT 和固定 SM8475/SM8450 厂商源码适配的只读显示 reader、`display` USB 命令及 event26，读取已有 DPU/TE 计数器和已启用的 raw timestamp。不使能新计数器、不改面板模式。
- CDC 回复保留 END frame 空间，修正 snprintf 长度累计越界；panic 前输出有界、无分配、无 registry 锁、无 MMIO 的已完成 probe 快照。

## 离线检查与构建

services195 使用私有 runtime-source 和 XNU 缓存，不写共享 services169 缓存。构建采用单任务，沿用 services182 实测 RAM/FD/template 和 33,792 字节 DarwinBoot24，保持八核。

已记录主机测试 73 项通过，安装器 fixture 的编译错误修正后相关 17 项通过；UI 安装兼容性 9 组场景通过。安装断电、原子交换和失败恢复使用实际 host 文件，显示门控、时间戳撕裂、CDC 长度及 probe 并发快照使用 ASan/UBSan。工作流预检、Python 语法、补丁应用和差异格式检查通过。接续时复用这些结果，没有无故重跑。

完整内核 63,763,064 字节、未解析符号 0；新增 reader/probe 为强符号，对象无静态构造器。嵌入的 GPU/UI/ADSP 归档范围、实际 merged 构建入口、源文件 overlay、刷新 patch 和 loader 配对检查通过。顶层旧构建脚本在私有 overlay 中按设计替换为 selected merged recipe。

启动镜像 101,183,488 字节；实际启动参数 **1022 ASCII 字节**，低于 Darwin24 的 1023 字节上限，包含 `cpus=8 fb_hz=120 fb_beat=0 diting_gpu_trace=2 diting_tu=1`。按用户要求跳过镜像哈希、审计和设备镜像回读。

新 GPU 安装 ID 为 `9a56a71f-502b-4826-b713-2de5948d6a6d`。UI 归档仍为第六版 ID `3d4f894a-5231-44f3-8ed9-b552dc3cb1d1`，backlight 类布局、既有电池和 ADSP 源码保持配对。未替换 Data/SystemOS，未重写 userdata 镜像。

## 本机记录与验收边界

本机入口均位于 `/home/dddqmmx/UniXNU/diting-work`，生成产物和原始设备日志不发布：

| 用途 | 相对本机入口 |
| --- | --- |
| 候选状态 | `build/services195/work-state.json` |
| 完整内核与打包 | `build/services195/kernel`、`build/services195/boot` |
| GPU runtime | `build/services195/gpu-runtime/build.json`、`build/services195/gpu-stage/build-report.json` |
| 主机与最终离线检查 | `logs/diting/services195/host-checks.json`、`link-check.json`、`offline-completion.json` |
| 基线 USB / Recovery | `logs/diting/services195/baseline-usb`、`baseline-recovery` |
| 部署回执 | `logs/diting/services195/install-boot.json` |
| 本轮集中状态采集 | `logs/diting/services195/first-state` |
| 接续脚本 | `tmp/services195` |

部署于 `2026-11-15T03:59Z`：目标序列号 `1d4eb066`、product=diting、unlocked=yes、slot=b、boot_b=192 MiB。稀疏两块全部 OKAY（4.586 秒），同镜像 Fastboot boot OKAY（2.517 秒）。原始回执保留生成时点的未验收字段，后续设备证据单独记录。

120 Hz 请求、软件刷新通知、GPU copy 完成、TE 脉冲和控制器 frame counter 都不能独立证明新合成帧的物理呈现。显示 timestamp 只保存 raw 单位；在校准和对应实际呈现之前，解码器继续记录 `hardware_performance_pass=false`。连续动画、颜色及完整场景的尾延迟仍须本轮实测确认。
