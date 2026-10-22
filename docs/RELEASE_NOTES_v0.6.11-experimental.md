# Magpie Experimental v0.6.11

本版更新 DLSS 补帧运行库，新增 DLSS SR／FG 模型预设和 DLSSNR 抗闪烁响应时间，修正暂停画面的截图处理，并统一截图文件命名。

## 功能更新

### Magpie 本体易用性更新

#### 暂停截图与文件命名

- 修复包含原始输入的截图在暂停、没有新捕获帧时可能等待后续画面、不能及时保存的问题。截图改用独立快照交接，已匹配的输入与输出无需等待下一次画面刷新即可保存；仅输入截图也不再等待输出呈现。
- 保留 **原始输入＋最终输出、仅输出、仅输入** 三种内容选择。成对截图继续匹配同一真实内容帧，不将不同时刻的输入和输出拼成一组。
- 截图文件改为 **`Magpie_YYYYMMDD_HHmmss_SSS_input.png`** 和 **`Magpie_YYYYMMDD_HHmmss_SSS_output.png`**。同组输入／输出共用时间戳；单图也保留对应的 `_input` 或 `_output` 后缀。同名冲突时追加短序号，避免覆盖已有截图。

### 效果器更新

新增参数都在 **效果组页面 → 对应效果器行右侧的参数按钮** 中，随各自效果实例保存。

#### DLSS SR 模型预设

- 新增 **自动、A–F（CNN）、J／K（Transformer 第一代）、L／M（Transformer 第二代）**。
- **默认 K**。新建效果和旧配置缺少该字段时使用 K，已有明确保存的有效选择保留；“自动”与 K 是不同选项。
- 修改后重新开始缩放生效。旧 A–F 预设仍可选择，实际采用情况由运行库决定；不同预设的画质与性能取决于显卡、驱动和素材。

#### DLSS FG 运行库与预设

- 将 `dlssg_for_sm86` 更新至 **0.4.0**，配套 `nvngx_dlssg.dll` 更新至 **310.9.16.0**。
- 新增 **自动（默认）、A、B、C** 预设。旧配置缺少该字段时使用自动，已有有效选择保留；修改后重新开始缩放生效。

#### DLSSNR 抗闪烁响应时间

- 在抗闪烁选项下新增 **抗闪烁响应时间：5–200 ms，步进 5 ms，默认 80 ms**。旧配置缺少该字段时仍使用 80 ms，与原固定时间保持一致。
- 抗闪烁关闭时隐藏时间控件并保留保存值；四种抗闪烁模式均使用这一设置，多 Pass 共用链末端的响应时间。
- 修改后在**下一有效捕获帧**应用，无需重新开始缩放。单独调整时间不清空已有历史，也不额外运行降噪网络。
- 数值越大越稳定、变化跟随越慢；数值越小响应越快。该数值控制累积响应，不代表同等时长的额外显示延迟。

#### DLSSNR 默认运行库

- 主包默认携带 **310.9.16 社区兼容版** `nvngx_dlssnr.dll`，供 RTX 20／30／40 系列使用。
- 可选包保留 **NVIDIA 原版 310.9.16（RTX 50 系列）**与同版**社区兼容版（RTX 20／30／40 系列）**两个选项，沿用已提供的两选项 ZIP；不再提供 SF-v2 选项。
- 社区兼容版并非 NVIDIA 原版。各型号的兼容性、画质与性能仍取决于实际运行结果。

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

This release updates the DLSS frame-generation runtime, adds DLSS SR / FG model presets and an adjustable DLSSNR anti-flicker response time, corrects paused-picture screenshot handling, and standardizes screenshot filenames.

## Feature updates

### Magpie usability updates

#### Paused screenshots and filenames

- Fix screenshots that include the original input potentially waiting for subsequent content while playback is paused and no new capture arrives. An independent snapshot handoff allows matched input and output to be saved without another picture refresh; input-only screenshots no longer wait for output presentation either.
- Retain **original input + final output, output only, and input only**. Screenshot pairs continue to match the same real content frame rather than combining input and output from different moments.
- Use **`Magpie_YYYYMMDD_HHmmss_SSS_input.png`** and **`Magpie_YYYYMMDD_HHmmss_SSS_output.png`**. A pair shares one timestamp; single screenshots retain the appropriate `_input` or `_output` suffix. Add a short sequence number on a filename collision to avoid overwriting existing screenshots.

### Effect updates

All new parameters are available through **the parameter button on the right of the corresponding effect row on the Scaling modes page**, and are saved per effect instance.

#### DLSS SR model presets

- Add **Auto, A–F (CNN), J / K (first-generation Transformer), and L / M (second-generation Transformer)**.
- **Default to K** for new effects and older configurations without this field. Retain existing explicitly saved valid choices; Auto and K are separate options.
- Restart scaling to apply a change. Legacy A–F presets remain selectable, while the runtime determines which request it adopts. Image quality and performance vary with GPU, driver and content.

#### DLSS FG runtime and presets

- Update `dlssg_for_sm86` to **0.4.0** with the matching **310.9.16.0** `nvngx_dlssg.dll`.
- Add **Auto (default), A, B and C** presets. Older configurations without this field use Auto, while existing valid choices remain. Restart scaling to apply a change.

#### DLSSNR anti-flicker response time

- Add **Anti-flicker response time: 5–200 ms, 5-ms steps, default 80 ms** below the anti-flicker option. Older configurations without this field continue to use 80 ms, matching the previous fixed value.
- Hide the time control when anti-flicker is Off and retain its saved value. All four anti-flicker modes use this setting; multiple passes share one response time at the end of the chain.
- Apply changes on the **next valid captured frame**, without restarting scaling. Adjusting only the response time retains existing history and does not run an extra denoising-network evaluation.
- Higher values favor stability with slower adaptation; lower values respond faster. This value controls accumulation response and does not add an equivalent amount of display latency.

#### Default DLSSNR runtime

- The main package now includes the **310.9.16 community compatibility** `nvngx_dlssnr.dll` for RTX 20 / 30 / 40 series.
- The optional package retains two choices: **NVIDIA original 310.9.16 (RTX 50 series)** and the **community compatibility version (RTX 20 / 30 / 40 series)**, using the previously supplied two-option ZIP. The SF-v2 option is no longer included.
- The community version is not the original NVIDIA binary. Compatibility, image quality and performance on individual GPU models still depend on actual operation.

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
