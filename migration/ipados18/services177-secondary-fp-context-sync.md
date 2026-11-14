# services177 副核 NEON 访问控制同步

2026-11-14，services176 实机：CPU1 early exception，reset=7 kernel=1，
用户提供 ESR=0x81000100、ELR=0xfffffff0089b00b0。

本轮 native 与最终 prelink 的符号地址一致：phystokv 起始 0xfffffff0089b00a4，
故 ELR=phystokv+0xc，正是 `dup.2d v3, x0`，第一条 NEON 指令。
在其之前 sub sp、stp fp/lr 和 add fp 已执行；异常不在 CPU_ON、GIC 或 arm_init_cpu 入口。
Linux clang-24 的 arm_vm_init.o 中该函数含自动向量化的 PTOV 查找，随后还有 ld3/cmeq 等。

common_start 写入 CPACR_EL1=CPACR_FPEN_ENABLE 后没有 ISB，在 phystokv 前也没有
其他上下文同步。因此本轮为 Diting 补 CPACR 写入后的 ISB，确保首次 NEON 指令看到
启用的访问控制。没有跳过 phystokv、修改 PTOV 结果、关闭向量化或冒充 CPU 上线。
ESR 按用户原值记录，不将它未经核对解码为某个标准 FP exception class。
同步遗漏和第一条 NEON 的异常 PC 已取得证据；是否消除异常仍须修正镜像实机结果。

保留 services174 的首个真实 CPU 失败处理、services175 的动态页表写回，
以及 services176 建立 current thread 后安装正常向量的保护。

## 构建与设备

本地补丁应用、内核链接与八核打包通过，101191680 字节。
2026-11-13 17:55:32 UTC（北京时间 11-14）稀疏刷 boot_b 两块 OKAY（4.415 秒），
随后临时启动 OKAY（2.456 秒），不写 userdata。用户反馈黑屏重启；
未取得新 PC/阶段，不能确认 ISB 消除旧异常。继续修复见 services178-scalar-bootstrap.md。
内核详细日志 `$DITING_WORK/build/services169/build-20261113T175223Z.log`。
产物、日志和回执位于 `$DITING_WORK/{build,logs/diting,tmp}/services177`。
