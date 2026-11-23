Magpie ships with a wide collection of built-in effects that can be chained together in effect groups. Most effects expose customizable parameters. Custom HLSL effects are stored in the `effects` folder—see the [MagpieFX](https://github.com/Blinue/Magpie/wiki/MagpieFX%20(EN)) guide if you want to write your own.

<a id="current-branch-setup"></a>

## Magpie Experimental: Chains, HDR & Native Effects / 当前分支组合与内置效果说明

🌍 **English** | [简体中文](#当前分支组合与内置效果说明)

This section covers the experimental native effects, frame-generation chains, color-space rules, and shared settings on the `experimental` branch. Actual effect availability depends on your release package, GPU, driver, and runtimes. Individual effect summaries are shown directly in Magpie's effect picker; this guide explains how they work together.

---

### 1. Shared Frame-Generation Rules

- **One generator per group**: Pick a single frame-generation backend per effect group. Start at **2×** and only increase the multiplier if you have enough GPU headroom.
- **Execution order**: Frame generation normally runs **after** all image-processing effects—dragging the FG entry up or down in the list does not change this stage. *(The only exception is VFX HDR chains with `TrueHDR`, covered below.)*
- **Pacing & idle behavior**: Base frame rate, duplicate-frame detection, and idle static-picture redraw are configured under **Frame rate & refresh** (see [FRAME_SYNC_GUIDE.md](FRAME_SYNC_GUIDE.md)). Generated frames improve visual smoothness, while the game itself still controls its real simulation/input rate.
- **Visual limits**: Fast motion, occlusions, and UI/text baked into the captured window can cause interpolation artifacts.

### 2. NVIDIA VFX (VSR, Upscale, VFG) & TrueHDR Chains

- **RTX Video (VSR / Denoise / Deblur)**:
  - **VSR** upscales video using **Standard**, **High bitrate**, or **Streaming** categories (*Streaming requires Ampere or newer and offers Medium / Ultra*). **Denoise** and **Deblur** preserve the input resolution.
  - Each category saves its own quality and scale settings. Quality switches **live**; changing category requires **restarting scaling**.
- **Nvidia VFX Upscale**:
  - Supports fixed **4/3×, 1.5×, 2×, 3×, and 4×** ratios (equal on both axes, integer output dimensions).
  - **Auto fit** picks the lowest legal ratio that covers your target size, then proportionally resizes to fit (targets above 4× add a second resize pass). Sharpness applies **live**; changing ratio or target size requires **restarting scaling**.
- **Nvidia VFX VFG (Video Frame Generation)**:
  - Takes **SDR** input at **2×–8×** (default: **2×**, **Balanced** model, automatic shot detection **On**).
  - Requires Windows x64, an **Ada or Blackwell** NVIDIA rendering GPU, a compatible driver, and the VFX runtime. Model switches **live**; multiplier and shot detection require **restarting scaling**.
- **Combining VFG with HDR (`TrueHDR`)**:
  - For **HDR sources**, convert to SDR first using **HDR → SDR** before VFG.
  - For **HDR output**, place **Nvidia VFX TrueHDR** after your SDR processing effects—it will process both real and generated frames. TrueHDR requires **Windows HDR** enabled on the output monitor; set peak brightness to match your display (all five parameters apply **live**).
  - The **first `TrueHDR`** in a group marks the start of the per-output-frame post-processing stage. Only the entries listed below are allowed after it; place all other image effects before the first `TrueHDR`. The `VFG` entry acts as the group's single generator marker and does not move this boundary.

<!-- vfx-post-effects:start -->
| Post-stage entry / 后处理段条目 | Effect ID / 效果 ID |
| --- | --- |
| Nvidia VFX TrueHDR | `RTXVideo\RTXVideo_HDR` |
| Nvidia VFX VFG (generator marker / 补帧标记) | `NvidiaVFX\VFG` |
| Bicubic | `Bicubic` |
| Bilinear | `Bilinear` |
| CAS | `CAS\CAS` |
| FSR RCAS | `FSR\FSR_RCAS` |
<!-- vfx-post-effects:end -->

### 3. FSR3 Frame Generation Chains

- **FSR3 FG** runs at a fixed **2×** multiplier and requires an enabled x64 build, matching runtimes, and a GPU that passes capability checks.
- **SDR only**: Input, output, and every image effect in the group must stay in **SDR**.
- **Optical flow**: Start with external **AMDOF (Quality)**, or use **NVOF** on supported NVIDIA hardware. Changing optical flow requires **restarting scaling**.
- **Compatible effects**: Only effects on the [complete compatibility list](experimental/effects/FSR3FG.md#compatible-effect-list) are admitted. Unlisted effects, CRT shaders, geometric distortion, and cropping outside the output window are rejected (scaling, letterboxing, and aspect-ratio fitting within the output window are supported). FSR upscaling is optional.

### 4. HDR Processing & Compatibility Conversion

- **Automatic SDR fallback**: Traditional SDR shaders (anime models, pixel art, conventional AA, CRT, most sharpeners, as well as VSR, Upscale, and video denoising) automatically run through an SDR compatibility conversion when placed in an HDR chain, then convert back to HDR. This can affect highlights, gradients, and color precision.
- **Notable exceptions**: `Bicubic` runs in floating-point HDR; native upscalers use their backend's HDR path; `xBR Hybrid` falls back to nearest-neighbor 2× in HDR compatibility mode; `xBR NoBlend` preserves the SDR palette it receives. `FSR3 FG` requires a full SDR chain.
- **Explicit `HDR → SDR` / `SDR → HDR` pairs**: You can wrap an SDR section manually by placing `HDR → SDR` (compatibility-pair mode) before it and `SDR → HDR` at the end to restore upstream parameters. *(Choose SDR display mode instead if you want direct SDR viewing.)*
- **Native `SDR → HDR` vs. `TrueHDR`**: Native white-point mapping simply maps SDR content onto an HDR display at your chosen paper-white brightness, whereas `TrueHDR` uses AI to reshape highlights and color.

### 5. OKLab Image Adjustment

Search for **OKLab** in the effect list to add it. It preserves resolution and provides full real-time color grading:

- **Color & tone**: Exposure, tonal controls, white balance, chroma, hue, an 8-color mixer, 3-way color wheels, color selections, curves, and `.cube` LUT import/export (LUTs are saved with your profile).
- **Detail & finishing** *(disabled by default to save GPU cost)*: Local contrast, texture, chroma-noise reduction, debanding, bloom, dehaze, geometric masks, and lockable Auto Exposure / Auto White Balance.
- **Live tools**: Six built-in presets, split comparison, A/B toggle, undo, and a draggable eyedropper in the running overlay to sample colors directly from the screen.
- **Applying changes**: Numeric adjustments apply **live**; **importing or clearing a LUT requires restarting scaling**.

### 6. DLSSNR, Temporal Upscalers, DLSS/XeSS FG & Frame-Rate Limiter

- **DLSSNR**:
  - **Passes**: Start with 1 pass (up to 3 passes, each with independent style and intensity).
  - **Model presets**: **Automatic (DLL Default)**, **HT**, and **CC Small FP8** (shared across passes; actual support depends on GPU and DLL).
  - **Resolution control (SDR)**: Choose **Adaptive** *(default for new effects, adjusts in 5% steps to meet your content-rate target)* or **Manual** (`25%–100%`, step `1%`). SDR always runs residual reconstruction, so residual/detail controls work even at 100% scale, alongside **Midtone Strength** (`0–2`, default `1`) and **global Chroma Protection** (`0–1`, default `0`).
  - **Processing modes**: **Every Frame** *(default)* or **Alternating Frames** *(SDR only; updates full NR every **2 / 3 / 4** new captures, default `2`, reusing historical enhancement in place or aligned via **AMDOF / NVOF**)*.
  - **Live vs. Restart**: Style, intensity, and SDR residual/detail sliders apply **live**; model, processing mode, pass count, anti-flicker, input scale, reuse method, and optical flow require **restarting scaling**.
  - *Tip*: If DLSSNR fails to initialize, single-pass may pass through the original image while multi-pass refuses to start—check the log for initialization details.
- **Temporal Super Resolution (`DLSS SR`, `FSR 2/3/4`, `XeSS SR`)**:
  - `DLSS SR` currently uses **Balanced/Preset J**; other backends follow their own scale and runtime requirements. Changing optical-flow method or quality requires **restarting scaling**. Inspect fast motion, occlusions, and HUD edges when tuning.
- **DLSS FG & XeSS FG**:
  - `XeSS FG` defaults to **2×, AMD optical flow, Quality** (3×/4× on non-Intel GPUs uses the bundled runtime's experimental compatibility path).
  - `DLSS FG` uses the bundled compatibility runtime with multipliers determined by the rendering GPU (restart Magpie after switching the rendering GPU).
  - Changing multiplier or optical flow on either generator requires **restarting scaling**.
- **Frame-rate limiter effect**: Controls the group's base input rate regardless of its position in the list. If multiple caps are active, the lowest target wins. When frame sync is enabled, **Frame rate & refresh** manages pacing.

---

<a id="当前分支组合与内置效果说明"></a>

## 当前分支组合与内置效果说明（简体中文）

[English](#current-branch-setup) | **简体中文**

本节介绍 `experimental` 分支的实验性原生效果、补帧组合、HDR 颜色转换规则及共用设置。具体可用效果以对应发布包、显卡、驱动和运行库为准；单项效果简介可在软件内的效果选择器直接查看，本指南重点说明**怎么搭配使用**。

---

### 1. 补帧共用规则

- **每组只选一种补帧**：从 **2×** 起步，确认显卡有余量后再提高倍率。
- **执行顺序**：补帧默认在所有图像处理效果**之后**执行，在列表里上下拖动补帧条目不会改变这个阶段。*（唯一例外是下文 VFX 的 `TrueHDR` 后处理段。）*
- **节奏与空闲**：基础帧率、重复帧检测与静态画面重绘统一在**帧率与刷新**里设置（详见 [FRAME_SYNC_GUIDE.md](FRAME_SYNC_GUIDE.md)）。补帧提高视觉流畅度，但游戏本身的逻辑与输入响应频率仍由游戏决定。
- **画面限制**：快速运动、物体遮挡以及捕获画面中的游戏文字／HUD 可能出现插值伪影。

### 2. NVIDIA VFX（VSR、Upscale、VFG）与 TrueHDR 组合

- **RTX Video（VSR / 降噪 / 去模糊）**：
  - **VSR** 提供**常规、高码率、流媒体**三种放大分类（*流媒体需 Ampere 及更新架构，提供中／极高档*）；**降噪**与**去模糊**保持原分辨率。
  - 各分类独立保存质量与缩放设置。质量**实时生效**，切换分类需**重新开始缩放**。
- **Nvidia VFX Upscale**：
  - 支持 **4/3×、1.5×、2×、3×、4×** 固定倍率（宽高同比例且输出为整数尺寸）。
  - **自动适配**会自动选择覆盖目标尺寸的最低合法倍率，再等比缩放到目标尺寸（超过 4× 会追加一次缩放）。锐化**实时生效**，更改倍率或目标尺寸需**重新开始缩放**。
- **Nvidia VFX VFG（视频补帧）**：
  - 接收 **SDR** 输入，支持 **2×–8×**（默认 **2×**、**均衡**模型、自动场切检测**开启**）。
  - 需要 Windows x64、**Ada 或 Blackwell** 架构 NVIDIA 渲染显卡、匹配驱动及 VFX 运行库。模型**实时切换**，倍率和场切检测修改需**重新开始缩放**。
- **VFG 搭配 HDR（`TrueHDR`）**：
  - **HDR 源**进 VFG 前，先用 **HDR → SDR** 转为 SDR。
  - 若需要 **HDR 输出**，把 **Nvidia VFX TrueHDR** 放在 SDR 图像处理之后，它会同时处理真实帧与生成帧。TrueHDR 需要输出显示器开启 **Windows HDR**，峰值亮度按显示器能力设置（五项参数均**实时生效**）。
  - 链中的**首个 `TrueHDR`** 开启逐帧后处理段，其后仅允许上方英文表格中列出的 6 种条目，其他图像效果请全部放在首个 `TrueHDR` 之前；`VFG` 条目仅作组内唯一补帧标记，拖动它不会移动分界。

### 3. FSR3 补帧组合

- **FSR3 FG** 固定为 **2×**，需要启用该功能的 x64 构建、匹配运行库及通过能力检查的渲染显卡。
- **全链 SDR**：输入、输出与组内所有图像效果必须保持 **SDR**。
- **光流设置**：建议从外部 **AMDOF（质量）** 起步，支持硬件的 NVIDIA 显卡也可选 **NVOF**；更改光流需**重新开始缩放**。
- **兼容清单**：仅接受[完整兼容清单](experimental/effects/FSR3FG.md#compatible-effect-list)中的效果，清单外效果、CRT、几何变形与超出输出窗口的裁剪会被拒绝（支持输出区域内的缩放、留黑边与宽高比适配）。不强制搭配 FSR 超分。

### 4. HDR 处理与兼容转换

- **自动 SDR 兼容转换**：传统 SDR 效果（动画模型、像素画、常规抗锯齿、CRT、多数锐化，以及 VSR、Upscale 和视频降噪）放在 HDR 链中时，会自动转到 SDR 处理再回到 HDR，可能影响高光、渐变与颜色精度。
- **特殊说明**：`Bicubic` 原生支持 HDR 浮点处理；原生超分走各自后端的 HDR 路径；`xBR Hybrid` 在 HDR 兼容模式下回退为最近邻 2×；`xBR NoBlend` 保留收到的 SDR 调色板；`FSR3 FG` 仅支持全 SDR 链。
- **显式 `HDR → SDR` / `SDR → HDR` 配对**：可用兼容配对模式手动包裹一段 SDR 处理（结尾放 `SDR → HDR` 恢复上游参数；如果直接看 SDR，请改选 SDR 显示模式）。
- **原生 `SDR → HDR` vs `TrueHDR`**：原生白点映射只是按指定白点亮度在 HDR 屏上呈现 SDR 内容；`TrueHDR` 则用 AI 重塑亮度与色彩观感。

### 5. OKLab Image Adjustment 调色

在效果器列表搜索 **OKLab** 即可添加。它保持原分辨率，提供完整的实时调色能力：

- **色彩与明暗**：曝光、明暗分区、白平衡、色度、色相、八色混合、三向色轮、选色、曲线与 `.cube` LUT 导入／导出（LUT 随配置保存）。
- **细节与质感** *（默认关闭以节省 GPU 开销）*：局部对比度、纹理、彩噪抑制、去色带、辉光、去雾、几何蒙版，以及可锁定的自动曝光／自动白平衡。
- **实时工具**：内置六组预设、分屏对比、A/B 切换、撤销，运行浮窗内可直接拖动吸管到画面上取色。
- **生效方式**：常规数值调节**实时生效**；**导入或清除 LUT 后需重新开始缩放**。

### 6. DLSSNR、时序超分、DLSS/XeSS 补帧与限帧效果

- **DLSSNR**：
  - **处理次数**：默认 1 次，最多 3 次，每次可独立设置风格与强度。
  - **模型预设**：**自动（DLL 默认）**、**HT**、**CC Small FP8**（所有处理层共用，实际支持取决于显卡与运行库）。
  - **输入分辨率（SDR）**：支持**自适应** *（新建效果默认，按内容帧率目标以 5% 步进自动调档）* 与**手动**（`25%–100%`，步进 `1%`）。SDR 始终启用残差重建（100% 比例也能调残差与细节），并新增**中间调强度**（`0–2`，默认 `1`）与**全域色度保护**（`0–1`，默认 `0`）。
  - **处理模式**：**逐帧** *（默认）* 或 **隔帧** *（仅 SDR，每 **2 / 3 / 4** 个新捕获帧更新完整 NR，默认 `2`，支持原位复用或通过 **AMDOF / NVOF** 对齐复用历史增强）*。
  - **生效方式**：风格、强度和 SDR 残差／细节数值**实时生效**；模型、处理模式、处理次数、抗闪烁、输入尺寸、复用方式与光流等结构设置更改后需**重新开始缩放**。
  - *排查提示*：若初始化失败，单次路径可能直接输出原图，多次路径会拒绝启动，请在日志中查看初始化结果。
- **时序超分（`DLSS SR`、`FSR 2/3/4`、`XeSS SR`）**：
  - `DLSS SR` 当前使用 **Balanced/Preset J**，其余按各自倍率与运行库要求使用。更改光流方式或质量需**重新开始缩放**；调参时重点观察快速运动、遮挡与 UI 边缘。
- **DLSS FG 与 XeSS FG**：
  - `XeSS FG` 默认 **2×、AMD 光流、质量**（非 Intel 显卡的 3×／4× 使用随附运行库的实验兼容路径）。
  - `DLSS FG` 使用随附兼容运行库，可用倍率由渲染显卡决定（切换渲染显卡后需重启 Magpie）。
  - 两者的倍率与光流修改均需**重新开始缩放**。
- **帧率限制效果**：无论放在效果组哪个位置，都控制整组的基础输入帧率；多个上限同时存在时取较低值。启用帧同步时由**帧率与刷新**统一管理节奏。

## Introduction to shipped effects

* ACNet: Port of [ACNetGLSL](https://github.com/TianZerL/ACNetGLSL). Suitable for anime-style images. Strong denoise effects.
  * Output size: twice that of the input

* AdaptiveSharpen: Self-adaptive sharpening algorithm. This algorithm focuses on sharpening the blurry edges in the images. Hence it has less image noise, ringing artifacts, and stripes compared to the common sharpening algorithms.
  * Output size: the same as the input
  * Parameter
    * Sharpness

* Anime4K_3D_AA_Upscale_US and Anime4K_3D_Upscale_US: 3D game scaling algorithms provided by Anime4K. The AA variant supports anti-aliasing.
  * Output size: twice that of the input

* Anime4K_Denoise_Bilateral_Mean, Anime4K_Denoise_Bilateral_Median, and Anime4K_Denoise_Bilateral_Mode: Denoise algorithms provided by Anime4K. Uses mean, median, and mode, respectively.
  * Output size: the same as the input
  * Parameter:
    * Strength: Denoise magnitude

* Anime4K_Restore family: Algorithms to restore the lines in animations. In increasing order of demand for computing power. The Soft variants are more conservative in sharpening.
  * Output size: the same as the input

* Anime4K_Thin_HQ: Algorithm to clarify lines in animations provided by Anime4K.
  * Output size: the same as the input
  * Parameters
    * Strength: The strength in each iteration.
    * Iterations: The number of iterations. Decreasing strength and increasing iterations improves the quality of the images, but will lower the processing speed.

* Anime4K_Upscale family: Anime-style scaling algorithms provided by Anime4K. The denoise variant includes denoise functionality. The GAN variant, which keeps more details, is still under experiment.
  * Output size: twice that of the input

* Bicubic: Interpolation algorithms. The lite variant is fast, but at the cost of quality degradation, Suitable for users will weak graphics cards.
  * Output size: determined by scale configuration
  * Parameters
    * B: Too large values will result in blurry images.
    * C: Too large values will result in ring artifacts.
  * Note: Different combinations of parameters will lead to different variants of the algorithm. For example: Mitchell(B=C≈0.333333), Catmull-Rom(B=0 C=0.5), bicubic Photoshop(B=0 C=0.75), Spline(B=1 C=0)

* Bilinear：Bilinear interpolation
  * Output size: determined by scale configuration

* CAS: Port of [FidelityFX-CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS). Lightweight sharpening effects.
  * Output size: the same as the input
  * Parameter
    * Sharpness

* CAS_Scaling：Port [FidelityFX-CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS). Supports scaling
  * Output size: the same as the input
  * Parameter
    * Sharpness

* CRT_Easymode: Easy-to-configure lightweight CRT shader.
  * Output size: determined by scale configuration
  * Parameters
    * Sharpness Horizontal
    * Sharpness Vertical
    * Mask Strength
    * Mask Dot Width
    * Mask Dot Height
    * Mask Stagger
    * Mask Size
    * Scanline Strength
    * Scanline Beam Width Min
    * Scanline Beam Width Max
    * Scanline Brightness Min
    * Scanline Brightness Max
    * Scanline Cutoff
    * Gamma Input
    * Gamma Output
    * Brightness Boost
    * Dilation

* CRT_Geom: One of the most popular CRT shaders, designed to emulate the look of Arcade-machine. This implementation does not support interlacing simulation. Check [Emulation General Wiki](https://emulation.gametechwiki.com/index.php/CRT_Geom).
  * Output size: determined by scale configuration
  * Parameters
    * Target Gamma
    * Monitor Gamma
    * Distance
    * Curvature: Whether to emulate the screen curvature
    * Curvature Radius
    * Corner Size
    * Corner Smoothness
    * Horizontal Tilt
    * Vertical Tilt
    * Horizontal Overscan
    * Vertical Overscan
    * Dot Mask
    * Sharpness: The larger the value is, the clear the image becomes
    * Scanline Weight
    * Luminance Boost
  
* CRT_Hyllian: Provides sharp and clear outputs with slight rims. Similar to Sony BVM displays.
  * Output size: determined by scale configuration
  * Parameters
    * Phosphor
    * Vertical Scanlines
    * Input Gamma
    * Output Gamma
    * Sharpness
    * Color Boost
    * Red Boost
    * Green Boost
    * Blue Boost
    * Scanline Strength
    * Min Beam Width
    * Max Beam Width
    * Anti-Ringing

* CRT_Lottes: Provides multiple masks emulating Bloom and Halation effects. Similar to CGA arcade displays.
  * Output size: determined by scale configuration
  * Parameters
    * Scanline Hardness
    * Pixel Hardness
    * Horizontal Display Warp
    * Vertical Display Warp
    * Mask Dark
    * Mask Light
    * Shadow Mask
    * Brightness Boost
    * Bloom-X Soft
    * Bloom-Y Soft
    * Bloom Amount
    * Filter Kernel Shape

* CuNNy family：Suitable for visual novel-style images. The DS variants offer a subtle denoise effect. Provided by [CuNNy](https://github.com/funnyplanter/CuNNy)
  * Output size: twice that of the input

* CuNNy2 family: An enhanced version of CuNNy
  * Output size: twice that of the input

* Deband
  * Output size: the same as the input
  * Parameters
    * Threshold：The threshold of difference below which a pixel is considered to be part of a gradient.
    * Range：The range (in source pixels) at which to sample for neighbours.
    * Iterations：The number of debanding iterations to perform. Each iteration samples from random positions, so increasing the number of iterations is likely to increase the debanding quality. Conversely, it slows the shader down.
    * Grain：Add some extra noise to the image. This significantly helps cover up remaining banding and blocking artifacts, at comparatively little visual quality.

* FineSharp: High-quality sharpening. The earliest version was the AviSynth script.
  * Output size: the same as the input
  * Parameters
    * sstr: Sharpening strength. If you change this parameter, you will have to also change cstr. Check the note below.
    * cstr: Balancing strength.
    * xstr: The sharpening strength in the final step of the XSharpen style.
    * xrep: To fix the artifacts in the last step of sharpening.
  * Note: mapping between sstr and cstr (sstr->cstr): 0->0, 0.5->0.1, 1.0->0.6, 2.0->0.9, 2.5->1.00, 3.0->1.09, 3.5->1.15, 4.0->1.19, 8.0->1.249, 255.0->1.5

* FSR_EASU: Port of the scaling step in [FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR). The DX10 variant works on graphics cards supporting DirectX feature level, but will be slower.
  * Output size: determined by scale configuration

* FSR_RCAS: Port of the sharpening step in [FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR).
  * Output size: the same as the input
  * Parameter
    * sharpness

* FSRCNNX: Port of FSRCNNX_x2_8-0-4-1
  * Output size: twice that of the input

* FSRCNNX_LineArt: Port of FSRCNNX_x2_8-0-4-1_LineArt
  * Output size: twice that of the input

* FXAA_Medium, FXAA_High, FXAA_Ultra: Fast anti-aliasing by approximation. In increasing order of demand to computing power.
  * Output size: the same as the input

* GTU_v050: Aims to emulate the blurry and mixed effects of CRT displays rather than masks or curvature. Supports scanning lines.
  * Output size: determined by scale configuration
  * Parameters
    * Composite Connection
    * No Scanlines
    * Signal Resolution Y
    * Signal Resolution I
    * Signal Resolution Q
    * TV Vertical Resolution
    * Black Level
    * Contrast

* ImageAdjustment：Image parameters adjustment
  * Output size: the same as the input
  * Parameters
    * Target Gamma
    * Monitor Gamma
    * Saturation
    * Luminance
    * Contrast
    * Brightness Boost
    * Black Level
    * Red Channel
    * Green Channel
    * Blue Channel

* Jinc: Scaling with the Jinc algorithm
  * Output size: determined by scale configuration
  * Parameters
    * Window Sinc Param: The smaller the value is the sharper the images become. Must be greater than 0. Default value: 0.5
    * Sinc Param: The larger the value is the sharper the images become. Must be greater than 0. Default value: 0.825
    * Anti-ringing Strength: The greater the value is the better the effect becomes, but the images will be more blurry.

* k7_modernAnime: anime-targeted super-resolution algorithm
  * Output size: twice that of the input

* Lanczos: Scaling with the Lanczos algorithm.
  * Output size: determined by scale configuration
  * Parameters
    * Anti-ringing Strength: The greater the value is the better the effect becomes, but the images will be more blurry.

* LCAS：Lightweight 3D image scaling algorithm
  * Output size: determined by scale configuration
  * Parameter
    * Sharpness

* LumaSharpen: A popular sharpening effect in reshade.
  * Output size: the same as the input
  * Parameters
    * Sharpening Strength
    * Sharpening Limit: Anti-ringing strength.
    * Sample Pattern: Filter type.
    * Offset Bias: Filter bias.

* MMPX：Pixel art scaling algorithm. It can add details while retaining the artistic style.
  * Output size: twice that of the input

* Nearest: Nearest neighbor interpolation
  * Output size: determined by scale configuration

* NIS: Port of [NVIDIA Image Scaling](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling).
  * Output size: determined by scale configuration
  * Parameters
    * Sharpness
  * Note: Only supports upscaling.

* NNEDI3 family：These shaders originally designed for deinterlacing and are also high-quality interpolation algorithms. NNEDI3_nns64_win8x6 produces higher quality results, but slower.
  * Output size: twice that of the input

* NVSharpen: Port of NVSharpen that was published along with NIS.
  * Output size: the same as the input
  * Parameter
    * Sharpness

* Pixellate: Scale with the Pixellate algorithm. Suitable for upscaling pixel arts.
  * Output size: determined by scale configuration

* RAVU family: Ported from https://github.com/bjin/mpv-prescalers
  * Output size: twice that of the input

* RAVU_Zoom family: Ported from https://github.com/bjin/mpv-prescalers
  * Output size: determined by scale configuration
  * Note: Only supports upscaling.

* SharpBilinear: Scale with the Sharp-Bilinear algorithm. Suitable for upscaling pixel arts.
  * Output size: determined by scale configuration

* SGSR: Port of [Snapdragon Game Super Resolution v1](https://github.com/SnapdragonStudios/snapdragon-gsr/tree/main/sgsr/v1).
  * Output size: determined by scale configuration
  * Parameter
    * Edge Sharpness: Edge sharpening intensity (The larger the value, the sharper the image.)
    * Edge Threshold: Edge detection threshold

* SMAA_Low, SMAA_Medium, SMAA_High, and SMAA_Ultra: SMAA anti-aliasing. In increasing order of demand for computing power.
  * Output size: the same as the input

* SSimDownscaler: Downscaling algorithm based on perceptron. Sharper than Catrom.
  * Output size: determined by scale configuration
  * Parameter
    * Oversharp: The larger the value, the sharper the image.

* xBR 2x, xBR 3x, and xBR 4x: Fixed-integer xBR LV2 scaling for pixel art. xBR is a separate rule set from xBRZ and may produce different contours.
  * Output size: fixed at 2x, 3x, or 4x, depending on the variant.
  * Parameters: Y Weight, Eq Threshold, LV2 Coefficient, and Preserve Small Details.

* xBR NoBlend 3x: Fixed 3x xBR scaling that selects colors from the sampled neighborhood instead of interpolating new RGB values.
  * Output size: fixed at 3x.
  * Parameter: LV2 Coefficient.

* xBR Hybrid 2x: Fixed 2x combination of xBR and Reverse-AA for low-resolution SDR material that already contains antialiasing or smooth gradients.
  * Output size: fixed at 2x.
  * Note: HDR compatibility mode uses a point-sampled 2x fallback until the gamma-domain path is validated for HDR.

* xBRZ_2x, xBRZ_3x, xBRZ_4x, xBRZ_5x, and xBRZ_6x: Scale with the xBRZ algorithm. Suitable for upscaling pixel arts.
  * Output size: determined by the variant.

* xBRZ_Freescale and xBRZ_Freescale_Multipass: xBRZ algorithm supporting any scaling factor.
  * Output size: determined by scale configuration
