# services188-battery：Diting 电量识别代码与主机验证

日期：2026-11-14（Asia/Shanghai）。目标：Redmi K50 至尊版 / diting / SM8475，
iPadOS 18.7.10 / 22H374 / Darwin24。源码基于 `bd21ea788120fc48037e4f5981f3893e8dbaa4c3` 加本轮未提交电池改动。

**电量识别代码、故障处理与主机编译已完成，实机电量显示尚未验收。**
本轮仅操作电池源码和独立构建目录，没有重启、刷写或读取设备；不改部署状态。
`services188-battery` 是独立的代码验证目录，不代表已部署的 services188 镜像。
GPU 并行任务的源码、内核缓存、产物、运行脚本与设备操作均未使用或修改。

## 协议依据与修正

官方流程：[UniXNU 电池状态上报](https://unixnu.com/zh/porting/power#battery)。
服务结构参考本机固定 UniXNU-sheng 源码；XNU API 对照实际 `xnu-11417.140.69` 的
`IOPMPowerSource.h/.cpp`。板级 PMIC 通道在 `dts/diting.dts` 为 `PMIC_RTR_ADSP_APPS`。

厂商协议参考固定 Xiaomi SM8450 源码 `eff4b40407a1b6f1b1ab84554cbb540bcb495175`：

- [qti_battery_charger.h](https://github.com/LineageOS/android_kernel_xiaomi_sm8450/blob/eff4b40407a1b6f1b1ab84554cbb540bcb495175/drivers/power/supply/qti_battery_charger.h) 定义属性与消息布局。
- [qti_battery_charger.c](https://github.com/LineageOS/android_kernel_xiaomi_sm8450/blob/eff4b40407a1b6f1b1ab84554cbb540bcb495175/drivers/power/supply/qti_battery_charger.c) 的 `battery_psy_get_prop` 定义单位转换，`battery_chg_notify_enable` 使用 TYPE_NOTIFY 与全零参数。

MiCode 的 `diting-s-oss` revision `df8603c6c66a25d50125bd431b9b7ca6b75862b1` 是 DT 仓库，
不含电池驱动，不能将它当成本轮电池驱动源码。所用 LineageOS revision 与 services186 触屏参考一致。

旧代码沿用 Nothing 属性表。Xiaomi 多出 `BATT_CONSTANT_CURRENT=12`，后续属性位移：
温度13、电荷计数15、循环16、设计17、满充18、预计充满20、放空21。
温度原始单位为0.01°C，电荷计数为mAh，充满时间为分钟；旧代码分别误作deci-C、µAh和秒。
电压与电流仍为微单位，电流从放电正号转换为 IOPMPowerSource 的充电正号。
完整字段表见 [DitingPower README](../../drivers/DitingPower/README.md)。

## 实现行为

- `ShengBattmgr.c/.h`：保留 API 名称和32位诊断字段索引，使用 Xiaomi 表，只发送 battery/USB GET 与通知订阅。
  核对消息 owner/type/opcode/property、精确长度、符号与范围；错误值不作为合法百分比或温度。
- 基础8项读数先提交快照，随后更新可选容量、时间和适配器信息。已提交值独立存储，下一周期的部分响应不会改写它。
  容量没有有效响应时不新建0%电池；合法0%仍能上报真实低电量。
- 5秒刷新、2秒请求超时、10分钟慢速重试。可选字段失败不反复拖延每次刷新；连续通知合并为一个待执行周期，
  保留已请求的慢速刷新。发送队列拒绝立即推进，不为尚未发出的消息等待固件超时。
- `DitingBatteryState.h`：缺失充电字段保留上次状态；VBUS与是否充电分开，只有固件 FULL 状态表示满充。
  两个独立采样周期确认电源变化，同轮基础/可选更新不重复计数；确认后再计算低电量策略。
- `DitingBatteryInbox.h`：有界队列与通道代次，零长度、超长和满队列如实拒绝。
  down/up 合并时仍丢弃旧数据。PMIC 的发送队列也在通道状态变化时由唯一发送线程清空；其他 ADSP 客户端保持原行为。
- `DitingBattery.cpp`：通过工作循环串行解析与发布，DSP 回调仅复制消息、更新状态并唤醒工作循环。
  首次属性拷入 IORegistry 后才注册服务；过期状态也通过 `setPSProperty/updateStatus` 通知订阅者。
  缺失测量属性从当前发布值中移除；通道断开保留上次读数并标记过期。
- `DitingPower.cpp`：电池 metaclass 失败单独记录，不伪造服务或阻断正常启动。
  保留 `diting_adsp=2` 启动入口、八核配置和全部共享 RPMh/GCC/SMMU 所有权。
- 两套 `build-diting-power-native.py` 均支持 `--battery-only`，保留各自输入解析方式。
  检查用 `libDitingBattery.a` 不代替生产链接的完整 `libDitingPower.a`。

## 主机结果

独立目录：`diting-work/{build,tmp,logs/diting}/services188-battery/`。
Darwin24 头文件复制到本轮树中，未调用共享 `services169/tree` 的内核准备或构建。
工具链：`/home/dddqmmx/.unixnu/run.sh`，patched clang24，`arm64-apple-ios18.0`、`-mkernel`、`-mgeneral-regs-only`。

| 检查 | 结果 |
| --- | --- |
| `python3 -m unittest discover -s tests -p test_diting_battery.py -v` | 11项 PASS，ASan/UBSan |
| 单独电池对象与 `libDitingBattery.a` | ARM64 Mach-O 编译及归档 PASS |
| 主脚本完整 `libDitingPower.a` | PASS |
| 实际 runtime overlay 的 merged recipe，完整 `libDitingPower.a` | PASS |
| 两套脚本语法、差异空白检查 | PASS |
| 完整内核链接、镜像打包、安装、硬件电池显示 | 未执行 |

最终源码的主入口完整归档与电池独立归档，回执和日志保存在
`diting-work/logs/diting/services188-battery/{main-power-final,battery-only-final}.{json,log}`，退出码均为0。
生产入口的逐对象编译日志保存在本轮 `production/tree/build/diting-power/`。

测试覆盖 Xiaomi 字段与单位、基础提前发布、精确消息校验、合法边界与无效哨兵、请求超时/拒绝/迟到响应、
可选失败退避、连续通知、已提交快照、充满状态、插拔确认、零电量与未知容量、队列溢出及合并重连；
另以5000组确定性畸形帧检查边界访问。
完整电源归档采用既有“音频容器缺失”编译模式，没有提供本机 CNT，结果仅用于编译验证，不作为 PCM 成功证据。
本轮没有镜像哈希、镜像审计或设备回读。

Linux 窄构建在工具链环境中先对**独立树**调用 `diting_build_host.linux_env(tree)`，再执行
`scripts/build-diting-power-native.py --tree <独立树> --battery-only`；生产验证改为完整归档并使用
`kits/DitingPlatform24/build-with-runtime.py --overlay-only` 生成的本轮 overlay。

## 实机验收边界

本轮未操作手机。需要本机 signed `adsp.mbn`、真实 PAS/ADSP ready 和 PMIC 通道响应，
编译成功不能证明这些前提已满足，也不能证明 status bar 或 Settings 已显示电量。
后续配对内核应保存 `DitingBattery: PMIC channel up` 与 `physical power source published` 日志，
比较真实容量/原始读数与 OS 的 `IOPMPowerSource` 属性，并观察插拔、充放电和屏幕恢复。
读取失败应看到 `DitingBatteryStale` 和缺失字段，不能变成默认0%或假满充。

协议没有 transaction ID，能可靠排除不同 owner/opcode/property 的迟到响应和上一通道代次的排队消息；
同一通道内、同一属性在后续重试时收到的异常迟到响应仍无法仅凭消息字节区分，需依赖实际固件响应时序。
没有改动 PAS 重启流程；通道重开与整个 ADSP fatal 后重新加载属于不同验收项。
