# DitingTouch

Redmi K50 Ultra / diting / SM8475 的原生 SPI 触屏协议与传输层。实机 Recovery 的
`fts_spi 5.2.4.1` 日志确认控制器为 ST61Y，固件 `0007`、协议 `06`。
live DT 同时列出 ST 与 GT9916S；GPIO119 原始高电平选择 ST，低电平选择 Goodix。

内核生产者仍以 `bsd/dev/diting_goodix.c` / `diting_goodix_init` 命名，增量源码在
`patches/xnu-ipados18/*-diting-st61y-touch.patch`。它使用既有 `/dev/diting-touch`
结构发布真实 DOWN/MOVE/UP，继续由已安装的 `userland/touch.c` 转为 HID，无需改变用户态 ABI。

## 硬件与协议

- QUP0 SE4 `0x990000`，mode 0，最大 12 MHz；GENI 以 4 MHz 初始化。
- GPIO16/17/18/19 为 MISO/MOSI/CLK/CS，cape mux1；GPIO21 IRQ、GPIO20 reset。
- `pm8350c_l2` / `ldoc2` 按实机 provider 的 1.8 V / NORMAL(mode7) 请求；AVDD 为 GPIO71 高使能。
- ST 读取为 opcode + 大端地址 + 一个 dummy + 数据；FIFO `87`，8 字节事件，深度32；
  chip ID `FB 20000000` 返回 `36 40`；System Info 用 `A7 0000` 读取208字节。
- reset 后要求真实 READY、chip ID、System Info 与 FIFO flush echo。scan `A0 00 01`
  与匹配厂商的 `setScanMode` 一样检查总线写入；提交命令还不证明触摸有效。
- 以事件高 nibble 区分 enter/motion/leave，保留其他手指状态；super-resolution 坐标为
  两个 LE16，按 live DT 的输入轴12200×27120归一化。SysInfo sensor轴不用于改输入轴或旋转。
- GENI FIFO FULL_DUPLEX opcode3，GPIO CS 覆盖完整读写并保持最后时钟后10µs；检查完整 RX、
  真实 DONE 与 idle。GENI 超时先 cancel/abort，未完成保留 owner，迟到完成后才回收。
- READY/error 立即释放触点并重新初始化，丢失 FIFO 批次也释放触点。三次连续总线失败触发
  恢复，最多三次恢复而没有真实 DOWN/MOVE；成功输入后恢复预算清零。
- `DitingPlatformResources` 统一 RPMh 与 GCC 投票所有权。只有带 HWCG 的 AHB 分支可在
  `CLK_OFF` + bit1 时视为可用，core/core2x/SE 分支必须真实运行。

初始化失败有界记录并返回正常系统启动。代码不刷触控固件、不运行校准、不写 OTP/工厂数据。

## 验证与来源

`python3 -m unittest discover -s tests -p 'test_diting_touch.py' -v` 执行带 ASan/UBSan 的
协议、FIFO/超时 owner、多指/复位、GCC 并发与 DT/ADT 契约测试。构建入口同时加入本目录
和 DitingPower 头文件，并在 POWER/BACKLIGHT 关闭时构建、链接 resources-only archive。

匹配协议参考是 [LineageOS fts_spi 5.2.4.1](https://github.com/LineageOS/android_kernel_xiaomi_sm8450/tree/eff4b40407a1b6f1b1ab84554cbb540bcb495175/drivers/input/touchscreen/fts_spi)，
不是较旧 fts 或 fst2 驱动。GENI、GCC 和 cape pinctrl 的寄存器依据
[NothingOSS SM8475](https://github.com/NothingOSS/android_kernel_msm-5.10_nothing_sm8475/tree/c402fcdb3a1523d10b15ef1123bced45456f7592)。
本目录为无 Linux 依赖的独立协议实现，许可证见 `LICENSE`。XNU 基础生产者保留 APSL 声明。

services186 已进入系统，用户确认点按、滑动及熄屏后亮屏触控恢复正常；双指手势尚未测试。主机验证与实机结果见
[本轮记录](../../migration/ipados18/services186-st61y-touch.md)，不能用主机测试代替触控验收。
