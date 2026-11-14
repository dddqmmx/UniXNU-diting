# services186：ST61Y 触屏协议与共享时钟投票

日期：2026-11-14（Asia/Shanghai）。Redmi K50 Ultra / diting / SM8475，
iPadOS 18.7.10 / 22H374 / Darwin 24.6.0，保持八核。

**services185 已进入系统，用户确认触屏无响应，但画面持续更新、实体按键有效。**
services186 已通过主机协议测试、完整原生内核链接、日志关闭配置编译与八核预链接/打包。
本轮尚未部署，触控尚未实机验收，持久 boot_b 与运行系统仍为 services185。

## 已确认的硬件

services178、services184 的本机 Recovery 日志一致报告 `TP is st 61y`、
驱动版本5.2.4.1、固件0007、协议06和sensor尺寸2711×1219；其扫描初始化完成。
触控固件更新缺 `.ftb`，并没有证据要求更新既有固件；本轮不刷触控固件、不写校准或工厂数据。

旧 XNU 生产者使用 Goodix Berlin-D 操作码，不能与实机 ST61Y 通信。旧 kmsg 中的
全零“Goodix 固件版本”不是控制器身份依据。live DT 同时列出 `st,spi` 和
`goodix,gt9916S`，GPIO119 原始高为 ST、低为 Goodix；新驱动按这一个明确 strap 选择协议。

| 资源 | 本机证据与候选配置 |
| --- | --- |
| SPI | QUP0 SE4 `0x990000/0x4000`，mode0，DT 上限12MHz；GENI 初始化4MHz |
| GPIO | MISO16、MOSI17、CLK18、CS19，cape mux1；IRQ21、reset20 |
| 供电 | AVDD71高使能；vdd→pm8350c_l2/ldoc2，provider init-voltage1.8V、NORMAL/VRM mode7 |
| 选择器 | GPIO119，读取原始电平，不把 active-low flag 当成身份反转 |
| 输入轴 | live DT 12200×27120，super-resolution=1；不以 sensor 尺寸覆盖或猜测旋转 |

## 同批修正

- 新 `DitingSt61yProtocol.h` 编码大端地址、SPI dummy、FIFO `87`、chip ID `FB 20000000`
  和208字节System Info。复位后要求真实 READY、`36 40` 身份、有效 firmware/protocol 和
  FIFO flush `A4 00 01` 的 echo `43 01 A4 00 01`。
- scan `A0 00 01` 按匹配厂商 `setScanMode()` 检查总线写入；没有添加并不存在的 echo gate。
  READY100ms、echo最坏10s，最多三次初始化。扫描命令提交与可用触摸分开记录。
- 以事件高 nibble 区分 enter/motion/leave，兼容 shape 低位；super-resolution 的 x/y
  为 LE16。按输入轴归一化，保留多指持久状态；空 FIFO 不释放其他手指。
- READY/error 立即释放触点、停止处理旧批次并重新初始化；SPI 丢失 FIFO 批次也释放触点，
  避免丢失 leave 后卡住。三次连续总线失败触发恢复，最多三次恢复而没有真实 DOWN/MOVE。
- 新 `DitingGeniSpi.h` 使用厂商 FIFO FULL_DUPLEX **opcode3**。opcode7 是另一操作；
  早期将3判为错误的解释已撤回。实际修正的是 CS_TOGGLE 在bit0与最后时钟后的10µs CS保持。
- 主引擎busy bit0、副引擎busy bit12；不接管继承的活动事务。完整 RX、真实 DONE 和 idle
  才返回成功；超时 cancel/abort，未完成保留 owner，迟到真实完成后才回收。
- GPIO与GENI模式/packing/clock配置加入回读。GENI初始化不可用时，只在已知固定引脚上使用
  GPIO SPI；不会在失败通信后切换或交换引脚试探。
- `DitingPlatformResources` 统一 touch/WLAN/USB/audio 的 GCC62008 RMW/readback，避免
  并发丢失投票。AHB的HWCG bit1可解释 `0x80000002`；core/core2x/SE没有这一豁免。
- 触屏移除私有TCS与错误的Nothing电源，使用已有共享RPMh owner。保留services185的
  BDF功放选择、AOP/QMP交易、全部八核与实测RAM，正常系统启动路径保持。
- 两套构建入口均加入touch头文件和对象flags stamp；POWER/BACKLIGHT关闭时构建并链接
  resources-only archive，ramoops关闭不留下未定义诊断符号。
- DTS加入真实ldoc2 provider与GPIO119，IRQ flags与live DT一致。ADT同步完整触屏资源、
  精确契约和Redmi物理身份。旧services182 hardware DTB缺新provider，不作为本轮ADT输入。

内核生产者保持 `bsd/dev/diting_goodix.c` / `diting_goodix_init` 文件与符号名，
`/dev/diting-touch` ABI和既有用户态HID bridge兼容。本轮没有更新userdata。
64条有界 `DITING TOUCH:` checkpoint包含选择器、供电、时钟、初始化、首批输入和统计。

## 主机验证

- `python3 -m unittest discover -s tests -p 'test_diting_touch.py' -v`：7项通过。
  C测试启用ASan/UBSan，覆盖12种初始化结果、真实wire格式、System Info坏数据、
  多指/shape/边界/首次MOVE/leave/空FIFO/复位/错误、丢失批次，GENI短RX/错误/FIFO边界/
  继承owner/超时cancel与abort/迟到完成/停止的时间源/10µs保持，GCC四客户端并发保票。
- 原始live DTB与新编译hardware DTB均通过触屏资源契约，ADT属性逐项匹配。
- services185源码快照→新增增量patch→当前源码比对通过，当前缓存也通过reverse check。
- 两套shell入口语法检查与`precheck-workflow.py`通过（0 problems）。
- `diting-power --resources-only`与触屏对象在ramoops关闭时实际编译通过，无未定义诊断引用。
  这是两个编译模式检查，不是所有配置组合的整机实测。
- 最终完整原生内核链接通过：`build-20261114T011148Z.log`。实际触屏编译flags含
  `XNU_DITING_TOUCH_BUILTIN=1`、ramoops配置、两个驱动include目录和`-mno-implicit-float`。
- 八核预链接/打包通过，候选101,183,488字节，loader33,792字节；设备结果待验证。本轮继续`--skip-image-verification`，不做镜像哈希、
  审计或设备镜像回读。

## 产物、源码与下一步

- Git源码基线：`e0900869cc4a5a5916fd34469bedcb5133a8b10d`。
- 增量patch：`patches/xnu-ipados18/zzzzzzzzzzzzzzzzzz-diting-st61y-touch.patch`，追加series。
  没有把后续patch混回0001聚合补丁。
- 源码快照：`diting-work/tmp/services186/source-before/`；内核缓存
  `diting-work/build/services169/tree/.cache/xnu-work`当前对应services186，目录名不是其版本。
- overlay、hardware DTB、kernel、boot分别在`diting-work/build/services186/`；
  构建、协议和编译模式证据在`diting-work/logs/diting/services186/`。
- 复用已部署的services182 FD/template与33,792字节DarwinBoot24；services185镜像保留作回退。

候选完成后在Fastboot装入services186，分别验收进入系统、实际点按/滑动/抬起、多指与
实体按键。若失败，读取本轮 `DITING TOUCH:` 日志定位阶段；不重复拿旧Goodix日志冒充本轮。

协议依据 [LineageOS fts_spi固定版本](https://github.com/LineageOS/android_kernel_xiaomi_sm8450/tree/eff4b40407a1b6f1b1ab84554cbb540bcb495175/drivers/input/touchscreen/fts_spi)
与 [NothingOSS SM8475固定版本](https://github.com/NothingOSS/android_kernel_msm-5.10_nothing_sm8475/tree/c402fcdb3a1523d10b15ef1123bced45456f7592)
的SPI/GCC/pinctrl。官方组件、patch、包装与UEFI契约已按前轮交接阅读。原始日志、
firmware/restore及所有产物留在本地；只提交源码、测试、配置和记录。
