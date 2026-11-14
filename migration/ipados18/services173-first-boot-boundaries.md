# services173 首次启动调用链取证

2026-11-13，44e63bf 加既有未提交 Linux 适配及 CPU 启动诊断。

services172 实机为 `platform=6 cpu=11 error=0`。此处涵盖 boot CPU 与
所有 secondary 的首次启动提交，不能把阶段 11 当作 boot CPU 阻塞的证据。
本轮保持八核与原 configure 顺序；不调整开关、超时、root layout 或成功条件。
沿已定位调用链细分软件记录，避免通过替换配置猜测原因。

## 源码事实

- `psci_smp_init()` 只查询 SecureDT，无锁或等待。
- Darwin24 `processor_boot()` 取 cluster_powerdown_lock、processor_updown_lock；
  主核随后 thread_bind、thread_block，再 cpu_start → cpu_machine_init → initCPU。
- secondary 在 processor_dostartup 中设置启动状态，再 cpu_start → PSCI。
- IOCPUInterruptController::registerInterrupt 的全核等待已由现有 Diting patch 排除，
  ArmCpu::probe 返回空，显式 CPU 构建不会被自动匹配重复启动。
- Sirius 是 Darwin22 流程，不能直接用它的 initCPU(true) 替代 Darwin24 的原生
  processor_boot；本轮没有跳过 StartedState、ml_cpu_up、自 IPI 或原断言。

## 快照解释

原 panic 同一致命条件增加 `firstboot=CPU/STEP init=0x... irq=0x...`。
读取软件全局状态，无分配、锁、registry 遍历或 MMIO。步骤表示最后到达边界，
不是成功状态。firstboot 初值 255/0 表示尚未进入 processor_boot。
init 与 irq 每个十六进制位分别代表 CPU0 至 CPU7（低位为 CPU0）；
每核单独写自己的槽，panic 汇总，不会被别的核覆盖。

| firstboot STEP | 当前边界 |
| ---: | --- |
| 1 | 正在取 cluster_powerdown_lock |
| 2 | 第一把锁已取到，正在取 processor_updown_lock |
| 3 | 两把锁已取到，正在检查首次启动状态 |
| 4 | 正在检查/创建 idle thread |
| 5 | 正在检查/创建 processor IPC |
| 6 | idle thread 和 IPC 就绪，选择主/副核路径 |
| 7 | 主核正在 ml_cpu_power_enable |
| 8 | 电源入口返回，正在 thread_bind |
| 9 | thread_bind 返回，正在 thread_block |
| 10 | thread_block 返回，正在 cpu_start |
| 11 | cpu_start 返回，正在确认/更新 offline state |
| 12 | 正在恢复原 thread binding |
| 13 | 原 binding 恢复 |
| 14 | 正在 secondary processor_dostartup |
| 15 | secondary processor_dostartup 返回 |
| 16 | processor_booted 已设置，正在解 processor_updown_lock |
| 17 | 正在解 cluster_powerdown_lock |
| 18 | processor_boot 返回前 |
| 20 | secondary processor_dostartup 状态检查 |
| 21 | 正在取 processor_start_state_lock |
| 22 | 锁已取得，设置 processor_instartup |
| 23 | 正在取 sched_available_cores_lock |
| 24 | cores lock 已取得，正在取 pset lock |
| 25 | pset lock 已取得，更新 offline/state 并解锁 |
| 26 | 正在 ml_cpu_power_enable |
| 27 | 正在 ml_cpu_begin_state_transition |
| 28 | 正在广播 CPU_BOOT_REQUESTED |
| 29 | 正在 secondary cpu_start（含 PSCI） |
| 30 | secondary cpu_start 返回，正在确认 startup state |
| 31 | 正在 ml_cpu_end_state_transition |
| 32 | 正在广播 CPU_ACTIVE |
| 33 | processor_dostartup 返回前 |

| init 每核位 | 当前边界 |
| ---: | --- |
| 0 | 尚未进入 initCPU |
| 1 | 读取 PAC feature registers |
| 2 | secondary redistributor wake（主核跳过） |
| 3 | enableCPUInterrupt |
| 4 | enableGic |
| 5 | enableCPUTimeBase |
| 6 | setCPUState(Running) |
| 7 | psci_smp_cpu_running |
| 8 | 自 IPI 提交 |
| 9 | initCPU 返回前 |

| irq 每核位 | 当前边界 |
| ---: | --- |
| 0 | 尚未进入 enableCPUInterrupt |
| 1 | ml_install_interrupt_handler |
| 2 | handler 安装返回，正在取 vectors[0].interruptLock |
| 3 | 锁已取得，正在更新 enabledCPUs |
| 4 | 最后一核正在调用 cpusRunning |
| 5 | cpusRunning 返回，正在 thread_wakeup |
| 6 | 正在解 vector lock |
| 7 | enableCPUInterrupt 返回前 |

## 构建与设备

补丁在 services172 对应的准备树上 `git apply --check` 通过。完整 runtime
overlay 已生成，本地原生内核链接与八核预链接打包通过；镜像 101191680 字节。
日志在 `$DITING_WORK/logs/diting/services173/local-build.log`、`package.log`，
内核详细日志为 `$DITING_WORK/build/services169/build-20261113T053704Z.log`。

2026-11-13 05:38:17 UTC，电脑检测到设备已在 Fastboot，核对
serial=1d4eb066、product=diting、unlocked=yes、current-slot=b 后临时启动。
Sending 2.392 秒、Booting 0.114 秒均 OKAY，总计 2.522 秒。
回执在 `$DITING_WORK/logs/diting/services173/temporary-boot.json`。
没有持久刷 boot_b 或重刷 userdata；设备阶段尚待反馈，不能宣称找到或修复根因。

2026-11-14 用户反馈 `firstboot=7/29`，CPU7 正在 secondary cpu_start 调用中。
重启后未取得同轮 init/irq，不足以断定 PSCI/GIC 的具体停点。
继续处理见 services174-secondary-start-failure.md：PE 的 void 转发丢弃 startCPU 失败，
本轮修正真实失败处理并保留首个失败核的复位/异常上下文。
