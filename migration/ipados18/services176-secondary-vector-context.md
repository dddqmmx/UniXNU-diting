# services176 副核正常异常向量的上下文

2026-11-14，services175 已本地链接、八核打包并刷 boot_b 后临时启动。
用户报告直接黑屏重启，没有新的可读内核报错；主机未取得 ADB/CDC 日志。
不能据此判定已越过 CPU1 的 MMU 停点或认定页表写回已解决根因。

services175 在 KVA trampoline 安装正常向量，但 phystokv、C 函数入口以及
早期 CPU 特性配置仍发生在 machine_set_current_thread 之前。正常异常入口依赖
TPIDR_EL1/current thread/cpu_data；这段空档仍应使用无需上下文的早期向量。

本轮把正常 VBAR 安装移动到 arm_init_cpu 的 machine_set_current_thread 之后。
保留真实上下文建立与所有架构初始化；没有跳过异常、配置或 fatal invariant。
新 C 入口快照直接按 MPIDR Affinity1 取槽，避免提前调用 cpu_number()/current_thread；
仅更新现有独占缓存行并 PoC clean，无锁、分配或 MMIO。

| reset | 最后边界 |
| ---: | --- |
| 16 | 已进入 arm_init_cpu C 入口，准备早期 PAN/架构特性配置 |
| 17 | current thread 已安装，正常 VBAR 安装完成 |
| 18 | cpu_init 返回 |
| 19 | PE_init_cpu 返回 |
| 20 | 准备 secondary_cpu_main |

此前 1–9 和原 PSCI kernel 阶段保持。异常仍由早期记录或真实 XNU panic 处理。
这个改动补齐异常上下文保护，尚不能解释 services175 的直接黑屏重启。

## 构建与设备

补丁在 services175 实际准备树上 git apply --check 通过。
本地内核链接与八核打包通过，镜像 101191680 字节。
2026-11-13 17:45:13 UTC（北京时间 11-14）核对 diting/unlocked/b 与 boot_b 容量后，
稀疏刷 boot_b 两块 OKAY，4.805 秒；随后临时启动 OKAY，2.516 秒。
不写 userdata。设备反馈 reset=7 kernel=1，已执行 KVA trampoline 的快照，
尚未确认 phystokv 返回或进入 C 入口。用户补充 early exception，ESR=0x81000100，ELR=0xfffffff0089b00b0；
地址对应 phystokv+0xc 的第一条 NEON（dup.2d v3, x0）。继续见 services177-secondary-fp-context-sync.md。
产物与日志使用 `$DITING_WORK/{build,logs/diting,tmp}/services176`。
