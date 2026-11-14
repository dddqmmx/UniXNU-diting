# services178 早期 C 启动的整数寄存器约束

2026-11-14，services177 已构建、八核打包、刷 boot_b 并启动；用户报告黑屏重启。
CPACR 同步修正的设备效果未确认，不能宣称它解决了第一条 NEON 异常。

services176 已有 PC 证据：CPU1 reset=7 kernel=1，ELR=phystokv+0xc，
指令 dup.2d v3, x0。Linux clang 自动向量化了早于 current thread/CPU 上下文的
PTOV 查找。既有构建约束只给 arm_init.o 加 -mgeneral-regs-only，arm_vm_init.o
里的 phystokv 没有同一约束，PSCI 新的 C 入口 helper 同样在上下文建立前调用。

本轮将同一整数寄存器约束补给 arm_vm_init.o 与 psci_smp.o。这两个文件没有
浮点算法或 NEON intrinsic；地址转换、页表和快照只处理整数。功能和结果不变，
避免编译器为这些早期入口隐式引入 SIMD 状态。保留实际 NEON 访问启用、ISB、
其他内核/驱动的 SIMD 实现、全部八核与原硬件入口。
对象模式 stamp 会使旧 arm_vm_init/psci_smp 对象失效，避免 Make 仅追踪源码时间
而未应用新的对象编译标志。公开脚本与实际 runtime overlay 选择的 merged 脚本同步。

## 构建与设备

本地内核链接通过，详细日志 `$DITING_WORK/build/services169/build-20261113T175936Z.log`。
arm_vm_init.o 的 phystokv 已全部使用整数寄存器，不再有 dup/ld3/fmov 等 SIMD 指令；
编译结果记录在 `$DITING_WORK/logs/diting/services178/phystokv-disassembly.txt`。
八核打包通过（101191680 字节）；2026-11-13 18:02:49 UTC 稀疏刷 boot_b 两块 OKAY（4.718 秒），随后临时启动 OKAY（2.469 秒）。用户确认仍会黑屏并自动重启。临时 Recovery 回读 pstore 为空；实际 ramoops 注册在 0xa7000000，不能据此认定未发生 panic。没有写 userdata。
产物与回执使用 `$DITING_WORK/{build,logs/diting,tmp}/services178`。
