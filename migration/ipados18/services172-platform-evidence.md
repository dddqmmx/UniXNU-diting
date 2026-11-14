# services172 必要平台初始化取证

2026-11-13，本地 HEAD=44e63bf 加未提交 Linux 构建适配。

用户要求停止无依据地换配置试机。services171 的平台注册顺序改动已撤回，
此版本只增加实际执行阶段和失败码，恢复八核 ADT。没有关闭硬件入口、放宽
成功判定或忽略平台缺失；CONFIG_SECURE_BSD_ROOT 与原 assert(pe) 保留。
平台缺失时先用同一致命条件报告阶段，然后通过真实 panic 路径停止。

## 已取得的事实

目前实机确认的事实是 IOSecureBSDRoot 在 30 秒内没有得到完成 matching 的
IOPlatformExpert。八核和单核均出现同样的 pe 断言。源码及打包数据确认原生
platform personality、ArmDTPlatform/ArmCpu 构造入口、必要 ADT 节点都存在。
2026-11-13 用户从 services172 设备画面提供：

```text
platform=6 cpu=11 error=0
```

这证明 configure 已发布 arm-io nubs，显式 CPU 启动已注册全部中断 vectors，
但 first boot 提交循环尚未返回。阶段 11 覆盖 psci_smp_init、boot CPU 的
diting_cpu_first_boot，以及随后每个 secondary 的 diting_cpu_first_boot；
目前不能确定哪一个调用阻塞。error=0 仅表示未记录失败返回，不表示 CPU 启动成功。
psci_smp_init 只查询 SecureDT，不含等待、硬件调用或锁。

新增 panic 文本：

```text
IOSecureBSDRoot: missing platform expert; platform=N cpu=N registered=N gic=N error=N
```

通过弱引用的原生诊断函数读取五项软件状态，没有新分配、registry 遍历或
MMIO 读取。没有该原生诊断函数的配置仍使用原 assert(pe)。

| platform | 最后到达的步骤 |
| ---: | --- |
| 0 | 未进入 start |
| 1 | 进入 start，正在继承的 IOPlatformExpert 启动路径中 |
| 2 | 进入 configure，尚未通过必要 DT/NVRAM 检查 |
| 3 | 必要输入检查通过，正在继承的 DT 配置路径中 |
| 4 | 继承的 DT 配置完成 |
| 5 | CPU/总线枚举完成 |
| 6 | arm-io nubs 已发布，正在显式 CPU 启动中 |
| 7 | CPU 启动返回成功，正在电源入口中 |
| 8 | 电源入口返回，正在 Wi-Fi/注册路径中 |
| 9 | registerService 已排队，正在 GPU 入口中 |
| 10 | configure 返回成功前 |
| 11 | 继承的 start 返回成功 |

| cpu | 最后到达的步骤 |
| ---: | --- |
| 0 | 未调用显式启动 |
| 1 | 枚举 CPU nub |
| 2 | 选中 boot CPU nub |
| 3 | 校验 boot CPU 编号完成 |
| 4 | 正在映射并检查 DT GIC |
| 5 | GIC 映射和版本检查完成 |
| 6 | distributor 准备完成 |
| 7 | boot redistributor 准备完成 |
| 8 | IOCPU::start 完成 |
| 9 | boot processor 注册完成，正在注册完整拓扑 |
| 10 | 中断控制器发布完成 |
| 11 | 全部中断 vectors 注册完成 |
| 12 | 已提交 first boot，正在等待完成 |
| 13 | first boot 等待结束 |
| 14 | ml_cpu_init_completed 返回 |
| 15 | cpusRunning 通知完成 |
| 16 | ArmCpu::start 返回成功前 |

error 为实际失败码：-1 缺少/错误 CPU reg；-2 编号无效或已存在；
-3 GIC mapping/identity；-4 distributor；-5 redistributor；
-6 IOCPU::start；-7 CPU name allocation；-8 ml_processor_register。
registered 和 gic 是原驱动计数及 GIC 就绪状态，均不能用于伪造服务。

## 构建与部署

已使用完整 runtime overlay 和 Sirius Linux 工具链完成本地内核链接。
日志为 `$DITING_WORK/logs/diting/services172/local-build.log` 和
`$DITING_WORK/build/services169/build-20261113T050652Z.log`。
源码 `zzzzzzzzzz-diting-platform-diagnostic.patch` 的补丁应用检查通过。
首次本地构建因新补丁 hunk 行数错误退出；修正为真实的 11 行新增后本地链接通过。

已取得阶段数字，根因尚未确定。本轮不使用此前失败镜像作为成功证据。
保留 services169 持久 boot_b 与已安装用户态；services172 未持久刷写。

本地预链接与八核 v4 镜像打包完成，101191680 字节。重启恢复上下文后核对设备
已在 Fastboot，检查 product=diting、unlocked=yes、current-slot=b，临时启动
services172：Sending 2.373 秒、Booting 0.112 秒均 OKAY，总计 2.501 秒，
2026-11-13 05:11:17 UTC 完成命令。该次只采集上述实际阶段，不切换功能配置。
回执在 `$DITING_WORK/logs/diting/services172/temporary-boot.json`。
用户已提供上述阶段数字；USB 没有枚举，不能把 Fastboot OKAY 当作启动成功。
