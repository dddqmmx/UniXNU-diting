# services190：系统内 USB 接管与诊断

2026-11-14，diting / SM8475，22H374 / Darwin24，Linux 主机。持久 boot_b=services190，八核。

**USB 首轮实机验收 PASS**：CDC 480 Mbps 枚举、实时日志、完整 help/status、真实 64 字节 RAM 读取、低地址拒绝；70 秒复测后收到 reboot accepted 与完整 END，再自主进入 Fastboot。原生 Mux/Finder 尚未验收。GPU 性能仍 FAIL，services189 用户报告动画瞬间到终态、没有连贯动画；本轮没有新的流畅度观察。

## 本机输入与实现

- 旧 services184 日志显示 loader LocateProtocol 返回 EFI_NOT_FOUND；services190 的实时 status 也读到 stage1/status=0x800000000000000e/handoff全零。services182 FD 排除了 vendor USB 依赖栈；不伪造 loader flags。
- 只有独立 DMA、真实 board PHY/时钟/供电就绪后，才退役 inherited DWC3。检查 active endpoint command，按 Linux6.12 跳过 EP0/EP1 ENDTRANSFER，EP≥2 使用真实 resource index；真实 DEVCTRLHLT、event rings 清理与软复位后申请 SID0。
- 本机 device override 是值/地址序列 `<0x50 6 0 7 0x63 8>`，即 6=0x50、7=0、8=0x63；host override 单独保留。语义核对 NothingOSS c402fcdb 的 `drivers/usb/repeater/repeater-i2c-eusb2.c`。
- ldob5=880 mV、ldoc10=1200 mV、smpb10=1800 mV。仅显式 provider init-mode 写模式，复用 Wi-Fi 的 generic4→raw7 映射；smpb10 没有 init-mode，不写 mode。ldob2 的 provider init 是3072 mV，但上游 USB consumer 最小请求为3075 mV；本机连接 Recovery 的 sysfs 实际读数也是3075000 µV，因此保留3075 mV。
- OS 的 `/diting/firmware/adsp.mbn` 缺失。本机 ADSP reservation 为0x9fd00000/0x3100000，替换旧0x85700000/40MiB值；签名 split 固件来自本机 modem_b 的 services171 只读取证。文件优先，仅 ENOENT 且显式 `diting_adsp_builtin=1` 时使用 builtin。
- 原始签名 ELF 为23,503,272字节，直接内嵌使 native 与固定 Apple prelink 地址重叠。现在用11,547,284字节的 zlib 容器存储，受限解压后由原有 MDT/PAS 路径消费；加载结束释放缓冲。原始签名字节不变，PAS 认证保留。
- CDC 回复以 BEGIN/END 分帧，4KiB回复优先于日志；`read HEX-KVA BYTES` 限1..512，通过 `ml_nofault_copy` 读取真实 managed RAM，拒绝低地址、溢出及缺失映射。传输失败保留 reply/log cursor。
- Linux 首次打开 tty 的默认 ECHO 曾把日志碎片拼入命令。主机工具改为暂停 DTR、raw mode、清队列，再恢复 DTR；完整 status/help/RAM/reboot 随后实机通过。
- 可选独立 worker 在约1/10/30/70秒 append+fsync `/private/var/logs/diting/usb-boot.log`，总长上限256KiB；Data skeleton补充 logs/diting。Recovery 文件验收继续记录在本地采集回执。

## 构建与实机证据

5项 USB ASan/UBSan、8项 ADSP ASan/UBSan 通过；完整本机签名固件经同一 native 解压器逐字节比对通过。生产 power archive、kern_exec 与完整内核链接通过，USB reader/snapshot、ADSP begin/end 为强符号，无未解析符号。

构建初次因 version 重复声明、ADSP FDT 被误用为 Wi-Fi 全局输入而失败，均已定位修正；ADSP 与 Wi-Fi 输入分开。打包重叠失败由固件压缩修正，不改变固定 Apple storage bindings 或 loader。

八核候选101,183,488字节，boot args986字节，loader仍33,792字节。核对 diting/unlocked/b 和192MiB boot_b 后，两块稀疏写入4.552秒、临时启动2.507秒均OKAY；Data/SystemOS 未替换，镜像哈希、审计与回读按用户要求跳过。

| 实机项 | 结果 |
| --- | --- |
| 物理枚举 | ffff:0128 / Diting XNU console / 480 Mbps |
| 真实控制器退役 | id=0x33313130；DEVCTRLHLT；end_errors=0；ownership=2 |
| ADSP / PMIC | stage13/error0，SCM0:0，RX/TX持续增长，PMIC_online=1 |
| 电池诊断 | stage4，percent100，flags0xb；界面及插拔未验收 |
| 命令与 RAM | help/status、64字节真实读数、地址0 copied=0均完整分帧 |
| 自主 Fastboot | reboot accepted与END均收到，随后主机检出1d4eb066 |
| 稳定复测 | 70秒，command_errors=0 |

本地证据在 `diting-work/logs/diting/services190`：`usb-live/validated/check.json`、各命令 raw log、`install-boot.json`、`link-check.json`、`adsp-real-decompression.json`、`adsp-tests-compressed.log`。本机 FDT/provider/consumer 差异在 `build/services190/usb-board/{contract,differences}.json`，Recovery电压读数在 `logs/diting/services190/usb-board/recovery-regulators.txt`。

当前 USB 链路用于继续读取 GPU/显示日志与有界内存。构建、GPU提交或拷贝完成不代表动画流畅；缺少物理 panel latch 和新的用户观察时，性能仍保持 FAIL。保留 ST61Y、services182 实测 RAM/FD/template/loader、services187 runtime 与独立电池任务全部修改；源代码未提交。
