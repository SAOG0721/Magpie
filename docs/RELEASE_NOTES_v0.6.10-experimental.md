# Magpie Experimental v0.6.10

本版汇总 0.6.9 之后的更新：重新整理帧率与刷新设置，加入 RTSS 联动、实验性 VRR 和 FSR 3 补帧，更新 NVIDIA VFX、HDR 手动效果链及窗口兼容性。

## 功能更新

### Magpie 本体易用性更新

#### 帧率与刷新

- 新增全局记忆的 **简单／高级** 模式，默认简单。简单模式集中显示内容帧率、静态画面重绘和 VRR；高级模式提供节奏、光标与重复帧检测等详细选项。切换时保留高级偏好。
- 内容帧率使用 **跟随源／自定义**，旧内容“自动”设置迁移为跟随源。手动内容、光标和空闲帧率均为 **15–360 FPS，整数，步进 1**。新配置默认内容 60 FPS、空闲重绘开启且为 30 FPS；已有有效数值保留。
- 简单模式使用 Front Edge 节奏与原始帧优先的光标补充刷新；高级模式保留 Front Edge／Async／NVIDIA Reflex 与光标策略选择。修改在重新开始缩放后生效，运行中可使用“保存并重启”。
- 统一模式与数值输入的排列、独立单位和窄窗口换行；简单模式“恢复默认”恢复可见设置并保留隐藏的高级偏好。

#### RTSS 目标应用限帧

- 新增 **RTSS 配置接管**，默认关闭，可管理全局或目标应用配置。开启联动后，缩放开始时自动准备已安装的 RTSS 与 Magpie 自身配置，再应用目标限帧。MSI Afterburner 为可选工具。
- 支持同步 Magpie 的基础内容目标，或指定 **15–360 FPS、步进 1** 的上限；同步目标可在 1–1000 FPS 内解析。跟随源时不使用同步限帧，保存的偏好保留。补帧输出帧率与源应用限帧分别处理。
- 提供工具状态、只读重新检测、初始化和当前会话重试。RTSS 异常显示具体原因，正常缩放可继续。默认正常结束时尝试恢复原限帧，也可选择保留修改；外部配置冲突时停止覆盖。
- 崩溃或强制结束可能保留最后写入值。“已写入”表示配置核对通过，游戏实际限帧仍取决于 RTSS 挂钩和运行状态。新配置与尚未迁移的设置默认请求管理员启动，之后保留用户关闭该选项的选择。

#### VRR、截图与引导

- 增加默认关闭的 **可变刷新率（实验）**。开启后重新开始缩放，按呈现路径、显卡和显示器能力请求 VRR。
- 截图可选择 **原始输入＋最终输出、仅输出（默认）、仅输入**，成对截图使用同一真实内容帧。保留效果／通道 PNG 与 DDS 导出，HDR PNG 转为 SDR sRGB，DDS 保留实际格式。
- 重写 **165 个内置效果器**的中英文说明与参数提示，优先说明用途、适合素材和起步方法，完整组合限制集中到使用指南。
- DLSSNR 增加持续状态和分类恢复引导，区分启动中、运行中、单层跳过和多层停止。

### 效果器更新

#### FSR 3 补帧（实验）

- 新增 **FSR3 FrameGeneration**，固定 **2×**、仅支持 **SDR**，可与超分配合或单独使用。一条链使用一个补帧效果。
- 必须使用外部光流，默认 **AMDOF／质量**，也可选择 NVOF。旧缺省或“无”选择迁移为 AMDOF 质量，已有合法选择保留。
- 支持 NIS／NVSharpen 等已列明的矩形组合及输出内部黑边。不支持 HDR、CRT、几何变形、超出输出的裁剪和清单外效果，详见 `FSR3FG-README.md`。
- 支持静止画面的实时参数更新，修改生效后刷新当前画面。

#### NVIDIA VFX 与 HDR

- 更新为匹配的 NVIDIA VFX 1.3.0.0 组件，整合 **Upscale、VSR、VFG、TrueHDR**。
- Upscale 支持原生放大与最终尺寸适配，锐度 **0–1，默认 0.4，步进 0.01**。VSR 提供 Standard／High bitrate／Streaming／Denoise／Deblur 分类，默认 Standard、Medium，兼容已有降噪配置。
- VFG 支持 SDR **2–8×，默认 2×，步进 1**；模型为 Performance／Balanced，默认 Balanced，旧 High 迁移为 Balanced。自动场景切换检测默认开启，关闭后不使用该自动检测。
- TrueHDR 默认开启去色带，关闭时不使用去色带处理；保留精确输入与实时更新。对比度／饱和度 **0–200、默认 100**，中间灰 **10–100、默认 50**，峰值亮度 **400–2000 nits、默认 1000**，均步进 1。
- 较高质量、放大或补帧倍率可能增加显存与处理成本，实际收益取决于 GPU 和素材。
- HDR 使用 **手动效果链**：自行添加、排列和调整 HDR→SDR、SDR→HDR 或 RTX HDR。
- 原生 SDR 白色的新编辑范围为 **80–4000 nits、默认 203**，基础步进 1，保留分段／Shift 粗调；旧低亮度值在主动编辑前保持兼容。HDR 节点参数独立，配对来源可查看。

#### DLSS 补帧与通用静态画面处理

- DLSS FG 使用固定版本的社区兼容运行库并验证文件身份，保留 **2–4×，默认 2×，步进 1** 与旧配置。显卡兼容性以实际初始化和运行结果为准。
- DLSS／XeSS／FSR3／VFG 统一使用通用重复帧检测，移除效果内部独立开关。新配置的通用检测默认关闭，已有显式通用设置保留。
- 开启空闲重绘后，约 **0.5 秒**没有新内容时按有效空闲率刷新缓存；关闭时保持最后画面等待。缓存重绘不重复运行效果或光流，新内容恢复时重置补帧历史。工具栏 **R/s** 单独记录缓存重绘提交，不计入生成帧率。

## 应该下载哪个文件？

### 从 0.6.9、更早版本或本地 Beta 升级，以及首次使用

统一下载完整主包 **`Magpie-Experimental-x64.zip`**。升级前备份设置并从托盘完全退出 Magpie，将 ZIP 完整解压到新目录，再运行其中的 `Magpie.exe`。保留旧目录及配置备份供回退；首次使用直接完整解压后运行即可。

### 可选附件

| 附件 | 用途 |
| --- | --- |
| `DLSSNR-DLL-Options.zip` | 两种 DLL：RTX 50 原版 310.9.16（10 月 16 日性能优化版）；RTX 20／30／40 社区兼容版 310.9.16。退出 Magpie 并备份原 DLL 后，按显卡选择一个替换。 |
| `NGX_OTA_Switch.bat` | 查看或切换系统级 NGX OTA 设置，需要管理员权限；使用 Restore default 恢复。正常安装无需运行。 |

---

# Magpie Experimental v0.6.10 User Guide

This release brings together the changes since 0.6.9: reorganized refresh settings, RTSS integration, experimental VRR and FSR 3 frame generation, updated NVIDIA VFX, manual HDR chains and window compatibility improvements.

## Feature updates

### Magpie usability updates

#### Frame rate and refresh

- Add a globally remembered **Simple / Advanced** mode, defaulting to Simple. Simple focuses on content rate, static-picture redraw and VRR; Advanced adds pacing, cursor and duplicate-detection options. Switching retains advanced preferences.
- Content rate offers **Follow source / Custom**; legacy content Automatic migrates to Follow source. Manual content, cursor and idle rates use **15–360 FPS, integers, 1-FPS steps**. New profiles default to 60-FPS content and enabled 30-FPS idle redraw; valid existing values remain.
- Simple uses Front Edge and prefers original frames with supplemental cursor refresh. Advanced retains Front Edge / Async / NVIDIA Reflex and cursor policies. Restart scaling to apply changes, or use Save and restart while running.
- Align mode and number controls, separate units and improve narrow-window wrapping. Reset in Simple restores visible settings while retaining hidden advanced preferences.

#### RTSS target-application limiting

- Add default-off **RTSS profile management** for global or target-application profiles. When enabled, starting scaling prepares an installed RTSS and Magpie's own profile before applying the target limit. MSI Afterburner is optional.
- Synchronize with Magpie's base content target or specify **15–360 FPS in 1-FPS steps**; resolved synchronized targets can span 1–1000 FPS. Follow source does not use synchronized limiting and retains saved preferences. Generated output FPS and source-application limiting are handled separately.
- Provide tool status, read-only recheck, initialization and current-session retry. RTSS failures show specific causes while normal scaling can continue. Normal shutdown attempts to restore the original limit by default; keeping changes is optional. Stop overwriting on external configuration conflicts.
- Crashes or forced exits may leave the last written value. A verified write confirms configuration; actual in-game limiting depends on RTSS hooking and runtime state. New and unmigrated settings request administrator startup by default, while later choices to disable it are retained.

#### VRR, screenshots and guidance

- Add default-off **Variable refresh rate (experimental)**. Restart scaling after enabling it. Request VRR according to the presentation path, GPU and display capabilities.
- Choose **original input + final output, output only (default), or input only** for screenshots. Pairs use the same real content frame. Retain effect/channel PNG and DDS export; HDR PNG converts to SDR sRGB, while DDS retains the actual format.
- Rewrite Chinese and English descriptions and parameter help for **165 built-in effects**, emphasizing purpose, suitable content and initial setup; collect full combination rules in usage guides.
- Add persistent DLSSNR state and cause-specific recovery guidance for startup, running, single-pass skipping and multi-pass stopping.

### Effect updates

#### FSR 3 frame generation (experimental)

- Add **FSR3 FrameGeneration**, fixed **2× SDR**, with or without super resolution. Use one frame-generation effect per chain.
- External optical flow is required, defaulting to **AMDOF / Quality**, with NVOF also available. Legacy missing or None selections migrate to AMDOF Quality; valid existing choices remain.
- Support listed rectangular combinations including NIS / NVSharpen and letterboxing inside the output. HDR, CRT, geometric distortion, cropping beyond the output and unlisted effects are unsupported; see `FSR3FG-README.md`.
- Support live parameter edits on static pictures and refresh the current picture after changes take effect.

#### NVIDIA VFX and HDR

- Update to matched NVIDIA VFX 1.3.0.0 components for **Upscale, VSR, VFG and TrueHDR**.
- Upscale supports native scaling and final size adaptation; sharpness uses **0–1, default 0.4, step 0.01**. VSR offers Standard / High bitrate / Streaming / Denoise / Deblur, defaults to Standard and Medium, and retains existing denoise compatibility.
- VFG supports SDR **2–8×, default 2×, step 1**. Models are Performance / Balanced, defaulting to Balanced; retired High migrates to Balanced. Automatic shot detection defaults to On; disabling it turns off that detection.
- TrueHDR enables debanding by default; disabling it turns off debanding. Retain precise entry and live updates. Contrast / saturation use **0–200, default 100**; middle gray **10–100, default 50**; peak brightness **400–2000 nits, default 1000**, all in steps of 1.
- Higher quality, scale or frame multipliers can increase memory use and processing cost; results depend on GPU and content.
- Use **manual HDR chains**: add, order and adjust HDR-to-SDR, SDR-to-HDR or RTX HDR.
- New native SDR-white edits use **80–4000 nits, default 203**, a base step of 1 and segmented / Shift coarse adjustments. Legacy dim values remain compatible until edited. HDR nodes retain independent parameters and show pair-source information.

#### DLSS frame generation and shared static-picture handling

- DLSS FG uses a pinned community compatibility runtime with file-identity verification. Retain **2–4×, default 2×, step 1** and existing configurations. GPU compatibility depends on actual initialization and operation.
- DLSS / XeSS / FSR3 / VFG share general duplicate-frame detection and remove separate effect switches. General detection defaults to Off for new settings; existing explicit general settings remain.
- With idle redraw enabled, about **0.5 seconds** without new content starts cached refresh at the effective idle rate. Disabling it holds the last picture. Cached redraw does not rerun effects or optical flow; resumed content resets FG history. Toolbar **R/s** counts cached redraw submissions separately from generated-frame rate.

## Which file should I download?

### Upgrading from 0.6.9, an older version or a local Beta, or installing for the first time

Everyone should download the complete **`Magpie-Experimental-x64.zip`**. Before upgrading, back up settings and fully exit Magpie from the tray. Extract the entire ZIP into a new folder and run its `Magpie.exe`. Retain the previous folder and configuration backup for rollback. For a first installation, simply extract the complete package and run it.

### Optional assets

| Asset | Purpose |
| --- | --- |
| `DLSSNR-DLL-Options.zip` | Two DLLs: NVIDIA original 310.9.16 for RTX 50 (October 16 performance update); community-patched 310.9.16 for RTX 20/30/40. Exit Magpie, back up the original DLL, and replace it with one option for your GPU. |
| `NGX_OTA_Switch.bat` | View or change system-wide NGX OTA settings with administrator privileges; use Restore default to restore them. Normal installation does not require it. |
