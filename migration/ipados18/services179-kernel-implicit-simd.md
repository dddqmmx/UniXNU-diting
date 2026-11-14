# services179 Linux 内核隐式 SIMD 编译约束

2026-11-14。services178 实机仍黑屏并自动重启；Recovery 的 pstore 为空，
没有足够日志确定这次复位的直接原因。此前 services176 已确定异常 PC 位于
phystokv 的第一条编译器生成 NEON 指令，ESR 原值为 0x81000100。

## 具体证据与改动

services178 的 cpu_timebase_init 仍把五个整数回调/地址赋值合并为
ld1.2d/st1.2d/str q0；该函数由 arm_init_cpu 在 cpu_init 之后调用。
只约束 arm_init、arm_vm_init、psci_smp 三个文件，未覆盖后续内核和驱动。
当前 upstream AArch64 clang 的 -mkernel 驱动输出没有 -no-implicit-float。

Linux Diting 构建环境 CROSS_EXTRA_FLAGS 增加 -mno-implicit-float，使各原生
驱动遵守内核 FP 状态所有权。内核两份 build-xnu-ipados18.sh 在 Linux 下也
将该参数加入 RC_NONARCH_CFLAGS 与既有 stamp，强制旧 C/C++ 对象失效。
它禁止整数代码隐式借用 SIMD，保留明确使用的向量类型、intrinsics 和汇编。
既有早期整数寄存器约束、真实 fatal invariant、八核与硬件入口保持。

## 本地结果

本地链接通过，详细日志 diting-work/build/services169/build-20261113T181904Z.log。
compile_commands 中 1011 个 C/C++ 条目均包含 -mno-implicit-float。
新 cpu_timebase_init、phystokv 与 ArmCpu.o 的指令操作数没有 SIMD 寄存器。
同一编译器复现例：五个整数赋值由 q0 加载/存储变为 x8/x9 加载/存储；
显式向量相加仍生成 add.2d。证据存于 diting-work/logs/diting/services179。

八核镜像打包通过，101191680 字节；2026-11-13 18:23:53 UTC 稀疏刷 boot_b 两块 OKAY（4.655 秒），随后临时启动 OKAY（2.474 秒）。用户确认仍黑屏并自动重启；不能据此宣称已修复启动故障。
产物、日志及回执使用 diting-work/{build,logs/diting,tmp}/services179。
未写 userdata 或更换用户态。
