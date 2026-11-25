# services214–216：键盘弹出与输入卡顿根因诊断

> 2026-11-18最新状态：用户已停止键盘修复，GPU优化和Wi-Fi开发继续暂停。最后匹配临时226；右侧全宽与左下globe截图改善，输入延迟仍FAIL_INPUT_LATENCY。当前无构建/采集，停止复核时CDC/Fastboot均未枚举，当前设备模式无法确认。恢复修复前不执行下方历史建议中的候选或测试。
> 最新交接：[交接-2026-11-18-services226-键盘布局改善-延迟未解决.md](交接-2026-11-18-services226-键盘布局改善-延迟未解决.md)；证据：[226停止摘要](services226-keyboard-stop-report.json)。


更新时间：2026-11-18，Asia/Shanghai。  
目标设备：Redmi K50 Ultra（`diting` / SM8475），运行 iPadOS 18.7.10（`22H374` / Darwin `24.6.0`），基于 [services213 GPU 优化暂停交接](交接-2026-11-17-services213-GPU优化暂停.md) 时的运行时配置（`diting_tu=2` 即 `gmem,nocb`、`fb_beat=0`、`fb_hz=120`、`diting_panel_hz=120`、`diting_cpu_policy=2`）。

> 第 1–7 节保留214–216的历史诊断。第8节是217/218早期主机阶段的记录；218之后已完成完整内核、打包、临时启动及实机采集。最新状态见第10节。用户补充的布局位置错误仍未解决，采集完成与运行库安装完成均不能替代输入和布局验收。

---

## 1. 核心结论

将 `services216` 捕获到的完整用户态调用栈、CPU 增量、HID 动作时间轴、GPU 提交与显示拷贝事件对齐后，可以看到“键盘弹出并输入时卡到几乎没法用”并非单一原因，而是**两类不同瓶颈分时段叠加**，外加**未定标的底层系统疑点**：

1. **导致打字时 240ms–2.89s 完全冻屏的直接元凶：前台宿主（实测 `Spotlight`）主线程 100% 占满单核**  
   在 `services216` 的两轮打字期（`+2.76s..+4.21s` 与 `+6.97s..+8.42s`），**GPU 几乎处于空闲状态**（每 `0.4s` 仅跑 `0.0ms..12.9ms`），但显示拷贝完成间隔却接连出现 `242.6ms..501.8ms` 的长断流（`services214` 最长 **`2,893.3ms`**）。原因在于 `Spotlight (pid 301)` 主线程在 `8.90s` 窗口内烧掉 **`7,149.9ms user + 1,079.9ms sys = 8,229.8ms` CPU**，从弹出后到结束全程处于 97%–100% 负载，一次都未回到事件循环空闲等待（`_mach_msg2_trap`）；相比之下，独立输入界面进程 `InputUI (pid 433)` 启动后在 `7.69s` 里仅使用了 **`1.5ms` user / `0.9ms` sys**，提交 **0 次 GPU 作业**。
2. **导致键盘/Spotlight 弹起动画期掉帧到 19–37Hz 的直接原因：`backboardd` 与 GPU 合成超 `8.33ms` 预算**  
   进入 `spotlight-show`（`+0.56s..+2.76s` 与 `+4.77s..+6.10s`）后，毛玻璃模糊叠加层把 `backboardd (pid 43)` 推到平均 ≈5.3 倍全屏像素过绘（`6,163` 个 GMEM pass、`5,439,046` kPixels），`cp_execution` 达到 `p95=13.28ms / max=15.46ms`（`104/622` 超 `8.33ms` 预算），同时 `tid=2017` 烧掉 `2.21s user + 0.39s sys`，且伴随重度主机/内核 cache 同步（`[gpu host sync]` 约 `105,453 MiB`、`[gpu cache]` 约 `97,249 MiB`）。`MiniIOMobileFramebuffer` 在弹出期实测仅 **37Hz**（`services214` 为 **19Hz**、`215` 为 **29Hz**），超过六成 swap 晚交。
3. **辅助拖累与两项待排除的系统级怀疑点**
   - `kbd (pid 127)` 同步目录扫描、同步网络请求与锁等待消耗了明显 CPU/系统调用；`InputUI`/`kbd` 拉起时触发约 `170 MiB` `DYLD shared region` unnest；`com.apple.keyboards/images` 缓存目录只有 `992–1056 B`。
   - **尚未实测闭环的怀疑点**：SM8475 CPU 实际运行频率从未在运行期通过 EPSS 回读验收（[`configs/diting-power-sm8475.json`](../../configs/diting-power-sm8475.json#L129) 标注 `"frequency_runtime_qualified": false`），而单次 `dyld` 符号解析（`trieWalk`）在 `Spotlight` 上竟然耗满连续 >800ms 纯 user CPU，高度怀疑存在“CPU 实际停留在低频/受热限”或“4K 页/未注入宿主放大 CPU 工作量”的情况；另外现有 `services214–216` 只抓了 Spotlight 弹键盘，尚未抓普通 App 输入框对照。

---

## 2. 诊断运行总览（2026-11-17）

`keyboard_diagnosis`（case 64）固定执行 2 轮流程：主页下拉触发 Spotlight 与键盘弹出（`spotlight-show`，约 `2.2s`），随后以约 `180ms` 间隔连续点击 8 个字母键（`letter-touch` 0..7）。

| 轮次 | 状态与栈权限 | GPU `cp_execution` (`pid 43`) | 帧间隔 / 最长冻屏 | 控制台 `MiniIOMobileFramebuffer`（诊断窗） | 备注 |
| --- | --- | --- | --- | --- | --- |
| `services214` | `PASS_DIAGNOSTIC`<br>`records=6550, missing=1`<br>`input_ui_stacks_read=false` | `n=403, p50=0.95ms`<br>`p95=11.96ms, max=12.35ms`<br>`over_budget=85` | 活跃窗 `max=210.53ms`<br>长断流最长达 **`2,893.33ms`** | **`102 frames / 5259ms (19/s)`**<br>GPU 等 `8,651us`，`83/102` 晚交（最长 `69ms`） | `Diting Metal` 报 `QuartzCore/default.metallib InvalidFile`、`V cache hit61 miss62` |
| `services215` | `FAIL_DIAGNOSTIC`<br>`records=8079, missing=0`<br>`input_ui_stacks_read=false` | 未生成 `decoded-trace.json` | — | **`150 frames / 5003ms (29/s)`**<br>GPU 等 `7,515us`，`110/150` 晚交（最长 `66ms`） | `state_error=4`；`backboardd.2057` CPU `+2.31s user / +0.40s sys` |
| `services216` | **`PASS_DIAGNOSTIC`**<br>`records=9464, missing=0`<br>`input_ui_stacks_read=true` | `n=622, p50=0.81ms`<br>`p95=13.28ms, max=15.46ms`<br>`over_budget=104` | 活跃窗 `p95=99.74ms, max=242.61ms`<br>长断流 **`499.02 / 501.82 / 486.36 / 462.21ms`** | **`186 frames / 5000ms (37/s)`**<br>GPU 等 `6,270us`，`123/186` 晚交（最长 `61ms`） | 首次完整读到 5 进程符号化调用栈（`23` 样本、`20` 动作） |
| `services217` | 主机单测 `FAIL`<br>未构建、未上机 | — | — | — | 原计划给 `InputUI`/`SpringBoard`/`Preferences` 的 `UIKeyboardLayoutStar`/`UIKeyboardImpl` 加 `DitingKeyboardTiming` 计时；卡在 `tests/diting_gpu_automation.cpp:46` `protocol` 断言 |
| `services218` | 主机检查 `PASS`（13 项）<br>**未构建内核、未上机** | — | — | — | 修复上述断言并扩展到 8 个键盘方法与 Spotlight 宿主；worker 无新产物（二进制仍是 216 构建），详见第 8 节 |

---

## 3. 根因一：打字阶段长冻屏——`Spotlight` 主线程 100% 占满单核

### 3.1 `services216` 动作 / CPU / GPU 对齐时间轴

以首个 CPU 采样点 `t0 = 235.881s`（系统单调时间）为 `+0.00s` 基准：

| 时段（相对 `t0`） | 测试动作与画面变化 | GPU 总耗时（每 `0.4s` 窗） | 关键进程主/热线程表现 |
| --- | --- | --- | --- |
| `+0.00s..+0.40s`（`[00]..[01]`） | 动作前桌面待机 | `1.9..4.0ms`（4–8 jobs，max `0.82ms`） | `Spotlight`/`kbd`/`backboardd`/`SpringBoard` 均在 `_mach_msg2_trap` 休眠 |
| `+0.56s..+2.76s`（`[02]..[06]`） | **Cycle 0 `spotlight-show`**：弹出 Spotlight + 键盘 | **剧增至 `47.2..213.7ms`**（`max=14.62ms`） | `Spotlight.3475` 跳到 `cpu=496→1000`；`backboardd.2017` `cpu=361→753`；`SpringBoard.275` `cpu=174→466`；`kbd.823` 峰值 `cpu=627`；`InputUI` 于 `[03]` 拉起后迅速回落 |
| `+2.76s..+4.21s`（`[07]..[11]`） | **Cycle 0 `letter-touch` 0..7**（其中 `idx=1..3` 连续 `540ms` 同 hash `f4a91020...`；`idx=4..5` 退回弹出前 hash `8db2b6ee...`） | **大幅回落到 `0.0..19.8ms`**（`[11]` 整窗 **`0.0ms`**） | **GPU 与 `backboardd`（`cpu=22..133`）已基本闲下来**，`InputUI` `cpu=0`，但 **`Spotlight.3475` 仍死锁在 `cpu=990..1000`**；显示出现 **`242.6ms`、`499.0ms`、`501.8ms`** 断流 |
| `+4.77s..+6.97s`（`[12]..[17]`） | **Cycle 1 `spotlight-show`** | `+4.85..6.07s` 再飙到 **`155.4..234.3ms`**（`max=15.46ms`），`+6.48..6.88s` 降至 `6.7..12.9ms` | `backboardd.2017` 先冲到 `cpu=488..751`，随后退闲；`Spotlight.3475` 继续全程 **`cpu=989..1000`** |
| `+6.97s..+8.42s`（`[18]..[22]`） | **Cycle 1 `letter-touch` 0..7**（`idx=2..7` 连续 **>1.08s** 全部同 hash `63b0d913fe44e79b`，按键气泡卡住） | **仅 `2.3..11.1ms`**（末尾 `[22]` 跳回 `83.5ms`） | `InputUI` 全程 `cpu=0..3`；`Spotlight.3475` 全程 **`cpu=970..1000`**；显示再现 **`359.6ms`、`242.2ms`、`374.5ms`、`486.4ms`、`238.0ms`、`462.2ms`** 断流 |

这个对齐关系直接排除了“打字卡死主要是 GPU 算不动”的判断：**真正该刷出按键回弹和输入字符的打字窗口里，GPU 大多时候压根没接到新帧**；真正卡住提交链条的是前台进程 `Spotlight` 的主线程 `tid=3475`。

### 3.2 `services216` 各进程 CPU 增量与 `Spotlight` 热点调用栈

采样起止跨度内，关键进程观测线程 CPU 增量如下：

| 进程 (`pid`) | 线程 (`tid`) | 采样跨度 | 累计 `dUser` | 累计 `dSys` | 说明 |
| --- | --- | --- | ---: | ---: | --- |
| `Spotlight (301)` | `3475`（主线程） | `8.90s`（23 样本） | **`7,149.9ms`** | **`1,079.9ms`** | 第 `[02]..[22]` 全部 21 帧保持 `cpu=496..1000`，合计烧掉 `8,229.8ms`（即 `8.90s` 中超过 92% 墙钟时间单核满载）；另有辅线程 `3479` 在 `4.85s` 内 `+206.8ms user / +60.6ms sys`（停在 `-[SPFederatedQueryTask handleOptionsForNewSections:] -> __dispatch_sync_f_slow -> ___ulock_wait`） |
| `backboardd (43)` | `2017`（合成/渲染线程） | `8.50s`（22 样本） | **`2,208.0ms`** | **`388.0ms`** | 主线程 `364` 仅 `+0.4ms`；`services215` 中对应线程 `2057` 达 **`+2,308.9ms user / +399.5ms sys`** |
| `SpringBoard (6)` | `275`（主线程） | `8.90s` | `900.1ms` | `164.0ms` | 另辅线程 `3082` 在 `6.07s` 内 `+153.9ms user / +11.9ms sys` |
| `kbd (127)` | `823`（主线程）+ 4 个工作线程 | `8.90s` | 主 `445.8ms`<br>辅合计约 `342ms` | 主 `398.6ms`<br>辅合计约 `524ms` | `878/5401/5451/2800` 等工作线程出现大量 `sys` 内核时间与锁等待 |
| `InputUI (433)` | `5410`（主线程） | `7.69s`（`[03]..[22]`） | **`1.5ms`** | **`0.9ms`** | 启动前已用 `50.4ms user / 49.0ms sys`，之后全程在 `_mach_msg2_trap` 等待；`services215` 中主线程 `6793` 也仅 `+1.3ms user / +0.7ms sys` |

`Spotlight.3475` 被采样抓到的主线程热点清楚指向四类同步重活：
1. **`dyld4` 符号与导出树遍历（连续两个采样窗卡 >800ms 纯 user CPU）**：`[16]`（`u=5286.2ms`）与 `[17]`（`u=5694.5ms`，单段 `404ms` 墙钟里跑了 `+408.3ms user`）栈顶完全一致，均为：  
   `MachOAnalyzer::withVMLayout -> Loader::forEachBindTarget -> Fixups::forEachBindTarget_ChainedFixups -> Loader::resolveSymbol -> Loader::hasExportedSymbol -> dyld3::MachOFile::trieWalk`  
   表明键盘/Spotlight 显示后期仍在主线程上同步触发动态库/插件的符号绑定解析。
2. **CPU 软件卷积模糊按键阴影**：`[18]` 停在 `-[UIKBRenderer renderShadowEffect:withTraits:] -> _CGContextDrawPath -> _RIPLayerGaussianBlur -> _vImageConvolve_Planar8 -> _conv4_8_ABCD`。也就是说键盘阴影模糊效果仍在由宿主进程主线程在 CPU 上逐像素做 8-bit 卷积。
3. **重复的区域/语言回退与听写配置查找**：`[06]`、`[19]`、`[20]` 落入 `AssistantServices::__AFDictationLanguageForKeyboardLanguageAndConfiguration -> __CFBundleCopyPreferredLanguagesInList -> libicucore::_ualoc_localizationsToUse / _ualoc_getAppleParent / _ztrans_getDynamicClassID` 与 `[03]` 的 `_res_getTableItemByKey`。
4. **文件系统图片探测、Asset 目录查找与配置搜索列表加锁查询**：`[10]`、`[21]` 停在 `UIKitCore` `____UIImageCollectImagePathsForPath_block_invoke -> _faccessat`；`[22]` 停在 `-[_UIAssetManager imageNamed:...] -> -[CUICatalog namedLookupWithName:...]`；`[12]` 停在 `-[CFPrefsSearchListSource alreadylocked_copyValueForKey:]`。

---

## 4. 根因二：弹起动画期帧率骤降——`backboardd` + GPU 合成过载

当 `Spotlight` 和键盘毛玻璃背景层一同滑出时，即使主线程暂时提交得出新帧，合成与 GPU 管线也无法守住 120Hz 所需的 `8.33ms` 预算：

1. **硬件提交与前端延迟数据（`services216`）**
   - GPU `cp_execution`（`pid 43`，622 次）：`p50=0.81ms`，**`p95=13.28ms`、`p99=15.10ms`、`max=15.46ms`**，**`104` 次超过 `8.33ms`**；从提交开始到 CPU 退休（`submission_begin_to_cpu_retire`）则有 **`143` 次超时**（`p95=13.81ms / max=15.77ms`）。
   - Metal 前端：`encode_age` 达 **`p95=21.12ms / max=54.78ms`**（`113/622` 超时），`completed_command_latency` 达 **`p95=16.04ms / max=36.04ms`**（`161` 次回调超预算），`poll` 阶段 `p95=13.50ms / max=31.19ms`；另在 `pid 43` 记录到 `25` 次 >1ms 的 `pipeline` 编译停顿（`p50=2.54ms / p95=4.35ms / max=34.98ms`）。
   - 帧缓冲交换统计：在 `spotlight-show` 期间 GPU 等待从待机的 `356us` 升到 `6,270us`（`214` 甚至达到 `8,651us`），实际 `MiniIOMobileFramebuffer` 只完成 **37Hz**（`214`: 19Hz；`215`: 29Hz），186 次 swap 中 123 次晚于目标 VSync（最长延迟 61ms）。

2. **代码与计数器定位到的三类 GPU/前端放大因素**
   - **高像素过绘 + `gmem,nocb` 禁用并发分桶与回退**：`services216` 诊断窗内 `render_modes` 记录 `pid 43` 共执行 **`6,163` 个 `gmem_passes`**，累计 **`5,439,046` kPixels**（约 54.4 亿像素；按 311 次拷贝帧平均，相当于每帧 ~1,750 万像素，在 `1220×2712` ≈ 331 万像素面板上约为 **5.3 倍全屏过绘**），其中 **`1,544` 个为 `unpadded_gmem_passes`**，`autotuned_sysmem_passes = 0`。由于启动参数设为 `diting_tu=2`，[DitingMetalPlugin.mm](../../drivers/DitingMetal/DitingMetalPlugin.mm#L163-L168) 会固定设置 `TU_DEBUG=gmem,nocb`，一方面全部 pass 强制走 GMEM 不允许 Turnip 按 pass 选择 `sysmem`，另一方面 **`nocb` 关闭了并发 binning**，使多层模糊与圆角透明合成全部串行化。
   - **频繁的大体量物理页 Cache 维护**：在 [NativeBridgeMetalFrontend.mm](../../drivers/DitingMetal/NativeBridgeMetalFrontend.mm#L526-L617) 的 `syncActiveImports` / `flushSurfaces`（于 `commit` 与 `completed` 双阶段调用，见同文件 [第 1668–1718 行](../../drivers/DitingMetal/NativeBridgeMetalFrontend.mm#L1668-L1718)），以及核态 [NativeAdrenoUserClient::syncHostBatch](../../drivers/DitingAdreno/submit/NativeAdrenoUserClient.cpp#L451-L490)、[NativeAdrenoVm::publishMapped](../../drivers/DitingAdreno/submit/NativeAdrenoVm.cpp#L404-L417) 与 [NativeAdrenoMemory::publishForDevice](../../drivers/DitingAdreno/submit/NativeAdrenoMemory.cpp#L450-L476) 中，每次命令提交和完成都会维护相关导入/映射缓冲的物理 cache 一致性。`services216` 运行短短十余秒即在控制台累计出：
     - `[gpu host sync] calls=8093 MiB=105453 total_us=1408736`（约 **103 GiB**、平均每次 submit 双相合计约 **26 MiB**；`services215` 为 `16,388 calls / 213,003 MiB / 2.66s`）；
     - `[gpu cache] submits=4096 ... maintain_us=2331080 MiB=97249`（`services215` 为 `8,192 submits / 185,326 MiB / 3.99s`）；
     - 此外 `services214` 中 `J surface cache` 还记录了 `17,219,469,312` 字节的用户态 `sys_dcache_flush`（`n=1802`），同时存在 `QuartzCore.framework/default.metallib` 的 `BinaryArchive(InvalidFile ...)` 失败（[NativeBridgeMetalFrontend.mm](../../drivers/DitingMetal/NativeBridgeMetalFrontend.mm#L2273-L2274)）及 `V cache hit61 miss62 wr0`。
     虽然单次 `frontend.host_sync` 仅占 `p50 ≈ 0.56ms / p95 ≈ 2.0ms`，但频繁 submit 叠加了总线竞争、核态耗时与前端 `backboardd.2017` CPU 占比。
   - **DDR/LLCC 真实持续带宽尚未测清**：[`services203–205`](services203-display-performance.md#L31) 已确认虽然 `MC3/SH5` 请求停在 `DDR 3,196,000 kHz / LLCC 1,066,000 kHz`，但 DDR BWMON 计数器借用失败，真实 DDR 频率与重模糊期间带宽压力仍属未定标项。

---

## 5. 辅助进程拖累与系统环境线索

1. **`kbd (pid 127)` 的高系统调用与同步等待**  
   在 `services216` 拉起键盘的窗口里，`kbd` 不仅主线程耗时 `445.8ms user + 398.6ms sys`，多个工作线程还在同时执行：
   - `LanguageModeling` 扫描目录构造 bundle（`_CFBundleCreateBundlesFromDirectory -> _open$NOCANCEL`）；
   - `CFNetwork` 同步联网（`+[NSURLConnection sendSynchronousRequest:returningResponse:error:]`）与 `ProtectedCloudStorage/CloudKit`；
   - 大量派发同步等待（`__dispatch_sync_f_slow -> ___ulock_wait`）。
2. **`InputUI` 与 `kbd` 的 `DYLD shared region` unnest 与按键图像缓存状况**
   - 在 `services214`、`215`、`216` 每次键盘唤起拉起 `InputUI` 和 `kbd` 时，内核控制台都记录对约 `170 MiB` 共享区段的 unnest（如 `services216`：`InputUI[433]` 与 `kbd[127]` 均触发 `0x1e7600000->0x1e7800000` 和 `0x1e7800000->0x1f2200000`）。
   - [`ui_compat18.c`](../../userland/ui_compat18.c#L1990-L2027) 中 `keyboard_retire_old_images` 会按 `.diting-keyboard-phone152` 标记清理一次旧键盘图片缓存，并仅在 `InputUI` 内安装 `UIDevice userInterfaceIdiom -> 0` 的手机布局钩子；而 `services214` 与 `services216` 的 `report.json` 记录 `/private/var/mobile/Library/Caches/com.apple.keyboards/images` 目录大小仅 `992–1056 B`（基本未形成预渲染键帽图片缓存）。
   - 另外 [`init.c`](../../userland/init.c) 中 `kern.exec_inject` 注入 `libditing-ui18.dylib` 的目标列表包含 `SpringBoard`、`backboardd`、`InputUI`、`Preferences` 等进程，**不包含 `Spotlight` 与 `kbd`**。

---

## 6. 尚未实测排除的关键怀疑点（推断与缺失证据边界）

1. **【怀疑点一（高优先级）】SM8475 CPU 实际频率未运行期确认，可能未跑在名义高频**
   - **依据与缺口**：按照 [`DitingCpuPolicy.h`](../../drivers/ArmCpu/DitingCpuPolicy.h#L94-L111) 与启动参数 `diting_cpu_policy=2`，内核线程会在延迟 `3s` 后向三个 EPSS 域写入最高非 boost LUT 请求（`A510: 1.8048 GHz / A710: 2.496 GHz / X2: 2.9952 GHz`）。然而 [`configs/diting-power-sm8475.json`](../../configs/diting-power-sm8475.json#L129) 明确标记 **`"frequency_runtime_qualified": false`**，[`services75-performance.md`](services75-performance.md#L31) 也指出真实频率受固件 EPSS/LMh 限频控制，从未在运行期回读 `DOMAIN_STATE` 实测确认；且 `services214–216` 的 `console.raw.log` 在 USB 建连之后才开始截取，**完全缺失启动第 3 秒时的 `Diting CPU:` 日志**（无法区分是正常写入还是触发了 `PDMEM unavailable` / `no valid native LUT` / 热限频）。
   - **为何必须优先怀疑**：在 2.5–3.0GHz 的核心上，`dyld3::MachOFile::trieWalk`、`libicucore` 查表与 `CUICatalog` 正常不应轻易卡死超过 `800ms–数秒`。若某一簇或全核实际还停留在固件初始低频，再叠加 Darwin24 强制 4K 页（[`0001-diting-darwin24.patch`](../../patches/xnu-ipados18/0001-diting-darwin24.patch#L12524) 中的 `FOURK_PAGE_SHIFT`），所有主线程工作量都会被成倍拉长。
2. **【怀疑点二】测试场景局限在 `Spotlight`，普通 App 文本框尚未验证**
   - `services214–216` 的动作脚本全都固定从桌面下拉触发 `Spotlight`，而 Spotlight 本身会叠加搜索索引、`QueryParser`、`AssistantServices` 听写语种查询、应用图标路径扫描等重负载。
   - **非 Spotlight 的纯文本输入场景**（例如“设置”内部搜索栏或单输入框页面）是否同样卡顿、`InputUI` 在那类场景下是否接管更多渲染，**目前没有实机 trace 数据**。

---

## 7. 后续建议验证顺序（只读/最小化排查）

> 2026-11-17 更新：只读 CPU 频率快照已实现（第 8 节），因此第 2 条的读取入口已有代码，但仍需在设备上确认；`services218` 也把 8 个键盘方法计时写进源码，默认不启用。

1. **先分流“Spotlight 问题”与“通用键盘问题”**：在普通文本框（如 `Preferences` / 备忘录）复测一次与 `services216` 同等权重的 CPU 栈 + GPU/显示采样，确认卡死主线程的到底是 `Spotlight` 特有逻辑，还是所有宿主都会在 `UIKBRenderer` / `InputUI` / `kbd` 处阻塞。
2. **开机后回读 EPSS 三域当前状态与实际频率（`0x17d91000` 窗口）**：直接确认 `DitingCpuPolicy` 在 `diting` 真机上到底写进去了什么、X2 / A710 / A510 当前是否真运行在满非 boost 档位。如果频率没上去，先修频率/调度再看上层耗时才有意义。
3. **如若普通场景仍明确在弹起期卡顿，再分别对症拆分**：
   - 弹起模糊掉帧（19–37fps）重点对照 `diting_tu=1 (nocb)` vs `diting_tu=2 (gmem,nocb)`、并发 binning 以及 `[gpu host sync]`/`[gpu cache]` 与模糊层合成成本（待用户允许恢复 GPU 试验后开展）；
   - 输入按键粘滞/冻屏重点启用 `diting_keyboard_trace=1` 的 8 个方法计时（`keyboard_latency18.h`，218 已实现但未上机），连同 `kbd` 阻塞情况一起定位具体卡在哪一段。

---

## 8. services217–218：早期主机阶段（2026-11-17，后续见第10节）

| 轮次 | 结果 | 产物与边界 |
| --- | --- | --- |
| `services217` | 主机检查 **FAIL** | 在 `tests/diting_gpu_automation.cpp:46` 的 `protocol` 断言失败（新增 `keyboard` profile 解析前测试未同步），`keyboard-broker-host-test` 被 SIGABRT；UI 库虽编译通过（install `d1784afa-9915-495d-8588-5c2de3d214e9`），worker 构建失败（`gpu-test18.mm` 编译 7 个错误），无内核、无部署 |
| `services218` | 主机检查 **PASS**（13 项） | broker 8 项（protocol/lifecycle/quiescence/overflow/replacement/broker/oracle/cadence）、`test_gpu_automation.py` 30 项、`test_diting_keyboard.py` 1 项、两处 `bash -n`；UI 库编译链接 PASS（install `81eca07e-c55d-424d-b536-47b8e773a959`，178,048 字节） |

### 键盘宿主判定与注入别名

- `diting_keyboard_host=1` 才安装 XNU 侧别名规则：只有当注入库恰为 `/diting/libditing-ui18.dylib`、配置词恰为 `InputUI`、目标进程名恰为 `Spotlight` 时才匹配。既有 `kern.exec_inject` 规则与其它进程不变（`patches/xnu-ipados18/zzzzzzzzzzzzzzzzzzzzzzzzz-diting-keyboard-host.patch`）。
- 宿主判定改为 `InputUI` / `Spotlight` / `SpringBoard` / `Preferences` / `MobilePhone`（`userland/DitingKeyboardHost.h`）；phone idiom 钩子对所有文本宿主安装，`InputUI` 与 `Spotlight` 不再进入 SpringBoard 专属钩子。
- `diting_keyboard_trace=1` 才安装 8 个方法计时（`UIKeyboardLayoutStar` 的 touchDown/touchUp/showKeyboard，`UIKeyboardImpl` 的 handleKeyboardInput/addInputString/playInputClick/setKeyboardInputMode，`UIKBRenderer` 的 renderShadowEffect），参数与返回值原样转发，不记录文本内容。**默认 boot args 未启用这两个开关，计时器从未在任何设备运行过。**

### CPU 频率只读快照（尚未上机）

- `drivers/ArmCpu/DitingCpuFrequency.h` 定义 240 字节有序快照：每域含 native LUT 上限、每核 `PERF_STATE` 请求、`DOMAIN_STATE`、`limit_status`、`PDMEM` 索引；`DitingCpuPolicy` 在延迟启动、固定请求之后与恢复循环内发布。
- 请求与 `DOMAIN_STATE` **不是实测频率**；`configs/diting-power-sm8475.json` 的 `frequency_runtime_qualified` 仍为 `false`。本轮没有任何设备回读。

### 218 的证据边界（重要）

- `runtime-build.json` 为 **FAIL**（`Linux needs the UniXNU toolchain, --sdk and a real --codesign tool`）；`work-state.json` 的 `full_kernel_build` / `link_check` / `boot_package` 均为 `NOT_STARTED`，`kernel/`、`kernel-package/`、`boot/` 目录为空，`deployed` / `temporary_boot` 为 `false`。
- `gpu-stage/` 只保留继承自 216 的运行时条目并重新生成 install id（`1b7d0e37-…`）与 `gpu-runtime.pack`；其中 `gpu-capture18` 与 216 的产物**逐字节相同**，`gpu-stage/automation/automation-build.json` 仍是 216 的构建记录（`result_bytes=131072`、binary 指向 216 目录）。
- 218 私有候选的 `patches/xnu-ipados18/series` 另含未提交主仓库的 `zzzzzzzzzzzzzzzzzzzzzzzzz-diting-wifi-runtime-install.patch`；因此仅凭主仓库无法重建与 218 私有候选完全一致的内核。

以上结论仅适用于17日的早期时点；218后续构建、安装与实机测试已完成，详见第10节。217仍未部署。

## 9. 证据与源码索引

- 实机日志与 Trace 证据目录（仓外工作区 `diting-work/`）：
  - `diting-work/logs/diting/services216/keyboard-platform-read-20261117-01/keyboard-diagnosis-01/{report.json,symbolicated-stacks.json,decoded-trace.json,console.raw.log,stack-summary.log,frame-0-spotlight-show.png,frame-0-typing-end.png,frame-1-spotlight-show.png,frame-1-typing-end.png}`
  - `diting-work/logs/diting/services215/keyboard-read-20261117-01/keyboard-diagnosis-01/{report.json,symbolicated-stacks.json,console.raw.log}`
  - `diting-work/logs/diting/services214/keyboard-latency-20261117-01/keyboard-diagnosis-01/{report.json,decoded-trace.json,console.raw.log}`
  - `diting-work/logs/diting/services217/keyboard-costs-20261117-01/`（主机单测失败日志与阶段状态）
  - `diting-work/logs/diting/services218/keyboard-host-20261117-01/`（13 项主机检查、runtime-build FAIL、UI 库构建记录）
- 已收入本仓库的小体积证据原文（完整 trace/console/像素帧仍只在本机）：
  - [services214 诊断报告](services214-keyboard-diagnosis-report.json)、[services215 诊断报告](services215-keyboard-diagnosis-report.json)、[services216 诊断报告](services216-keyboard-diagnosis-report.json)、[services216 符号化栈摘要](services216-keyboard-stack-summary.log)、[services218 主机检查](services218-keyboard-host-host-checks.json)
- 仓库内关键源码与既有文档：
  - [交接-2026-11-17-services213-GPU优化暂停.md](交接-2026-11-17-services213-GPU优化暂停.md)
  - [services203-display-performance.md](services203-display-performance.md)
  - [services75-performance.md](services75-performance.md)
  - [NativeBridgeMetalFrontend.mm](../../drivers/DitingMetal/NativeBridgeMetalFrontend.mm)
  - [DitingMetalPlugin.mm](../../drivers/DitingMetal/DitingMetalPlugin.mm)
  - [NativeAdrenoUserClient.cpp](../../drivers/DitingAdreno/submit/NativeAdrenoUserClient.cpp)
  - [NativeAdrenoVm.cpp](../../drivers/DitingAdreno/submit/NativeAdrenoVm.cpp)
  - [NativeAdrenoMemory.cpp](../../drivers/DitingAdreno/submit/NativeAdrenoMemory.cpp)
  - [DitingCpuPolicy.h](../../drivers/ArmCpu/DitingCpuPolicy.h)
  - [ui_compat18.c](../../userland/ui_compat18.c)
  - [init.c](../../userland/init.c)


## 10. services218–226修复过程（2026-11-18，现已按用户要求停止）

GPU优化保持暂停。键盘任务沿既有GPU渲染实现继续，保留`gmem,nocb`、八核、触控、电池、ADSP、Wi-Fi和Data/SystemOS；只临时启动候选，没有写入boot_b或userdata。

| 轮次 | 最终阶段/实机结果 | 已确认的边界 |
| --- | --- | --- |
| 218 | 完整内核、链接、打包、匹配临时启动PASS；诊断采集完成 | 用户反馈布局位置错误；延迟和文字送达未通过。早期worker的头文件编译错误及容量断言已修复，不能再将218归为未部署 |
| 219 | 完整构建与匹配实机PASS；采集20546条trace、43个样本、30个动作，无missing/skipped/drops | 两轮Spotlight及一轮Preferences搜索；主线程仍持续繁忙。新增真实22H374手机布局工厂文件选择，整体边界正确仍不能证明键帽位置正确 |
| 220 | 完整构建与匹配实机PASS；采集16545条trace、43个样本、30个动作，无missing/skipped/drops | 结果906767字节；压缩截图显示右侧p/删除/按钮、左下globe裁切，按键气泡停留。加入phone screen traits及256×576有界BGRA-zlib帧，结果容量1MiB、chunk1024、报告ABI1064字节 |
| 221 | 构建/安装完成，实际测试FAIL | 私有`frameForKeyWithRepresentedString:`对`UIKBSplitImageView`发送`.key`，触发NSInvalidArgumentException与SIGABRT。该查询已撤回；禁止再次使用或吞异常掩盖 |
| 222 | 内核/GPU/UI/Wi-Fi匹配，均stage6/error0；实际结果FAIL_APPLICATION | 19966条trace、43个样本、30个动作，无missing/skipped/drops；Spotlight三次因原生MPS设备不可用而SIGABRT。自动报告的PASS_DIAGNOSTIC已更正，仅表示采集完成 |
| 223 | 完整构建、匹配临时启动PASS；实际FAIL_APPLICATION | 23091条trace、43样本、30动作；504字节原生计数读取PASS。原生Espresso MPS栈仍SIGABRT；发现UIKeyboardAutomatic为375点、居中偏移15.83点 |
| 224 | 完整构建、匹配实机PASS；实际FAIL_APPLICATION_MPS_DEVICE | 外层宽度修复为406.67点，右侧p/删除/return恢复；glyph原生调用8414降到6。左下globe裁切、输入阻塞及MPS崩溃仍存在 |
| 225 | 完整构建、匹配实机PASS；实际FAIL_INPUT_BACKLOG_AND_DOCK | 17716条trace、43样本、30动作，无missing/skipped/drops；采集窗无MPS崩溃，连续输入仍积压，globe仍裁切 |
| 226 | 完整构建、匹配临时启动PASS；实际FAIL_INPUT_LATENCY，用户已停止修复 | 21583条trace、43样本、30动作，无missing/skipped/drops；原生点/mm补为5.809524，globe截图完整可见，输入仍积压；采集窗无MPS SIGABRT |

### 实际布局证据

222的Spotlight与Preferences均记录到：screen/window为`0,0,406.67,904`，键盘frame/bounds为`0,0,406.67,233`，传入原生分片视图的cachedWidth及image size也为`406.67,233`。然而`UIView convertRect:toView:`给出的窗口位置为`15.83,613,406.67,233`：整块键盘向右偏移15.83点，右边超过窗口。下一步沿父视图frame/bounds/transform定位产生偏移的一级，而不是盲改图像宽度。

### 实际稳定性与计数边界

222退出前原生日志三次明确写入：`MPSCNNNeuronLinear initWithDevice: Failed to get MPSDevice`，随后launchd记录Spotlight SIGABRT。该问题与221的私有查询异常不同。原生CoreML/MPS调用链仍待确认，不能关闭搜索或伪造MPS设备来宣称通过。

诊断期间控制台日志会与USB回复交错，且原来的全进程512条计时上限可能先被图标调用耗尽。因此220/222“计划触摸数大于计时行数”不能单独证明漏键。223改为每方法有界日志，并由task read port读取同一进程内的原生已完成调用计数；写入中或epoch变化的快照拒绝采纳。它仍不等于字符已送达输入框。

### 222时点的回退及设备

撤回221后临时boot了220，但220的旧目标备份与当前库不同，安装器保留备份并返回UI stage4/error17，未完成回退。随后通过已获授权的同步重启进入Fastboot，再以新安装ID临时启动222，GPU/UI/Wi-Fi均stage6/error0，UI写入3项。当时运行222，后续已被匹配225替代；持久boot_b仍为198，没有刷分区或删除旧备份。

本机证据在`diting-work/logs/diting/services{218,219,220,221,222,223}/`对应候选目录；最新226状态以`diting-work/build/services226/keyboard-dock-20261118-01/work-state.json`为准。原始Apple ABI、镜像、完整console/trace及像素帧仅保存在本地。私有基线仍含尚未同步主仓库的phone stage v2与Wi-Fi安装实现，不能宣称仅主仓库可完全复现全部候选。


## 11. 225实机结果与226修复（2026-11-18，现已停止）

225的完整内核、匹配GPU/UI/Wi-Fi运行库及临时启动均PASS。17716条trace和43个CPU样本完整取得，但采集完成不能作为键盘响应通过：Spotlight主线程在17.07秒内消耗14.10秒user和2.26秒system，Preferences消耗6.51秒user和1.00秒system。Preferences的8次计划触摸到最后采样仅完成2次touchDown和1次touchUp，截图中输入仍落后。报告已从PASS_DIAGNOSTIC修正为FAIL_INPUT_BACKLOG_AND_DOCK，原采集状态单独保留。采集窗内没有MPS设备失败或SIGABRT；更长时间的稳定性仍需分别确认。

225保留224的外层宽度修复、真实native glyph缓存，向匹配22H374 Espresso镜像的NSGlobalDomain读取提供CPU引擎偏好视图，并缓存原生设备语言派生结果。没有修改持久偏好或Espresso全局、没有关闭搜索。

左下裁切的定位依据是原生UIKeyboardDockItemButton为0×0，UIImageView的x=-16点、宽31.67点。匹配22H374的UIKeyboardDockView用UIScreen._nativePointsPerMillimeter计算按钮frame及触摸padding；该屏幕方法依赖MobileGestalt main-screen-pitch，缺失值会产生零尺寸。捕获DT两种匹配面板均为70×155mm，原生像素1220×2712、nativeScale3，对应横向5.809524点/mm。226只在原生密度无有效值且像素/比例匹配Diting时使用此物理尺寸，保留有效原生值和未知屏幕结果，交由原生布局重新计算按钮。

输入栈反复出现UIDictationController的可用性通知、UISystemKeyboardDockController.updateDockItemsVisibility和UIKeyboardInputModeController.updateEnabledDictationLanguages。226复用相同真实输入模式、听写语言、建议/限制语言、启用字典、SLS配置和locale下稳定的原生刷新字典；原生调用前后配置不一致时拒绝缓存。force=YES和非主线程调用仍执行原生方法，字典读取后的实时可用性检查保留。缓存返回可变副本，调用者修改不污染缓存；不记录语言或输入文本。

226仅改userland/keyboard_compat18.h，13项主机门禁及严格arm64 UI编译链接PASS。GPU渲染、已签名诊断worker、Wi-Fi及其它驱动源码保持225基线；完整八核内核、链接、打包与匹配临时启动随后均PASS，已完成一次矩阵；最终结果见第12节。用户要求停止修复后，没有再启动新候选或测试。

本机证据：diting-work/logs/diting/services225/keyboard-espresso-20261118-01/keyboard-diagnosis-01/{report.json,keyboard.ndjson,symbolicated-stacks.json,stack-summary.log,console.raw.log,frame-*.png}；226源码、构建与临时脚本在diting-work/{build,logs/diting,tmp}/services226/keyboard-dock-20261118-01。原始Apple ABI与二进制片段仍仅保存在本地。


## 12. 226最终结果与停止交接（2026-11-18）

用户要求停止修复，将当前状态写入文档交接。226已完整构建、安装匹配运行库并临时启动；21583条trace、43个CPU样本、30个动作、970147字节结果，missing/skipped/kernel drops均0。GPU/UI/Wi-Fi均stage6/error0，UI写3项，GPU/Wi-Fi写0；没有刷boot_b或完整userdata。

实机日志直接确认原生屏幕点/mm为0，物理尺寸回退提供5.809524点/mm；dock按钮从0×0变为65.52×66.15，左下globe在截图中完整可见，右侧全宽修复保留。听写刷新缓存与原生Espresso CPU偏好视图均已命中，但输入仍失败：Spotlight主线程15.137秒user+1.130秒system/17.062秒，Preferences6.276秒+0.857秒/7.745秒，最后原生touchDown/touchUp分别6/6与2/2，对应计划16与8次触摸。截图仍有字符送达落后及气泡停留。计数不是最终字符送达或漏键证明，整体报告已从采集PASS_DIAGNOSTIC修正为FAIL_INPUT_LATENCY。

采集窗未观察到MPS设备失败或SIGABRT；不能据此宣称长期稳定性。原生同步听写可用性查询与dock更新仍在活跃栈中，226缓存未证实降低延迟。完整布局/命中与物理帧率均未验收，GPU优化继续暂停。

最后采集worker为job1/state3/quiescing0，没有遗留主机构建、打包或USB采集。09:28 UTC停止复核时，CDC与Fastboot均未枚举设备，当前模式无法确认；最后一次226匹配记录保留，停止复核未重启或部署。本次不commit/push，保留既有未提交及独立改动。

完整接手约束、源码/证据位置及未解决问题见[交接-2026-11-18-services226-键盘布局改善-延迟未解决.md](交接-2026-11-18-services226-键盘布局改善-延迟未解决.md)，小体积证据见[226停止摘要](services226-keyboard-stop-report.json)。用户恢复修复前不得启动新候选或测试。
