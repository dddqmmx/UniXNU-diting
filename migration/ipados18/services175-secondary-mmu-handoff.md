# services175 副核 MMU 切换

2026-11-14，services174 用户反馈 `Diting CPU1 start timeout: reset=5 kernel=1`，
同一行 `TTBR1=0x906c06000`。PSCI 请求已经受理，复位代码已找到 CPU1 数据、
校验 start_cpu 并读取非零且 4 KiB 对齐的 cpu_ttep，尚未进入 arm_init_cpu。

源码确认：

- cpu_ttep 的根表由 arm_vm_init 的 alloc_ptpage(TRUE) 分配，后续 pmap 子表也动态分配。
- PSCI 启动前只清理 Mach-O 静态段和 cpu_data，这些动态页表不在该范围。
- 新核心进入时 MMU/cache 关闭；common_start 先恢复动态 TTBR1，再启用 MMU。
- 原 common_start 在启用 MMU 之前已经把 VBAR 切成正常内核向量；如果切换期间异常，
  当时还没有 current thread/cpu_data 上下文，无法可靠取得早期异常记录。

本轮补齐已有内核页表的 PoC clean：按当前 L1 根到 L3 叶表遍历，只跟随有效 table
descriptor，跳过 block/leaf 数据映射；先 clean 子表后 clean 父表，用已有 DSB SY
保证在 CPU_ON 前完成。读取 CPU 正在使用的实际 cpu_ttep，不增加固定物理地址，
不建立替代页表，不改变有效位、权限或映射。入口的 clean 与普通 pmap 更新没有新增锁依赖。

副核保留物理早期异常向量直到成功进入 KVA trampoline，再安装正常内核向量。
新增 reset=6（启用 MMU 后仍在物理执行路径）、7（已经进入 KVA trampoline）、
9（phystokv 已返回，准备进入 arm_init_cpu）；cache-on 的快照写入后 clean 该核独占行。
原 reset=5 和 kernel=1/2/3 保留；真实 CPU 超时和 fatal invariant 保留。

页表遗漏是可修正的源码事实，是否导致本次 CPU1 超时仍须此镜像实机验证。
本轮不修改八核拓扑、GIC/PSCI 参数、平台服务注册顺序或超时。

## 构建与设备

补丁在 services174 的实际准备树上 git apply --check 通过。
完整本地链接与八核打包通过，镜像 101191680 字节。
内核详细日志 `$DITING_WORK/build/services169/build-20261113T173537Z.log`。
2026-11-13 17:37:17 UTC（北京时间 11-14）核对 serial=1d4eb066、diting/unlocked/b、
boot_b 容量 201326592 字节后，稀疏刷 boot_b 两块均 OKAY，4.873 秒；
随后临时启动 Sending 2.318 秒、Booting 0.113 秒 OKAY，总计 2.456 秒。
不写 userdata，保留 services169 回退输入。
用户报告直接黑屏重启，没有新报错；无法确认 MMU 或 C 入口进展。
继续处理见 services176-secondary-vector-context.md。
产物、日志与临时脚本使用 `$DITING_WORK/{build,logs/diting,tmp}/services175`。
