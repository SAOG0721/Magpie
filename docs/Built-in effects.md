Magpie ships with a handful of effects that can be used in combinations. Most of the effects have parameters that can be customized. All effects are stored in the `effects` folder. You can easily add effects if you are familiar with HLSL. Check the [MagpieFX](https://github.com/Blinue/Magpie/wiki/MagpieFX%20(EN)) page.

<a id="current-branch-setup"></a>

## 当前分支的组合与 HDR 使用说明 / Current-branch setup

本节对应 `feature/effect-description-standards` 的源码说明。各发布包的可用效果以该版本说明为准。选择器介绍单项用途，下面集中说明组合、颜色转换和共用设置。

This section describes the source on `feature/effect-description-standards`. Available effects depend on the release package. The picker explains individual effects; this section covers chains, color conversion, and shared settings.

### 补帧的共用设置 / Shared frame-generation settings

每个效果组选择一种补帧方案，从 2× 开始，再按处理余量增加倍率。“帧率与刷新”设置基础帧率、重复检测和空闲持续重绘。补帧增加显示用的中间画面，游戏自身的更新频率仍由游戏决定。实际输出和延迟取决于捕获、处理能力与显示节奏。

补帧通常在图像处理后执行，拖动补帧条目不改变这个阶段；VFX 的 HDR 组合按下节规则处理。没有新画面时，空闲重绘显示缓存结果；恢复新内容后再继续补帧。快速运动、遮挡及捕获画面中的游戏文字可能出现插值伪影。

Choose one frame generator per group, starting at 2x and increasing the multiplier according to processing headroom. Frame rate and refresh controls the base rate, duplicate detection, and continuous idle redraw. Generated images add display frames; the game controls its own update rate. Actual output and latency depend on capture, processing, and display pacing.

Frame generation normally runs after image processing; moving its entry does not change this stage. VFX HDR chains follow the rules below. Idle redraw reuses cached output while no new image arrives, and generation resumes with new content. Fast motion, occlusion, and game text baked into captured images can produce interpolation artifacts.

### VFX 视频增强与 HDR 组合 / VFX video and HDR chains

VSR 按常规、高码率或流媒体分类放大；降噪、去模糊分类保持输入尺寸。各分类分别保存质量和缩放设置。流媒体需 Ampere 及更新架构，提供中／极高档。分类修改需重新启动缩放，质量可实时切换。

Upscale 可选 4/3×、1.5×、2×、3×、4× 固定倍率，宽高比例相同且输出须为整数尺寸。自动适配选择覆盖目标的最低合法倍率，然后等比整理到目标尺寸；超过 4× 时追加缩放。设备尺寸上限仍适用。运行面板显示原生处理与适配后的尺寸。锐化实时生效，倍率或目标尺寸修改需重新启动缩放。

VFG 接收 SDR，支持 2×～8×，默认 2×、均衡模型、自动场切检测开启。需要 Windows x64、Ada／Blackwell 架构的 NVIDIA 渲染 GPU、适配驱动及 VFX 运行库。模型实时切换，倍率和场切检测修改需重新启动缩放。

HDR 源使用 VFG 时，先将输入通过 HDR → SDR 转换为 SDR。若需 HDR 输出，可在 SDR 图像处理之后加入 TrueHDR；它会处理真实帧与生成帧。TrueHDR 需要输出屏幕开启 Windows HDR，峰值亮度按显示器能力设置，五项参数均可实时调整。

链中的**首个 TrueHDR** 开始逐帧后处理段；其前面的图像效果先处理，再补帧。没有 TrueHDR 时，图像效果都在补帧前执行。后处理段允许的条目如下；其他图像效果放在首个 TrueHDR 之前。VFG 行用于标识组内唯一补帧器，拖动它不会移动处理分界。

<!-- vfx-post-effects:start -->
| 后处理段条目 / Post-stage entry | 效果 ID / Effect ID |
| --- | --- |
| Nvidia VFX TrueHDR | `RTXVideo\RTXVideo_HDR` |
| Nvidia VFX VFG（补帧标记 / generator marker） | `NvidiaVFX\VFG` |
| Bicubic | `Bicubic` |
| Bilinear | `Bilinear` |
| CAS | `CAS\CAS` |
| FSR RCAS | `FSR\FSR_RCAS` |
<!-- vfx-post-effects:end -->

VSR upscales with Standard, High bitrate, or Streaming categories; Denoise and Deblur keep the input size. Each category saves its own quality and scale. Streaming requires Ampere or newer and offers Medium/Ultra. Category changes require restarting scaling; quality switches live.

Upscale offers fixed 4/3x, 1.5x, 2x, 3x, and 4x ratios, equal on both axes with integer output dimensions. Auto fit selects the lowest legal ratio covering the target, then resizes proportionally to fit; targets above 4x add further resizing. Device size limits apply. The runtime panel shows native and fitted sizes. Sharpness applies live; ratio or target-size changes require restarting scaling.

VFG takes SDR at 2x–8x, defaulting to 2x, Balanced, and automatic shot detection. Requires Windows x64, an Ada/Blackwell NVIDIA rendering GPU, a suitable driver, and the VFX runtime. The model switches live; multiplier and shot-detection changes require restarting scaling.

For HDR sources, convert to SDR with HDR → SDR before VFG input. For HDR output, place TrueHDR after the SDR processing section; it processes both real and generated frames. TrueHDR requires Windows HDR on the output display. Set its peak brightness for the display; all five parameters apply live.

The **first TrueHDR** starts per-output-frame post-processing. Earlier image effects run before generation; without TrueHDR, all image effects run before generation. The table lists all admitted post-stage entries. Place other image effects before the first TrueHDR. The VFG row is the group's single generator marker; moving it does not move the stage boundary.

### FSR3 FG 组合 / FSR3 FG chains

FSR3 FG 固定 2×，需要支持该功能的 x64 程序、匹配运行库与通过能力检查的渲染 GPU。使用 SDR 输入及输出；效果组中的图像处理也须保持 SDR。外部光流从 AMDOF 质量档开始，也可使用硬件支持的 NVOF；修改光流需重新启动缩放。

支持的图像组合以[完整清单](experimental/effects/FSR3FG.md#compatible-effect-list)为准。清单外效果、CRT、几何变形与超出输出窗口的裁剪会被拒绝；支持输出区域内的缩放、留黑边与宽高比调整。它可独立于 FSR 超分使用，捕获方式按程序提供的选项选择。

FSR3 FG is fixed at 2x and requires an enabled x64 build, matching runtimes, and a rendering GPU that passes capability checks. Input, output, and image processing must remain SDR. Start with external AMDOF at Quality, or use NVOF on supported hardware; optical-flow changes require restarting scaling.

Use the [complete compatibility list](experimental/effects/FSR3FG.md#compatible-effect-list). Unlisted effects, CRT, geometry distortion, and cropping outside the output window are rejected. Scaling, letterboxing, and aspect-ratio fitting within the output area are supported. FSR upscaling is optional; capture methods follow the options available in the application.

### HDR 处理与兼容转换 / HDR processing and conversion

传统 SDR 效果在 HDR 链中通过 SDR 兼容转换处理，再回到 HDR。这适用于动画模型、像素画、常规抗锯齿、CRT、多数锐化等沿用 SDR 运算的效果，也包括 VSR、Upscale 和视频降噪。转换涉及亮度范围与颜色精度，高光、渐变和颜色会受到影响；这是一条共用处理规则。

具体区别：Bicubic 使用 HDR 浮点处理；原生超分的 HDR 路径按对应后端处理；xBR Hybrid 在 HDR 兼容模式采用最近邻 2×；xBR NoBlend 保留的是它收到的 SDR 调色板。FSR3 FG 仅接受全 SDR 链；VFG 的 SDR 输入及 HDR 输出组合见上节。

显式 HDR → SDR / SDR → HDR 配对可以在中间加入 SDR 处理段：前者选兼容配对，后者放在该段末尾并恢复上游参数。配对中间画面不是直接观看用的 SDR，若直接输出 SDR 则选择 SDR 显示模式。转换、量化或后续处理会影响恢复结果。原生 SDR → HDR 白点映射调节白色亮度，TrueHDR 则用 AI 重塑亮度与颜色。两者用途分别是呈现 SDR 内容和改变 HDR 观感。

Traditional SDR effects process HDR chains through SDR compatibility conversion and then return to HDR. This covers anime models, pixel art, conventional anti-aliasing, CRT, and most sharpeners that use SDR arithmetic, as well as VSR, Upscale, and video denoising. The conversions affect brightness range and color precision, including highlights, gradients, and colors; this is a shared processing rule.

Specific cases: Bicubic uses floating-point HDR processing; native upscalers use their respective backend HDR paths; xBR Hybrid uses nearest-neighbor 2x in HDR compatibility mode; xBR NoBlend preserves the SDR palette it receives. FSR3 FG requires an entirely SDR chain. VFG SDR input and HDR output combinations are covered above.

An explicit HDR → SDR / SDR → HDR pair allows an SDR section in between: select compatibility-pair mode upstream and restore its settings after the section. The intermediate image is for processing; choose SDR display mode for direct SDR viewing. Conversion, quantization, and subsequent processing affect restoration. Native SDR → HDR white-point mapping sets white brightness, while TrueHDR uses AI to reshape brightness and color: one presents SDR content, the other changes the HDR look.

### 其他原生效果设置 / Other native-effect settings

DLSS SR 当前使用 Balanced／Preset J；其余时序超分按各自倍率与运行库要求使用。时序超分的光流方式和质量修改需重新启动缩放。运动来自捕获图像估计，快速运动、遮挡与界面边缘是选择时重点观察的部位。

DLSSNR 从单次处理开始，可增加至三次并逐次设置风格与强度。SDR 下“调整输入分辨率”启用工作尺寸和细节控制；细节控制在全部处理结束后应用一次。HDR 路径不提供这组控制。风格、强度等实时生效；处理次数、抗闪烁、输入尺寸与光流修改需重新启动缩放。功能初始化失败时，单次路径可能输出原图，多次路径会拒绝启动；排查无变化时查看日志中的功能初始化结果。

处理模式依次为逐帧、隔帧、异步逐帧、异步隔帧。两个隔帧模式可选每 2／3／4 个新捕获帧更新完整 NR，默认每 2 帧。隔帧固定连续复用受保护的历史增强，硬失效时同规格重建；异步逐帧在后台更新并连续复用同规格增强，允许最多 3 帧真实历史龄期，仅硬失效时重建；异步隔帧在后台更新，失效区域使用当前原图。四模式均输出当前画面，不运行轻量 NR，也不提供恢复策略或整图延后选项。处理模式和更新间隔修改需重新启动缩放。

XeSS FG 默认 2×、AMD 光流、质量。各倍率的可用光流选项按当前程序与硬件能力提供；非 Intel 显卡的 3×／4× 使用随附运行库的实验兼容路径。DLSS FG 使用随附兼容运行库，可用倍率以渲染 GPU 的能力为准；切换渲染 GPU 后需重启喜鹊。两者的倍率与光流修改均需重新启动缩放。

帧率限制效果控制整组的基础输入频率，拖动条目不改变限帧范围；多个有效上限共同生效并取较低目标。启用帧同步时由“帧率与刷新”的目标管理节奏。

DLSS SR currently uses Balanced/Preset J; other temporal upscalers follow their own scale and runtime requirements. Changing temporal-upscaler optical-flow method or quality requires restarting scaling. Motion comes from captured images; fast motion, occlusion, and UI edges are useful areas to inspect when choosing settings.

Start DLSSNR with one pass; up to three passes have independent style and intensity. In SDR, Adjust Input Resolution enables working-resolution and detail controls; detail controls apply once after all passes. The HDR path does not offer these controls. Style and intensity apply live; pass count, anti-flicker, working resolution, and optical flow require restarting scaling. If feature initialization fails, the single-pass path can pass through the image and multiple passes reject startup. Check feature initialization in the log when diagnosing an unchanged image.

Processing modes are Every Frame, Alternating Frames, Async Every Frame and Async Alternating Frames. Both alternating modes update full NR every 2, 3 or 4 new captures, defaulting to 2. Alternating Frames continuously reuses protected history and rebuilds hard-invalid history at the same specification. Async Every Frame continuously reuses same-spec history up to 3 captures old while updating in the background; only hard-invalid or unavailable history requires rebuilding. Async Alternating Frames updates in the background and uses the current original image in invalid regions. All four output the current picture without lightweight NR or a whole-picture delay control. Changing the mode or update interval requires restarting scaling.

XeSS FG defaults to 2x, AMD optical flow, Quality. Optical-flow options at each multiplier depend on the current application and hardware capabilities; 3x/4x on non-Intel GPUs uses the bundled runtime's experimental compatibility path. DLSS FG uses the bundled compatibility runtime, with multipliers determined by the rendering GPU; restart Magpie after changing that GPU. Multiplier and optical-flow changes in either generator require restarting scaling.

The frame-rate limiter controls the group's base input rate regardless of its position. Multiple active caps combine at the lower target. With frame synchronization enabled, Frame rate and refresh manages pacing.

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
