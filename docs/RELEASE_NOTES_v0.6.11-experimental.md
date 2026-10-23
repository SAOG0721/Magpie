# Magpie Experimental v0.6.11

本版新增可调整大小的性能分析浮窗和捕获／输出图表，完善 XeSS FG 帧间隔显示，加入 DLSS 模型预设与 DLSSNR 抗闪烁响应时间，并改进暂停截图和文件命名。

## 功能更新

### 性能分析浮窗

- 默认显示各效果器耗时及总耗时，展开“图表”可查看最近 10 秒的变化。
- 帧率、帧间隔分别显示**捕获／输出**两条曲线；效果器耗时保留一张总耗时图。捕获统计有效新帧，输出包含补帧，不包含光标、界面刷新及旧画面重放。
- 帧间隔提供均值和峰值，修复 **XeSS FG 2×／3×／4× 输出帧间隔无法显示**的问题。无法获取的数据显示为缺测；计时覆盖不完整时显示“部分统计”。
- 拖动右下角可调整浮窗大小，尺寸和位置会被记住。图表默认折叠，内容支持纵向滚动；没有内容时隐藏“开发者选项”。

### 暂停截图与文件命名

- 修复暂停画面时，包含原始输入的截图不能及时保存的问题。继续支持**输入＋输出、仅输出、仅输入**，成对截图对应同一画面。
- 截图统一使用 **`Magpie_YYYYMMDD_HHmmss_SSS_input.png`** 和 **`Magpie_YYYYMMDD_HHmmss_SSS_output.png`**，同组共用时间戳，重名时自动追加序号。右键效果／通道导出同样使用时间戳，并保留效果、Pass、纹理用途后缀及 PNG／DDS 格式。

### DLSS SR 模型预设

在**效果组页面 → 对应效果器行右侧的参数按钮**中选择，修改后重新开始缩放生效。

- 新增**自动、A–F（CNN）、J／K（Transformer 1代）、L／M（Transformer 2代）**。
- 默认 **K**；旧配置未设置时也使用 K，已有选择保留。
- 修复所选预设在部分质量模式下未生效的问题。

### DLSS FG 运行库与预设

- 更新 `dlssg_for_sm86` 至 **0.4.0**，配套 `nvngx_dlssg.dll` 至 **310.9.16.0**。
- 在倍率和光流选项之间新增模型预设：**自动（默认）、A（标准）、B（UI 重建 - 不生效）、C（性能模式）**。B 的 UI 重建功能当前不生效；修改预设后重新开始缩放。

### DLSSNR 抗闪烁响应时间

- 新增**抗闪烁响应时间**：范围 **5–200 ms**，步进 **5 ms**，默认 **80 ms**。数值越大越稳定、跟随变化越慢；数值越小响应越快。
- 调整后随下一帧画面生效，无需重新开始缩放。关闭抗闪烁时隐藏控件并保留设置值。

### DLSSNR 默认运行库

- 主包默认携带 **310.9.16 社区兼容版** `nvngx_dlssnr.dll`，供 RTX 20／30／40 系列使用。
- RTX 50 系列可从可选附件中使用 **NVIDIA 原版 310.9.16**。可选包保留原版和社区兼容版两个选项。

### RTSS 兼容提示

使用 RTSS 时，如果切换 DLSS／XeSS 效果链出现崩溃，可先退出 RTSS 后重试。相关说明见包内 `XESSFG.md`。

## 应该下载哪个文件？

首次安装或从任意旧版本、本地 Beta 升级，统一下载完整主包 **`Magpie-Experimental-x64.zip`**。

将 ZIP 完整解压到新目录，运行其中的 `Magpie.exe`。升级前先备份设置并从托盘完全退出旧版，保留旧程序和配置备份供回退。

### 可选附件

| 附件                     | 用途                                                                                                                                                                                  |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `DLSSNR-DLL-Options.zip` | 提供 RTX 50 使用的 NVIDIA 原版 310.9.16，以及 RTX 20／30／40 使用的社区兼容版。主包已包含社区兼容版。切换前退出 Magpie 并备份原 DLL，将所选 `nvngx_dlssnr.dll` 放到 `Magpie.exe` 旁。 |
| `NGX_OTA_Switch.bat`     | 管理 NVIDIA NGX OTA 更新，需要管理员权限；选择 Restore default 恢复默认设置。常规安装无需使用。                                                                                       |

---

# Magpie Experimental v0.6.11

This release adds a resizable performance profiler with capture/output charts, improves XeSS FG interval reporting, introduces DLSS model presets and DLSSNR anti-flicker response time, and improves paused screenshots and filenames.

## Feature updates

### Performance profiler

- Show individual effect timings and their total by default. Expand **Charts** to view changes over the last 10 seconds.
- Display separate **Capture / Output** curves for frame rate and frame interval, plus one total effect-time chart. Capture counts valid new frames; output includes generated frames and excludes cursor updates, UI refreshes and replays of old content.
- Show mean and peak frame intervals, and fix missing output intervals for **XeSS FG 2× / 3× / 4×**. Unavailable data is marked as missing; incomplete timing coverage is marked **Partial timing**.
- Drag the lower-right grip to resize the window. Its size and position are remembered. Charts start collapsed, content scrolls vertically, and empty **Developer options** are hidden.

### Paused screenshots and filenames

- Fix screenshots containing the original input not being saved promptly while the picture is paused. Retain **input + output, output only, and input only**, with each screenshot pair showing the same picture.
- Use **`Magpie_YYYYMMDD_HHmmss_SSS_input.png`** and **`Magpie_YYYYMMDD_HHmmss_SSS_output.png`**. Pairs share a timestamp, and filename collisions receive a sequence number. Right-click effect/channel exports also use timestamps, retaining their effect, pass and texture-purpose suffixes and PNG/DDS formats.

### DLSS SR model presets

Select a preset through **the parameter button on the right of the effect row on the Scaling modes page**. Restart scaling to apply a change.

- Add **Auto, A–F (CNN), J / K (Transformer 1), and L / M (Transformer 2)**.
- Default to **K**, including older configurations without a saved preset. Retain existing choices.
- Fix selected presets not taking effect in some quality modes.

### DLSS FG runtime and presets

- Update `dlssg_for_sm86` to **0.4.0** with the matching **310.9.16.0** `nvngx_dlssg.dll`.
- Add model presets between the multiplier and optical-flow controls: **Auto (default), A (Standard), B (UI Recomposition - No Effect), and C (Performance Mode)**. B's UI recomposition is currently inactive. Restart scaling after changing the preset.

### DLSSNR anti-flicker response time

- Add **Anti-flicker response time**, ranging from **5–200 ms** in **5-ms steps**, with a default of **80 ms**. Higher values favor stability with slower adaptation; lower values respond faster.
- Changes take effect with the next picture update without restarting scaling. Turning anti-flicker off hides the control and retains its value.

### Default DLSSNR runtime

- The main package includes the **310.9.16 community compatibility** `nvngx_dlssnr.dll` for RTX 20 / 30 / 40 series.
- RTX 50 users can select the **NVIDIA original 310.9.16** from the optional package, which contains both the original and community versions.

### RTSS compatibility note

If switching between DLSS and XeSS effect chains crashes while RTSS is running, try again with RTSS exited. See `XESSFG.md` in the package for details.

## Which file should I download?

For a first installation or an upgrade from any older version or local Beta, download the complete **`Magpie-Experimental-x64.zip`**.

Extract the entire ZIP into a new folder and run `Magpie.exe`. Before upgrading, back up settings and fully exit the old version from the tray. Keep the previous program and configuration backup for rollback.

### Optional assets

| Asset                    | Purpose                                                                                                                                                                                                                                                              |
| ------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `DLSSNR-DLL-Options.zip` | Contains NVIDIA original 310.9.16 for RTX 50 and the community version for RTX 20 / 30 / 40. The main package already includes the community version. To switch, exit Magpie, back up the current DLL and place the selected `nvngx_dlssnr.dll` beside `Magpie.exe`. |
| `NGX_OTA_Switch.bat`     | Manage NVIDIA NGX OTA updates with administrator privileges. Choose Restore default to restore the default setting. Normal installation does not require this tool.                                                                                                  |
