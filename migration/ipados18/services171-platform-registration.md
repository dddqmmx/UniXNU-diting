# services171 平台注册修复候选

2026-11-13，基于本地未提交的 services169/170 构建适配，HEAD 为 `44e63bf`。

用户报告 `IOKitBSDInit.cpp:1112 assertion failed: pe`，PC
`0xfffffff000f814cc`，LR `0xfffffff000f81430`。匹配源码为
`IOSecureBSDRoot()` 等待 `IOPlatformExpert` 30 秒后断言。该等待要求服务完成
matching；存在平台对象或已排队 `registerService()` 都不足以满足它。

源码排查确认 ArmCpu 的自动 `probe()` 返回 nullptr，不能据此认定 CPU 重复启动。
所用 ADT 的根名为 device-tree，包含 cpus、arm-io、defaults、options 与
8192 字节 CHRP NVRAM proxy，原生平台 personality 也已编入内核。
这些输入证据不能证明实机 configure() 成功。

ArmDTPlatform 新增 start()，仅在继承的启动路径和必需的 CPU/GIC 配置成功后
注册服务。可选电源、USB、Wi-Fi 和 GPU 启动移到独立线程，该线程有界等待
实际 ArmDTPlatform 完成 matching。必需步骤的错误保留，并补充阶段日志。
保留 CONFIG_SECURE_BSD_ROOT、IOSecureBSDRoot 的断言和原生硬件驱动。
这是针对初始化依赖顺序的修复候选，根因及实机效果仍需日志确认。

## 本地构建

使用 Sirius 的隔离 Linux 工具链，完整 runtime recipe overlay，复用
services169/tree 编译缓存。kernel/native archives 本地链接完成；复用已修正
USB 参数解析的 services170 DarwinBoot24.efi，本地完成预链接和 v4 打包。
八核、A730、QCA6490、电源与 USB 功能开关保留；使用 services169 已安装卷 UUID，
没有更新 userdata。按用户要求跳过镜像哈希、审计与设备回读。

生成物和日志：

- `$DITING_WORK/build/services171/kernel/`
- `$DITING_WORK/build/services171/boot/diting-xnu-initial.img`，101191680 字节
- `$DITING_WORK/tmp/services171/build-local.sh`
- `$DITING_WORK/logs/diting/services171/local-build.log`、`package.log`
- 内核详细日志 `$DITING_WORK/build/services169/build-20261113T043936Z.log`

## 设备尝试

核对 serial=1d4eb066、product=diting、unlocked=yes、current-slot=b 后，
`fastboot boot` 发送 2.318 秒、Booting 0.114 秒均 OKAY，总计 2.450 秒。
回执为 `$DITING_WORK/logs/diting/services171/temporary-boot.json`。
此结果只证明临时启动命令被接受，不能证明系统启动、平台 matching、USB 或 UI。
持久 boot_b 仍为 services169；未写入 services171。

用户随后确认八核版本仍是原来的 `IOKitBSDInit.cpp:1112 / pe` 断言，并提供照片。
同一内核的单核 ADT 也经临时启动出现同样报错，不能用关闭多核解决。
可选外设启动顺序修改没有取得根因证据，已在 services172 源码中撤回。
用户要求停止穷举后，停止试启动候选，改为记录必要初始化的实际阶段。

临时 Recovery 使用 services168 boot_a 备份中的真实 Linux 5.10.256 内核与
Diting TWRP v0.1 ramdisk；禁止自动启动 Recovery GUI，移除 data/metadata/cache
挂载条目。实际 ADB shell 可用，挂载列表不含这些分区，但 pstore 为空。
Linux 日志确认 ramoops 是 0x400000@0xa7000000，console 与 pmsg 各 2 MiB、ECC=0，
因此不能把没有日志归因于 Linux 没有注册 ramoops 驱动。
本轮仅将 modem_b FAT 只读挂载到 /tmp，获取真实 ADSP split ELF 输入到本地，
未写入运行时系统卷；该输入后续需按加载器的 ELF 契约组装。
