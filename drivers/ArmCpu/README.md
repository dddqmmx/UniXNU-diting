# ArmCpu

Redmi K50 Ultra（diting / Qualcomm SM8475）的 ARM CPU provider：GICv3 中断控制器与 ARM generic timer，从核由固件 PSCI 启动。

## 适配硬件

- SM8475 八核：1× Cortex-X2（2.995 GHz 请求上限）+ 3× Cortex-A710（2.496 GHz）+ 4× Cortex-A510（1.8048 GHz），4+3+1 三个 EPSS 域
- GICv3 distributor/redistributor，19.2 MHz ARM generic timer
- EPSS 性能状态寄存器 0x17d91000 起，每域一页；只写固件已启用的域

## 仓库关系

上游 [UniXNU/ArmCpu](https://github.com/UniXNU/ArmCpu) 的 Diting 变体。相比上游，这一份面向 Darwin 22/24 重写了 GICv3 与 PSCI 从核启动路径，并新增仅 Darwin 24 编译的 `DitingCpuPolicy.h`（EPSS 请求、原生 LUT、4+3+1 拓扑）。

## 说明

- 启动参数 `diting_cpu_policy`：0 关闭投票，1 负载调频，2（默认）每域保持最高的非 boost 请求。EPSS/LMh 仍负责实际电压、频率与热限。
- `diting_cpu_frequency_diagnostic` 提供三个域的有序 RAM 快照：原生 LUT 上限、每核 PERF_STATE 请求、PDMEM、DOMAIN_STATE 与限频状态。USB 可读此快照，无需直接访问 MMIO；请求和 DOMAIN_STATE 不能代替实际时钟测量。
- 编译：`scripts/build-diting-armcpu*.py` 以 `DITING_DARWIN_MAJOR` 选择 Darwin 22 或 24 接口。

构建、接口与组件目录见 [UniXNU 组件目录](https://unixnu.com/reference/components)，板级输入与编译入口在 [UniXNU-diting](https://github.com/Chengdong-Yu/UniXNU-pong)。

## 实现细节

### 文件

- `ArmCpu.cpp`：`IOCPUInterruptController` 实现。GICv3 distributor 与 redistributor 映射、`ICC_SRE_EL1`/`ICC_PMR_EL1` 设置、SPI/PPI/SGI 分发、IPI 与 `rtclock_intr` 绑定。
- `DitingCpuPolicy.h`：仅 Darwin 24 编译。三个 EPSS 域的 0x17d91000 页、原生 LUT 回读、4+3+1 逻辑 ID 映射，以及 `diting_cpu_idle_mach` 的空闲钩子。
- `Info.plist`：`org.blu-sharky.ArmCpu`，匹配 `IOPlatformDevice`，probe score 2000。

### 启动顺序

`diting_cpu_start` 先由板级平台专家调用，完成 GIC 与 timer 后由固件 PSCI 拉起从核；从核进入 WFI 前会调用 `diting_cpu_startup_finished` 通知控制器。Darwin 24 上额外走 `diting_cpu_first_boot` 等待首个 processor 上线。

2026-11-13：增加 `armcpu_start_phase` 与 `armcpu_start_error`，记录实际 boot CPU 初始化阶段和原有失败返回点。它们仅为缺少平台专家的 panic 提供上下文，不更改 CPU/GIC 成功条件；阶段与失败码见 `migration/ipados18/services172-platform-evidence.md`。八核与单核的旧候选均仍触发同一个平台断言，不能据此将原因归为多核启动。

services172 实机为 `platform=6 cpu=11 error=0`，范围是首次启动提交循环，尚不能确定阻塞核。services173 加入每核 `armcpu_init_phase` 与原生 `processor_boot` 的锁/调度边界记录，panic 可直接读取软件快照；保持八核、真实首次启动与原硬件入口。构建、设备结果及阶段说明见 `migration/ipados18/services173-first-boot-boundaries.md`。

2026-11-14：services173 实机反馈 `firstboot=7/29`。services174 在内核 PSCI 实现中修正 Darwin24 必需 CPU 启动失败被 PE void 转发丢弃的问题：在首个真实失败处报告固件状态、早期异常或复位边界；CPU 运行成功条件保持不变。详细证据见 `migration/ipados18/services174-secondary-start-failure.md`。

services181 把本核 RUNNING 发布移到 `initCPU()` 的最后一步，避免 SGI 初始化尚未返回时被另一个核视为成功。PSCI 仅接受确切的 RUNNING 状态；OFF、未完成或属于另一 MPIDR 的异常记录仍是失败。每核异常槽保留入口号、SPSR、栈和页表上下文，原生 `processor_boot` 等待继续确认调度器上线。构建与实机结果分别记录在 `migration/ipados18/services181-secondary-fault-publication.md`。
