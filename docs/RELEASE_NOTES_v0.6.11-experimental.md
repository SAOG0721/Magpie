# Magpie Experimental v0.6.11

本版新增可调整大小的性能分析浮窗和捕获／输出图表，补齐 XeSS FG 输出帧间隔；更新 DLSS 补帧运行库，新增 DLSS SR／FG 模型预设和 DLSSNR 抗闪烁响应时间，修正暂停截图并统一截图命名。

## 功能更新

### Magpie 本体易用性更新

#### 性能分析浮窗

- 默认显示各效果器耗时及总耗时，手动展开二级“图表”后查看最近 10 秒变化。没有实际内容时隐藏“开发者选项”。
- **帧率、帧间隔**分别显示“捕获／输出”两条曲线；**效果器耗时**保持单图。捕获按当前去重规则接收的有效新帧统计，输出包含补帧，排除光标、界面刷新和旧画面的空闲重放。
- 帧间隔使用真实事件时间戳，保留均值和峰值，不从 FPS 或补帧倍率倒算。修复随附运行库下 **XeSS FG 2×／3×／4× 输出帧间隔缺测**；没有可用逐帧数据的后端或运行库仍明确显示缺测。
- 输出数据描述内容提交／后端报告，不代表显示器实际扫描帧率。效果器计时不包含所有原生 SDK 内部工作时，会保留“部分统计”说明。
- 浮窗右下角支持拖动调整大小，记忆尺寸和位置；展开图表不强制放大窗口，内容可纵向滚动。默认折叠图表，按需查看。

#### 暂停截图与文件命名

- 修复包含原始输入的截图在暂停、没有新捕获帧时可能等待后续画面、不能及时保存的问题。截图改用独立快照交接，已匹配的输入与输出无需等待下一次画面刷新即可保存；仅输入截图也不再等待输出呈现。
- 保留 **原始输入＋最终输出、仅输出、仅输入** 三种内容选择。成对截图继续匹配同一真实内容帧，不将不同时刻的输入和输出拼成一组。
- 截图文件改为 **`Magpie_YYYYMMDD_HHmmss_SSS_input.png`** 和 **`Magpie_YYYYMMDD_HHmmss_SSS_output.png`**。时间取请求时的本地时间，精确到毫秒；同组输入／输出共用时间戳；单图也保留对应的 `_input` 或 `_output` 后缀。同名冲突时追加短序号，避免覆盖已有截图。右键效果／通道导出也使用时间戳，并保留效果、Pass 和纹理用途后缀及原有 PNG／DDS 格式。

### 效果器更新

新增参数都在 **效果组页面 → 对应效果器行右侧的参数按钮** 中，随各自效果实例保存。

#### DLSS SR 模型预设

- 新增 **自动、A–F（CNN）、J／K（Transformer 1代）、L／M（Transformer 2代）**。
- **默认 K**。新建效果和旧配置缺少该字段时使用 K，已有明确保存的有效选择保留；“自动”与 K 是不同选项。
- 修改后重新开始缩放生效。
- 修复部分质量模式没有使用所选 SR 预设的问题，使预设在各质量模式下正确传递。

#### DLSS FG 运行库与预设

- 将 `dlssg_for_sm86` 更新至 **0.4.0**，配套 `nvngx_dlssg.dll` 更新至 **310.9.16.0**。
- 新增 **自动（默认）、A、B、C** 预设。旧配置缺少该字段时使用自动，已有有效选择保留；修改后重新开始缩放生效。
- 预设位于倍率和光流选项之间；补充 **A（标准）**、**C（性能模式）**说明。B 标为 **UI 重建 - 不生效**，当前接入不提供该 UI 重建功能。

#### DLSSNR 抗闪烁响应时间

- 在抗闪烁选项下新增 **抗闪烁响应时间：5–200 ms，步进 5 ms，默认 80 ms**。旧配置缺少该字段时仍使用 80 ms，与原固定时间保持一致。
- 抗闪烁关闭时隐藏时间控件并保留保存值；四种抗闪烁模式均使用这一设置，多 Pass 共用链末端的响应时间。
- 修改后在**下一有效捕获帧**应用，无需重新开始缩放。单独调整时间不清空已有历史，也不额外执行 DLSSNR。
- 数值越大越稳定、变化跟随越慢；数值越小响应越快。该数值控制累积响应，不代表同等时长的额外显示延迟。

#### DLSSNR 默认运行库

- 主包默认携带 **310.9.16 社区兼容版** `nvngx_dlssnr.dll`，供 RTX 20／30／40 系列使用。
- 可选包保留 **NVIDIA 原版 310.9.16（RTX 50 系列）**与同版**社区兼容版（RTX 20／30／40 系列）**两个选项，沿用已提供的两选项 ZIP；不再提供 SF-v2 选项。
- 社区兼容版并非 NVIDIA 原版。各型号的兼容性、画质与性能仍取决于实际运行结果。

## 已知限制

- XeSS 输出帧间隔的本机验证覆盖随附运行库及 NVIDIA GPU；其他显卡、HDR／VRR 和长期游戏体验仍需实际检查。未知运行库的原生路径保留功能，无法安全观测时显示缺测。
- 部分 RTSS Reflex 标记设置可能在 DLSS／XeSS 跨效果链切换时触发崩溃。受影响时可先退出 RTSS 对照；本机采用仅关闭 Magpie 专用 `ReflexSetLatencyMarker` 的规避设置后通过复测。主包不会自动修改 RTSS 配置。

## 应该下载哪个文件？

### 从 0.6.10 升级

下载完整主包 **`Magpie-Experimental-x64.zip`**。先备份设置并从托盘完全退出 Magpie，将 ZIP 完整解压到新目录，再运行其中的 `Magpie.exe`。不要只替换 EXE；保留旧目录与配置备份供回退。

### 从更早版本或本地 Beta 升级，以及首次使用

同样使用完整主包。升级时按上述步骤备份并完整解压；首次使用直接完整解压后运行即可，不要在压缩包内启动程序。

### 可选附件

| 附件 | 用途 |
| --- | --- |
| `DLSSNR-DLL-Options.zip` | 两种 DLL：`NVIDIA-Original-310.9.16-RTX50-Series-Only` 中的 NVIDIA 原版，以及 `Community-Patched-310.9.16-RTX20-RTX30-RTX40-Series` 中的社区兼容版。RTX 50 用户可切换至原版；主包已包含社区兼容版，无需重复安装。切换前完全退出 Magpie、备份原 DLL，再把所选 `nvngx_dlssnr.dll` 放到 `Magpie.exe` 旁。包内 README 保留原有 0.6.10 来源标记。 |
| `NGX_OTA_Switch.bat` | 查看或切换系统级 NGX OTA 设置，需要管理员权限；使用 Restore default 恢复。正常安装无需运行。 |

---

# Magpie Experimental v0.6.11 User Guide

This release adds a resizable performance profiler with capture/output charts and XeSS FG output intervals. It also updates the DLSS frame-generation runtime, adds DLSS SR / FG model presets and an adjustable DLSSNR anti-flicker response time, fixes paused screenshots, and standardizes screenshot filenames.

## Feature updates

### Magpie usability updates

#### Performance profiler

- Show individual effect timings and their total by default. Manually expand the secondary **Charts** section to view the last 10 seconds. Hide **Developer options** when it has no content.
- Show separate **Capture / Output** curves for both **Frame rate** and **Frame interval**, with one **Effect timings** chart. Capture counts new frames accepted by the current duplicate-detection policy. Output includes generated frames and excludes cursor, UI and idle replays of old content.
- Measure intervals from real event timestamps, retaining mean and peak values, without deriving them from FPS or a generation multiplier. Fix missing output intervals for **XeSS FG 2× / 3× / 4× with the bundled runtime**. Backends or runtimes without usable per-frame observations continue to show missing data explicitly.
- Output represents content submissions or backend-reported counts, not physical display scanout. Retain the **Partial timing** notice when measured effect timings do not cover all native SDK work.
- Drag the lower-right grip to resize the window; retain its size and position. Expanding charts does not force a larger window, and content can scroll vertically. Charts start collapsed for optional viewing.

#### Paused screenshots and filenames

- Fix screenshots that include the original input potentially waiting for subsequent content while playback is paused and no new capture arrives. An independent snapshot handoff allows matched input and output to be saved without another picture refresh; input-only screenshots no longer wait for output presentation either.
- Retain **original input + final output, output only, and input only**. Screenshot pairs continue to match the same real content frame rather than combining input and output from different moments.
- Use **`Magpie_YYYYMMDD_HHmmss_SSS_input.png`** and **`Magpie_YYYYMMDD_HHmmss_SSS_output.png`**. The local timestamp is captured when the screenshot is requested, with millisecond precision. A pair shares one timestamp; single screenshots retain the appropriate `_input` or `_output` suffix. Add a short sequence number on a filename collision to avoid overwriting existing screenshots. Right-click effect and channel exports also use timestamps, with effect/pass/texture purpose suffixes and their existing PNG/DDS formats.

### Effect updates

All new parameters are available through **the parameter button on the right of the corresponding effect row on the Scaling modes page**, and are saved per effect instance.

#### DLSS SR model presets

- Add **Auto, A–F (CNN), J / K (Transformer 1), and L / M (Transformer 2)**.
- **Default to K** for new effects and older configurations without this field. Retain existing explicitly saved valid choices; Auto and K are separate options.
- Restart scaling to apply a change.
- Fix selected SR presets not being applied in some quality modes, so the requested preset is passed correctly across quality modes.

#### DLSS FG runtime and presets

- Update `dlssg_for_sm86` to **0.4.0** with the matching **310.9.16.0** `nvngx_dlssg.dll`.
- Add **Auto (default), A, B and C** presets. Older configurations without this field use Auto, while existing valid choices remain. Restart scaling to apply a change.
- Place the preset between multiplier and optical-flow controls, with **A (Standard)** and **C (Performance Mode)** descriptions. Label B as **UI Recomposition - No Effect**; the current integration does not provide that UI recomposition feature.

#### DLSSNR anti-flicker response time

- Add **Anti-flicker response time: 5–200 ms, 5-ms steps, default 80 ms** below the anti-flicker option. Older configurations without this field continue to use 80 ms, matching the previous fixed value.
- Hide the time control when anti-flicker is Off and retain its saved value. All four anti-flicker modes use this setting; multiple passes share one response time at the end of the chain.
- Apply changes on the **next valid captured frame**, without restarting scaling. Adjusting only the response time retains existing history and does not trigger an extra DLSSNR evaluation.
- Higher values favor stability with slower adaptation; lower values respond faster. This value controls accumulation response and does not add an equivalent amount of display latency.

#### Default DLSSNR runtime

- The main package now includes the **310.9.16 community compatibility** `nvngx_dlssnr.dll` for RTX 20 / 30 / 40 series.
- The optional package retains two choices: **NVIDIA original 310.9.16 (RTX 50 series)** and the **community compatibility version (RTX 20 / 30 / 40 series)**, using the previously supplied two-option ZIP. The SF-v2 option is no longer included.
- The community version is not the original NVIDIA binary. Compatibility, image quality and performance on individual GPU models still depend on actual operation.

## Known limitations

- Local XeSS output-interval validation covers the bundled runtime on an NVIDIA GPU. Other GPUs, HDR / VRR and long gaming sessions still require practical checks. Native operation is retained with unknown runtimes, with missing intervals shown when safe observation is unavailable.
- Some RTSS Reflex marker settings can trigger a crash during transitions between DLSS and XeSS effect chains. Affected users can first compare with RTSS exited. Local retesting passed after disabling only the Magpie-specific `ReflexSetLatencyMarker`; the package does not automatically modify RTSS settings.

## Which file should I download?

### Upgrading from 0.6.10

Download the complete **`Magpie-Experimental-x64.zip`**. Back up settings and fully exit Magpie from the tray. Extract the entire ZIP into a new folder and run its `Magpie.exe`. Do not replace only the EXE; keep the previous folder and configuration backup for rollback.

### Upgrading from an older version or local Beta, or installing for the first time

Use the same complete package. Follow the backup and full-extraction steps above when upgrading. For a first installation, extract the entire package and run it; do not start the program inside the ZIP.

### Optional assets

| Asset | Purpose |
| --- | --- |
| `DLSSNR-DLL-Options.zip` | Contains the NVIDIA original in `NVIDIA-Original-310.9.16-RTX50-Series-Only` and the community version in `Community-Patched-310.9.16-RTX20-RTX30-RTX40-Series`. RTX 50 users can switch to the original. The main package already contains the community version, so reinstalling it is unnecessary. Fully exit Magpie and back up the current DLL before placing the selected `nvngx_dlssnr.dll` beside `Magpie.exe`. The included README retains its original 0.6.10 source-version label. |
| `NGX_OTA_Switch.bat` | View or change system-wide NGX OTA settings with administrator privileges; use Restore default to restore them. Normal installation does not require it. |
