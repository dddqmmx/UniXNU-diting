# services182：本机 RAM 区间、主窗口分配与固件日志导出

> 后续状态（2026-11-14）：services185 已由用户确认进入系统，但触屏不工作；当前
> boot_b=services185。services183/184 的 OEM minidump 已确认 services179–182 八核
> 上线、三卷挂载与原生 launchd 执行。见 [当前修复与验收记录](services185-bdf-power-boot.md)
> 和 [minidump/RPMh 取证](services184-rpmh-minidump.md)。下方保留 services182 当轮结果。

日期：2026-11-14；设备 Redmi K50 Ultra / diting / SM8475，序列号 `1d4eb066`；iPadOS 18.7.10 / 22H374 / Darwin 24。

services182 新固件、加载器和完整八核启动镜像均已完成，并于北京时间 06:05 稀疏写入 boot_b 后临时启动。用户确认仍黑屏自动重启，故障没有修复。当前持久 boot_b 为 services182；services183 的固件菜单诊断已临时启动，用户确认显示 Advanced Logger，日志内容待取得。

## 明确的输入错误

Recovery 的有效 unflattened `memory/reg` 与 services169 `live.dtb` 一致：

| 基址 | 长度 | 结束（不含） |
| --- | --- | --- |
| `0x80000000` | `0x6a000000` | `0xea000000` |
| `0xf4600000` | `0xba00000` | `0x100000000` |
| `0x800000000` | `0x39700000` | `0x839700000` |
| `0x839d00000` | `0x1e00000` | `0x83bb00000` |
| `0x840000000` | `0x140000000` | `0x980000000` |

bank sum 为 `0x1f0f00000`（8,337,227,776 字节），不是 nominal physical capacity。MemTotal 为 `7312428 kB`，本轮选用 8 GiB 机型的配置。旧 JSON 保留在 `configs/diting-memory-sm8475-12g-services57a.json`，其谱系不改写成本轮实测。

旧固件源码主区 `0x900000000+0x140000000` 结束于 `0xa40000000`，超过报告的 RAM 上界；在本机该基址之上只剩 2 GiB，无法容纳 4 GiB 引导窗口。原 loader 只相信 EFI conventional descriptor，未用真实 RAM whitelist 检查主窗口。旧模板 FD 是否使用同份源码尚未核实；**源码错误不等于已确定黑屏根因**。

## 同批修正

- 固件 `MemoryMapLib.c` 主区改为实际连续 5 GiB bank `0x840000000..0x980000000`；仍保留 4 GiB bootstrap window 与八核。
- `dts/diting.dts` 使用五个实际 banks；默认 JSON 按有效 live reservations 和固件 staging 范围生成 allocatable 列表，ramoops 使用 `0xa7000000+0x400000`。
- `diting_memory.py` 校验范围、对齐、重叠、保留区、bank sum 和 nominal capacity；生成/打包入口检查硬件 DTB 与 RAM profile 一致。`diting-dram-ranges` 与 `diting-allocatable-ram` 分别表达物理 banks 和可分配范围。
- DarwinBoot24 在主分配前验证完整 metadata 与 EFI map，只合并有效 WB conventional descriptors，求交后分配，保留 Mach-O 的 2 MiB block offset，并排除 framebuffer。缺失/非法 metadata、越界、重叠 descriptor、错误 AllocateAddress 返回值均拒绝；额外 leases 使用相同范围/属性检查，保留 256 MiB firmware pool。
- `board.toml` 与 ADT/容器入口默认八核，nominal capacity 从匹配 profile 读取；generic UniXNU encoder 通过精确、幂等兼容补丁接受多 banks，要求显式 nominal capacity。单核 output audit 的旧限制也修正，实际本轮仍按用户要求跳过镜像审计。
- CI 从 DTS 同时生成 FDF 内嵌 DTB 与启动输入，应用 pmsg 补丁。整轮编译失败直接失败，不再因旧/部分 FD 存在而误通过。
- 固件 dump 同时读取 pmsg/console，分别使用 write-cursor/oldest-byte 语义，每个 zone 最多导出 16 KiB，保留换行并拒绝损坏 header。当前日志保留流程仍未可靠；这里只修复原有漏读与解码缺口。
- 新 LLVM 检出上游 keypad 的未使用 `mKeyEverSeen` latch；删除已不被读取的变量/赋值及过时注释，保持实际按键和已有 reset 路径。

两份 `build-ipados18-prelinked-boot.py` 均维护，overlay 的既有 rootful policy 与外设配置选择保留。新 CLI 显式接收 loader/template/hardware DTB，内核构建输入与本轮打包输入分别记录。

## 本地验证与构建

- 实际主分配器 19 个 host EFI mocks 用例通过：新 bank 正向、旧 bank 越界、WB/非 WB 拼接、runtime/reserved、非法 stride/size/overlap/overflow、缺失 metadata、framebuffer、失败 lease 与错误地址返回。
- 实际固件 dump 10 个用例通过：两种 cursor、部分/满/环绕记录、非法 header、16 KiB 上限、换行/控制字符、空记录。
- 默认八核、physical/allocatable wire、错误 12 GiB DTB 与 nominal capacity 拒绝、generic encoder 多 bank/幂等/显式容量要求、Python 源码编译检查通过。
- 新 DTB 与内核实际构建时的 DTB 只有 `/memory@80000000/reg` 不同；其余 driver 输入一致。因此复用 services181 内核，无需重新编译相同内核；本轮打包 DTB 单独显式记录。
- 新 FD：固定 `Emilelu/uefi-mondrian` revision `e06211de59325bbd086c312324bdbf40e21d1045`，`diting-work/build/services182/uefi-source/Build/ditingPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd`，3 MiB。本机构建先补齐固定源 NASM 依赖，再修复上述 dead latch；日志与失败回执保留。
- 新 loader：`diting-work/build/services182/darwinboot/artifact/DarwinBoot24.efi`，33,792 字节，Darwin 24 boot_args ABI 保持 0x480/1024-byte command line。
- 新 template：`diting-work/build/services182/template/diting-unixnu-uefi.img`，从上述新 FD/BootShim/DTB 组装；**没有复用 services168 的旧 template**。

证据在 `diting-work/logs/diting/services182/`：`firmware-prepare.log`、`firmware-build{,-2,-3}.{json,log}`、`mu-nasm-*.json`、`allocator-test.log`、`memory-source-validation.json`、`kernel-input-reuse.json`、`template-package.log`、`package.{json,log}`。dump 测试脚本/host harness 在本轮 `tmp/`。源码边界和对象编译检查与实机结果分开；没有计算 FD/boot 镜像哈希或做设备镜像回读。Mu 构建框架自身默认的 PE 模块检查有运行，记录在固件 build log。

## 部署与实机结果

完整镜像 `diting-work/build/services182/boot/diting-xnu-initial.img` 为 101183488 字节，实际使用本轮新 FD、BootShim、DTB 和 loader，复用 services181 的八核内核。

`2026-11-13T22:05:56.688449+00:00` 部署完成：diting / unlocked=yes / current-slot=b / boot_b=192 MiB 检查通过；稀疏写入两块全部 OKAY，4.700 秒；随后临时启动 OKAY，2.481 秒。逐条证据为本轮 `install-boot.json` 和 `install-boot.log`。用户确认仍黑屏自动重启并回到 Fastboot；`hardware_boot_confirmed=false`。源码范围错误已修正，但其修正没有解决本次启动故障。

services183 用本轮同一镜像和同尺寸 payload 制作只打开 Tools 的临时配置，没有 XNU 或额外 loader 入口，也未覆盖 boot_b。用户确认只显示 Advanced Logger。这证明本轮固件能达到菜单，不能单凭菜单证明 XNU 进入或副核上线。固件日志随后已收齐，initrd/FAT/config/menu 路径正常；SCM/clock/chipinfo protocol 的缺失保留为固件日志，不将它们自动认定为 XNU 重启根因。OEM minidump 后续已确认八核和用户态阶段，见上述 services184 记录。菜单回执在 `diting-work/logs/diting/services183/firmware-menu-{package,boot}.json`。

不写 userdata；没有提交、推送、goal 或代理任务。保留原始项目改动、匹配的 restore/固件输入与既有回退候选。
