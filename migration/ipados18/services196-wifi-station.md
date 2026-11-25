# services196/wifi-20261115：独立 Wi-Fi 联网候选

日期：2026-11-15，Diting SM8475 / QCA6490，iPadOS 18.7.10 / 22H374 / Darwin 24。
用户确认实机接入点为 WPA2-Personal 或 WPA2/WPA3 兼容模式，密码在原生设置输入。

**实际 station 主机检查 255 项 PASS；完整内核链接和八核打包 PASS；未部署，实机联网 UNTESTED。**
本任务优先级低于独立 GPU 工作。设备、串口和共享构建缓存均未被本任务占用；
独立候选以已完成的 services195 为基线，保留它的 GPU、USB、触控、亮度、ADSP 和电池源码。
GPU 后续变更需要再合入 Wi-Fi；此候选不自动替换正在运行的系统。

## 实现

- `drivers/DitingWifi/ath11k/station.patch` 在构建副本中修正实际上游 station：
  请求和状态由锁保护，工作线程独立持有完整加入请求；扫描取消仍完成原生通知，
  切换接入点先撤销旧密钥、信道和队列，断线立即拒绝新 TX。
- nonce 使用 XNU `read_random`；WPA2 验证 EAPOL 长度、MIC、ANonce 和 replay counter，
  M1/M3/组密钥消息重传不重复安装相同密钥，不重置硬件 PN；GTK 按索引保留和撤销，
  组密钥应答使用已安装 PTK 加密。实际密钥安装失败保持关联失败。
- RX 仅在授权后交给网络接口，检查接收错误、目标地址和解密状态；
  beacon miss 的存活判断只使用当前接入点流量。TX 队列最多 128 个帧，报告背压。
- `DitingWifiNet.cpp` 在 `mbuf_adj(..., 14)` 前调用 `bpf_tap_in`，把完整 Ethernet
  帧交给地址配置前的 DHCP/BPF 消费者。发送路径调用 `bpf_tap_out`，处理复制失败、
  断线和队列满，并把 Linux shim errno 转成 Darwin errno。
- 主构建和 merged 构建入口都应用同一 station patch；原生归档默认来自当前
  `--tree` 内的 ath11k 构建，避免链接共享目录中的旧 station。固定上游源码不变。

适用范围为开放网络，以及 2.4/5 GHz CCMP-128 WPA2-Personal。
WPA2/WPA3 兼容模式须提供 PSK 且不强制 PMF。纯 WPA3/SAE、强制 PMF、企业认证和
6 GHz 未实现；返回真实失败，不用成功占位。

## 主机证据

`tests/test_diting_wifi_station.py` 编译被打补丁的实际 station、实际 crypto 和 Linux shim，
以主机接入点模拟器完成扫描、认证、关联、四次握手、密钥安装和双向数据。
使用真实 mac80211 ABI、ASan/UBSan，并校验 WPA2 PMK、RFC 2202 HMAC-SHA1 和
RFC 3394 AES unwrap 已知向量。255 项检查涵盖错误密码、非法密钥消息、原始 PMK、
重传、GTK 轮换、队列满、接入点切换、扫描取消、重入请求和 beacon miss。
这是主机模拟器证据；模拟器的驱动操作不能证明 QCA6490 固件或无线链路可用。

在独立源码目录执行：

```sh
nice -n 19 ionice -c 3 python3 -m unittest discover -s tests -p test_diting_wifi_station.py -v
```

固定公共输入：

| 输入 | 版本 |
| --- | --- |
| Ath12kWifi station/shim | `f6bf561f92cc42761bdd417fce1d53fe1db9477c` |
| Linux ath11k/MHI/QMI | v7.0.14，`458c6079fc1d41d564c37679c8ace02cd83ee817` |
| GPU/系统构建基线 | services195，源码提交 `e6c7a9f7f74f8d303022ce0d1683f2e47b17e7ad` 及该轮未提交快照 |

官方契约见 [Wi-Fi 适配](https://unixnu.com/zh/porting/network)；本机已读取
`UniXNU/docs/zh/porting/network.md`、实际 `nw_abi.h`、XNU BPF 和固定上游实现。

## 独立构建与本机入口

所有路径相对 `/home/dddqmmx/UniXNU`：

| 用途 | 路径 |
| --- | --- |
| 独立源码 | `diting-work/tmp/services196/wifi-20261115/checkout` |
| 构建实际 overlay | `diting-work/build/services196/wifi-20261115/runtime-source` |
| 独立 XNU 缓存与归档 | `diting-work/build/services196/wifi-20261115/tree` |
| 状态 | `diting-work/build/services196/wifi-20261115/work-state.json` |
| 主机检查 | `diting-work/logs/diting/services196/wifi-20261115/station-host-tests-03.log` |
| 源码隔离 | `diting-work/logs/diting/services196/wifi-20261115/kernel-source-isolation.json` |
| 链接检查 | `diting-work/logs/diting/services196/wifi-20261115/link-check.json` |
| 内核构建入口 | `diting-work/tmp/services196/wifi-20261115/run-kernel-build.py` |
| 源码交付补丁 | `diting-work/tmp/services196/wifi-20261115/wifi-source.patch` |
| 交付检查 | `diting-work/logs/diting/services196/wifi-20261115/source-delivery.json` |
| 启动镜像 | `diting-work/build/services196/wifi-20261115/boot/diting-xnu-initial.img` |

构建使用 `chrt --idle 0`、`nice -n 19`、`ionice -c 3`、jobs=1。
缓存通过强制 reflink 复制到独立目录，生成的 Makefile/依赖路径重定位后继续增量构建。
固件、restore、二进制和原始日志仅留本机；镜像哈希、审计和设备镜像回读按既有要求跳过。
最终编译的 station 与通过 255 项检查的源码一致。Wi-Fi/BPF 必需符号均为强符号，
原生内核未解析符号为零，内核为 63,779,608 字节；八核镜像为 101,183,488 字节。
打包按实际 `XNU_DITING_WIFI=1` 加入 `diting_wifi=1`，使用匹配的 33,792 字节 services182 loader。
复用 services195 的 GPU 安装 ID `9a56a71f-502b-4826-b713-2de5948d6a6d` 和
UI 安装 ID `3d4f894a-5231-44f3-8ed9-b552dc3cb1d1`，未修改这些运行时归档。

主工作树保持原状，源码交付补丁只包含 Wi-Fi 驱动、配方、测试和相应文档。
补丁通过对当前主工作树的 `git apply --check`，未实际应用。
下一轮合入前仍需对届时的工作树检查冲突；不要把本候选的 GPU 基线覆盖到更新版本上。

## 实机验收

当前未刷 boot、未临时启动、未打开 USB CDC，未更改 Data/SystemOS。
GPU 任务释放设备后，把 Wi-Fi 改动合入届时的最新候选，再在原生设置中加入用户确认的接入点。
应分别记录固件/扫描、关联/密钥、DHCP 地址及路由/DNS、网页加载、断线重连与持续收发结果。
当前没有这些设备结果，不能标记联网 PASS。
