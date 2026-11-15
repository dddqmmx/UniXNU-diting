# services193 亮度与设置图标（2026-11-15）

本任务按低优先级实施，用户已授权将亮度及此前的图标修复刷入设备并测试。
所有构建采用私有源码和对象缓存、`nice -n 19 ionice -c 3`、单编译任务；没有改动另一项 GPU 工作的源码或共享 services169 XNU 缓存。

**亮度：第四版用户确认“亮度正常图标不显示”，真实滑块请求及 DCS 回读 PASS。图标：第四版视觉 FAIL，第六版已部署，视觉待验收，图标测试日志尚未产生。**
当前持久 boot_b 与运行内核为 `ui-brightness-20261115-06`，设备保留在正常 XNU 系统。
GPU 120 fps 目标仍未通过；本轮亮度及图标测试不构成 GPU 性能证据。
更早的 USB、GPU、触控结果见 [services192 交接](交接-2026-11-14-services192-USB已打通-GPU120fps未达标.md)。

## 亮度实现与实际证据

[DitingBacklight.cpp](../../drivers/DitingPower/DitingBacklight.cpp) 与
[DitingBacklightProtocol.h](../../drivers/DitingPower/DitingBacklightProtocol.h) 接入真实 Diting L12 DSI0 DCS `0x51/0x52`。
控制范围0–4095、最低非零1，回读原始单位为控制值的4倍；最高值允许16383饱和。
实际 AppleARMBacklight conformance、首次有效测量后发布、非阻塞请求邮箱、独立 IOWorkLoop、渐变和迟到完成所有权详见
[亮度驱动说明](../../drivers/DitingPower/README.backlight.md)。

[ui_compat18.c](../../userland/ui_compat18.c) 对匹配22H374且具有真实Diting协议/ADT的空映射初始化使用原厂默认曲线。
映射建立后恢复原实现。早期系统保存的精确零偏好会在首次启动时迁移到0.5，并写入一次完成标记；后续用户选择保持原生行为。

| 证据 | 已取得结果 |
| --- | --- |
| 主机协议、ADT、兼容保护和迁移 | 12项检查通过ASan/UBSan |
| 第三版直接IOKit/DCS | 0、1、512、2048、4095、超上限、500 ms渐变、连续最新请求、相同目标、恢复均PASS |
| 第三版黑屏 | 测试恢复原值1；设备偏好实际为 `SBBacklightLevel2=0.0`，用户观察黑屏 |
| 第四版一次迁移 | 设备偏好保存0.5及 `DitingBacklight18InitialValueMigrated=true` |
| 第四版实际滑块 | 用户确认亮度正常；USB实测level=goal=501、requests805/writes473/reads474 |
| 后续偏好保留 | 只读Recovery得到用户选择0.12231758236885071，迁移标记仍为true |
| 第六版正常运行 | 匹配内核USB实测level=goal=623、requests5/writes5/reads6，服务已发布，无在途事务 |

第一版单位错误、第二版上限测试误判、第四版UIScreen setter未触发系统请求的FAIL日志均保留。
第四版UIScreen自动测试失败不能覆盖真实设置滑块的用户PASS。
第六版另有一次性测试，使用实际设置采用的BKS brightness transaction进行0.25/0.75/0.5/恢复，
只读IOKit检查真实DCS与请求变化；生产库不包含该测试。该日志已回收：1024/4096、3072/12288、2048/8192、恢复623/2492四步均PASS，failures=0（控制值/原始DCS回读）。
本轮熄屏后亮屏恢复尚未验收。

## 图标的缓存读取修复

原始 [services192 图标候选](services192-settings-icons.md) 已在第四版加载，Recovery日志包含
`22H374 Settings type icon sync/async recovery installed`，但用户仍观察不到Wi-Fi、蓝牙、通用等图标。
匹配22H374的 `_IconServices_SwiftUI` 实际使用 `imageForDescriptor:` 和
`getImageForImageDescriptor:completion:`；原厂缓存/存储命中可以绕过旧候选覆盖的生成方法。

[settings_icons18.h](../../userland/settings_icons18.h) 在 `ISTypeIcon` 上覆盖这两个实际读取入口。
真实位图保持原结果；透明空位图经私有32×32内存像素检查后进入已有MobileCoreTypes声明及原厂IconServices资源回退。
没有替换 `ISConcreteIcon` 的共享方法，也没有改变其他进程的GPU或CoreImage策略。
异步保持原厂调度，实际回调只含一个IFImage参数；描述器和图标通过正确的block捕获释放。

12项匹配22H374的方法ABI、188项原始图形资源及block所有权检查PASS。
第六版一次性测试检查10种真实缓存读取结果的像素/颜色差及一次异步Wi-Fi回调；
像素检查和 `local=bitmap` 日志都不能代替用户可见图标验收。

## 当前部署与本机记录

| 项目 | 状态 |
| --- | --- |
| 设备 | diting / SM8475，序列号 `1d4eb066`，22H374 / Darwin24 |
| 几何与目标 | unlocked=yes、slot=b、boot_b=`0xC000000` |
| 当前镜像 | `diting-work/build/services193/ui-brightness-20261115-06/boot/diting-xnu-initial.img` |
| 构建 | 完整内核无未解析符号；八核；120 Hz请求；services182的33792字节loader |
| 刷入与启动 | 稀疏boot_b两块4.485秒OKAY；同镜像临时启动2.541秒OKAY |
| UI安装 | ID `3d4f894a-5231-44f3-8ed9-b552dc3cb1d1`，stage6/error0，首次写入3个文件 |
| GPU安装 | 仍stage1/error17/files-written0；没有将此独立问题修掉或写成成功 |
| 设备保留 | 正常第六版系统；没有替换Data/SystemOS或刷完整userdata |

构建、日志和临时脚本分别在本机 `diting-work/build/services193/`、
`diting-work/logs/diting/services193/`、`diting-work/tmp/services193/` 的对应候选目录。
第六版关键记录为 `install-boot.json`、`after-boot-device-state.json`、`native-abi-checks.json`、
`host-checks.json`、`native-settings-brightness-result.json` 与实际BKS测试日志；图标测试日志尚未产生。
第五版仅完成内核构建，没有打包或部署；当前回退候选为实际使用过的第四版。
镜像哈希、审计和镜像回读按用户偏好跳过；私有系统输入、二进制和日志只留本机，未提交或推送。

待完成：第六版图标视觉观察、打开设置后的图标日志回收、熄屏恢复。本文件会随实际结果更新，不把部署OKAY写成视觉PASS。
