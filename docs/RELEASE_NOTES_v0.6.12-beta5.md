# Magpie Experimental v0.6.12-beta5

## 本次更新

- DLSSNR 处理模式仅保留“逐帧”和“隔帧”，移除“异步逐帧”和“异步隔帧”。旧配置中的异步逐帧自动回退为逐帧，异步隔帧自动回退为隔帧；保留已保存的有效 NR 更新间隔、模型预设、输入分辨率比例。
- 新增 WGC／VRR 短暂空档补交：连续约两个目标帧周期没有新内容提交时，可按目标节奏再次提交最近已完成的画面。
- 新画面待提交或后台仍在处理时，优先等待这些内容；连续约 250 ms 没有新内容提交后，停止这类补交。
- 自动用于 Graphics Capture（WGC）捕获、效果链包含 DLSSNR、Front Edge 节奏且未启用补帧的受支持输出路径。需要开启 VRR，并且当前输出通道的 tearing 支持实际可用；更改捕获、节奏或 VRR 设置后重新开始缩放。
- 继承 Beta 4 的 WGC 捕获间隔与等待优化、静止画面轮询优化，以及切换焦点、DWM 遮挡／最小化恢复和捕获边界修复。
- 保留 DLSSNR 自适应分辨率及参数保存修复。

## 使用与限制

- “帧率与刷新”中的独立 Async 节奏选项继续保留。
- 补交复用已完成的画面，不重跑 DLSSNR；实际新内容帧率仍取决于源程序、捕获和效果处理速度。此优化不启用于其他捕获方式、Async／NVIDIA Reflex 节奏或带补帧的效果链。
- VRR 仍为实验功能，默认关闭。帧间隔和目标帧率表现会随源程序、驱动、显示器及配置变化。
- 首次安装和升级均使用完整主包 `Magpie-Experimental-x64.zip`。先备份设置并从托盘完全退出旧版，将整个 ZIP 解压到新目录，再运行 `Magpie.exe`。
- 保留 Beta 4 程序目录及升级前的设置备份。需要回退时，完全退出 Beta 5，恢复原设置后从 Beta 4 目录运行。

---

# Magpie Experimental v0.6.12-beta5 User Guide

## Changes

- DLSSNR processing now offers only Every Frame and Alternating Frames; Async Every Frame and Async Alternating Frames have been removed. Old configurations automatically fall back from Async Every Frame to Every Frame and from Async Alternating Frames to Alternating Frames. Valid saved NR update intervals, model presets and input-resolution scales are retained.
- Added refreshes across short WGC/VRR gaps: after roughly two target frame periods without a new content submission, Magpie can resubmit the most recently completed picture at the target cadence.
- Pending new pictures and processing already in progress take priority. These refreshes stop after roughly 250 ms without a new content submission.
- This applies automatically on supported output paths using Graphics Capture (WGC), a chain containing DLSSNR, Front Edge pacing and no frame generation. VRR must be enabled, and tearing support must be available on the active output path. Restart scaling after changing capture, pacing or VRR settings.
- Includes Beta 4's WGC capture-interval and waiting improvements, reduced polling for static pictures, and fixes for focus changes, DWM occlusion/minimize recovery and capture boundaries.
- Retains DLSSNR adaptive resolution and parameter-persistence fixes.

## Use and limitations

- The separate Async pacing option under Frame rate and refresh remains available.
- Refreshes reuse a completed picture without rerunning DLSSNR. New-content FPS still depends on the source application, capture and effect processing. This optimization is inactive with other capture methods, Async/NVIDIA Reflex pacing or chains using frame generation.
- VRR remains experimental and defaults to Off. Frame intervals and the ability to meet the target rate vary with the source application, driver, display and settings.
- Use the complete `Magpie-Experimental-x64.zip` for both installation and upgrades. Back up settings and fully exit the previous version from the tray, extract the entire ZIP into a new folder, then run `Magpie.exe`.
- Keep the Beta 4 program folder and a pre-upgrade settings backup. To roll back, fully exit Beta 5, restore the previous settings and run Magpie from the Beta 4 folder.
