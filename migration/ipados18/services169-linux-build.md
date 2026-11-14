# services169 — Linux 本地构建与匹配系统盘

2026-11-13：Arch Linux 上复用 UniXNU-sirius 的隔离工具链，完整编译
`xnu-11417.140.69`、SM8475 板级 archive 和 DarwinBoot24，并在本地完成
22H374 Apple kext 预链接、HID 匹配、8 CPU ADT 与 Android boot v4/PONGFAT1 打包。
本地编译成功与设备运行结果分别记录。

## 输入与入口

需要 UniXNU Linux 的 patched clang、cctools、MIG/IIG、xcrun，以及 LLVM
CLANGPDB、dtc、mtools。使用已有 `~/.unixnu/run.sh` 隔离环境，无须改宿主 `/usr/bin`。
Apple 源码按 `feeds.toml` 固定：XNU `43a90889846e00bfb5cf1d255cdc0a701a1e05a4`，
libplatform-349.140.6、libdispatch-1521.140.2、AvailabilityVersions-143.7，
IOStorageFamily-323。公开 Wi-Fi 源码按 `configs/diting-native-sources.toml` 固定。

先在 ignored `local.toml [diting_build]` 配置源码和真实固件路径，再生成仓库外的
runtime recipe overlay；它使用 `kits/DitingPlatform24/build-support/merged-scripts`，
其中共享 RPMh/SCM/SMMU 实现是完整 native 驱动链接的必要部分。

```sh
python3 kits/DitingPlatform24/build-with-runtime.py --overlay-only \
  --overlay-dir "$DITING_WORK/build/services169/runtime-source"

# 在 overlay 中使用完整板级配置；DITING_VOLUME_LAYOUT 必须来自待安装的 userdata。
~/.unixnu/run.sh python3 "$DITING_WORK/build/services169/runtime-source/scripts/build-ipados18-diting-macos.py" \
  --linux-support /path/to/UniXNU/scripts/linux \
  --tree "$DITING_WORK/build/services169/tree" \
  --artifact-dir "$DITING_WORK/build/services169/kernel" \
  --volume-layout "$DITING_VOLUME_LAYOUT" \
  --hardware-dtb "$DITING_WORK/build/services169/hardware.dtb" \
  --config-path /path/to/UniXNU-diting/configs/diting_ipados18_defconfig \
  --jobs 12 --skip-image-verification
```

`DITING_KERNELCACHE` 指向本地解压的原始 22H374 kernelcache。
`DITING_LIVE_DTB` 指向经过仓库 WLAN fixup 的本机 live DTB，
`DITING_ADRENO_FIRMWARE` 指向真实 A730 固件目录。
已有 Wi-Fi 文件没有 active-slot REC capture 记录，本轮显式设置
`DITING_WIFI_FIRMWARE_SOURCE=local`，来源记为 `external local directory`，
`active_slot_verified=false`。默认 recovery 模式仍要求原有真实抓取记录；
没有伪造 REC 元数据。固件的必要 payload 和 board data 仍必须存在。

DarwinBoot24 的 MMU-off trampoline 起止符号必须在同一段；已修正原先
CLANGPDB 把空 end section 丢弃导致的链接失败。使用独立产物目录：

```sh
python3 scripts/build-darwinboot24-macos.py \
  --edk2 /path/to/Mu_Basecore --package /path/to/UniXNU/package --llvm /usr/bin \
  --output-dir "$DITING_WORK/build/services169/darwinboot" \
  --artifact-dir "$DITING_WORK/build/services169/darwinboot/artifact"

python3 "$DITING_WORK/build/services169/runtime-source/scripts/build-ipados18-peripheral-boot.py" \
  --artifact-dir "$DITING_WORK/build/services169/kernel" \
  --original "$DITING_KERNELCACHE" --volume-layout "$DITING_VOLUME_LAYOUT" \
  --loader "$DITING_WORK/build/services169/darwinboot/artifact/DarwinBoot24.efi" \
  --output-dir "$DITING_WORK/build/services169/boot" --cpus 8
```

打包时 `DITING_BOOT_TEMPLATE` 使用已有 package run `43353149433` 的 UEFI 容器。
仅复用该容器中的固件，本轮内核与 DarwinBoot24 均为本地编译。
生成的命令行 896 ASCII 字节，保留 Wi-Fi/GPU/音频参数；超过 1023 字节会明确拒绝，
不会截断。AppleARMCPU 等平台匹配项由 library-only prelink 去除，原生板级驱动保留设备所有权。
本轮未携带 system-volume auth blob，使用仓库已有开发根卷策略。

## 配套 userdata

经用户允许，只有需要 macOS 的准备用户态步骤交给
[Actions run 43358740511](https://github.com/dddqmmx/UniXNU-diting/actions/runs/43358740511)，
对应提交 `44e63bf`。修正 `hdiutil -size` 原先把 MiB 当 KiB 的错误，Data 为带 GPT 的
8192 MiB APFS 磁盘，SystemOS 为 5120 MiB。原错误 run `43357148095` 的镜像不能使用。

本轮三个卷为：

| 卷 | UUID | userdata 内偏移 | 长度 |
| --- | --- | ---: | ---: |
| OS | B07DF049-7AD9-47B5-9F8F-9EE728056398 | 1048576 | 6845104128 |
| SystemOS | 3B023D74-AC86-4F32-9907-4FD9662F4720 | 6846152704 | 5368709120 |
| Data | E5FD1797-50E9-4DA7-87CE-A45A4029DEC8 | 12231639040 | 8589934592 |

整体镜像 20821573632 字节。下载四分片后在 Linux 本地拼接，使用宿主 android-tools 的
`img2simg <raw> <sparse> 4096` 转 Android sparse；Fastboot 用 `-S 512M` 分块发送。
设备 product=diting、unlocked=yes、活动槽 b，userdata 容量 240092426240 字节；
写入和启动回执保存在工作目录。
本轮跳过镜像哈希、镜像审计与设备镜像回读。TFA amp container 输入缺失，
Wi-Fi 关联、PCM、Metal 和系统 UI 均不能从本地构建成功推定。

## 本轮设备结果

- userdata sparse 输出 11330757180 字节，23 个发送/写入块全部 OKAY，276.246 秒。
- 第一次 raw boot_b 下载 OKAY 后，设备没有返回 flash 状态；USB 连接重置后主机报
  `Status read failed (No such device)`。该次不能记为成功。
- 用户将手机重新进入 Fastboot 后，把同一 boot image 用 `img2simg` 转 sparse，
  `fastboot -S 32M flash boot_b` 两块均发送与写入 OKAY，共 4.880 秒。
- `fastboot reboot` OKAY，2026-11-13 04:11:49 UTC 记录启动请求。
  手机保留在当前运行状态，没有恢复旧 boot 或尝试启动 Android。
- 用户报告停在“内核文字或黑屏”；未提供具体屏幕行。不能据此确认 APFS、launchd 或 UI。
  主机在启动后的 120 秒捕获窗口内未发现 native USB CDC；
  没有 USB 输出不能单独判定具体失败阶段。

本地回执在 `$DITING_WORK/logs/diting/services169/`：
`install-receipt.json` 保存 userdata 和首次 raw boot 尝试，
`install-receipt-boot-retry.json` 保存成功的 sparse boot_b 写入及启动，
`fastboot-install.log` 保存完整命令响应。
