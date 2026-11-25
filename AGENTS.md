> 键盘停止交接（2026-11-18）：用户明确要求停止修复、把当前状态写入文档；后续“继续”仅接续被打断的文档整理。键盘修复已停止，GPU优化与Wi-Fi开发继续暂停。最后匹配临时services226 / keyboard-dock-20261118-01，GPU/UI/Wi-Fi均stage6/error0，UI写3项，GPU/Wi-Fi写0；持久boot_b保持198。完整八核内核、UI、链接、打包与临时启动PASS；一次采集21583trace/43样本/30动作无missing/skipped/drops。右侧全宽与左下globe在截图中已改善，但Spotlight主线程15.137s user+1.130s system/17.062s，Preferences6.276s+0.857s/7.745s，最后原生触摸计数分别6/6与2/2，输入延迟仍FAIL_INPUT_LATENCY，完整布局命中及物理帧率未验收；采集窗无MPS SIGABRT。226原生点/mm从0按真实70×155mm面板补为5.809524，听写刷新缓存命中未解决CPU满载。停止复核09:28Z时CDC/Fastboot均未枚举，当前设备模式无法确认，不能当作新的226匹配复核；末次worker job1/state3/quiescing0，无遗留主机构建/打包/采集。本次未重启或刷写，不commit/push。用户恢复修复前不启动新候选或测试；恢复后正常优先级/jobs8、私有源码/缓存，保留gmem,nocb/八核/ST61Y/电池/ADSP/Wi-Fi/Data/SystemOS，不reset/clean共享缓存、覆盖独立修改或写boot_b/userdata，跳过镜像哈希/审计/回读，不启动子代理。常规匹配安装、临时Fastboot boot与有界USB键盘验证已有授权；停止请求当前优先。入口：[交接-2026-11-18-services226-键盘布局改善-延迟未解决.md](migration/ipados18/交接-2026-11-18-services226-键盘布局改善-延迟未解决.md)、[services214–226键盘记录](migration/ipados18/services214-216-keyboard-latency.md)。以下“当前运行/继续/构建中”均为历史时点。

> 最新停止状态（2026-11-17）：用户认为当前性能“已经很不错”，明确要求暂停GPU优化、写文档并commit/push。手机保留临时运行services213；完整八核内核／链接／打包／GMU/CP与匹配USB均PASS，GPU安装6/0/9，UI/Wi-Fi均6/0/0，持久boot_b仍198。212颜色矩阵142项PASS、16个不同完成帧未见精确红蓝互换，用户确认212正常界面“闪烁已消失”；213颜色复测按暂停请求跳过。213选择gmem,nocb并加入事件84实际render-mode计数；一次UI评估功能3/4 PASS、设置前台前提FAIL，120Hz运动观察未通过，物理120fps仍UNQUALIFIED。最后匹配213、worker job1/state4/quiescing0，无活动测试或后台构建/采集。GPU优化已暂停，用户恢复前不要启动新候选或测试。保留独立工作、私有源码/缓存、GPU/UI/Wi-Fi包、键盘、八核、触控、电池、ADSP与Data/SystemOS；不写boot_b/userdata，不reset/clean或改共享缓存，跳过镜像哈希/审计/回读。恢复后正常优先级/nice0/jobs8。最新入口：[services213暂停交接](migration/ipados18/交接-2026-11-17-services213-GPU优化暂停.md)。以下“继续开发/当前运行”和旧停止段落均为历史时点。

> 最新交接状态（2026-11-16）：用户要求停止试验、整理文档交接。手机最后确认临时运行 services208 / gpu-refresh-solid-20261116-01，持久 boot_b 保持 services198；208完整内核、链接、八核打包、临时启动与匹配USB均PASS，GPU/UI/Wi-Fi安装均stage6/error0/written0。软件通知间隔p95由207的75.046ms降到208的8.348ms，计划期限至回调迟到p95为0.023542ms，1626次通知均成功、invalid/skipped周期均0，但仍有未归因长间隔。用户确认SSPP常量源对照为“稳定的纯蓝色”，六个原值及flush恢复PASS；该对照绕过GPU/DDR且改变UNPACK，不能排除正常fetch/缓存/扫描问题。用户随后确认正常画面“颜色和闪烁仍异常”；地图、卡顿和120fps未解决。自动UI功能4/4 PASS、动画三场景均FAIL，31,545条trace无missing/skipped/drops/recovery，PID43 CP执行p95为8.117760ms，物理新合成帧UNQUALIFIED。services209 / gpu-fetch-order-20261116-01仅完成私有源码/缓存与主机检查，增加有界display fetch-swap对照，内核/链接/打包/上机均NOT_STARTED。没有遗留构建/部署/采集进程，本次未启动新候选或写分区。入口：[2026-11-16完整任务交接](migration/ipados18/交接-2026-11-16-services208-GPU显示与性能未达标.md)。

> 当前工作约束：Wi-Fi暂停；用户已取消低优先级，恢复开发后用正常优先级/jobs8，旧SCHED_IDLE/nice19/ionice idle/jobs1描述失效。保持私有源码/缓存、GPU/UI/Wi-Fi包、键盘/八核/触控/电池/ADSP及Data/SystemOS，不reset/clean或覆盖独立修改，不写共享缓存，跳过镜像哈希/审计/回读。208/209完整实现主要在工作区根目录的diting-work，尚未完整同步主仓库。此前常规构建、匹配安装、临时Fastboot启动和有界USB诊断已有授权；本次停止请求优先，用户恢复开发前不执行交接文档中的候选命令。DDR实际频率与持续容量仍未测定。以下services207/206/205/198等“当前运行”或“构建中”段落均为历史时点。

> 最新 GPU/显示接续（2026-11-16）：当前临时运行 services205 / gpu-panel-colour-20261116-01，持久 boot_b 保持 services198。完整内核／链接／八核打包／临时启动与匹配USB状态已通过，GPU／UI／Wi-Fi均stage6/error0/written0。真实DCS c5/10/00与捕获DT选择匹配，panel stage6/error0/after2/batches3，校准timestamp节奏121.039Hz；address_mode00、pixel_format07、colour_writes0。用户最新纠正“比如地图还是有问题。整个画面还是有很多错误甚至颜色闪烁”；地图、整体颜色与闪烁均未解决，不能沿用此前“地图没问题”的判断。USB自动UI功能4/4通过，三场景动画均FAIL；PID43 GPU执行p95桌面6.248ms／设置10.003ms／多任务7.793ms，copy间隔p95为24.299／34.787／38.562ms。LLCC量化1ms峰值19.071／42.795／38.011GB/s，均已恢复；DDR因enable0与filter_mask2拒绝借用（mask802/writes0），实际频率与持续容量仍未测定。Wi-Fi暂停，优先颜色／闪烁及明显卡顿，继续私有源码／缓存、SCHED_IDLE/nice19/ionice idle/jobs1，保留键盘资源、八核、触控、电池、ADSP及Data/SystemOS。物理120fps仍未通过。入口：[services203–205 显示与性能接续](migration/ipados18/services203-display-performance.md)。以下services198等段落是历史时点。

> 当前GPU自动化状态（2026-11-15）：持久boot_b与匹配运行内核为services198。完整内核/八核打包、稀疏写boot_b和临时boot已完成，GPU安装ID `544c1397-c1d5-4b57-bad8-6642003bf1e4`，stage6/error0/written8/step17；UI第六版ID `3d4f894a-5231-44f3-8ed9-b552dc3cb1d1` 保持stage6/error0。一次USB自动矩阵执行97项（auto35/sysmem31/gmem31），固定颜色60项与GPU负载18组通过；桌面、设置、多任务动画评估均FAIL，校准panel VSYNC时间戳节奏60.489Hz，120fps/8.333ms目标仍FAIL。27085条trace，missing/skipped/kernel drops/recovery均0，物理新合成帧UNQUALIFIED。用户要求通过USB自主测试和评估，不再请求手动滑动/开关应用；先解决整批可预见问题及主机检查，再进行下一候选的一次实机矩阵，不重测同一实现。入口：[services197/198 USB GPU自动化](migration/ipados18/services197-gpu-automation.md)。原始报告在 `diting-work/logs/diting/services198/automatic-01`；06:13Z只读复核匹配198，daemon16/error0，末job3/state3/quiescing0，无活动worker。源码HEAD e6c7a9f，包含大量未提交差异；私有198源码/缓存，未改共享缓存，Data/SystemOS与独立电池/ADSP保留，镜像哈希/审计/回读继续跳过。以下193与192内容均为历史时点。

> 历史亮度/图标任务状态（2026-11-15）：当时持久boot_b与运行内核为services193的 `ui-brightness-20261115-06`，UI安装ID `3d4f894a-5231-44f3-8ed9-b552dc3cb1d1`、stage6/error0。第四版用户确认“亮度正常图标不显示”；第六版原生设置BKS事务低/高/中/恢复均通过实际DCS回读，failures0。图标第四版视觉FAIL，第六版缓存读取修复已刷入，打开设置后的图标日志和视觉验收仍待完成；当轮熄屏恢复未验收。入口：[services193亮度与图标](migration/ipados18/services193-ui-brightness.md)。当时GPU安装stage1/error17/files-written0，此安装错误已由后续GPU候选替代；不能将193历史值用作198当前状态。该独立任务使用私有源码/缓存、nice19/ionice3/jobs1，未改GPU/USB/电池/ADSP源码或共享services169缓存。

> 历史交接状态（2026-11-14）：持久 boot_b 与最近临时启动均为 services192，完整内核、八核打包和部署已完成。系统内 USB 链路已打通，192 枚举 ffff:0128 /dev/ttyACM0，完整 status 与两份有界 gpu 时钟回复 PASS、command_errors=0。services190/191 已实测实时日志、真实 RAM 读取、坏地址拒绝及 ACK 后自主回 Fastboot；192 未重复重启。ADSP stage13/error0、SCM0:0、PMIC_online1；电池 stage4/percent100/flags0x7，界面电量、插拔、PCM、Mux/Finder 与长期稳定未验收。

> GPU 性能仍 FAIL，目标为120 fps / 8.333 ms，不能改为60 fps验收。services190 用户确认动画仍跳到终态；匹配运行内核的 gRefreshHz 已实测为120，旧“60 Hz对照”说法过时。services191 最新反馈“还是极其卡顿”，连续USB采集16792条事件，已检查范围无缺失、kernel drops=0，提交至退役p95=17.576ms、CP执行p95=15.472ms。services192 只增加GPUCC/GMU/RBBM只读取证，尚无新的UI/触控/流畅度验收；频率投票、软件通知和GPU copy不能证明物理120fps。当前交接入口：[services192 USB与120fps进度](migration/ipados18/交接-2026-11-14-services192-USB已打通-GPU120fps未达标.md)。

> 保持services186 ST61Y、services182实测RAM/FD/template/33792字节loader及services187真实GPU runtime（安装ID 0912509e-45dd-4e4b-8852-2683ee553258），Data/SystemOS保留。设备仓库HEAD bd21ea7，有大量未提交修改；共享services169 XNU缓存现在对应192。保留各轮产物和未实机验收的显式186软件回退候选，独立电池/ADSP改动必须保留，不reset/clean。用户已授权构建、稀疏写boot_b、临时启动、Recovery取证和有界USB/RAM诊断，不重复询问常规许可；继续使用--skip-image-verification，跳过镜像哈希、审计和镜像回读。本次用户要求文档交接，手机留在192系统，没有运行中的构建/部署/采集、活动goal或代理任务。

> services186 历史基线（2026-11-14）：用户确认进系统、触屏点按和滑动正常，熄屏后亮屏触控恢复正常。八核，未更新 userdata，双指手势和长期稳定尚未测试。入口：[`services186-st61y-touch.md`](migration/ipados18/services186-st61y-touch.md)。源码 4d478a5 已推送；7 项 host 测试、两个日志关闭编译模式、完整内核和八核打包通过。

> services185 历史状态（2026-11-14，后由 services186 替换）：当轮已稀疏写 boot_b 并临时启动，用户确认“进系统了”，随后报告触屏不工作。保持八核，未更新 userdata；长期稳定性和各外围功能未验收。历史入口为 [`services185-bdf-power-boot.md`](migration/ipados18/services185-bdf-power-boot.md)；最新状态见[任务交接](migration/ipados18/交接-2026-11-14-services186-启动与触控正常.md)。

# Diting probe policy

> 当前状态以本文开头及 [services208任务交接](migration/ipados18/交接-2026-11-16-services208-GPU显示与性能未达标.md) 为准，services197/198和services192记录保留历史证据。`D:\ai\diting\交接-2026-11-11-准备用户态.md` 与 `D:\ai\diting\DeepSeek执行计划-进系统与Diting整理-20261111.md` 是历史路线。下方带日期的段落是历史实机与构建记录；其中旧 boot_b、缓存和设备模式不代表当前状态。

- Optional probes must record PASS, FAIL, SKIP or RUNNING and return to the normal boot path. Never terminate boot with panic, halt or an unbounded loop to display a probe result.
- On failed prerequisites, log the reason and skip dependent MMIO/DMA work. Do not fabricate successful services, media or hardware resources to keep boot moving.
- Save bounded result snapshots through DitingProbeRecord. The early panic hook must print completed snapshots before the real panic message, without allocating, taking registry locks or accessing hardware.
- Preserve real kernel panic handling and functional invariants. A failed optional probe does not authorize suppressing a genuine fatal kernel error.
- Keep Diting DT/hardware constants specific to SM8475. Refer to Sheng for implementation structure and adapt its SM8550 values before use.
- Prefer temporary fastboot boot for device trials. The user has also authorized entering Android to read partitions and clearing userdata/other required partitions for this port. Use that authorization when a concrete installation step needs it; verify target geometry and keep required boot/firmware backups before destructive writes.
- After completing a build and installing any matching runtime update, temporary fastboot boot is the default authorized next step. A later explicit request to hold a particular image takes precedence. Leave the device running after boot for the user's observations.

# Local build preference

- The user explicitly requested skipping image hashes, image audits and device image readback verification. Use `--skip-image-verification` for kernel/boot builds and the default non-verifying Diting userland workflow. Do not run verification or benchmarks unless the user asks.
- Keep generated candidates and runtime artifacts under `/Volumes/DitingDev/build/servicesNN`, logs/install receipts under `/Volumes/DitingDev/logs/diting/servicesNN`, and temporary task scripts under `/Volumes/DitingDev/tmp/servicesNN`. Never write these into the user's Downloads directory.
- The user requested deleting superseded temporary candidates and logs. Retain current deployment metadata and required compiler/firmware inputs; retire the previous candidate after the replacement is deployed. Keep original project sources and user scripts.
- For system updates, the user requests changed parts only. Refresh the unmounted guest OS with `scripts/sync-diting-os-for-update.py` before local edits, then use begin/stage/finish and install changed bands. Keep REC quiescent until the update is installed so guest filesystem changes are preserved. Do not blindly overlay old host APFS metadata after a guest boot.
- DarwinBoot24 and the selected XNU boot_args ABI have a 1024-byte command-line array, including the NUL terminator. Keep the assembled ASCII boot command at most 1023 bytes. Shorten optional log filters instead of changing the ABI or silently truncating functional boot arguments.

# Repository layout

- This is the Diting SM8475 format 2 device repository, targeting iPadOS 18.7.10 / 22H374 / Darwin24. Follow https://unixnu.com/zh/porting/migration and https://unixnu.com/zh/porting/device-repository.
- Keep the ARM64, SM8475, OS profile and board layers separate. The versioned SM8475 seed is under migration/format2/soc-seed; local overrides belong in ignored local.toml.
- Preserve community component identity DitingPlatform24. Do not label the A730/QCA6490 implementation as the upstream A740/WCN7850 driver.
- Publish source/configuration/porting records only. Apple restore binaries, generated ABI dumps, firmware, userdata, credentials and build artifacts stay local.
- Record host build results separately from device results in porting.toml. A successful build or Fastboot command is not evidence of working Wi-Fi, PCM audio or buttons.
- Keep XNU_DITING_POWER independent of Wi-Fi. Shared RPMh/SMMU locks belong to DitingPlatformResources, and Darwin24 links native4K helpers once through libDitingSmmu4K.a.
- Native public prerequisites are pinned in configs/diting-native-sources.toml. Use scripts/fetch-diting-native-sources.py and ignored local.toml paths; do not reintroduce host volume paths into recipes.

# Diting packaging status (2026-11-06)

- Keep scripts/package.sh identical to the UniXNU template and use device driver submodules. The workflow pins the previously used UniXNU revision and installs the device OS profile and component catalog before packaging.
- The pinned upstream core receives a temporary generic kernel-component patch; board.toml selects DitingPlatform24. The workflow uses the CI configuration, native-only kernel staging and the volume-layout artifact from the specified run. This integration has only local tests, not a successful macOS run. The layout must match the userdata image retained from that run. Do not treat the output as device-tested.
- Diting firmware is a reference, not validated Diting firmware. Record host builds and hardware results separately.
- The Diting firmware FDF separately embeds `Platforms/Nothing/ditingPkg/FdtBlob/sm8475-nothing-diting.dtb`; it identifies Nothing Phone (2), regardless of the Diting DTB generated for the boot image. Do not claim that the firmware itself carries a Diting tree.
- Diting's firmware region at 0xA7100000–0xA7400000 overlaps the Diting ramoops reservation at 0xA7000000–0xA7400000. Diting firmware is not a Diting boot candidate, even if it compiles; derive and validate a Diting memory map before device testing.
- `uefi-mondrian-reference` is an independent Xiaomi SM8475 firmware reference, not a Diting port. Its documented UEFI menu result does not establish XNU boot; its DarwinBoot targets Darwin 22, while this project targets Darwin 24.
- The reconstructed Diting DTS retains incomplete board resources and an unverified RAM declaration. `dtc` success is not evidence that the original DTBO, UEFI tree, memory map or peripherals are correctly adapted.

# Diting Wi-Fi 构建进展（2026-11-07）

- Actions 运行 42350648631 的内层日志显示，所用 Sheng main 引入了与设备清单不同的 Ath12kWifi，31 个编译单元因此找不到 `linux/kconfig.h`；Linux 稀疏检出还缺少 MHI 和 QMI 源码。
- `scripts/prepare-ci-wifi.py` 根据 `configs/diting-native-sources.toml` 固定 Linux、Sheng 和 Ath12kWifi，检查 ath11k、MHI、QMI、shim 及头文件。工作流先独立编译 Wi-Fi，再进入较耗时的内核构建；检查日志会随失败产物上传。
- 使用清单中的 Linux 和 Ath12kWifi 固定版本，本机 Linux 交叉编译已生成 arm64 Mach-O 对象及 `libDitingAth11k.a`。macOS/Xcode、内核最终链接、固件加载和 Diting 真机 Wi-Fi 仍需分别验证。
- 这是本地未提交修改；在 33 授权提交推送前，远端 Actions 不会使用这些改动。请勿把编译归档或占位卷布局当成设备功能证据。

# Diting 实机 panic 与构建检查（2026-11-08）

- 33 的照片显示 XNU 已进入 IOKit。greenA 内核 PC 0xfffffff00793ef74 对应 OSMetaClass::postModLoad，访问的 FAR 为 0xfffffff00da591e0，恰好等于 IOBlockStorageDevice::gMetaClass 符号 0xfffffff00da591d0 加 0x10；该地址不属于 greenA Mach-O 的任何映射段。
- 存储绑定由 prepare-ipados18-storage.py 根据 22H374 内核和固定 prelink 基址 0xfffffff00b000000 生成，board.toml 当前 slide 为 -0x1000000；greenA 实际 __PRELINK_TEXT 为 0xfffffff004130000。检查原始输入和 bindings 之前不要盲改 slide，也不要继续把 greenA 用于真机。
- 新增 scripts/check-storage-bindings.py，工作流在 v4 镜像组装前检查最终内核的 IOBlockStorageDevice 绝对符号是否落在映射段，方法地址另检查可执行权限。greenA 实测报 9 个不匹配，内存中模拟已映射符号的正向检查通过；这只是静态范围检查，不证明类结构、映射或启动成功。尚未提交或运行 CI。
- 另需修复 greenA 内核内 Diting ramoops 的 0x85200000 地址和 Diting 实机 0xa7000000 保留区不一致，并核对与固件区间的冲突及 pstore 格式。未经核实不要将保留区改成任意地址。
- 2026-11-08：根据 greenA 已映射段和旧配置反推的 22H374 起始地址，候选 slide 设为 0x5ed0000，使预链接基址对齐存储绑定的 0xfffffff00b000000；Diting 两份配置暂时关闭 Diting 固定物理地址的 ramoops；实际 payload 启用 -v，manifest 从 payload 配置读取相同参数。提交 43e14aa 已推送并启动 Actions 42404848268；CI、生成映射和真机启动均待验证。
- Action 42404848268 在内核链接失败：关闭 ramoops 后 kern_exit.o 仍引用 diting_ramoops_diagnostic_write。提交 92b3ef7 将诊断入口和调用按同一构建开关编译，并把宏传入 kern_exit.o；两种开关状态的预处理检查通过。新 Action 42407447526 已启动，尚未通过 CI 或上机。
- Action 42407447526（92b3ef7）成功：内核以 0x5ed0000 预链接，9 个存储绝对绑定落入映射段，实际 boot args 含 -v；Wi-Fi 仅通过预编译。尚未在 Diting 测试该镜像。后续提交 7fa0fb9 只处理 ramoops 开关切换后的增量对象失效，未被本轮 CI 覆盖。
- 33 要求刷入后，Action 42407447526 的镜像已写入 Diting 的 boot_b（Fastboot 写入 OKAY）；当前活动槽仍为 a。随后同一镜像经 fastboot boot 临时启动，命令返回 OKAY；OS 实际状态未确认，暂未发现 ADB 或串口。未通过 Fastboot 写入 boot_a 或 userdata。
- 33 随后提供的实机画面显示 corecrypto/pthread 的入口范围校验失败，接着 `IOPlatformExpertDevice::generatePlatformUUID` 调用 `sha1_init`，空指针访问 0x28 导致 panic。对 CI 42407447526 内核分析发现组件 Mach-O 的入口落在组件代码段，但父内核的 `__PRELINK_INFO` 结束地址低于这些组件；Darwin 24 将其用作静态地址上界，`ml_static_slide` 可能将高地址段清零，解释范围校验失败，需用下一轮实机验证。不要通过跳过 OSKext 检查掩盖。
- 手机重启回 Android 后，ADB root 直接检查 `/sys/fs/pstore`，目录为空；Android 实际 ramoops 参数为基址 0xa7000000、总长 0x400000、console 与 pmsg 各 0x200000、ECC 和 record_size 均为 0。该轮 XNU 因先前禁用写入器不能回读其日志。当前修改正使用设备配置为 Diting 传递该基址，并修复预链接信息位置；构建和设备日志回读尚待验证。
- Action 42411722700（6d1299b）通过并已临时启动，33 报告出现内核文字或 panic；随后 Android pstore 为空。内核反汇编确认日志地址 0xa7000000。向 Android `/dev/pmsg0` 写入短标记并正常重启后，pstore 仍为空；Android dropbox 未找到记录，原因未确认，不能把持久日志回读标为成功。该轮没有覆盖 boot_b。
- 33 明确允许清空 userdata 安装；但必须等匹配的 22H374 OS、SystemOS、Data 完整就绪、恢复写入环境可用、Diting UFS 和实际几何经过核对后才执行，不能提前擦除。当前 Android `/data` 为 F2FS，实测 userdata 4K 首 LBA 0x2f8a00、末 LBA 0x3adf3fa。
- CPU 诊断镜像越过 AppleARMCPU，现停在 Failed to mount root device。其 ADT 缺少 ufshc；当前 Diting UFS 固定使用另一机型的 userdata 几何，且设备尚未安装 SHNGROOT/APFS。Diting UFS 几何适配在独立 fork 准备中，OS、SystemOS、Data 不在当前 CI 成品中。
- 557e4f8 的 CPU 匹配修正在 Action 42416561746 通过；同等本地诊断镜像已真机越过 AppleARMCPU，停在根设备挂载。Diting UFS 分区范围适配已在独立仓库 nagatoquin33/diting-ufs 提交 4fe0173（unixnu topic），主仓库 c67bc72 接入该子模块、发布 UFS 节点和实测边界；安装准备期间保留只读。Action 42419087356 验证中，尚未声明 UFS 实机工作。

# Diting bring-up state (2026-11-08)

## Target
- Device: Redmi K50 Ultra `diting`, SM8475 (Cape family), Android 16, bootloader **unlocked**
  (`fastboot getvar unlocked` = yes; the `ro.boot.flash.locked=1` property is spoofed), active slot `_a`.
- Target OS: iPadOS 18.7.10 / 22H374 / Darwin 24, built as `DEVELOPMENT_ARM64_DITING` (MACHINE_CONFIGS=DITING).

## What is proven
- CI `package` runs green end to end: kernel links (`mach.development.diting` ~17.8 MB), payload, three containers.
- Diting's own UEFI (`ditingPkg`, RELEASE, 3,145,728 B `SILICIUM_UEFI.fd`) builds in a Linux job and is packed
  into `boot/diting-unixnu-uefi.img` (kernel slot = gzip(112 B BootShim + FD) + DTB, ramdisk = PONGFAT1 envelope).
- On hardware, `fastboot boot` of that image reaches the UniXNU menu, renders at 1220x2712, finds block IO
  (`blockio=1 volumes=1`) and DarwinBoot returns `HANDOFF EXIT RETURN 0`. The kernel then dies silently.

## Open blockers
1. Kernel death after handoff: no screen output even with `-v debug=0x144`, `/sys/fs/pstore` is empty.
   Next: firmware log via `Tools -> Advanced Logger` (QR, OneSend) and/or a real UART.
2. Wi-Fi: sources compile locally (`libDitingAth11k.a`, 2,981 symbols) but CI stops in
   `build-diting-wifi-native.py` because the build reads `device-tree/build/diting/hardware.dtb`, not
   `DITING_HARDWARE_DTB`. The live tree (`/sys/firmware/fdt`) plus `platform-firmware/dtb-fixups/apply.py`
   provides PCIe/PHY/GPIO; the refgen clock is at `freq[10]` with `clock_names[8]` = `pcie_phy_refgen_clk`.
3. Wi-Fi firmware is checked in under `platform-firmware/wifi/` (amss.bin 5,470,128, amss20.bin 5,296,128,
   m3.bin 266,684, regdb.bin 24,278), pulled from `/vendor/firmware_mnt/image/qca6490` on the device.

## CI notes
- `.github/workflows/package.yml` has two jobs: `firmware` (ubuntu, builds the Diting FD and uploads
  SILICIUM_UEFI.fd + BootShim.bin + mkbootimg.py + diting.toml) and `package` (macos-15-intel, needs firmware).
- Before pushing, run `scripts/precheck-workflow.py` (bash -n on every run block, duplicate env keys,
  and every `--flag` against the script it targets). Both mistakes that broke earlier runs would be caught.
- `DITING_HARDWARE_DTB` is only meaningful for the Wi-Fi step; the kernel still uses the device-tree component's
  `hardware.dtb`.

## Artifacts
- `D:\ai\diting\artifacts\ci-20261107-diting-firmware\diting-unixnu-uefi.img` (first hardware boot, sha ac5aa923).
- `D:\ai\diting\build-work\greenB\...\diting-unixnu-uefi.img` (Tools menu enabled, sha FB2867EE).

- Apple 22H374 的 OS、SystemOS、AppOS 已在 WSL 本地提取/解密并只读挂载，未上传。用户态 helper Action 42458543571 成功，输出在 `D:\ai\diting\build-work\runtime-42458543571`。用户态 APFS 写入仅通过小样文件、属主与权限的独立读取；完整卷及设备均未验证，尚无可刷入的 userdata 成品。

## 2026-11-08 17:2x — Advanced Logger / firmware CDN

- On hardware the menu works, `Tools` is present (payload entry), but `Tools -> Advanced Logger`
  shows `Advanced Logger: no log`.
- Cause: the plugin reads through `AdvancedLoggerAccessLibGetNextFormattedLine`, which does
  `LocateProtocol(gAdvancedLoggerProtocolGuid)`. That protocol is installed by the DXE_CORE
  instance of `AdvancedLoggerLib`, and Mu's stock `Mu_Basecore/MdeModulePkg/Core/Dxe/DxeMain.inf`
  never consumes `AdvancedLoggerLib`, so the buffer is never created.
- Fix in the firmware job: insert `AdvancedLoggerLib` into `[LibraryClasses]` of `DxeMain.inf`
  before `[Guids]`, idempotently, in place (a patch failed because CI checks out Mu_Basecore
  3c9c7b2e, not the workstation revision, and the patch file lost its CRLF).
- Current blocker: the firmware job cannot fetch its external dependencies. Both
  `basetools-v2026080000.0.1.tar.gz` and `OneCrypto-Accelerated.zip` return **HTTP 504** from the
  GitHub release CDN, also from this workstation with curl, so it is an upstream outage, not our
  configuration. Retries (3x) and longer backoff do not help; the build resumes when GitHub serves
  the assets again.



## 2026-11-08 19:1x — ext deps vendored

- Run 42474456806 still died in stuart update with HTTP 504 for the same two assets. From this
  workstation the same URLs returned the real gzip and zip.
- platform-firmware/vendor/ now carries those two upstream archives. seed-ext-deps.py checks the
  sha256 values from Mu ext_dep descriptors and unpacks them into Mu-Basetools_extdep and
  onecrypto-bin_extdep with extdep_state.yaml, so stuart update does not download them.
- The DxeCore AdvancedLoggerLib link has still not been compiled.


## 2026-11-08 19:5x — nested Mu submodules

- Run 42477512954 seeded the ext deps, then Build.py failed: MdePkg.dec include
  Library/MipiSysTLib/mipisyst/library/include is not in the workspace.
- stuart setup skips a dirty submodule. Inserting AdvancedLoggerLib dirties Mu_Basecore
  before setup, so the recursive checkout of mipisyst never runs. The firmware job now
  initializes Mu_Basecore submodules before that edit.

## 2026-11-08 20:3x — logger firmware packaged

- Run 42478126479 on 4570412 is green. The firmware job checked out mipisyst
  370b5944, seeded the vendored ext deps, and linked DxeCore with
  DxeCoreAdvancedLoggerLib. SILICIUM_UEFI.fd is 3145728 bytes, sha256
  b6782ca101d0fd52d55a6e9e40c072ad4af240ac8ae69bbefcddc9b62a9359cb.
- Package artifact images-ipados-18.7.10-22H374. The fastboot candidate
  boot/diting-unixnu-uefi.img is 101109760 bytes, sha256
  e0a4bd1f1d026dadc78346ef8f2fa4467675a11bdedebdaba3bed8d565e6bbe2;
  its fd_sha256 matches that volume. hardware_tested is false.
- check-storage-bindings.py reported 9 storage bindings inside mapped
  segments, with prelink info after the components. Runtime behavior is
  still unchecked.
- No partition was written. Tools -> Advanced Logger is still a device
  observation; this build only creates the DXE logger buffer.

## 2026-11-08 20:4x — temporary boot of the logger image

- fastboot boot of the run 42478126479 image
  diting-unixnu-uefi.img, 101109760 bytes, built from 4570412.
  Host copy:
  D:\ai\diting\artifacts\ci-42478126479\...\boot\diting-unixnu-uefi.img.
- Device 9e0c2b55, product diting, model 22081212C, unlocked yes,
  current slot a. Sending and Booting both returned OKAY.
  No partition was written and the slot was not changed.
- Fifteen seconds later the phone was not in fastboot and not in
  Android adb. The host did not observe the screen. Tools ->
  Advanced Logger is still unread.

## 2026-11-08 20:5x — Advanced Logger was empty

- Temporary boots of run 42478126479 still showed Advanced Logger: no log.
  The menu reads only the Advanced Logger buffer. RELEASE builds pass
  -D MDEPKG_NDEBUG, and DebugLib.h turns DEBUG() into if(FALSE), so the
  buffer stays empty. On-screen DARWINBOOT lines are Print(), which never
  enters that buffer.
- The firmware build now undefines MDEPKG_NDEBUG, keeps DEBUG_ERROR as the
  runtime filter, retries a missed logger LocateProtocol, installs AdvLogDxe
  before DarwinBoot, and mirrors the boot-refused / inputs-validated Prints
  into DEBUG_ERROR.

## 2026-11-08 21:3x — logger fix booted

- Run 42482636331 on 585b305 is green. The firmware log shows the locate
  retry and the DarwinBoot DEBUG_ERROR mirrors, and DarwinBoot.c was
  compiled with -U MDEPKG_NDEBUG after -D MDEPKG_NDEBUG.
- SILICIUM_UEFI.fd sha256
  91554cbc12a1f40d313a52f0c04b6bb17616709f73918780cc5f61a43ffdb9a7,
  3145728 bytes. diting-unixnu-uefi.img is 101191680 bytes, sha256
  d49888c27b41235db026de8beee25844bee2ed00bb785900a309e479635e321e.
- fastboot boot on 9e0c2b55, product diting, unlocked yes, slot a.
  Sending and Booting returned OKAY. No partition was written.
  The Advanced Logger screen for this image was not read from the host.


## 2026-11-09 — the storage path is the wrong driver (documentation audit)

`unixnu.com/zh/porting/ufs`, `/zh/reference/components` and `/drivers/sheng-ufs` state the supported
storage implementation and its integration; the device repository does not follow it yet:

| requirement | documentation | this repository |
| --- | --- | --- |
| driver source | `UniXNU/ShengUfs` vendored at `drivers/ShengUfs` (catalog id `ShengUfs`, revision e2b32cc) | `drivers/NativeUfs`, imported by hand from UniXNU-diting |
| publication module | `RootPublication.h`, `RootWindowRead.h`, `GccUfs.h` | absent |
| boot argument | `native-ufs-root=1` | not used anywhere in the repository |
| component selection | `[components] include` contains `ShengUfs`, kernel links `component:ShengUfs` | `include = ["DitingPlatform24", "XnuPrelink"]` |
| compatible | `qcom,sm8550-ufshc` (board constant to adapt) | Diting constants `diting,ufs-*` |

Bring-up stage list (`/zh/porting/bring-up`) puts us at **stage 6**: the kernel reaches the platform
expert and IOKit, then panics `Failed to mount root device`. Stage 6 requires the block driver to
print link speed and the command queue, publish `OS` and `SystemOS`, and match
`root_match_volume_uuid`. The imported Diting probe driver never publishes root media, so the current
driver cannot pass stage 6 regardless of tuning.

Next: vendor `drivers/ShengUfs` at e2b32cc, add `ShengUfs` to `[components] include`, link its
archive (catalog aliases `build/native-ufs/libNativeUfs.a` to `build/sheng-ufs/libShengUfs.a`), add
`native-ufs-root=1`, and adapt the board constants (diting: controller 0x01d84000, SMMU 0x15000000,
stream 0xe0, SPI 265, UFS compatible `qcom,sm8475-ufshc` — verify against the live FDT).


## 2026-11-09 02:30 — storage experiments (what is actually true now)

- The imported `drivers/NativeUfs` is already adapted for diting: `kGccUfsPage = 0x00187000`,
  `kUfsHost = 0x01d84000`, `kUfsStreamId = 0xe0`, ADT properties `diting,smmu-base = 0x15000000`,
  `diting,smmu-size = 0x100000`, `diting,smmu-context-banks = 78`, `diting,smmu-match-groups = 120`,
  `diting,ufs-spi = 265`. The ADT generator writes the ufshc node with
  `compatible = qcom,sm8475-ufshc\0qcom,sm8450-ufshc\0` (make-diting-adt-ipados18.py:244), which is
  exactly what the driver's `compatible()` accepts, so matching is not the blocker.
- Swapping in the supported ShengUfs component is **not** a board.toml edit: `[kernel]
  builder_component = DitingPlatform24` makes the core skip `build_archives()` entirely, the kit recipe
  rebuilds `drivers/NativeUfs` unconditionally from `DITING_NATIVE_DRIVER_SOURCE` and unlinks any alias,
  and the archive the kernel links is hard-coded at three sites in the **merged** copy
  `kits/DitingPlatform24/build-support/merged-scripts/build-xnu-ipados18.sh` (the repo copy is not what
  runs). A real switch needs a `UFS_DRIVER`/`UFS_ARCHIVE` parameter, a `--driver` option in
  `build-diting-ufs-ipados18-macos.py`, VerifiedRoot/VirtioMmioBlock include paths, and the
  bindings.json producer to keep writing `mac-build/native-ufs/bindings.json` (Adreno platform reads
  it). Until that is done, tune the imported driver.
- `native-ufs-root=1` must **not** be set while the imported driver is used: the core rejects the
  payload with `native UFS root boot needs exactly one sheng-pmgr=2`, and that PMGR personality is
  not part of this board.
- Current experiment (build 1c04458): `diting_ufs_stall=1` in `kernel_args`. On a UFS failure the
  driver logs the `formatUfsRootDiagnostics` block and then holds the CPU (`IOSleep` loop), so the
  last console lines stay on the panel for a photograph. `panic()` cannot be used: it is a macro in
  this SDK and the symbol is not linkable from the archive.

## 2026-11-09 03:20 — device state and what to look for

- The phone was written with `build-work/userdata/userdata-root.img` (SHNGROOT + OS + SystemOS,
  manifest sha f237878b…) over adb (`dd` to `/dev/block/sda37`), verified by reading back the first
  eight bytes. Android's fastboot cannot do this write on this host: the 32-bit fastboot aborts with
  `std::bad_alloc` on a 10 GiB image, and `-S 256M` does not help.
- The image booted is `diting-unixnu-uefi.img` from run 42509554099 (sha256 F2EDDF7F…, 101,191,680 B),
  built from 1c04458. It contains `diting_ufs_stall=1`.
- Expected panel states and what each means:
  * a `DITING_UFS ...` report with gdsc/axi/ahb/cap/ver/hcs/hce lines and the boot stops → the driver
    matched but the transport or the power/clocks gate failed; the values name the failing gate;
  * `DITING_UFS PROBE FAILED: <property> ...` → a board property check failed, the name is the answer;
  * no stall and the root-mount panic returns → the transport succeeded and the failure moved to the
    root volume (UUID or manifest), which is a different problem;
  * the menu again → the payload never handed off.
- The stall build is deliberately sticky: the phone stays as it is until someone reboots it.

## 2026-11-09 03:40 — the ADT the driver receives is correct

Added the reusable `scripts/dump-diting-adt.py` (parses the Apple device tree out of any payload
image without knowing the FAT layout; `--all` lists blobs, `--index` selects one).

`payload.img` from run 42509554099 carries **two** device-tree blobs:

```
[0] 0xc3800   13732 bytes  7 top-level nodes   (arm-io/ufshc has only reg/compatible/interrupts)
[1] 0x3227800 14684 bytes  7 top-level nodes   (the one with the board properties)
```

Blob 1, `/device-tree/arm-io/ufshc`, carries exactly what `drivers/NativeUfs` validates:

```
compatible                qcom,sm8475-ufshc qcom,sm8450-ufshc
diting,ufs-hc-base          0x1d84000
diting,ufs-hc-size          0x3000
diting,gcc-ufs-page         0x187000
diting,smmu-stream-id       0xe0
diting,smmu-base            0x15000000
diting,smmu-size            0x100000
diting,smmu-context-banks   0x4e
diting,smmu-match-groups    0x78
diting,ufs-spi              0x109
unixnu,userdata-first-lba 0x2f8a00
unixnu,userdata-last-lba  0x3adf3fa
```

So matching and every board-property check pass; the remaining suspects are the GCC power/clock gate
(`0x187004` bit 31 and the AXI/AHB/aggregate enable bits) and the transport itself. Both are reported
by the stall build now on the phone.

## 2026-11-09 03:55 — the persistent log channel already exists

The firmware reads a ramoops ring at `0xA7000000` on every boot and dumps its tail as `RAMOOPS|`
lines through `DEBUG()` (`Platforms/Xiaomi/UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.c`,
`DumpRamoops`), which is exactly what the Advanced Logger plugin exports.

Header layout, shared by both sides:

```
uint32 sig   = 0x43474244   ('DBGC')
uint32 start
uint32 size
// then capacity = 0x200000 - 12 bytes of ring, written by the kernel
```

The kernel side is `osfmk/arm64/diting_ramoops_console.h` from `patches/xnu-ipados18`:
`diting_ramoops_console_init()` maps `CONFIG_XNU_DITING_RAMOOPS_BASE` (0xa7000000 in both diting
defconfigs) plus 4 MiB, writes the magic and a `DITING DIAGNOSTIC v92` line, and every later console
write is mirrored into the ring.

Consequence: the console of a hung boot — including every `DITING_UFS` line — should survive in DRAM
and be readable from the firmware on the **next** boot, without a screen and without a QR scan of the
panic screen. The one observation so far (`RAMOOPS: sig=0x1400382E size=0x0`) predates any kernel run
that had the mirror enabled, so the channel is untested rather than broken.

Next device session, in this order:
1. reboot the stalled phone,
2. `fastboot boot` the same image again (the firmware dumps the previous ring at startup),
3. `Tools -> Advanced Logger`, scan with OneSend, send the `.log`.

That file should contain the kernel console tail of the stalled run.

## 2026-11-09 03:45 — root UUID verified against the shipped images

Added `scripts/apfs-volume-uuid.py` (lists every APFS volume superblock of a raw container with its
UUID and name; offsets from apfsprogs `include/apfs/raw.h`: magic 32, uuid 0xF0, name 0x2C0).

Measured on the images this port actually ships:

```
OS.dmg        volume b07df049-7ad9-47b5-9f8f-9ee728056398  name CrystalUpdate22H374.J171J172OS
SystemOS.dmg  volume 4a77ba00-2826-44c4-b883-1fa92ac36880  name CrystalUpdate22H374.J171J172SystemCryptex
AppOS.dmg     volume 78e7965a-108b-4dc9-a7a4-7ed503838e4f  name AppCryptex
```

The OS volume UUID equals `profile.toml os_volume_uuid` and the payload's `root_match_volume_uuid`
(`b07df049-7ad9-47b5-9f8f-9ee728056398`), so a published OS window is mountable by UUID.

**Discrepancy to fix:** `profile.toml systemos_volume_uuid` records
`D44F8E57-4171-4C6B-B21A-1808F6EF2E20`, which is the UUID of a *prepared* SystemOS volume made by the
macOS prepare step; the window we flash is the raw `SystemOS.dmg` whose volume is
`4a77ba00-2826-44c4-b883-1fa92ac36880`. Nothing consumes the SystemOS UUID for the root mount, but
the record is wrong for the image we ship and must be corrected before the SystemOS volume is
resolved by UUID.

`profile.toml systemos_volume_uuid` corrected in the same commit: `check-ci-volume-layout.py`
compares the layout's measured UUIDs against the profile, and the window this port flashes is the raw
`SystemOS.dmg`, whose volume is `4a77ba00-2826-44c4-b883-1fa92ac36880`, not the UUID of a macOS-prepared
copy.

## 2026-11-09 04:30 — reading the UFS report (ready-made mapping)

The stall build prints `formatUfsRootDiagnostics` before it freezes; this is what each field means
and what to do next. Line names come from `drivers/NativeUfs/RootDiagnostics.h`.

| field | meaning | next action |
| --- | --- | --- |
| `powered=0` (GDSC bit 31 clear at `0x187004`) | the firmware handed off with the UFS PHY power domain down | enable `gcc_ufs_phy_gdsc` (0x187004) and the PHY branches before reading the host, or make the payload keep it up |
| `buses=0` (AXI 0x187018 / AHB 0x187020 / AGGRE 0x1870d4 enable bits clear) | the clock branches are gated | enable the three branch bits, then re-read; `hwcg` may still report CLK_OFF on an idle branch, so judge by the enable bit only |
| `cap=0`/`cap=ffffffff` or `ver` out of range | the host block is not clocked (or not mapped) | same as above; do not trust any other field until `ver` is sane |
| `hcs` bit0 clear | the device is not present on the link | link/firmware handoff problem, not a driver bug |
| `hcs[3:0] != 0xf`, `db != 0`, or `is != 0` | the controller still owns commands from the previous owner | the request list is not ours to take; find who left it running (the bootloader's UFS driver) |
| `stage=` | exact step in `hostBringUp`/`RootQueue`/`RootCommands` | read the matching source line |
| `manifest=` | `RootLayout` status: 0 ok, 1 no magic, 2 version/count, 3 hash, 4 bounds, 5 names, 6 overlap, 7 attributes | the flashed image's SHNGROOT block; we verified ours, so a non-zero here means the partition was overwritten |
| `GPT/read lba=… userdata=…` | the GPT entry the driver resolved | must equal 0x2F8A00..0x3ADF3FA |

Two variants are built for the next session:

- `main` (2e6fc90): `diting_ufs_stall=1`, freezes on a UFS failure so the panel keeps the report.
- `no-stall` (12a8596): identical but without the stall, so the boot continues to the root-mount panic.

Both are otherwise the same kernel; the stall only decides whether the failure is frozen on screen.

Both next-session images are built and downloaded:

| variant | run | path | sha256 (first 16) | boot arg |
| --- | --- | --- | --- | --- |
| stall | 42509554099 | `build-work/greenD/.../boot/diting-unixnu-uefi.img` | `F2EDDF7F1B7B0466` | `diting_ufs_stall=1` |
| no-stall | 42519844089 | `build-work/greenE/.../boot/diting-unixnu-uefi.img` | `589710228B605371` | — |

The `no-stall` branch (12a8596) holds the variant; main keeps the stall build. The branch was rebuilt
after the first attempt accidentally committed `git commit -a` gitlink deletions for the drivers that
are not checked out locally (`drivers/ArmDTPlatform`, `DarwinBoot24`, `NativeRamBlock`, `DitingAdreno`,
`DitingBootCapture`, `DitingMetal`, `DitingPower`), which made that CI checkout fail with a missing source
file. Only `board.toml` is in the branch commit now.

## 2026-11-09 08:10 — why the manifest failed, and the fix

The panel from the stall run reported:

```
GPT/read lba=2f8a00 userdata=2f8a00..3adf3fa magic=474e4853 544f4f52 manifest=3
WINDOWS found=0 prepared=0 published=0 bytes=0 policies=0 verify=ffffffff
DITING_UFS: no media published; continue boot
```

So the UFS path itself works: GPT parsed, the first userdata block was read, and its `SHNGROOT` magic
matched. `manifest=3` is `RootLayout`'s header-SHA failure, and the local image still hashes exactly
(`ae82e3ea…` stored and computed) — the block on the phone was changed after the write.

Cause: the image was written with `dd` from a running Android and then `adb reboot bootloader`. That
is an orderly reboot, so Android unmounts `/data` and f2fs writes its superblock at offset 1024 of the
partition, which sits inside the SHNGROOT block — the magic survives, the hash does not.

Fix: write the partition **from the bootloader**, with Android out of the picture. 32-bit fastboot
cannot take the 10 GiB raw image, so `scripts/make-sparse-image.py` encodes it with the Android sparse
format (645 chunks, 10,809,777,752 bytes) and `scripts/flash-userdata-image.py` checks the product and
unlock state, flashes that, and can boot the payload in the same step.

Fallback if the sparse flash is refused: `dd` the raw image, then set the bootloader control block to
`boot-fastboot` in `/dev/block/by-name/misc` and force an immediate reboot (`echo b >
/proc/sysrq-trigger`), so Android never unmounts `/data`. Not used yet.

## 2026-11-09 21:15 — userdata written from the bootloader (sparse)

`fastboot flash userdata userdata-root.sparse.img` with the phone in the bootloader succeeded where
the raw image failed: 14 sparse chunks, every `Sending`/`Writing` line OKAY, total 276 s. Android never
ran, so nothing could rewrite the SHNGROOT block.

Pre-flight facts checked before the write: `fastboot devices` -> 9e0c2b55, `product=diting`,
`unlocked=yes`, `partition-size:userdata = 0x37E69FB000` (240,092,426,240 bytes = the 223.6 GiB the
board records), sparse encoding round-trip verified against the raw image on the host.

Then `fastboot boot diting-unixnu-uefi.img` (stall variant, F2EDDF7F) was sent: `Sending ... OKAY`,
`Booting OKAY`. The panel is the only remaining witness; the phone dropped off adb, which is what a
running payload looks like.

## 2026-11-09 22:20 — **root volume mounted** (stage 6 reached)

Three-window userdata image (OS 6,845,104,128 at 0x100000; SystemOS 3,963,617,280 at 0x198100000;
Data 8 GiB GPT+APFS at 0x285000000; image 19,411,238,912 bytes) written from the bootloader with
`fastboot flash userdata` on the sparse encoding (25 chunks, 492 s), then `fastboot boot`.

Panel evidence:

```
DITING_UFS: OS root binding UUID b07df049-7ad9-47b5-9f8f-9ee728056398 bound to OS UFS window /dev/disk2s1
DITING: APFS: diting
DITING_UFS: wait for SystemOS UUID D44F8E57-4171-4C6B-B21A-1808F6EF2E20, timeout=5s
DITING_UFS: SystemOS UUID wait timed out
DITING_UFS: SystemOS media identity published on /dev/disk2s1
DITING DEVELOPMENT: scratch Data mount SKIP error=-6 stage=0
Diting init: Loading helper /diting/init
Diting init: helper exec result=2
panic: Diting init helper exec failed, errno 2 @kern_exec.c:7067
```

What each line establishes:

- the OS window is published, the APFS root volume is mounted, and the kernel resolves it by UUID;
- the SystemOS wait used the **stale** `D44F8E57…` value (the profile entry corrected in 2e6fc90 has
  not been in a build yet); the media that appears is `4a77ba00-2826-44c4-b883-1fa92ac36880`;
- the Data window is accepted by the manifest/GPT and its mount is skipped, not fatal;
- `load_init_program` runs and execs the configured init, so the failure is only that `/diting/init` is
  not part of the stock Apple OS volume.

Next experiment (this commit): `diting_init_path=/sbin/launchd`, so the stock launchd is the init and the
boot no longer depends on our userland helper being installed. Only the payload changes, so the flash
is not needed again; a `fastboot boot` of the new image is enough.

## 2026-11-09 23:20 — Data window matches, launchd is the init now

- The CI workflow hard-codes the Data volume UUID it compiles in
  (`.github/workflows/package.yml`, volume layout step: `'data': {'uuid': '337CF716-F62B-446B-B959-4A5F12A7701C'}`),
  and `build-ipados18-diting-macos.py` passes `layout['data']['uuid']` as `XNU_OWNER_DATA_VOLUME_UUID`.
  Our first Data window was created with a random mkapfs UUID, so the kernel skipped the mount
  (`scratch Data mount SKIP error=-6`).
- `scripts/make-data-window.py` now takes `--uuid` and passes it to mkapfs **-u** (the volume UUID;
  `-U` sets the container UUID). Rebuilt with `337cf716-f62b-446b-b959-4a5f12a7701c` and verified with
  `scripts/apfs-volume-uuid.py`.
- The SystemOS UUID also comes from `profile.toml` through the same layout step, so the 2e6fc90
  correction (`4A77BA00-2826-44C4-B883-1FA92AC36880`) took effect in the 42634647682 build.
- Rebuilt userdata (19,411,238,912 bytes), sparse (1157 chunks), flashed from the bootloader in 494 s,
  then `fastboot boot` of the build with `diting_init_path=/sbin/launchd` (sha A135720C).

Panel: the OS root binds by UUID, APFS mounts, the SystemOS wait now uses 4a77ba00, the Data mount no
longer reports the UUID mismatch, the kernel then tries to exec `/sbin/launchd` and panics in
`kern_exec`. The exact errno needs a legible photo of the last lines before concluding whether the
root tree really lacks the file or the exec is rejected for another reason.

## 2026-11-09 23:55 — the init path whitelist, and the Data mount is SKIP not panic

`patches/xnu-ipados18/0001-diting-darwin24.patch` (load_init_program):

```c
if (PE_parse_boot_argn("diting_init_path", diting_init_path, sizeof(diting_init_path))) {
    if (strcmp(diting_init_path, "/diting/init") != 0)
        panic("Unsupported Diting init path");
    ... exec /diting/init ...
}
// only reached when diting_init_path is absent:
for (i = 0; i < init_programs...) load_init_program_at_path(...);
```

So `diting_init_path=/sbin/launchd` was itself the panic. The argument is removed; with it absent the
kernel walks its normal `init_programs` list and `/sbin/launchd` is tried as the stock init.

The Data lines before the panic are not fatal and are separate:
`UUID candidate ... accepted=0 window=Data` → `UUID lookup MISS` → `writable UFS Data UUID
unavailable` → `scratch Data mount SKIP error=6 step=0` (ENXIO). The driver publishes every window
read-only while `diting_ufs_readwrite=0`, so the kernel finds no *writable* Data device and skips the
mount instead of panicking. Making only the Data window writable while OS/SystemOS stay read-only is
a driver change to do after launchd is seen running.

## 2026-11-10 00:35 — stock init path enabled

`diting_init_path` removed from `kernel_args` (commit 865ff27), build 42650203427 green, image sha
F354D89C, booted on the phone. With the argument absent the kernel leaves the Diting branch and walks its
normal `init_programs` list, so `/sbin/launchd` is attempted as the stock init. No userdata change, so
no reflash was needed.

## 2026-11-10 00:50 — launchd runs; services die on EACCES

With `diting_init_path` absent the kernel walks its normal init list, `/sbin/launchd` executes, and the
console scrolls: launchd is managing services. Every service then fails the same way:

```
Service could not initialize: posix_spawn(/usr/libexec/timed)
error 0xd - Permission denied
Initialization failure: 22H374: xpcproxy
xpcproxy exited due to exit(78)
```

and the same for `/usr/libexec/sharingd`, `/usr/libexec/ptpd`,
`/System/Library/PrivateFrameworks/CoreDuetContext.framework/Resources/contextstored`.

`xpcproxy` itself starts, i.e. execution from the mounted OS volume works; the denial happens when
xpcproxy spawns the service. The kernel skipped the Data mount just before
(`scratch Data mount SKIP error=6 step=0`, ENXIO) because the driver publishes every window read-only
while `diting_ufs_readwrite=0`, so `writable UFS Data UUID unavailable` even though the UUID candidate is
accepted. A missing `/private/var` is the most plausible cause of the EACCES storm.

Experiment: `diting_ufs_readwrite=1`. The root volume is mounted read-only by APFS policy regardless of
the block device's write flag, so this mainly lets the Data window be published writable; if the
services stop failing, the hypothesis is confirmed and the driver can later be restricted to publish
only the Data window writable.

## 2026-11-10 01:35 — writable window build booted

`diting_ufs_readwrite=1` (commit e8aafae), build 42658401487 green, image sha 5AA052E2, booted.
`native_diting_ufs_data_volume_device` -> `native_diting_ufs_volume_device(uuid, 2, ...)` accepts a Data
window only when the block reports `DitingWritable` **and** the IOMedia reports writable, so this flag is
exactly the gate that produced `writable UFS Data UUID unavailable` with the read-only build.

Watching for: `DITING APFS: UUID ... bound to Data UFS window`, `scratch Data mount` no longer SKIP, and
whether the `posix_spawn(...) error 0xd - Permission denied` loop stops.

## 2026-11-10：服务启动权限诊断

- 22H374 原生 launchd 与 xpcproxy 已实机运行，多个目标服务的 `posix_spawn` 返回 EACCES；Data 在只读轮次未挂载，写入轮次尚未观察到其最终状态。
- `applog=1` 现有逻辑会把 spawn 文件操作中输出到 `/dev/null` 的路径重定向到 `/dev/console`；这是一条待验证的失败路径，不能认定为实际原因。
- `diting_exec_diag=1` 启用有次数上限的 xpcproxy spawn 诊断，区分文件操作、路径查找、执行权限、镜像激活、persona、签名约束及 Data 状态；诊断不修改原错误或权限行为。实际根因必须以该镜像的设备日志为准。

## 2026-11-10：applog 读写重定向修正

- 22H374 的 xpcproxy 以 `O_RDWR | O_NOCTTY` 打开输出的 `/dev/null`；console 默认权限为 `root:wheel 0622`，非 root 读写打开可能触发 EACCES。
- `e331648` 保留读写打开的 `/dev/null`，仍将只写打开的标准输出或标准错误按 `applog` 设置重定向到 console。本地源码级验证通过，设备上是否消除 EACCES 仍需确认。
- greenH 对照镜像仅将 `applog=1` 改为 `applog=0`，已通过 `fastboot boot` 临时启动；观察结果尚未收到。Action 42698962449 构建的是此前的诊断提交 `835b22a`，不含本次修正。
- greenH 的 `applog=0` 对照启动进入了 `watchdogd[186]` 退出引起的 panic，退出原因 namespace 2、code 5 对应 SIGTRAP；此前 EACCES 是否全部消失仍待完整日志。现有用户异常抓取名单已加入 watchdogd，下一镜像可打印异常 PC、LR、共享缓存 slide 和部分用户栈，供定位退出位置。
- Action 42727444462（aa093e2）已成功，构建日志确认编译 `diting_ui_exception.o`。其镜像追加 `panic_on_crash_disabled=1` 后离线生成 `D:\ai\diting\build-work\watchdog-trace-no-panic.img`，尚未实机启动；该开关只抑制非 launchd 进程退出触发的 panic，不能消除进程崩溃或其他 panic。
- 2026-11-10 实机日志：SpringBoard、accountsd 等已 spawn，但以 `couldn't dlopen libobjc-trampolines.dylib` 退出；本地 OS 卷缺少该库，匹配的 SystemOS 卷中有该库。当前非 union 模式将 SystemOS 挂在 `/private/preboot`，标准 `/usr/lib` 路径不可见；不能仅凭此确认所有崩溃的根因。
- 独立实验分支 `ci/systemos-union`（1ec3b7b）启用已有 SystemOS 根目录联合挂载，Action 42817934102 构建中；不要将实验构建当作实机通过。
- Union 实机日志出现 `Unable to set current working directory /tmp` / ENOENT；OS 中 `/tmp -> private/var/tmp`，本地 8 GiB Data 种子 APFS 为空。实验提交 9628bd4 在核验 Data 可写后创建缺失 tmp，已测试、构建 Action 42829451753 尚未完成；完整 Data 种子仍需按现有 userland/init 方案处理。
- 2026-11-10 文档复核：新图显示 `mobile.usermanagerd` SIGABRT，描述 `Daemon failed to load persona manifest`。原始 OS 的 launchd `Boot.usermanagerd` 和 `Boot.launchd_cache_loader` 均没有 `PerformInRestore`；`userland/build-ipados18.py:adapt_launchd18` 与 `migration/ipados18/services.md` 已记录相应准备步骤。是否为此次失败的直接原因仍须核对实际启动模式。
- 空 Data 卷不是完整 Data seed；现有 tmp 补丁只是局部实验。当前 package 工作流没有 userdata job 或 ipsw_url 输入，README 已修正。继续按匹配 Data 种子、OS 用户态准备和可回读日志的路径排查，勿直接套用 Sheng 镜像或覆盖手机数据。

## 2026-11-10 23:45 — Data skeleton generated from the matching OS volume

The OS restore volume already carries the complete `/private/var` skeleton with the right owners and
modes (mounted read-only at `/home/nagatoquin/unixnu/diagnostics/mounts/diting-os/root`). That removes
the need for a macOS-prepared Data image for the directory layout: `scripts/gen-var-skeleton.py` reads
the mounted volume and emits `patches/xnu-ipados18/zzzzzzzzzzz-diting-data-skeleton.patch`, which creates
every missing directory (mode, uid, gid) right after the Data volume is mounted at `/private/var` and
leaves existing entries untouched. 87 directories at depth 2, app bundles excluded; metadata only, no
Apple file contents.

The patch follows the pattern the Data tmp experiment established (`mkdir`/`chmod` syscall wrappers
called from `native_owner_data_prepare`), and adds `chown` for the recorded owner.

## 2026-11-11 01:30 — Data skeleton builds (patched into the base XNU patch)

The first attempt shipped the skeleton as its own follow-up patch
(`zzzzzzzzzzz-diting-data-skeleton.patch`), and the CI rejected it with
`Patch does not apply cleanly`: a follow-up hunk cannot be anchored reliably because every earlier
patch shifts the line numbers, and `git apply` only searches a short distance from the hint. A second
attempt anchored at line 1 failed the same way.

Fix: `scripts/gen-var-skeleton.py --insert-into patches/xnu-ipados18/0001-diting-darwin24.patch` inserts the
generated table and loop into the base patch itself, where the context is guaranteed to match, and
`scripts/recount-patch.py` recomputes every hunk header from its body afterwards. Build 42859003034
(4147d47) is green, so the kernel compiles with the new code.

The code creates the 87 directories at depth 2 (app bundles excluded) right after the Data volume is
mounted at `/private/var`, using the `mkdir`/`chmod`/`chown` syscall wrappers already proven by the Data
tmp experiment, and prints `DITING APFS: var skeleton created=%d failed=%d`.

## 2026-11-11 02:15 — skeleton build booted (verification pending)

Build 42859003034 (4147d47) produced image sha C5D58F80; `fastboot boot` OKAY on 9e0c2b55 and the phone
left adb/fastboot, i.e. the payload is running. The panel is the only witness so far: the expected line
is `DITING APFS: var skeleton created=87 failed=0`, and the next questions are whether the `/tmp` ENOENT
and the persona/usermanagerd failures changed.

## 2026-11-11 02:20 — launchd boot task inspected (read-only)

The mounted OS volume's `/sbin/launchd` (560,752 bytes) embeds its boot-task plist. The `usermanagerd`
task is:

```
<key>usermanagerd</key>
<dict>
    <key>PerformAfterUserspaceReboot</key><true/>
    <key>ProgramArguments</key><array>
        <string>/usr/libexec/usermanagerd</string><string>--init</string>
    </array>
    <key>AllowCrash</key><true/>
</dict>
```

so it carries `PerformAfterUserspaceReboot` but **no `PerformInRestore`**, which matches the handover's
claim. `PerformInRestore` appears on other tasks (`restore-datapartition`, `exclaves-boot`), so the key
is understood by this launchd build. Whether the current boot counts as restore mode decides if the
persona initialisation runs; that is the next thing to establish from the panel, not to guess.

## 2026-11-11：合并已构建的 Data 骨架与 SystemOS 路径

- 4147d47 的 Data 目录骨架和 ci/systemos-union 的只读根卷合并此前位于不同分支；本次把后者的 CI 配置接入包含骨架的 main，避免新构建回到仅依靠 Cryptex 备用路径的配置。之前照片未完整证明 trampoline 已解决；合并后的挂载、Data 骨架、用户态与界面均待实机验证。

## 2026-11-11：二维码日志实机复核

- 33 使用 `greenI2-log-menu.img` 的 Tools → Advanced Logger 成功通过二维码导出日志到 `C:\Users\nagatoquin\Downloads\Redmi K50 Ultra-20190100-000250(1).log`。二维码导出与固件日志已实机成功，不应再表述成该功能不存在。
- 导出内容包含 `RAMOOPS: sig=0x1400382E size=0x0 (no xnu log)`；本次未取到上轮 XNU 日志。原因尚待核对复位类型、内存保留和内核写入，不能只凭这一份日志判断所有重启场景。该日志是固件输出，不代表已实现独立落盘日志。
- 6ec0b96 将已构建的 Data 骨架与已有 SystemOS union CI 配置合并；Action 42877431747 已根据 33 的反馈取消，合并版尚未编译或启动。不要因目录存在而重写 Data 或假称系统可用。

## 2026-11-11 03:55 — prepared userland installed; both window writes verified by sampling

The previous session finished writing the prepared 22H374 OS and Data into their userdata windows
before it hit the usage limit, and left the phone in the temporary TWRP (`diting-recovery-boot.img`,
root adb, no dd/gzip process). This round did **no boot and no write**: the user is away and a failed
`fastboot boot` cannot be power-cycled back to fastboot without them.

Read-only verification (`scripts/verify-ufs-window.py`, added here):

- SHNGROOT v1 at userdata offset 0: 3 windows, `imageBytes=19,411,238,912`, directory checksum ok.
  OS `0x100000` (6,845,104,128), SystemOS `0x198100000` (3,963,617,280), Data `0x285000000`
  (8,589,934,592).
- OS window equals `prepared-420734-os.img` at 7/7 sampled 64 KiB regions from 0 to 6.375 GiB,
  including the final 64 KiB of the window: the sequential dd reached the end.
- Data window equals `build-work/windows/Data.img` at 7/7 samples; the 4 KiB block holding
  `Diting development Data seed for 18.7.10 / 22H374` (image offset 0x184e000) is byte-identical on the
  device.
- SystemOS was not rewritten; its first 4 KiB still starts with an APFS `NXSB` object, so the OS
  stream did not overrun 0x198100000.

The directory still records the previous OS/Data sha256 (`be43bec0…`, `e5bced3f…`); the installed
content hashes to `45b6344f…` and `1378adfe…`. `RootLayout.h` checks only the 4064-byte directory
checksum and bounds/names/flags, and `DitingUfsTransport.cpp` never compares the per-window digests, so
the stale record does not block publication — but it must not be quoted as the installed digests.

`scripts/inspect-diting-userland.py` (added here) passes on the installed triple: OS UUID B07DF049,
`/diting/init` 755, `/diting/etc/var.skel`, `/diting/var-seed`; `/sbin/launchd` is byte-identical to
`/diting/init` (the Diting static init wrapper) while `/sbin/launchd.ios` is Apple's 560,752-byte launchd
and carries `PerformInRestore` for `usermanagerd` and `launchd_cache_loader`; Data UUID 337CF716 with
the seed marker, `keybags 0700`, `mobile 0711`, `db`, `containers`, `preferences`, `tmp 01777`.

Pairing for the next boot: build 42859003034 (4147d47) image `C5D58F80`,
`build-work/greenI2/.../boot/diting-unixnu-uefi.img`. Its payload args have no `diting_init_path` and do
carry `diting_ufs_readwrite=1 diting_ufs_stall=1 diting_exec_diag=1 applog=1`; the ADT has B07DF049 /
4A77BA00 and mounts SystemOS at `/private/preboot`. The DITING init list starts at `/sbin/launchd`,
which this userland replaces with the wrapper, so the native path runs the Diting init and execs
`launchd.ios`. No `diting_init_path` and no SystemOS union (6ec0b96) needed — that merged build is still
unbuilt/unproven and must not be conflated with this pairing.

Still unobserved, therefore not acceptance: kernel-side re-publication of the windows, root binding,
Data mount at `/private/var` and launchd services with the prepared userland. Those need the next
manual boot; see `D:\ai\diting\交接-2026-11-11-准备用户态.md` for the exact commands and screen
criteria.

## 2026-11-11 04:20 — live window UUIDs read from the device

`scripts/read-ufs-window-apfs.py` (added here) reads only the APFS metadata blocks a container needs
and follows the GPT for the Data window, so the UUID match can be checked while the phone still sits
in recovery (48–64 device blocks, 192–256 KiB per window, no full-image readback):

- OS `0x100000`: volume `CrystalUpdate22H374.J171J172OS` UUID B07DF049-7AD9-47B5-9F8F-9EE728056398,
  role 0x1, sealed=False, container xid 442. Matches XNU_OWNER_ROOT_VOLUME_UUID.
- SystemOS `0x198100000`: volume `CrystalUpdate22H374.J171J172SystemCryptex` UUID
  4A77BA00-2826-44C4-B883-1FA92AC36880, sealed=True. Matches XNU_OWNER_CACHE_VOLUME_UUID.
- Data `0x285000000`: GPT with the APFS partition at 0x100000; volume `Data` UUID
  337CF716-F62B-446B-B959-4A5F12A7701C. Matches XNU_OWNER_DATA_VOLUME_UUID.

The built payload embeds those three strings (plus `/private/preboot` and the DYLD_* environment), and
the `DITING APFS: writable UFS Data UUID unavailable` literal proves the UFS Data branch is compiled,
not the md0/RAM fallback. `native_owner_data_prepare` mounts "apfs" at "/private/var" with flags 0
and then rejects the mount if it is MNT_RDONLY, so `diting_ufs_readwrite=1` is required and present.
That code does not consult the APFS volume role or volume group; both the OS and Data images carry a
zero volume group id, so the Data UUID rewrite did not create a group mismatch.

## 2026-11-11 04:30 — file-level identity of the installed windows

`read-ufs-window-apfs.py` grew `--stat/--hash` and `--image`, so the same report runs on the device
and on the local window source. Against the live windows:

- OS: `/sbin/launchd` is the wrapper (71,392 B, sha256 `a80747bb…`, identical to `/diting/init`),
  `/sbin/launchd.ios` is Apple's launchd (560,752 B, sha256 `81cefc45…`), `/diting/etc/var.skel`
  31,219 B. Both file hashes equal the `prepared-420734-os.img` values, and the container UUID/xid
  (`afc0f9bd…`, 442) match, so the installed OS window carries the prepared image at the file level,
  not just at the sampled offsets.
- Data: `/.diting-data-22H374` (48 B, sha256 `defb18e6…`) and
  `/mobile/Library/Preferences/com.apple.purplebuddy.plist` (146 B, 501:501) match
  `windows/Data.img`; container `3475eb93…`, xid 2.
- SystemOS: container `975fa298…`, xid 13, volume 4A77BA00… sealed. The local `SystemOS.dmg` has the
  same container UUID/xid, but block 926542 is zero in both source and device and the sealed live
  root tree resolves through it, so the in-tree apfs reader cannot list files on that volume. This is
  a reader limitation, not a device fault (apfs-fuse listed the same volume earlier); file-level
  evidence for SystemOS still comes from that mount, not from this reader.

## 2026-11-11 04:40 — boot preflight gate passes end to end

`scripts/preflight-diting-boot.py` (added here) is the single gate before a one-way `fastboot boot`.
It checks the device is in recovery and reports `diting`, the boot image sha256 (C5D58F80), the
embedded args (must keep `diting_ufs_readwrite=1`, must not set `diting_init_path`), samples both windows
against their source images, and reads the three APFS volume UUIDs from the live windows. It writes
nothing and does not boot. Run today against `9e0c2b55` with the C5D58F80 image and the installed
windows: all checks passed (window sampling plus the three UUID reads), so the remaining acceptance
work is the boot itself, not the inputs.

## 2026-11-11 08:15 — C5D58F80 booted again with the prepared userland

- `fastboot getvar`: product=diting, unlocked=yes, current-slot=a. Boot image sha256 checked in full
  (`C5D58F8088900F6BAB15B177B52FD3A080A94E9CBE6E6ABE4185D13CDB98F833`), then `fastboot boot` reported
  `Sending 'boot.img' (98820 KB) OKAY` and `Booting OKAY`.
- 60 s after boot neither adb nor fastboot had a device on USB, so the panel is the only witness.
  Phase 6 scoring waits for the user's photo; do not treat OKAY as a hardware pass.
- A screen-line table for that photo now sits in `D:\ai\diting\交接-2026-11-11-准备用户态.md`
  (strings quoted from `drivers/NativeUfs` and `patches/xnu-ipados18/0001-diting-darwin24.patch`).

## 2026-11-11 09:50 — the cache loader panic; fix was already recorded

- The panel from the 08:15 boot showed `panic: launchd_cache_loader(1) exited -- exit_reason
  namespace 7 subcode 0x1 description: Failed to verify code directory`. The prepared wrapper did
  exec `/sbin/launchd.ios`, so this is the restore-mode cache loader, not a storage failure.
- `migration/ipados18/services.md` already records the fix (services9): the loader supports
  `launchd_unsecure_cache=1`, which lets the stock loader load the locally adapted cache instead of
  pairing a modified plist with Apple's old signature. The C5D58F80 payload did not carry it;
  `board.toml [payload] kernel_args` now does (assembled args line 757 bytes, under the 1023 limit).
- Logs no longer need a photo: the firmware dumps the `0xA7000000` ramoops ring through
  Tools -> Advanced Logger on the next boot (see 2026-11-09 03:55 above). The 02:45 export showed
  `size=0` because it predates a mirror-enabled run; the next export must be checked before claiming
  the channel works.
- Repo identity: README.md was still the Diting / Nothing Phone (2) page. Rewritten for Diting with the
  current stage and the ramoops/QR log note. The AGENTS line "This is the Diting SM8475 ..." was
  corrected to Diting. Component identity DitingPlatform24 stays: the drivers are the community Diting
  SM8475 implementations, not the upstream A740/WCN7850 source.
- No device access this round (phone disconnected); nothing was written or booted.

## 2026-11-11：DeepSeek 执行计划

- 33 将任务改为编写接手计划，不继续试验。计划位于 `D:\ai\diting\DeepSeek执行计划-进系统与Diting整理-20261111.md`；优先把现有准备用户态所需的 `launchd_unsecure_cache=1` 放入实际 payload，然后推进系统界面。Diting 命名迁移列出 board/kit/firmware/组件/机器配置/脚本/产物的完整调用链，全部在 main 完成。
- 原 OS 窗口写入后台任务已确认完整结束；本轮写计划时没有新增设备写入或启动。较新的准备用户态与 cache loader 状态见 `交接-2026-11-11-准备用户态.md` 及上方 09:50 记录，不能再以旧空 Data 状态继续。

## 2026-11-11 09:55 — launchd_unsecure_cache=1 is in an actual boot image

- `scripts/patch-boot-payload-args.py` (commit b995b82) rewrites the xnu args in
  `EFI/UniXNU/unixnu.toml`, the development boot manifest, the ADT's `boot-manifest-hash` (SHA-384) and
  the PONGFAT1 CRC, leaving the kernel, ADT tree and firmware untouched.
- Output: `D:\ai\diting\build-work\diting-boot-launchd-unsecure.img`, 101,191,680 bytes, sha256
  `cb56ac9de5b4573c8ddcbbfc24215b38742a4dfe5b194251145492ffb31a412b`. Verified: same size, kernel
  slot byte-identical, only the ramdisk region changed; TOML and manifest `boot_args` both contain
  `launchd_unsecure_cache=1`; the ADT digest equals the manifest SHA-384; the envelope CRC is valid.
- Action 42941361215 was cancelled: it used the SystemOS-union CI defconfig, not the pairing of the
  installed prepared userland.
- Next device step: `fastboot boot` this image after the product/slot checks, then read the previous
  XNU console through the ramoops/Advanced Logger QR export on the next boot. No screen photo.

# Diting 全量改名（2026-11-11）

- 机器配置改为 DITING：`config/MASTER.arm64.diting`、`pexpert/arm64/DITING.h`、`-DARM64_BOARD_CONFIG_DITING`、对象目录 `DEVELOPMENT_ARM64_DITING`、产物 `mach.development.diting`；补丁系列与文件同步改名。
- 驱动已收编进仓库（12 个 submodule 转普通目录，保留来源与许可证）；`Diting*` 目录、文件、类名、启动参数、DT 属性、日志前缀统一改为 Diting；打包产物与配置改为 `diting-*`。
- 固件入口改为 `Platforms/Xiaomi/ditingPkg`；CI 的 package job 从固定的 `Emilelu/uefi-mondrian` 加 `platform-firmware/ditingPkg` 准备固件树，release 用 `--fd` 接收 Linux 固件 job 构建的 FD（核心分支 `diting-release-fd`，commit `0ce9779`）。
- 保留的外部接口（不可单独改名）：`PONGFAT1` 信封、`Binaries/diting/RawFiles` 固件二进制、`pong-boot-manifest` 文件名/格式、上游 Diting 仓库 URL 与版权声明、以及**已安装 prepared OS 的 `/diting` 运行时约定**。
- 重要：新内核要求 Diting 用户态（`/diting/init`、`/dev/diting-*`、`dev.unixnu.diting.*`）。当前已安装 prepared OS 仍是 `/diting`，只与改名前内核配对；验证界面用 `D:i\ditinguild-work\diting-boot-launchd-unsecure.img`，重装 Diting userdata 前不要用新内核启动。
- 构建状态：本次改名后尚未完成 CI 构建。

# Diting 构建通过（2026-11-11）

- CI run 42963886855：`firmware` 与 `package` 两个 job 全绿，改用 `--fd` 让 package job 消费 Linux 固件 job 的卷。
- 产物：`diting-unixnu-uefi.img` 101,191,680 B sha256 `a96a33339489ef3c8b6579db8bc3f33d8509f623579559f832f381f8008e0cd1`；`diting-xnu-initial.img` sha256 `1d7c151accad4b40d813ffd8a34a0da7da02e61bd6e44902cc93460a41df6bbc`；`kernel.macho` 51,793,920 B sha256 `c096ca2aee97ff29ab8991805ae8b4507e82a78894561b7347737739ae23c2d6`。
- 该内核的运行时命名空间已是 Diting（`/diting/init`、`/dev/diting-*`、`dev.unixnu.diting.*`、`diting_*` 启动参数）；当前已安装 prepared OS 仍是 `/pong`，**不能直接引导新内核**。设备界面验证继续用旧内核镜像 `D:\ai\diting\build-work\diting-boot-launchd-unsecure.img`（sha256 `cb56ac9d…`）；要上机新内核必须先重编并重装 Diting userdata。
- 保留的外部接口：`PONGFAT1` 信封、`Binaries/pong/RawFiles` 固件二进制、`pong-boot-manifest` 格式/文件名、上游 Pong 仓库 URL 与版权声明。

# 2026-11-11 上游对齐与驱动审计

- Sheng 上游对齐结论：保持 22H374；只借其打包/运行时/固件层（`[payload.runtime]`、`make-data-seed.py`、`runtime.py`、固件预检链、CI seed 缓存）；不搬 WiFi/BT/Metal/基带驱动。详见 `D:\ai\diting\Sheng上游对齐-结论与待办-20261111.md`。
- 驱动真实状态：WiFi/GPU/Metal 代码在，但刚绿的 CI（42963886855）用 `diting_ipados18_ci_defconfig`（`WIFI=n`、`ADRENO=n`、`BACKLIGHT=n`、无 `POWER`），三个驱动树都没编进内核；该产物只做编译验证，不能配 prepared OS 进界面。
- 基带：无驱动、无 `QcomBaseband`、目标 IPSW 是 Wi-Fi iPad（`iPad7,11`，无基带固件/消费者）；移出目标，只在文档记录原因。
- userdata 写入器几何已修（`73f73c8`）：`scripts/` 里的 `sda13`、`0x413248/0x75a40ea`、8 GiB 偏移改为 `sda37`、`0x2F8A00/0x3ADF3FA`、`0x285000000`；写入前仍须 recovery 现场核对 start/size/logical。
- Wi-Fi：老谱系有用户确认连接记录（`porting.toml` services107/108/112），当前构建未启用；阻塞是 CI 无 WiFi 固件 secret、`WIFI=n`、缺 `diting_wifi=1`。不要搬 Sheng 的 WLAN ADT 节点，Diting 已用剥离 `_LimitLoadToDeviceTree` 的方式解决 wifid。
- Metal：老谱系有设备证据（services124/135/155/167）；当前缺 `diting_adreno_start=1`/`diting_metal=1`，CI 不构建 Metal bundle；`drivers/AdrenoMetal` 是未使用的重复副本。
- 许可证：`THIRD_PARTY_NOTICES.md` 未列 GPL-2.0（ath11k/MHI/QMI）、metal2vulkan、Mesa/Turnip；公开产物前补。
- 当前真正卡点仍是 `launchd_cache_loader` 的 code directory 校验；WiFi/Metal/基带都不在进入界面的关键路径。

# Sheng 答复要点（2026-11-11）

- Sheng 未改 OS 镜像：OS/SystemOS 原样；驱动在内核；Metal 插件走 payload gpu-addon 写到 Data；owner trust 合入 trust cache；无 /sheng 包装器、无 launchd cache 改动。
- Sheng 的 RootLayout 是 v1 两窗口（OS/SystemOS）+ payload Data seed；Diting 的 v1 三窗口是 fork 自己的选择。
- Data seed 是 packed_ramdisk DBRDISK1（APFS NXSB），960 GiB 逻辑容量打包后仅 1.28 MB；在 macos-15 runner 上用 seed-data-grow.py 生成。
- userdata 在 WSL Linux 上用 unixnu build userdata 生成；21H461 无 AEA，udif.py 可直接解码；18.7.10 的 AEA 才需要 macOS。
- 对 Diting：A 路线（v1 两窗口 + seed、不改 OS）可完全 Linux 化，但需替代 /diting 包装器与 launchd 补丁；B 路线（保留 prepared OS，只重建 6.8 GB OS 窗口）峰值约 14 GB，仍需一台 Mac。先走 B，A 作为目标。

# 2026-11-11 Claude 结论与重建路线

- 选 B：保留 prepared OS，只重建 OS 窗口；SystemOS 与 Data 第一次不动。纯 A（OS 原样、用户态全进内核）需要重写 Diting 的 mini-AMFI + restore launchd + kern.exec_inject 适配，不做。
- 更正：托管 `macos-15-intel` 可以跑完整 userdata 链（run 42073458178 / job 127407532963，34 分钟，reclaim disk 到 80 GiB，峰值约 37 GiB）。旧 userdata job 在 `425c309` 被删，已恢复为 `.github/workflows/diting-userdata.yml`，只上传 OS 窗口与报告。
- 新增 `configs/diting_ipados18_bringup_defconfig`：`CONFIG_XNU_OWNER_CRYPTEX_FILES=y`、`CONFIG_XNU_OWNER_UNION_SYSTEMOS=n`；WiFi/Adreno/backlight 保持关。`package.yml` 增加 `defconfig` 输入。
- 顺序：舞台 1（旧镜像 `cb56ac9d…`）→ userdata job 出 OS 窗口 → bring-up 内核 → TWRP 写 OS 窗口（`sda37`，`seek=1`）→ `fastboot boot` 新镜像 → 逐步开驱动。
- 写 OS 窗口前核对 `sda37` start=24924160、size=468930520；回退用 `prepared-420734-os.img`。
- provenance 修正：`porting.toml` 里继承的 “Pong live Android DT” 等历史输入不再改成 Diting。

# 2026-11-11 阶段 2a/2b 完成

- userdata job `42987806310` 绿：新 OS 窗口 `diting-ipados18-system.cdr` 6,845,104,128 B，sha256 `27c507c8972275c9170806bb975b03d762ebbb9379945b890d5665a154e8102d`；根卷 UUID `B07DF049…`，`/sbin/launchd` == `/diting/init`（71,392 B，sha256 `95cb7341…`），`launchd.ios` 含 `PerformInRestore`，`/diting/libditing-ui18.dylib` 174,976 B，`/pong` 不存在。已下载到 `D:\ai\diting\artifacts\ci-42987806310`。
- 该 job 的 `volume-layout.json` 里 SystemOS/Data 是新建容器 UUID（`771CEF3F…`/`1C04B790…`），**不要**用于设备或内核；设备继续用 `4A77BA00…`/`337CF716…`。
- bring-up 内核 run `42994674560` 绿（`diting_ipados18_bringup_defconfig`）：`diting-unixnu-uefi.img` 101,191,680 B sha256 `e9b615bf9ae0143cbe6195f9625105b92cbbc877c5e35e97ec7b6d6130b0d35d`；`kernel.macho` 51,793,920 B sha256 `6399f2ca5dbde521a3aaf352028da2a22f704803272e2c8665a56491d995ff76`；配置 `diting_ipados18_bringup_defconfig`。
- 镜像参数已逐个核对：`launchd_unsecure_cache=1`、`diting_ufs_readwrite=1`、`diting_exec_diag=1` 均在；无 `diting_init_path`、无 `pong_`；manifest 与 TOML 一致，ADT 的 `boot-manifest-hash` 等于 manifest SHA-384，PONGFAT1 CRC 有效；内核里三个 UUID 字符串都在。
- CI 修正：`5863dd7`（按所选 defconfig 走打包路径）、`31e710c`（断言 `[package.defconfig]` 与输入一致）。
- 写入 runbook：`D:\ai\diting\OS窗口写入与回退-安全版-20261111.md`；路线图 §2c 已改为 `shell -T` + `count=6528` + `pipefail` + receipt，并用 `ANDROID_ADB_SERVER_PORT=5038` 适配只读校验脚本。
- 下一步：手机可用时先舞台 1（`diting-boot-launchd-unsecure.img`，`cb56ac9d…`）取日志；再按 runbook 写 OS 窗口、`fastboot boot` bring-up 镜像、Advanced Logger 取日志。

# 2026-11-12 全开配对（private firmware + live DTB）

- run `43057086317`（`23d3a92`，`diting_ipados18_defconfig` + `firmware_tag=fw-20261111`）在 package 步骤失败：Wi-Fi native 构建 `prepare-diting-wifi.py:54` `KeyError: 'qcom,phy-sequence'`。原因：`scripts/build-ipados18-diting-macos.py:46` 把 `DITING_HARDWARE_DTB` 覆盖成内核 `hardware.dtb`（format-2 精简树），Wi-Fi 的 PHY 参数来自 live 树。修复 `2e406ef`：新增 `DITING_LIVE_DTB`，`build-diting-wifi-native.py` 优先读它；workflow package env 指向 `$RUNNER_TEMP/diting-live.dtb`。
- 音频可缺省：`prepare-diting-audio.py` 在 `tfa98xx.cnt` 缺失时写哨兵 profile 并跳过；手机 `/vendor` 确实没有 TFA 容器（DTB 音频是 Qualcomm WSA），`POWER` 仍能编译出电量/背光。
- 私有固件：QCA6490 的 222 个文件（`amss/amss20/m3/regdb/bdwlan*`，解包 25.7 MB，压缩 7.2 MB）放在私有仓库 `nagatoquin33/diting-firmware` 的 release `fw-20261111`；公共仓库用 `FIRMWARE_TOKEN` secret 拉取，固件不进公共仓库。`stage-diting-firmware.py` 允许只带部分分组（wifi）。
- 全开配置：`diting_ipados18_defconfig` 进入 CI 选项；`board.toml` 的 `cpus=1` 改为 `cpus=8`，并加 `diting_wifi=1`、`diting_adreno_start=1`、`diting_metal=1`。
- 新 run `43068678253`（`2e406ef`）验证以上修复。

# 2026-11-13 Linux 构建后的平台专家阻塞

- services172 实机由用户报告 `platform=6 cpu=11 error=0`：arm-io nubs 与全部 CPU 中断 vectors 已注册，首次启动提交循环尚未返回。该阶段覆盖主核与副核，不能据此确定阻塞核或宣称 CPU 启动成功。
- services171 未证实的平台注册顺序修改已撤回；保持八核、原 configure 顺序和真实 fatal invariant。不要再次切单核或猜测性关闭硬件入口。
- services173 沿已定位的 processor_boot 锁/调度、secondary processor_dostartup、initCPU 和 IRQ 安装边界加入软件快照；本地链接与八核打包通过，2026-11-13 05:38:17 UTC 核对 diting/unlocked/b 后临时 Fastboot boot，Sending 2.392 秒、Booting 0.114 秒 OKAY。设备细分阶段待反馈，根因及修复尚未确认。
- 证据见 `migration/ipados18/services172-platform-evidence.md`、`migration/ipados18/services173-first-boot-boundaries.md`；当前持久 boot_b 为 services169，本轮未写 userdata 或持久 boot。日志/产物使用当前 Linux 工作区 `diting-work/{build,logs/diting,tmp}/servicesNN`。用户明确当前没有 Android，不再要求进入 Android。

# 2026-11-14 services173 的 firstboot 反馈

- 用户提供 `firstboot=7/29`，CPU7 已到 secondary cpu_start，尚未记录返回；重启后未取得同轮 init/irq。不要仅据此断定 CPU7 自身故障或改 GIC/PSCI 配置。
- PE_cpu_start_internal 的 void 转发丢弃 startCPU 失败；现有 PSCI 每核同步等 5 秒，连续失败可遮蔽首个失败并越过 IOSecureBSDRoot 的 30 秒。services174 在 Diting PSCI 的实际失败处执行必需 CPU 的 fatal invariant，保留固件返回、早期异常和独立缓存行的复位边界。没有把请求受理冒充 CPU 上线。
- services174 补丁应用检查、本地内核链接与八核打包通过（101191680 字节）；2026-11-13 17:23:59 UTC（北京时间 11-14）依本轮编译并刷入的明确要求，核对 diting/unlocked/b 和 192 MiB boot_b 后稀疏刷 boot_b：两块发送写入 OKAY，4.823 秒；随后 fastboot boot Sending 2.336 秒、Booting 0.113 秒 OKAY。当前持久 boot_b 与运行镜像均为 services174，services169 保留作回退，不写 userdata；用户反馈 `Diting CPU1 start timeout: reset=5 kernel=1`：CPU_ON 受理且复位代码读到 cpu_ttep，尚未进入 arm_init_cpu；继续检查 MMU 到内核入口。记录见 `migration/ipados18/services174-secondary-start-failure.md`。

# 2026-11-14 services174–176 副核 MMU 与异常上下文

- services174 实机 CPU1 `start timeout reset=5 kernel=1`，用户补 TTBR1=0x906c06000。CPU_ON 已受理，复位代码找到 cpu_data/handler 并读取页表地址，尚未进 arm_init_cpu。
- services175 补齐动态内核页表的 PoC clean，保留早期向量到 KVA trampoline；本地链接与八核打包通过，17:37:17 UTC 稀疏刷 boot_b 两块 OKAY（4.873 秒），随后 Fastboot boot OKAY（2.456 秒）。用户报告直接黑屏重启，无新报错；不能认定页表写回解决了根因。
- services176 进一步将正常 VBAR 安装延后到 arm_init_cpu 的 machine_set_current_thread 之后，保留 phystokv/函数入口的早期异常捕获，并按 MPIDR 记录 C 入口 16–20。补丁应用、本地链接、八核打包和部署通过；实机异常见后续 services176 记录。详见对应 migration/ipados18/services174、175、176 记录；当前仍未确认副核上线或系统正常启动。

# 工作方法（长期有效，不可省略）

以下方法不因阶段或目标改写而失效；每次改 objective/交接时必须保留。

1. 先读 unixnu.com 官方文档与 Sheng/UniXNU 上游实现，按文档契约修改；不靠猜测，不用“关掉功能/哨兵”绕过阻塞。
2. 本地能验证的先本地验证（静态检查、离线校验、DTB/固件/配置比对），再走 CI。
3. 每次失败先读日志定位根因；同一批问题一次改完，只派发一次；禁止重跑碰运气，网络性失败在步骤内重试。
4. 证据、哈希、阻塞与具体条件写入本文件；未在真机观察确认，不声称成功。
5. 用 Advanced Logger/ramoops 等日志取证，不拍屏；分区写入/刷机前必须得到明确授权。
6. 保留 Pong 来源说明与许可证合规；不把厂商 blob 提交或发布到公共仓库。
7. 以 Sheng 上游为参照消除重复劳动，对比架构与工具链；不为“看起来完成”而删减范围。

# 2026-11-14 services176–179 后续设备证据与编译约束

- services176 已刷 boot_b 并启动；CPU1 early exception reset=7 kernel=1，ESR=0x81000100、ELR=0xfffffff0089b00b0，对应 phystokv+0xc 的第一条 NEON。保留 ESR 原值，不据此强行判断标准 FP exception class。
- services177 与 services178 均已本地链接、八核打包、稀疏刷 boot_b 并启动；用户分别反馈黑屏重启，services178 确认为自动重启。不能将 CPACR ISB 或三个对象的标量约束当作设备修复成功。
- services178 临时 Recovery 的 pstore 仍为空，实际 ramoops 使用 0x400000@0xa7000000；未挂载 data/metadata/cache。日志读完后已通过 ADB 回到 Fastboot，未写 userdata。
- services179 有具体后续调用证据：cpu_timebase_init 仍因 Linux clang 隐式生成 ld1/st1/str q0。Linux 构建环境及内核全局 flags 增加 -mno-implicit-float，保留显式 SIMD；已有 stamp 使旧内核对象失效。链接通过，1011 个 C/C++ 编译条目有约束，cpu_timebase_init/phystokv/ArmCpu 对象未见 SIMD 操作数。八核打包和部署已完成；实机仍黑屏自动重启，故障未解决。详见 migration/ipados18/services179-kernel-implicit-simd.md。

- services179 八核打包通过（101191680 字节），18:23:53 UTC 稀疏刷 boot_b OKAY（4.655 秒），临时启动 OKAY（2.474 秒）；用户确认仍黑屏自动重启。该时点持久 boot_b 为 services179（随后被 services180 替换），启动故障未解决，services169 保留回退。

- services180 针对缺失的新异常 PC：KVA trampoline 将早期 VBAR 改为 KVA，避免 PAN/清除 TTBR0 后物理捕获失效；保留捕获到 PE_init_cpu 返回，在 FIQ/timer/调度入口前安装正常向量。未重写异常栈或修改 DAIF；真实初始化及 CPU 成功条件保持。新增 reset=21–28 边界，本地补丁应用、链接和八核打包通过；18:31:27 UTC 稀疏写 boot_b OKAY（4.762 秒），临时启动 OKAY（2.484 秒）。用户确认仍黑屏自动重启，无新错误；当前持久 boot_b 为 services180。详见 migration/ipados18/services180-secondary-bootstrap-vectors.md。

# 2026-11-14 用户要求交接

- 用户对继续完成提出疑问，要求将当前任务状态写入文档并交接另一模型。本会话已停止编译与刷机，仅整理文档。交接时 fastboot 可见 1d4eb066，ADB 空；未发现遗留构建、打包或刷机进程。交接入口：migration/ipados18/交接-2026-11-14-services186-启动与触控正常.md。没有创建后台自动刷机或代理任务。

# 2026-11-14 services185 进入系统，触屏待修复

- services183 有效 OEM minidump 已证明 services179–182 八核全部上线、UFS 三窗口发布、OS/SystemOS/Data 挂载及原生 launchd 执行；不要继续沿用“停在 CPU1 / 副核未上线”的旧判断。
- services184 修正 APSS DRV2 active TCS0–2、sleep TCS3–4、wake TCS5–6、fast TCS7 的布局；GPU/WLAN/音频通过 DitingPlatformResources 共享完整交易。超时保留所有权，真实完成后才回收。10 个 host 交易场景通过，但 services184 实机仍重启。
- services184 三条同轮 pmsg 均停在提前使能 smpe3 后。Nothing CNSS2 的普通 regulator list 不含 IPA；services185 按最终 ELF/BIN BDF 响应可选 host_bdf_data bit0 选择功放：外置不配置 S3E，内置用 64 字节 AOP wlan_pdc 消息，缺失 TLV 保留默认状态。普通电源保持厂商 consumer 顺序，仅显式 provider init-mode 才写模式。
- WLAN 与 ADSP 共用 AOSS QMP owner；超时不清空邮箱、不伪造 ACK，真实 AOP 清零后才能消费迟到完成。POWER/BACKLIGHT 独立归档与链接条件已同步两套构建入口。
- services185 原生驱动编译、完整内核链接和八核打包通过；07:42 稀疏写 boot_b 两块全 OKAY（4.670 秒），临时启动 OKAY（2.459 秒）。用户先确认“进系统了”，随后报告触屏不工作。进入系统是实机观察；Wi-Fi 功放响应、联网、PCM、GPU 和长期稳定性不能据此标为通过。
- 触屏旧轮日志已显示 `goodix: clocks stay off`、全零固件版本与 Goodix 初始化失败；这是 services184 证据，不能冒充 services185 日志。当时先定位 Goodix 初始化/事件链，后由 services186 修正 ST61Y 协议。
- 本地状态、构建和用户观察分别保存在 `diting-work/{build,logs/diting,tmp}/services185`。`services169/tree/.cache/xnu-work` 当时对应 services185，现已对应 services186，不能作 services169 原始对象。用户已明确要求提交和推送更改及文档状态；所有私有输入和产物留在本地。

# 2026-11-14 services186 ST61Y 接续

- 原始live DT与Recovery日志确认ST61Y / fts_spi 5.2.4.1 / firmware0007 / protocol06；GPIO119原始高为ST，低为GT9916S。旧Goodix全零版本日志不是芯片身份依据。
- 以匹配LineageOS固定版本的协议和NothingOSS SM8475寄存器契约修正。GENI FIFO opcode3合法，CS_TOGGLE在bit0，主/副引擎active为bit0/bit12；不延续早期opcode判断错误。只有真实READY、chip ID、System Info、flush echo后提交scan，命令提交不是触控成功。
- 触屏使用共享RPMh owner的ldoc2请求与GPIO71 AVDD；touch/WLAN/USB/audio用共享GCC62008 RMW/readback锁，AHB HWCG与core/core2x/SE的halt判定区分。
- 真实事件高nibble、LE16 super-resolution、多指状态、空FIFO、丢失批次释放和复位后立即恢复已实现。错误初始化/超时保持有界并返回正常UI；不刷触控firmware、不校准/写OTP或工厂数据。
- 7项host协议/资源测试、两套shell语法和workflow预检通过；ramoops关闭的resources-only archive和touch对象实际编译无未定义诊断引用，完整内核及八核打包通过。新增18个z前缀的增量XNU patch已追加series，未重写0001聚合补丁。
- 实机镜像101183488字节，继续services182 FD/template和33792字节loader，新hardware DTB只作本轮资源输入。09:27稀疏写boot_b并临时boot全部OKAY；当前boot_b/运行系统为services186，用户确认进系统、点按与滑动正常，熄屏后亮屏触控恢复正常；双指手势尚未测试。只推送源码/测试/配置/记录，所有原始日志、firmware/restore和产物本地保存。

# 2026-11-15 设置图标独立候选（低优先级，未部署）

- 用户确认仅 Game Center、iCloud 图标可见。匹配22H374分析证实既有适配未覆盖主列表的ISTypeIcon同步/异步生成；新增Preferences内失败/占位图回退，重用已安装的MobileCoreTypes声明和原生符号/命名图像，并补UIImage两参数及pad/2x回退。
- 独立arm64 UI库编译/链接、12个实际ABI、188个图形类型资源和block捕获检查PASS。没有新实机日志，LaunchServices或GPU具体失败层尚未证实，图标显示未验收；没有占用设备、部署、GPU/共享XNU缓存/运行态修改，现有独立改动保留。
- 入口：[services192-settings-icons.md](migration/ipados18/services192-settings-icons.md)。本机候选/日志在diting-work/{build,logs/diting,tmp}/services192/settings-icons-20261115；后续以当时的完整UI源码合并重编译，不能覆盖GPU任务后续产物。
