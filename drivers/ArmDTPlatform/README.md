# ArmDTPlatform

通用 AArch64 Darwin 平台专家（`IODTPlatformExpert`）：接管传入的 Apple 格式设备树，并由它发布 CPU、`arm-io` 与平台设备。

## 适配硬件

- 任何以 Apple 格式设备树启动的 AArch64 平台；在 UniXNU-diting 中用于 Redmi K50 Ultra（diting）/ SM8475
- 要求设备树提供 `/chosen`（CHRP NVRAM 代理，`nvram-bank-size`、`nvram-proxy-data`）、`/cpus`、`/arm-io`、`/defaults`、`/options`

## 仓库关系

上游 [UniXNU/ArmDTPlatform](https://github.com/UniXNU/ArmDTPlatform) 的 Diting 变体：保留通用结构，按 Diting 的设备树与 Darwin 24 的平台服务做了适配，并在 CPU nub 发布后调用板级电源入口。

## 说明

- 不含板名判断，也不含 Diting 专有寄存器；板级差异由设备树与 `drivers/DitingPower` 提供。

构建、接口与组件目录见 [UniXNU 组件目录](https://unixnu.com/reference/components)，板级输入与编译入口在 [UniXNU-diting](https://github.com/Chengdong-Yu/UniXNU-pong)。

## 实现细节

### 文件

- `ArmDTPlatform.cpp`：`IODTPlatformExpert` 派生。匹配 DT 根名 `device-tree`（probe score 5000），建立 CHRP NVRAM 代理，遍历 `arm-io` 发布 `IOPlatformDevice`，再枚举 `/cpus` 让 CPU provider 附着到平台专家而不是 `arm-io`。
- `Info.plist`：`org.blu-sharky.ArmDTPlatform`，`IONameMatch` 为 `device-tree`。

### 启动顺序

`configure()` 检查设备树结构和 CHRP NVRAM 输入，发布 CPU 与平台设备，并完成真实 CPU/GIC 启动；必要步骤失败时返回失败。电源、Wi-Fi 和平台注册按原启动顺序执行，GPU 入口随后执行。设备树缺少 CHRP 代理时记录错误并返回失败。

2026-11-13：services171 调整可选外设与平台注册顺序后，实机八核和单核均仍触发 `IOSecureBSDRoot()` 的 `assert(pe)`，该顺序改动已撤回。services172 只记录实际启动阶段与 CPU 失败点，供同一项致命检查报告上下文；不以计数器替代硬件成功条件。
