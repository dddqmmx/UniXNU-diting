# services183/184：OEM minidump 与 APSS RPMh 交易

日期：2026-11-14。设备 diting / SM8475，系统 iPadOS 18.7.10 / 22H374。

services184 已编译、稀疏写入 boot_b 并临时启动，用户仍报告黑屏自动重启。
后续 services185 已进入系统，见 [services185](services185-bdf-power-boot.md)。

## 取证结果

services183 从未挂载 data/metadata/cache 的 root Recovery 取得完整 OEM minidump，
证明 services179–182 均完成八核上线、UFS 三窗口发布、OS/SystemOS/Data 挂载与
原生 `/sbin/launchd.ios` 执行。之前“仍停在 CPU1 / 副核上线未确认”的结论已过时。
普通重启的 pstore 保留对照仍失败，不能用空目录判断没有 panic。

services184 的完整 minidump 有三个同 epoch=`1794611395` 的记录，均停在：

```text
smpe3 mode=7 complete
smpe3 millivolts=0x898 complete
smpe3 enable address=0x43404 value=1
RPMH submit
```

后面没有 complete、timeout 或真实 panic 文本，ldoe7 已全部完成。Recovery dmesg
另有 `PMIC_SID4_OCP` 复位历史，但不能将该 PON 历史逐条绑定到特定 XNU 启动 epoch。
这支持调查提前使能 IPA 的供电契约，不能单凭缺失文本推定具体异常类型。

同轮 kmsg 还显示 `goodix: clocks stay off: 0 0x80000002 0x4220`、固件版本全零及
`Diting touch: Goodix initialization failed; normal UI boot continues`。
这是 services184 的触屏初始化失败证据，services185 触屏的当轮日志尚未取得。

## RPMh 修正

本机 live DT 的 APSS DRV2 布局为 active TCS0–2、sleep TCS3–4、wake TCS5–6、
fast TCS7。旧 GPU TCS3 / WLAN TCS4 使用了 sleep 槽。现在 GPU/WLAN/音频在
`DitingPlatformResources` 中共用完整 active 交易 owner，选择真实空闲 active 槽并
检查同地址冲突；命令 ISSUED/COMPL、控制位回读与 idle/fresh 状态共同决定完成。

提交后超时保持交易所有权，不清除尚未完成的命令，也不让下一请求覆盖它。
真实迟到完成后才回收。10 个 host 场景验证布局、继承请求、地址冲突、控制位
应答、完成、超时保留及迟到回收；完整原生内核链接和八核打包通过。
这些修正保留在 services185，但 services184 自身没有解决重启。

## 解码与读取约束

`scripts/decode-diting-minidump.py` 按实测十槽格式读取：`cigamgol`，raw header
偏移 `0x400`，槽跨度 `0x600000`，`Raw_Dmp!` header 88 字节，section 64 字节，
数据偏移是分区绝对偏移。offset 8 的字段作为观测推定 next-write cursor，offset 12
的 `10` 不当作累计启动次数；本轮顺序为槽3–9、0–2。

DBGC zone header 为 `<III>`，magic=`0x43474244`，capacity=`2 MiB−12`。
console cursor 表示 oldest，pmsg cursor 表示 write cursor。OEM 截短如实报告，
TZ/HYP/CPU section 保留二进制；原始文本和缺失量分别保存，不补造 panic。
候选归属按唯一 payload epoch 匹配，不能仅用内核 banner 判断。

`scripts/capture-diting-recovery.py` 使用 `adb shell -T` 分离 stderr 并保留远端退出码。
二进制读取必须同时退出0、达到声明长度才能晋升为完整文件，失败保留 `.partial`。
SDAM 的 offset64 后192字节尝试实际只返回64字节，失败回执保留，不能当成完整 PON FIFO。

本地证据：

- `diting-work/logs/diting/services183/recovery/extra/minidump.bin`
- `diting-work/logs/diting/services184/recovery/minidump.bin`
- `diting-work/logs/diting/services184/recovery/{provenance.json,decoded/records.json}`
- `diting-work/logs/diting/services184/recovery/decoded/slotNN/md_{kmsg,pmsg}.log`

原始分区、固件、生成对象和完整设备日志留在本地，不发布到仓库。
