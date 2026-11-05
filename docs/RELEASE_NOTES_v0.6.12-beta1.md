# Magpie Experimental v0.6.12 Beta 1

本地测试版。

## Magpie 本体

- 调整窗口大小后，临时 DirectComposition 输出会尝试恢复 DXGI；确认替代帧有效后再撤除兜底画面。恢复失败时保留可用输出并记录原因，静态画面也能触发恢复。
- 参数面板恢复单页分组与多列布局，保留原有控件和字号，两轴均不滚动；窄浮层按内容自动扩宽。标签精简，成对复选框按列中线对齐。性能分析窗口记住图表、耗时等分区的展开状态。

- 修复参数弹窗点击空白区域后，再展开下拉框导致整窗关闭的问题；关闭重开时保留选项与编辑状态，避免旧选项回写。

## DLSSNR

- 新增模型预设：自动（默认）、HT、CC Small FP8。设置适用于全部 Pass，修改后需要重建效果链；实际采用的模型由运行库与显卡支持情况决定。
- 新增逐帧（默认）、隔帧、异步低延迟处理模式。后两种仅支持 SDR，过期或失配的增强使用当帧轻量 NR 保底。异步模式同时占用轻量与完整推理资源，适合按实际画质和性能选择。
- 性能高级设置按处理模式、历史帧复用、光流排列。支持历史帧原位复用（默认）和历史帧重投影；后者需要 AMDOF 或 NVOF。历史时效固定为 2 个有效捕获帧。关闭光流后，依赖光流的抗闪烁自动回退到静态累积，重投影改为原位复用。
- 保留中间调明度强度，删除分区色度。中性和肤色保护合并为全域色度保护：0～1、默认 0、步进 0.05；值越高越少改动原图颜色，明暗调整独立保留。
- 修复 AMD 光流置信度被误当成增强强度、导致隔帧和异步画质交替的问题；可信历史保持完整增强，遮挡、越界与过期历史仍被拒绝。
- SDR 始终使用残差重建，隐藏旧输入分辨率调整开关；升级保留原比例，缺省为 100%。

## OKLab Image Adjustment

- 修复效果组参数弹窗的空白下拉项、工具堆叠和参数缺失，以及普通效果器误显示 OKLab 工具的问题。
- 修复小输入画面下运行参数右侧被裁切的问题；参数浮层可使用整个输出窗口，包括画面留黑区域。
- OKLab 效果组弹窗和运行浮层将编辑区域、预设、重置、A/B 与撤销集中在公共第一列，分类工具和参数从第二列向右展开，列间增加淡色分隔；修复运行浮层快速拖动标题栏不移动的问题。
- 运行浮层无专属工具的分类从列顶排列，修复空工具区造成的留白；补全阈值、白点偏移等标签所需字形。
- 修复打开运行参数时的资源字符串缓存崩溃；保留原有分区选择，当前分区的工具与参数同页分列，名称统一为 OKLab Image Adjustment。
- 新增独立效果器，可在效果器列表搜索“OKLab”。保持输入尺寸，分区提供曝光、明暗、白平衡、色度、色相、八色混合、三向调色、选色、曲线及 LUT。
- 提供六组预设、分屏比较、A/B、撤销、色轮和曲线编辑；运行浮窗可拖动吸管到画面选取颜色。
- 支持局部对比度、纹理、彩噪抑制、去色带、辉光、去雾、局部蒙版及自动调整。邻域功能默认关闭，复杂组合增加 GPU 开销。各参数的范围与默认值显示在对应控件中。
- 常规数值可实时调整；导入或清除 LUT 后需重新开始缩放。LUT 资产随配置保存。HDR 链使用线性颜色入口，请结合实际显示效果调整。

## 安装

完全退出旧版，备份设置，将 `Magpie-Experimental-x64.zip` 完整解压到新目录，再运行 `Magpie.exe`。主包包含配套 EXE、资源与运行库；可选 `DLSSNR-DLL-Options.zip` 沿用原版 RTX 50／社区 RTX 20、30、40 两种组件及切换说明。

---

# Magpie Experimental v0.6.12 Beta 1 User Guide

Local test build.

## Magpie application

- After resizing, temporary DirectComposition output retries DXGI recovery and keeps its fallback until a valid replacement frame is confirmed. Failed recovery retains available output and logs the reason; static scenes can also trigger recovery.
- Parameter panels restore a single page with grouped columns, original controls and font sizes, and no horizontal or vertical scrolling. Narrow overlays widen to fit their content. Labels are concise and paired checkboxes align to the column midpoint. The profiler remembers graph, timing and other section expansion preferences.

- Fixes parameter flyouts closing when opening a dropdown after clicking blank space. Reopening retains selections and editor state instead of writing stale selections back.

## DLSSNR

- Model presets: Automatic (default), HT and CC Small FP8. The selection applies to every pass and requires rebuilding the effect chain. The runtime library and GPU determine the actual model used.
- Processing modes: Every Frame (default), Alternating Frames and Asynchronous Low Latency. The latter two support SDR only and use current-frame lightweight NR when enhancement expires or no longer matches. Asynchronous processing uses both lightweight and full inference resources; choose based on observed quality and performance.
- Performance controls are ordered as processing mode, history reuse and optical flow. History Reprojection requires AMDOF or NVOF; History in Place is the default. History age is fixed at two accepted captures. Disabling optical flow changes dependent anti-flicker modes to Static Accumulation and reprojection to in-place reuse.
- Retains midtone lightness and removes zonal chroma controls. Global Chroma Protection replaces neutral and skin protection: range 0–1, default 0, step 0.05. Higher values reduce changes to original color while retaining separate lightness adjustments.
- Fixes alternating NR quality caused by treating AMD optical-flow confidence as enhancement strength. Trusted history retains full enhancement; occluded, out-of-bounds and expired history remain rejected.
- SDR always uses residual reconstruction. The old input-resolution toggle is hidden; upgrades retain the existing ratio, with 100% as the default.

## OKLab Image Adjustment

- Runtime sections without tools start at the column top. Threshold and white-point offset labels include their required glyphs.

- Fixes empty dropdowns, overlapping tool sections and missing parameters in the effect-group popup, and OKLab tools appearing in unrelated effects.
- Fixes clipped runtime parameter columns with small input images. The parameter overlay can use the entire output window, including letterboxing.
- Both OKLab panels keep section selection, presets, reset, A/B and undo in a common first column. Section tools and parameters continue from the second column, with subtle separators. Fast title-bar drags now move the runtime panel reliably.
- Fixes the resource-string cache crash when opening runtime parameters. The original section selector is retained, with the selected section's tools and parameters in columns on the same page. The effect consistently displays OKLab Image Adjustment.
- Search for “OKLab” in the effect list. This standalone effect preserves input dimensions and provides exposure, tonal controls, white balance, chroma, hue, an eight-color mixer, three-way grading, selections, curves and LUTs.
- Includes six presets, split comparison, A/B, undo, wheel and curve editors. Drag the eyedropper from the running overlay onto the image to sample a color.
- Includes local contrast, texture, chroma-noise reduction, debanding, bloom, dehaze, local masks and automatic adjustment. Neighborhood features are disabled by default; complex combinations increase GPU cost. Each control shows its range and default.
- Ordinary numeric changes apply live. Restart scaling after importing or clearing a LUT; LUT assets are saved with the configuration. HDR chains use a linear color input; adjust against the actual display output.

## Installation

Completely exit the previous version, back up settings, extract all of `Magpie-Experimental-x64.zip` into a new folder, then run `Magpie.exe`. The package contains matching executable, resources and runtime files. The optional `DLSSNR-DLL-Options.zip` retains the original RTX 50 and community RTX 20/30/40 choices and switching instructions.
