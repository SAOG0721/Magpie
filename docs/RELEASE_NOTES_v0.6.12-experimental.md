# Magpie Experimental v0.6.12

本版新增 OKLab Image Adjustment 调色效果器、DLSSNR 模型与性能选项，改进参数编辑、帧率统计、捕获和 VRR 呈现。

## 功能更新

### 参数界面与性能分析

- 参数面板按可用宽度分组排列，运行浮窗支持纵向滚动，并可在画面留黑区域内操作。改进窄窗口、长标签、勾选项与底部按钮的布局，减少控件裁切和重叠。
- 编辑运行参数时，在浮窗内按住 **Ctrl＋滚轮**调整界面大小：范围 **50%–200%**，每档 **10 个百分点**，默认 **100%**；缩放比例会被记住。
- 修复打开或展开参数页时，隐藏控件改写已保存选项的问题；修复部分下拉框误关弹窗、选项空白、参数缺失，以及打开 VFG、RTXVideo HDR 和部分 CRT 参数时的崩溃。
- 性能分析器记住窗口与分区的折叠状态，以及是否显示各 Pass 耗时。无补帧时，工具栏 FPS 按新内容统计，旧画面重放、光标和界面刷新不计入新内容帧率。

### OKLab Image Adjustment

- 在效果器列表搜索 **OKLab** 添加。保持画面尺寸，提供曝光、明暗、白平衡、色度、色相、八色混合、三向调色、选色、曲线和 LUT 调整。
- 提供局部对比度、纹理、彩噪抑制、去色带、辉光、去雾、几何蒙版，以及自动曝光和自动白平衡；可锁定自动调整结果。
- 提供六组预设、分屏比较、A/B、撤销、色轮与曲线编辑工具；运行浮窗中的吸管可拖到画面上取色。支持 `.cube` LUT 导入、导出，LUT 随配置保存。
- 常规数值可实时调整；**导入或清除 LUT 后重新开始缩放**。局部与邻域效果默认关闭，复杂组合会增加 GPU 开销。其他参数的范围和默认值见对应控件及悬停说明。

### DLSSNR

- 新增模型预设：**自动（DLL 默认，默认选项）、HT、CC Small FP8**，所有处理层共用选择。实际模型支持取决于 GPU 和所用运行库；更改后重新开始缩放。
- 新增 **自适应／手动** 分辨率控制。新建效果默认自适应，按内容帧率目标以 **5 个百分点**调整输入比例；手动范围仍为 **25%–100%**、步进 **1 个百分点**。旧配置保留手动设置，切回手动时恢复已保存的比例；自适应时可查看当前使用的比例。
- 提供 **逐帧（默认）／隔帧**两种处理模式。隔帧仅支持 **SDR**，可选择每 **2／3／4** 个新捕获帧更新完整 NR，默认每 **2** 帧；重复刷新、重绘和生成帧不推进更新周期。
- 隔帧时继续更新当前画面，可原位复用历史增强，或通过 **AMDOF／NVOF** 对齐后复用。关闭光流时，历史重投影自动改为原位复用，依赖光流的抗闪烁选项改为静态累积。
- 新增 **中间调强度**：范围 **0–2**、默认 **1**、步进 **0.05**，单独调整中间亮度区域的明暗修正。新增 **全域色度保护**：范围 **0–1**、默认 **0**、步进 **0.05**，减弱 NR 对原图色相和颜色浓度的改变，同时保留明暗调整。
- SDR 始终使用残差重建，输入比例为 **100%** 时也能调整残差和细节。改进低频／高频残差、颜色保护和最终颜色限制的处理顺序。

较低输入比例减少处理量，也可能损失细节；隔帧间隔越大，推理次数越少，历史增强也越旧，运动中可能出现拖影或增强减弱。自适应切档和首次准备新尺寸仍有额外开销。模型、处理模式、输入尺寸、复用方式及光流等结构设置更改后需重新开始缩放；SDR 残差与细节数值可实时调整，界面会标明其他参数的生效方式。

### 捕获、VRR 与鼠标

- 优化 WGC 捕获间隔和等待方式，减少静止画面的空轮询。改善切换焦点、遮挡／最小化、尺寸变化后的捕获与输出恢复，以及 DWM 捕获边界处理。
- 修复部分鼠标重新定位、光标形状更新造成的呈现停顿，并完善停止缩放时的鼠标位置与限制恢复。
- 在受支持的 **Graphics Capture（WGC）＋DLSSNR＋Front Edge＋VRR、无补帧**输出路径，连续约两个目标帧周期没有新内容提交时，可按目标节奏补交最近已完成的画面。需要开启 VRR，且当前输出通道的 tearing 支持实际可用。
- 待提交的新画面与正在处理的内容优先；连续约 **250 ms** 没有新内容提交后停止这类补交。补交复用已完成画面，不重跑 DLSSNR；实际新内容帧率仍取决于源程序、捕获和效果处理速度。

VRR 仍为实验功能，默认关闭。更改捕获、节奏或 VRR 设置后重新开始缩放；帧间隔表现会随源程序、驱动、显示器和配置变化。刷新设置见包内 `FRAME_SYNC_GUIDE.md`。

## 安装、升级与回退

首次安装和从旧版升级均使用完整主包 **`Magpie-Experimental-x64.zip`**。先备份设置并从托盘完全退出旧版，将整个 ZIP 解压到新目录，再运行其中的 `Magpie.exe`。

使用过本地 Beta 配置时，DLSSNR 的异步逐帧自动回退为逐帧，异步隔帧自动回退为隔帧；保留有效更新间隔、模型预设和输入比例。“帧率与刷新”中的独立 **Async** 节奏继续保留。

保留旧程序目录与升级前的设置备份。需要回退时，完全退出当前版本，恢复原设置后从旧版目录运行。

## 附件用途

| 文件 | 用途与使用方式 |
| --- | --- |
| `Magpie-Experimental-x64.zip` | 可独立安装的完整主包，包含配套程序、资源与运行库。主包沿用 310.9.16 社区兼容 DLSSNR，供 RTX 20／30／40 系列使用。 |
| `DLSSNR-DLL-Options.zip` | 沿用 NVIDIA 原版 RTX 50 与社区兼容 RTX 20／30／40 两个选项。切换前退出 Magpie 并备份原 DLL，将所选 `nvngx_dlssnr.dll` 放到 `Magpie.exe` 旁；具体步骤见包内说明。 |
| `NGX_OTA_Switch.bat` | 可选的 NVIDIA NGX OTA 更新管理工具，需要管理员权限；Restore default 恢复默认设置。常规安装无需运行。 |

## 致谢

- 感谢 [Len-huan](https://github.com/Len-huan) 在 [issue #73](https://github.com/SAOG0721/Magpie/issues/73) 中提出 DLSSNR 权重预设建议并提供相关分析。
- 感谢 [yejincheng420](https://github.com/yejincheng420) 在其 [Magpie 分支](https://github.com/yejincheng420/Magpie/tree/turing-ampere) 中分享隔帧处理的基础思路。

---

# Magpie Experimental v0.6.12 User Guide

This version adds the OKLab Image Adjustment effect and DLSSNR model/performance controls, and improves parameter editing, frame-rate accounting, capture and VRR presentation.

## Feature updates

### Parameter interface and performance profiler

- Group parameters according to the available width. The running overlay supports vertical scrolling and remains usable in letterboxing. Improve narrow-window layouts, long labels, checkboxes and footer buttons to reduce clipping and overlap.
- While editing running parameters, hold **Ctrl and scroll the mouse wheel** inside the overlay to resize its interface: **50%–200%**, in **10-percentage-point** steps, default **100%**. The zoom setting is remembered.
- Fix hidden controls rewriting saved choices when opening or expanding parameter pages. Fix dropdowns unexpectedly closing their parent popup, blank choices, missing parameters, and crashes when opening VFG, RTXVideo HDR and some CRT parameter pages.
- Remember profiler window/section collapse states and the preference for per-pass timings. Without frame generation, the toolbar FPS counts new content; replays, cursor updates and interface refreshes do not count as new-content FPS.

### OKLab Image Adjustment

- Search for **OKLab** in the effect list to add it. It preserves image dimensions and provides exposure, tonal controls, white balance, chroma, hue, an eight-color mixer, three-way grading, selections, curves and LUT adjustments.
- Includes local contrast, texture, chroma-noise reduction, debanding, bloom, dehaze, geometric masks, automatic exposure and automatic white balance. Automatic adjustments can be locked.
- Includes six presets, split comparison, A/B, undo, wheel and curve editors. Drag the running overlay's eyedropper onto the picture to sample a color. Import/export `.cube` LUTs; LUT assets are saved with the configuration.
- Ordinary numeric changes apply live. **Restart scaling after importing or clearing a LUT.** Local and neighborhood effects are disabled by default; complex combinations increase GPU cost. See the controls and hover descriptions for other parameter ranges and defaults.

### DLSSNR

- Add model presets: **Automatic (DLL Default, the default choice), HT and CC Small FP8**, shared by all processing passes. Actual model support depends on the GPU and runtime library. Restart scaling after changing the selection.
- Add **Adaptive / Manual** resolution control. New effects default to Adaptive, adjusting the input scale in **five-percentage-point** steps according to the content-rate target. Manual remains **25%–100%** in **one-percentage-point** steps. Existing configurations retain Manual, and switching back to Manual restores the saved scale. Adaptive mode shows the current scale in use.
- Offer **Every Frame (default) / Alternating Frames** processing. Alternating Frames supports **SDR only**, updating full NR every **2 / 3 / 4** new captures, default **2**. Repeated refreshes, redraws and generated frames do not advance the update cycle.
- Alternating Frames keeps the current picture updating while reusing historical enhancement in place or aligning it with **AMDOF / NVOF**. Turning optical flow off changes reprojection to in-place reuse and motion-dependent anti-flicker modes to Static Accumulation.
- Add **Midtone Strength**: **0–2**, default **1**, step **0.05**, controlling lightness corrections in midtone regions. Add **global Chroma Protection**: **0–1**, default **0**, step **0.05**, reducing NR changes to original hue and color intensity while retaining lightness adjustments.
- SDR always uses residual reconstruction, allowing residual and detail adjustments even at **100%** input scale. Improve the processing order for low-/high-frequency residuals, color protection and final color limiting.

Lower input scales reduce processing work and can lose detail. Longer alternating intervals reduce inference frequency but retain older enhancement, which can cause trails or weaker enhancement in motion. Adaptive tier changes and preparing a new size still have additional costs. Restart scaling after changing structural settings such as model, processing mode, input size, reuse method or optical flow. SDR residual/detail values apply live; the interface indicates how other parameters take effect.

### Capture, VRR and mouse

- Improve WGC capture intervals and waiting, reducing polling for static pictures. Improve capture/output recovery after focus changes, occlusion/minimization and size changes, along with DWM capture-boundary handling.
- Fix presentation stalls caused by some cursor repositioning and cursor-shape updates, and improve restoration of cursor position and confinement when scaling stops.
- On supported **Graphics Capture (WGC) + DLSSNR + Front Edge + VRR output paths without frame generation**, Magpie can resubmit the most recently completed picture at the target cadence after roughly two target frame periods without a new content submission. VRR must be enabled, and tearing support must be available on the active output path.
- Pending new pictures and processing already in progress take priority. These refreshes stop after roughly **250 ms** without a new content submission. They reuse a completed picture without rerunning DLSSNR; new-content FPS still depends on the source application, capture and effect processing.

VRR remains experimental and defaults to Off. Restart scaling after changing capture, pacing or VRR settings. Frame intervals vary with the source application, driver, display and settings. See the packaged `FRAME_SYNC_GUIDE.md` for refresh controls.

## Installation, upgrades and rollback

Use the complete **`Magpie-Experimental-x64.zip`** for a first installation or an upgrade from an older version. Back up settings and fully exit the previous version from the tray, extract the entire ZIP into a new folder, then run its `Magpie.exe`.

For configurations from local Betas, DLSSNR Async Every Frame automatically falls back to Every Frame, and Async Alternating Frames to Alternating Frames. Valid update intervals, model presets and input scales are retained. The separate **Async** pacing option under Frame rate and refresh remains available.

Keep the previous program folder and a pre-upgrade settings backup. To roll back, fully exit the current version, restore the previous settings and run Magpie from the older folder.

## Assets

| File | Purpose and use |
| --- | --- |
| `Magpie-Experimental-x64.zip` | The complete, standalone installation package with matching program, resources and runtime files. It retains the 310.9.16 community compatibility DLSSNR for RTX 20 / 30 / 40 series. |
| `DLSSNR-DLL-Options.zip` | Retains the NVIDIA original for RTX 50 and the community version for RTX 20 / 30 / 40. Exit Magpie and back up the existing DLL before placing the chosen `nvngx_dlssnr.dll` beside `Magpie.exe`. See the included instructions for details. |
| `NGX_OTA_Switch.bat` | Optional NVIDIA NGX OTA update-management tool requiring administrator privileges. Restore default restores the default settings. Normal installation does not require this tool. |

## Acknowledgments

- Thanks to [Len-huan](https://github.com/Len-huan) for proposing DLSSNR weight presets and providing supporting analysis in [issue #73](https://github.com/SAOG0721/Magpie/issues/73).
- Thanks to [yejincheng420](https://github.com/yejincheng420) for sharing the foundational idea behind alternating-frame processing in their [Magpie fork](https://github.com/yejincheng420/Magpie/tree/turing-ampere).
