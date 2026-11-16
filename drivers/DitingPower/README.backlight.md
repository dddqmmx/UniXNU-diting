# Diting 亮度控制

services204 私有候选在每次一字节 DCS 读取前单独协商返回长度，避免继承亮度查询的两字节长度。
`display panel` schema 2 保留每次 DA/DB/DC、地址模式、像素格式及刷新档位的原始回包。
地址模式和像素格式查询失败只记录可选证据错误；身份和档位未确认时不发送新时序。
本候选主机检查已通过，实机结果待验证。

## 板级输入与回读单位

- Diting L12 AMOLED，1220 × 2712；设备树含 `l12_36_02_0b_dsc_cmd` 和 `l12_42_02_0a_dsc_cmd` 两种面板。
- SM8475 DSI0 寄存器为 `0x0ae94000`，长度 `0x400`，VC0。
- DCS `0x51` 写入：0 表示关闭，最低非零值 1，最高 4095。线路字节顺序为高字节、低字节；`bl-inverted-dbv` 是字节交换标记。
- DCS `0x52` 两字节回读：**每个写入步长对应 4 个原始回读单位**。services92 已记录 1604→6416、787→3148、3894→15576、4045→16180；2026-11-15 第一版设备测试又得到 1→4。
- 原始回包保留于 `DitingBrightnessRawReadback`，换算后的控制值为 `DitingBrightnessReadback` 和 `IODisplayParameters.brightness.value`。`DitingBrightnessReadScale=4`，控制值的 `DitingBrightnessReadQuantum=1`。
- 原始值按最近写入步长换算，满量程限制在 4095；超出 14 位回读范围、错误 VC、畸形包和一字节包均拒绝，不把错误回包当成测量成功。

输入见 [diting-display-sm8475.json](../../configs/diting-display-sm8475.json)。ADT `/chosen/diting-dsi-backlight` 是精确 32 字节 `<Q6I`：基址、长度、最大值、最低非零值、字节交换、通道、版本。驱动要求上述 Diting 输入；原先检查最低非零值为 10 会拒绝实际生成的 ADT。

## 服务与事务

[DitingBacklight.cpp](DitingBacklight.cpp) 在独立 IOWorkLoop 上使用命令 FIFO。亮度事务最多两个 DWORD；L12 时序事务按数据包边界拆分，最多 16 个 DWORD（固定厂商实现的 64 字节 FIFO），不分配物理 DMA 缓冲。
[DitingBacklightProtocol.h](DitingBacklightProtocol.h) 定义数据包、单位换算和完成所有权规则。

首次真实回读通过后才发布亮度属性和 `backlight-control`，并使用实际 AppleARMPlatform 的 AppleARMBacklight metaclass 提供 conformance。类晚到时每 100 ms 查询，最多 30 秒；不可用时记录原因，保持正常系统启动。板级类名称仍是唯一的 DitingBacklight，不注册重复的 AppleARMBacklight 类，也不修改猜测的 vtable 槽位。

事务只临时借用命令引擎相关控制位，保留像素流水线、PLL、显示供电、视频/刷新 IRQ 和其他硬件故障。DMA 完成且控制器不忙才允许回收；读事务还要求真实 BTA 完成。50 ms 超时保留 FIFO 所有权，迟到完成可以由后续请求收取，空闲状态本身不算完成。

写入退休后等待至少 20 ms 再读面板。已退休但未被接受的值最多切换 HS/LP 重试一次，记录实际错误并保留后续请求。恢复目标已经等于当前测量值时，若仍有旧错误则重新回读，成功后才清除错误。

`brightness` 支持有符号 32/64 位数字，负值归零；`FadePeriod` 为 16.16 秒，最多 5 秒渐变。可选 `backlight-level` 只作 CoreBrightness 元数据。未知属性和错误类型拒绝。独立请求定时器与最新值邮箱防止并发请求的唤醒丢失，调用方不等待 DSI 事务。

配套 [ui_compat18.c](../../userland/ui_compat18.c) 只对匹配 22H374 CoreBrightness 的空映射初始化窗口使用该运行时自带的默认曲线；通过真实协议服务或完整 Diting ADT 安装保护。映射建立后恢复原函数。默认曲线不构成面板光度校准。

早期未有效启用背光的系统可能保存 `SBBacklightLevel2=0.0`；真实控制启用后，该值使面板降至最低亮度。首次启动时，SpringBoard 在初始化亮度控制器前将这个精确零值迁移到0.5，并保存 `DitingBacklight18InitialValueMigrated`。完成标记使后续用户选择（包括最低滑块值）保持原生行为；已有正值、缺失值和非数字不被改写，保存失败允许下次重试。

## 构建与部署

- `CONFIG_XNU_DITING_BACKLIGHT=y` 独立于 Wi-Fi；`build-diting-power-native.py` 构建静态归档，显示成功启动后调用亮度初始化。
- 打包器加入 `diting_backlight=1`。实际源码、recipe 和 merged 构建入口都必须使用同一候选。
- UI 更新可通过 `scripts/build-diting-ui-stage.py` 与 `DITING_UI_RUNTIME_ARCHIVE` 在 launchd 前安装 `/diting/libditing-ui18.dylib`。先保存本候选独立备份，再原子替换 UI 文件；GPU 文件和标记保持原状态。
- 低优先级工作使用私有 XNU 源码/对象缓存、`nice -n 19 ionice -c 3` 和单编译任务，不写 GPU 工作的共享缓存。

## 当前验证

2026-11-15：12项实际协议/ADT/兼容保护/首次偏好迁移 host 测试通过ASan/UBSan。图标修复及亮度实现已集成；第三版直接IOKit/DCS测试的0、1、512、2048、4095、超上限、500ms渐变、连续最新请求、相同目标及恢复均PASS。

第三版恢复原值1后，用户观察为黑屏；只读设备偏好确认为 `SBBacklightLevel2=0.0`。第四版的一次迁移实际保存0.5及完成标记。用户随后确认“亮度正常图标不显示”；滑块操作后的USB读回为level=goal=501、requests805/writes473/reads474。再次只读Recovery取得用户选择0.12231758236885071及完成标记，证实迁移没有覆盖后续正值选择。实际滑块亮度调节PASS，设置图标在第四版FAIL，熄屏后亮屏本轮未验收。

第六版保留第四版生产亮度代码，并覆盖设置图标的两个真实缓存读取入口；完整内核、八核打包、稀疏boot_b及同镜像临时启动通过。USB匹配UI安装ID `3d4f894a-5231-44f3-8ed9-b552dc3cb1d1`、stage6/error0，实际level=goal=623、requests5/writes5/reads6。第六版一次性BKS事务日志已回收：低1024/4096、高3072/12288、中2048/8192、恢复623/2492均PASS（控制值/原始回读），failures=0。图标像素日志尚未产生，可见图标和熄屏恢复待观察；这些读回不能代替视觉验收。

第一版单位换算错误、第二版上限测试误判以及第四版UIScreen setter未触发系统请求的FAIL日志均保留；UIScreen自动测试失败不覆盖用户真实滑块的PASS。4095写入的原始回读可饱和为16383，其余等级仍要求精确4倍对应。设备原始证据保存在本机 `diting-work/logs/diting/services193/ui-brightness-20261115{-02,-03,-04,-06}/`，不上传私人日志。

## 固定参考

services205 候选（2026-11-16，主机检查与完整内核构建通过，设备验证待完成）：`diting_panel_hz=120` 在实际 GPU 屏幕拷贝及 DPU 刷新交接完成后，唤醒同一工作循环。Xiaomi 型号由保存 live DT 的 chosen 选择和 `mi,panel-id` 确定，ADT 单独传递经过 DSI0/RGB/1220×2712 校验的 `diting-dsi-panel`。[DitingPanelMode.h](DitingPanelMode.h) 读取 DA/DB/DC 作硬件资格检查；本机 L12_36 的实际回复为 c5/10/00，型号名里的 36/02/0b 不作 DCS 回复。L12_42 时序计划保留，但缺少已验证 DCS ID 时不写入。

每次单字节读取先独立退休最大回包长度设置。型号查询已退休但失败时，仍收集独立地址／像素模式证据。仅在型号资格、有效 GET(0b) 和像素模式77全部成立，且真实 BGR bit3 已置位时，保留其他地址模式位，使用 DCS SET(36) 清除 BGR，等待20ms后回读；失败执行一次原值恢复并回读。不能据条件命令接受声称物理颜色正确。DCS 命令编号来自固定 Linux `mipi_display.h`；bit3 采用标准 MADCTL BGR 约定并要求本面板实际回读资格，不设置猜测的方向值或像素格式。

随后回读 2f 原始档位，使用匹配 DTS 字节流切换请求时序，保留 HS 模式和全部等待时间。目标 2f 回读通过后，按 19.2MHz/(2756×请求Hz) 更新 TE 计数。失败时执行有界原档位恢复；未退休命令继续保有同一 FIFO，亮度请求不能覆盖它。`display panel` 输出独立的型号、颜色和时序诊断；这些配置接受结果不能单独证明物理120fps。

- [UniXNU 显示与亮度文档](https://unixnu.com/zh/porting/display)：OS 属性、工作循环、渐变及完成/失败处理。
- LineageOS `android_kernel_xiaomi_sm8450-modules` revision `3b30ba7df6608c97c0ba2d55d679a6b5987c4c66` 的 Qualcomm `dsi_panel.c`、`dsi_ctrl_hw_cmn.c`、`dsi_ctrl_reg.h` 及 Xiaomi `mi_dsi_panel.c`：Diting 面板范围、字节交换和控制器协议。
- NothingOSS SM8475 revision `c402fcdb3a1523d10b15ef1123bced45456f7592` 仅作同 SoC 寄存器和 FIFO 参照；面板身份和板级参数以 Diting 设备树为准。
- [Sheng 亮度服务](https://github.com/UniXNU/UniXNU-sheng/blob/08cde20/package/AppleARMBacklight.cpp) 作 conformance/sandbox 结构参照，不采用其 KTZ8866、双 I²C 或 SM8550 板级参数。
