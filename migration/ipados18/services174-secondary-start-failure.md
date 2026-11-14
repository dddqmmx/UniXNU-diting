# services174 副核启动失败处理

2026-11-14，基于 services173 用户提供的 `firstboot=7/29`。
CPU7 已进入 processor_dostartup 的 cpu_start 调用，尚未记录返回。
用户重启后旧画面丢失，没有同一轮的 init/irq 值；不根据此数值猜测 GIC 或 PSCI 根因。

源码确认 PE_cpu_start_internal 返回 void，直接丢弃 IOCPU::startCPU 的实际返回。
当前 PSCI 回调对每个核最多同步等 5 秒；失败后继续下一核，可累计越过
IOSecureBSDRoot 的 30 秒平台注册等待，覆盖首个失败的上下文。
Darwin24 processor_wait_for_start 本来要求所有注册核真实上线并在失败时 panic。

本轮在 Diting 的 PSCI 启动失败处执行同一必需 CPU 启动的 fatal invariant：
CPU_ON 被拒绝时直接报告实际固件状态；早期异常或 5 秒启动超时保留首个失败核。
没有把固件接受请求当作 CPU 上线成功，仍须进入真实 initCPU 和原生 scheduler。
没有改变核数、平台注册顺序、PSCI conduit、GIC 地址或原等待期限。
此修复解决失败被丢弃的问题；副核未上线的硬件/汇编原因仍待新镜像证据。

早期复位记录每核独占 64 字节缓存行，SM8475 按 MPIDR Affinity1 编号。
启动前清零当前核的行，由已有 __DATA clean 写回；副核在 MMU 关闭时写入，
启动失败后由发起核读取最新内存。panic 本身不新增 MMIO、分配或 registry 遍历。

| reset | 最后边界 |
| ---: | --- |
| 0 | 尚未写复位记录 |
| 1 | reset_vector 已安装早期向量，准备读取 ResetHandlerData |
| 2 | 找到匹配 cpu_data，后续 handler 字段保留实际入口 |
| 3 | reset handler 校验通过，准备跳转 |
| 4 | 进入 start_cpu，准备装载 boot args 和各核栈 |
| 5 | common_start 已读取 cpu_ttep，TTBR1 字段保留该值 |
| 8 | 未找到 cpu_data、handler 无效或 handler 意外返回 |

kernel 字段仍采用 PSCI 原记录：1 请求提交，2 进入 arm_init_cpu，3 initCPU running。
异常报告的 ELR/LR 为实机物理地址，应结合本轮加载基址换算，不能直接当作 Mach-O VA。

若仍触发原平台注册失败，附加 cpustart=核/步：40 cpu_start 入口；
41 副核 CoreSight 配置；42 cpu_data 缓存写回；43 PE 转发；44 PSCI 缓存准备；
45 CPU_ON 调用；46 CPU_ON 已返回；47 等待 running；48 running 成功；49 启动失败。

## 构建与设备

补丁在 services173 的实际准备树上 git apply --check 通过。
本地完整内核链接与八核预链接打包通过，镜像 101191680 字节。
本轮构建日志为 `$DITING_WORK/build/services169/build-20261113T172124Z.log`。
首次打包因误指 CI 容器的父目录失败；改为该 run 的实际 boot 文件路径后打包通过，
没有重编内核、修改配置或重跑 CI。
产物与回执位于 `$DITING_WORK/{build,logs/diting,tmp}/services174`。

2026-11-13 17:23:59 UTC（北京时间 2026-11-14）：核对设备 serial=1d4eb066、
product=diting、unlocked=yes、current-slot=b、boot_b 容量 201326592 字节。
使用 img2simg 后 `fastboot -S 32M flash boot_b` 两块发送/写入全部 OKAY，4.823 秒；
随后 `fastboot boot` Sending 2.336 秒、Booting 0.113 秒 OKAY，共 2.469 秒。
services169 本地原 boot 镜像作为必需回退输入保留；本轮不写 userdata。
回执 `$DITING_WORK/logs/diting/services174/install-boot.json`，完整响应 `fastboot-install.log`。
当前持久 boot_b 与运行镜像均为 services174，设备实际新报错/系统阶段仍待用户反馈。

用户反馈：`Diting CPU1 start timeout: reset=5 kernel=1`。首个未上线核为 CPU1；
CPU_ON 成功受理，已完成 reset handler 校验并读取 cpu_ttep，尚未进入 arm_init_cpu。
这排除了未受理 CPU_ON、未找到 cpu_data 和未进入 start_cpu；仍需区分 MMU 切换、
TTBR1 取值或其后的架构初始化。用户随后补充 TTBR1=0x906c06000；handler/entry 未抄录。继续修复见 services175-secondary-mmu-handoff.md。
