# services192 设置图标候选修复（2026-11-15）

状态：源码和独立 UI 库编译/链接 **PASS**，匹配 22H374 的接口及资源检查 **PASS**；
**未部署、实机图标显示未验收**。本任务优先级低，用户要求不得影响另一项 GPU 加速工作。
未占用设备/USB/串口，未重启、刷机或修改运行态；未修改 GPU 源码、构建 overlay、共享 XNU 缓存或部署回执。
现有 GPU、USB、电池/ADSP 修改保留。当前系统和 GPU 性能状态仍以
[services192 交接](交接-2026-11-14-services192-USB已打通-GPU120fps未达标.md) 为准。

## 定位依据与边界

用户确认目前只有 Game Center、iCloud 能显示图标。
本轮只对本机保留的 OS/SystemOS 做指定文件和代码段的有界只读分析；没有读取设备当前数据库或新运行日志。

- 匹配 22H374 的 Preferences 主列表使用 `ISIcon initWithType:` / `initWithBundleIdentifier:`，
  然后交给 `_IconServices_SwiftUI.AsyncIconImage`。Wi-Fi、蓝牙、通用、通知、声音、专注、电池、隐私等
  使用 `com.apple.graphic-icon.*` 类型。原有 `settings_compat18.c` 的修补只覆盖两个 `UIImage imageNamed` 入口
  和 MobileIcons 的旧缩放函数，没有覆盖 `ISTypeIcon` 的同步/异步图像生成。
- 非空结果不一定是图标。原厂 `ISConcreteIcon` 明确区分 `IFImage.placeholder`；异步完成回调只接收
  **一个 IFImage 参数**。只补 UIImage 或只检查 nil 无法完整处理这条路径。
- `/System/Library/CoreServices/MobileCoreTypes.bundle/Info.plist` 和 CoreGlyphs 公共/私有符号目录存在。
  原始声明包含 188 个图形类型，其中 185 个直接给出符号配置；另外三个通过父类型或命名图像提供资源。
  例如 Wi-Fi 为 `wifi / blue / white`，专注为 `moon.fill / indigo / white`。
  设置程序中的图形类型引用均能从这些原始声明解析，未发现需要编造图标的资源缺口。
- 次级 UIImage 路径也有适配遗漏：实际程序引用的 `imageNamed:inBundle:` 没有被拦截；
  原有 pad 回退只改 idiom，未覆盖本机小型设置资产目录已裁剪为 pad/2x、呈现身份为 phone/3x 的组合。

**已证实的是现有适配遗漏主列表 IconServices 路径，以及 UIImage 回退不完整。**
类型记录解析或远端图像生成返回失败/占位图，是本候选覆盖的故障分支；
没有新实机日志，不能宣称 LaunchServices 数据库损坏、GPU 渲染失败或任一具体运行态原因已被证实。
历史 services38 已安装强制软件 CIContext 仍空图，不能再次把切换全局渲染器当作已验证的解决方案。

## 实现

源码为 [settings_compat18.c](../../userland/settings_compat18.c) 和
[settings_icons18.h](../../userland/settings_icons18.h)。新增头文件由原有编译单元直接包含，无需修改构建配方。

1. 只在 Preferences 的 `ISTypeIcon` 类上添加同步/异步生成覆盖，使用实际继承的原 IMP。
   安装匹配 IconServices UUID `866FB200-4484-311E-B812-3493572D2E60`、可执行映射和实时方法 ABI。
   没有替换 `ISConcreteIcon` 的共享继承方法。
2. 原结果为真实位图时保持原结果；nil、占位图或缺少位图时，只有已安装声明中的图形类型才进入回退。
   从原始 MobileCoreTypes plist 解析 `UTTypeIcons`，有界跟随 `UTTypeConformsTo`，包括 Passwords/Intelligence 的父类型。
   继续使用原厂 `ISResourceProvider`、`ISGraphicSymbolResource` 和 `IFImage`，不维护手绘图标或替代符号表。
3. 符号使用与 `ISSymbolIcon` 相同的原生生成接口，传递原描述器的尺寸、scale、明暗外观、对比度、方向和 tint。
   命名图像保留原生 treatment；尺寸和比例检查限制无效或过大的请求。
   本地也无法生成真实位图时返回原结果，不把占位图标为成功。
4. 异步保持原厂调度、重试和最终缓存，每次原回调只向调用者传递一次结果。
   复制描述器，使用 C block 的 `NSObject` 捕获属性，让取消请求时销毁回调也能释放图标、描述器和完成 block。
5. 补上 `UIImage imageNamed:inBundle:`，失败时合并原 traits 和 pad/2x traits，保留原明暗外观。
   回退仍限于原有系统/Preferences bundle 范围；MobileIcons 的 Metal 缩放代码及 GPU 策略保持原实现。

## 主机检查与候选

所有编译/分析命令为 `nice -n 19 ionice -c 3`；编译器逐个运行，不调用完整内核构建。

- 设置编译单元使用 `-Wall -Wextra -Werror` 编译通过。
- 五个原有 UI 编译单元完成 arm64/iOS18 链接，候选带实际链接器生成的 ad-hoc 签名及原 install name。
  Linux Clang 23 对公共 `ui_compat18.c` 的既有 `poster_store_original` 产生 `-Wunused-but-set-global`；
  只在独立构建该公共编译单元时关闭这一项警告，没有修改共享源码。设置编译单元保持全部严格诊断。
- 12 个实际 22H374 私有/公开方法签名检查通过；188 个原始图形类型的资源解析通过。
  这是接口/资源静态检查，不是已执行的设备渲染测试。
- LLVM IR 确认回调复制时保留两个对象、复制完成 block，销毁时按相反顺序释放；完成接口参数数目为 1。
- 未运行性能测试、镜像哈希/审计或设备镜像回读。

以下路径相对 `/home/dddqmmx/UniXNU`，Apple 资源、生成接口数据及产物仅保留本机：

| 内容 | 路径 |
| --- | --- |
| 独立候选 UI 库 | `diting-work/build/services192/settings-icons-20261115/libditing-ui18.dylib` |
| 候选源码快照 | `diting-work/build/services192/settings-icons-20261115/source/userland/` |
| 编译命令/结果 | `diting-work/logs/diting/services192/settings-icons-20261115/adapter-build.json` |
| 22H374 接口/资源/捕获检查 | `diting-work/logs/diting/services192/settings-icons-20261115/native-abi-checks.json` |
| 构建/检查脚本 | `diting-work/tmp/services192/settings-icons-20261115/build_adapter.py`、`check_native_abi.py` |
| 修改前源码 | `diting-work/tmp/services192/settings-icons-20261115/source-before/settings_compat18.c` |

## 后续验收

候选没有写入当前 GPU runtime 或部署目录。后续合并时，应将 `.c` 和新增 `.h` 一起纳入当时的真实源码 overlay，
基于当时的 UI 源码重编译整个 adapter；不要用此候选覆盖另一任务随后生成的 UI 库。
部署与打开设置的验收应安排在 GPU 工作允许的窗口，沿用已有变更部分更新流程。

检查 `/private/var/mobile/Library/Logs/Diting-Preferences18.log` 的安装记录和最多 12 条
`type icon ... native=... local=bitmap/unavailable` 记录，再确认 Wi-Fi、蓝牙、通用、通知、声音、专注、电池、隐私，
以及命名图像的 Passwords 等在明暗模式和重新打开设置后显示正确。
`local=bitmap` 仅证明返回了真实非占位位图，不能代替可见内容、颜色和布局验收。
若空图对应非占位但内容透明的位图，或本地原生渲染仍失败，需继续依据该次运行结果定位下一层。
