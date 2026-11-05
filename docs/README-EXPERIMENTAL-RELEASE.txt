Magpie Experimental v0.6.12-beta5 x64

安装或升级
备份设置并从托盘完全退出旧版 Magpie，将完整 Magpie-Experimental-x64.zip 解压到新目录，再运行 Magpie.exe。
普通配置：%LOCALAPPDATA%\Magpie\config\v4e\config.json。
便携配置：程序目录内的 config\v4e\config.json。
保留 Beta 4 程序目录和升级前的设置备份。回退前完全退出 Beta 5，恢复原设置后从 Beta 4 目录运行。

本次更新
DLSSNR 仅保留逐帧和隔帧。旧配置中的异步逐帧自动回退为逐帧，异步隔帧自动回退为隔帧；保留已保存的有效 NR 更新间隔、模型、输入比例。“帧率与刷新”中的独立 Async 节奏继续保留。
在支持的 Graphics Capture（WGC）＋DLSSNR＋Front Edge、无补帧输出路径，开启 VRR 且 tearing 支持实际可用时，自动补交短暂空档中的已完成画面。
约两个目标帧周期没有新内容提交后可开始补交；待提交的新画面及正在处理的内容优先，约 250 ms 没有新内容提交后停止。
补交复用已完成画面，不重跑 DLSSNR。实际新内容帧率取决于源程序、捕获和效果处理速度。更改捕获、节奏或 VRR 设置后重新开始缩放。
继承 Beta 4 捕获、等待与恢复修复，并保留 DLSSNR 自适应分辨率。

完整更新与限制见 RELEASE-NOTES.md；刷新设置见 FRAME_SYNC_GUIDE.md。
保留 LICENSE-Magpie.txt、THIRD-PARTY-NOTICES.md、组件许可证及 build-manifest.json。

English

Install or upgrade
Back up settings and fully exit the previous Magpie version from the tray. Extract the complete Magpie-Experimental-x64.zip into a new folder, then run Magpie.exe.
Normal settings: %LOCALAPPDATA%\Magpie\config\v4e\config.json.
Portable settings: config\v4e\config.json under the program folder.
Keep the Beta 4 program folder and a pre-upgrade settings backup. Before rolling back, fully exit Beta 5, restore the previous settings and run Magpie from the Beta 4 folder.

Changes
DLSSNR now offers only Every Frame and Alternating Frames. Old configurations automatically fall back from Async Every Frame to Every Frame and from Async Alternating Frames to Alternating Frames, retaining valid saved NR update intervals, models and input scales. The separate Async pacing option under Frame rate and refresh remains available.
On supported Graphics Capture (WGC) + DLSSNR + Front Edge output paths without frame generation, enabling VRR with tearing support available automatically permits refreshes of completed pictures across short gaps.
Refreshes can begin after roughly two target frame periods without a new content submission. Pending new pictures and processing in progress take priority; refreshes stop after roughly 250 ms without a new content submission.
Refreshes reuse completed pictures without rerunning DLSSNR. New-content FPS depends on the source application, capture and effect processing. Restart scaling after changing capture, pacing or VRR settings.
Includes Beta 4's capture, waiting and recovery fixes, and retains DLSSNR adaptive resolution.

See RELEASE-NOTES.md for complete changes and limitations, and FRAME_SYNC_GUIDE.md for refresh settings.
Retain LICENSE-Magpie.txt, THIRD-PARTY-NOTICES.md, component licenses and build-manifest.json.
