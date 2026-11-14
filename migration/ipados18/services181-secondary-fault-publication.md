# services181：副核失败发布与首个 panic 保存

日期：2026-11-14；设备 Redmi K50 Ultra / diting / SM8475，序列号 `1d4eb066`；iPadOS 18.7.10 / 22H374，Darwin 24。

services180 的黑屏自动重启尚未修复。services181 已完成本地八核内核构建、打包、稀疏写 boot_b 和临时启动；用户确认仍黑屏自动重启，没有新的可读错误。故障尚未修复。

## 已确认的源码问题

- 原 PSCI 以 `stage >= RUNNING` 判断启动成功，数值更大的 OFF 也会通过。现只接受 RUNNING；`ArmCpu::initCPU()` 在本核 SGI 初始化返回后才发布该状态，原生 `processor_boot` 仍须确认调度器上线。
- stage 数组原来共用缓存行，CPU_OFF 和启动核的 cache maintenance 可能涉及另一核的数据。现每核使用独立的 64 字节行，并发布至 PoC。
- 原异常记录由所有副核共用。现每核使用独立 256 字节槽，记录首次异常；`valid=2` 是未完成，`valid=1` 才可消费。启动核遇到未完成记录继续等到提交或原期限。
- 异常记录补充 vector slot、SPSR、DAIF、CPACR、SCTLR、VBAR、TTBR0/1、TCR、MAIR、线程指针、MIDR、SP_EL0/1。在 EL1 通过当前 SP 读取 SP_EL1，避免非法的 `MRS SP_EL1`；异常处理不再读取可能触发原故障的 timer。
- cache-off 捕获使用 stores/DSB，cache-on 捕获清理全部四条缓存行后提交。CPU_OFF 若返回，单独保留返回值，不覆盖原异常。
- 真正 panic 的文本在 debugger/stackshot/IPI 工作前写入既有 pmsg 记录器，使用 `va_copy` 和递归保护；没有放宽 fatal invariant 或改写正常 panic 路径。

这些修改修复了状态判断和取证路径的具体缺陷，**尚不能确立黑屏根因**。services176 的 ESR `0x81000100` 仍按原值保留；其 EC=`0x20`，不能仅凭 PC 在 NEON 指令上将其认定为 FP access trap。

## 源码与构建

- 新增并置于 series 最后的补丁：`patches/xnu-ipados18/zzzzzzzzzzzzzzzzz-diting-secondary-fault-publication.patch`。
- `drivers/ArmCpu/ArmCpu.cpp`：RUNNING 发布边界。
- 两份 `build-xnu-ipados18.sh` 同步向 `psci_smp.o` 和 `debug.o` 传递日志开关，trace stamp 升至 `minimal-ramoops-v4-panic-snapshot`，切换开关时使相关对象失效。
- 准备树：`diting-work/build/services169/tree/.cache/xnu-work`，现在对应 services181，不能用它解释 services169 的旧 PC。
- 构建入口：`diting-work/tmp/services181/build-local.sh`；使用 `--jobs 12 --skip-image-verification`。
- 内核：`diting-work/build/services181/kernel/mach.development.diting`，`DITING_BUILD_RESULT.json` 的 `build_complete=true`。
- 完整内核日志：`diting-work/build/services169/build-20261113T202724Z.log`；包装日志：`diting-work/logs/diting/services181/build-local.log`。
- 八核打包：`diting-work/build/services181/boot/diting-xnu-initial.img`；loader 显式使用 `diting-work/build/services169/darwinboot/artifact/DarwinBoot24.efi`；日志在本轮 `package.log`。

补丁正向/反向应用检查、shell 语法及编译对象检查通过。编译后的 16 个异常入口间距为 128 字节、VBAR 基址 2048 字节对齐；psci/debug 对象无 SIMD 操作数。使用实际编译数据库命令关闭日志开关后，两者均可编译且没有日志符号依赖。证据：`diting-work/logs/diting/services181/build-validation.json`、对象反汇编和 `*-logging-disabled.log`。这些是源码/对象检查，按用户要求没有运行镜像哈希、镜像审计或设备镜像回读。

## services180 后的 Recovery 取证

临时启动既有 `diting-work/build/services171/recovery-log-reader.img`，root ADB 可用，data/metadata/cache 未挂载。证据保存在 `diting-work/logs/diting/services181/` 的 `recovery-*.json/txt/log` 和 `pstore/`。

pstore 仍为空。一个 malformed DT 节点失败，但实际 ramoops 后端已注册为 `0x400000@0xa7000000`，ECC=0。Recovery 没有 `/dev/mem`、`/proc/last_kmsg`，也不支持 debugfs（mount 返回 No such device）；`/dev/pmsg0` 存在。PON 历史有 warm/hard/button reset，但不能归因于某次 XNU 启动。

当前固件配置的 FD 基址为 `0xa7400000`、长 3 MiB；AGENTS 中 `0xa7100000` 的重叠描述是历史配置。RAM 是否保留、在哪一步被 bootloader/Recovery 覆写仍未确认，不能声称持久日志正常或没有 panic。

## 部署与实机结果

`2026-11-13T20:40:12.048853+00:00` 部署完成：核对 diting/unlocked=yes/current-slot=b/192 MiB boot_b 后，稀疏写入两块全部 OKAY（4.651 秒），随后临时启动 OKAY（2.453 秒）。持久 boot_b 现为 services181。回执为 `diting-work/logs/diting/services181/install-boot.json`，逐条日志同目录 `install-boot.log`；`hardware_boot_confirmed=false`。

用户确认仍黑屏自动重启，无新错误；已手动返回 Fastboot 并连接。回退候选 services169 保留，但未证明可正常进入系统。继续取证，不能把这轮作为已解决黑屏的候选。

本轮不写 userdata。用户已明确要求继续完成，早先交接中的停止指令是历史状态。

## services181 后的追加取证与 services182 接续

上述 Fastboot 状态之后，已临时启动 services171 的 Recovery 日志读取镜像。root ADB 可用，data/metadata/cache 未挂载；没有写入 userdata。取证文件在 `diting-work/logs/diting/services181/after-boot-recovery/`。

独立 tracefs 可挂载并使用 scoped kprobe；没有加载自制模块或绕过 CFI。ramoops 的两个 zone 正常映射，当前缓冲有 DBGC header 和 Recovery 日志，`old_log`/`old_log_size` 为零。所有诊断 probes/instances 均清理；tracefs 保持挂载。

向 `/dev/pmsg0` 写入短标记，然后软件重启到 Fastboot、再次临时启动 Recovery，标记仍未回读，pstore 仍空。对照在 `retention-control/`。**当前重启取证流程不可靠，不能用空 pstore 推断没有 panic，也不能确认由哪个阶段丢弃记录。**

曾按假定 XNU 物理加载基址读取四个 scalar，数值不一致，不能作为保留的 XNU 上下文。不要解码这些结果或继续盲扫 RAM。新读取的 `/sys/firmware/fdt` blob 已部分损坏；以下范围来自有效的 unflattened `memory/reg`，并与 services169 的 `live.dtb` 一致。

本机五个 RAM 区间为 `0x80000000+0x6a000000`、`0xf4600000+0xba00000`、`0x800000000+0x39700000`、`0x839d00000+0x1e00000`、`0x840000000+0x140000000`；区间和为 `0x1f0f00000`，独立于选用的 8 GiB nominal capacity。旧硬件 DTS 是单段 12 GiB 占位，旧内存 JSON 来自 services57a；固件源码的 `0x900000000+0x140000000` 结束于 `0xa40000000`，超出本机报告的 RAM 上界 `0x980000000`。

这是一项明确的源码配置错误，**尚未证明旧模板 FD 与该源码完全一致，也不能直接认定为黑屏根因**。同时确认固定上游固件 `DumpRamoops()` 只读取 console 的第一个 2 MiB，遗漏保存首个异常/panic 的 pmsg 第二个 2 MiB。两项问题及相关调用链由 [services182](services182-measured-ram-window.md) 同批处理。
