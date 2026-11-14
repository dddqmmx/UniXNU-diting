# services180 副核初始化的异常捕获覆盖

2026-11-14。services179 实机仍黑屏并自动重启，没有可读新错误。
其 Linux 隐式 SIMD 约束有本地指令证据，但尚不能解释或解决设备复位。

## 取证范围

services176 的早期捕获止于 machine_set_current_thread；后续 cpu_init、
cpu_timebase_init 直接使用正常 XNU 异常路径，当前黑屏没有新异常 PC。
实际副核启动设置 SP_EL1=cpu_excstack_top、SP_EL0=cpu_intstack_top；尚无
证据证明异常栈值错误，因此本轮不重写栈指针。

在 arm_init_tramp 已进入内核 pmap 后，将 LowSecondaryExceptionVectorBase
写入 VBAR 的 KVA，而不是沿用 MMU 关闭时的物理地址。这是覆盖 C 初始化
所需的地址契约：arm_init_cpu 启用 PAN 并在后续清除 TTBR0 的 V=P 映射，
捕获代码与其记录的 ADRP 必须经内核地址映射执行。

正常 VBAR 安装移至 PE_init_cpu 返回后、FIQ/timer 上下文和调度器入口前。
这条启动路径保持原有 DAIF 全掩码，未添加中断屏蔽或跳过架构初始化。
发生真实早期异常仍由已有 PSCI 记录/CPU_OFF 路径报告给等待的主核，主核
执行真实 panic；成功仍须进入 ArmCpu 并报告 PSCI_SMP_CPU_RUNNING。
这是缺少错误 PC 时的取证补齐，尚未证明自动重启根因。

| reset | 最新完成边界 |
| ---: | --- |
| 17 | current thread 安装完成，继续保留早期向量 |
| 21 | 准备 wfe_timeout_init |
| 22 | wfe_timeout_init 返回 |
| 23 | pmap_clear_user_ttb 返回 |
| 24 | flush_mmu_tlb 返回 |
| 25 | cpu_machine_idle_init 返回，准备 cpu_init |
| 18 | cpu_init 返回 |
| 26 | cpu_timebase_init 返回 |
| 27 | 准备 PE_init_cpu |
| 19 | PE_init_cpu 返回 |
| 28 | 正常 VBAR 安装完成 |
| 20 | 准备 secondary_cpu_main |

## 本地与设备

新补丁在 services179 实际 XNU 树 git apply --check 通过，本地链接与八核打包通过（101191680 字节）。原生对象反汇编确认 KVA 早期 VBAR 设置，正常向量安装在 PE_init_cpu 之后且位于 FIQ/timer 之前。详细内核日志 diting-work/build/services169/build-20261113T182905Z.log。
产物、日志及刷入回执使用 diting-work/{build,logs/diting,tmp}/services180。
八核与现有功能配置保留；不写 userdata 或更新用户态。


2026-11-13 18:31:27 UTC（北京时间 11-14）核对 diting/unlocked/b 与容量后，
稀疏刷 boot_b 两块发送/写入全部 OKAY（4.762 秒），随后临时启动 OKAY
（Sending 2.349 秒、Booting 0.113 秒，总计 2.484 秒）。用户报告仍黑屏自动重启，
没有新的可读报错；故障未解决，未确认进入任何新增 reset 阶段。

用户要求停止本轮尝试并交接，完整状态见
[交接文档](交接-2026-11-14-services180-黑屏自动重启.md)。
