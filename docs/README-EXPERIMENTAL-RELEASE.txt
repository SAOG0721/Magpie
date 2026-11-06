Magpie Experimental v0.6.12 x64

安装或升级
备份设置并从托盘完全退出旧版 Magpie，将完整 Magpie-Experimental-x64.zip 解压到新目录，再运行 Magpie.exe。
普通配置：%LOCALAPPDATA%\Magpie\config\v4e\config.json。
便携配置：程序目录内的 config\v4e\config.json。
保留旧程序目录与升级前的设置备份。回退前完全退出当前版本，恢复原设置后从旧版目录运行。

使用要点
在效果器列表搜索 OKLab 添加调色效果器。常规数值可实时调整；导入或清除 LUT 后重新开始缩放。
DLSSNR 提供自动／HT／CC Small FP8 模型、自适应／手动分辨率，以及逐帧／隔帧处理。隔帧仅支持 SDR，可每 2／3／4 个新捕获帧更新；较长间隔可能出现拖影或增强减弱。更改模型或结构设置后重新开始缩放，模型支持取决于 GPU 和运行库。
本地 Beta 的异步逐帧自动回退为逐帧，异步隔帧自动回退为隔帧，保留有效更新间隔、模型与输入比例；独立 Async 节奏继续保留。
编辑运行参数时，在浮窗内按 Ctrl＋滚轮缩放界面，范围 50%–200%，每档 10 个百分点，默认 100%，会记住选择。
支持的 WGC＋DLSSNR＋Front Edge＋VRR、无补帧路径可在短暂空档中补交已完成画面；复用完成画面，不重跑 DLSSNR。

完整更新、适用条件与限制见 RELEASE-NOTES.md；刷新设置见 FRAME_SYNC_GUIDE.md。
主包已包含社区兼容 DLSSNR。DLSSNR-DLL-Options.zip 为原版 RTX 50／社区 RTX 20、30、40 两选项包；替换前退出 Magpie 并备份 DLL。NGX_OTA_Switch.bat 为可选管理员工具，常规安装无需运行。
保留 LICENSE-Magpie.txt、THIRD-PARTY-NOTICES.md、组件许可证及 build-manifest.json。

English

Install or upgrade
Back up settings and fully exit the previous Magpie version from the tray. Extract the complete Magpie-Experimental-x64.zip into a new folder, then run Magpie.exe.
Normal settings: %LOCALAPPDATA%\Magpie\config\v4e\config.json.
Portable settings: config\v4e\config.json under the program folder.
Keep the previous program folder and a pre-upgrade settings backup. Before rolling back, fully exit the current version, restore the previous settings and run Magpie from the older folder.

Essentials
Search for OKLab in the effect list to add the color-adjustment effect. Ordinary numeric changes apply live; restart scaling after importing or clearing a LUT.
DLSSNR offers Automatic / HT / CC Small FP8 models, Adaptive / Manual resolution and Every Frame / Alternating Frames processing. Alternating Frames supports SDR only, with full updates every 2 / 3 / 4 new captures; longer intervals can cause trails or weaker enhancement. Restart scaling after model or structural changes. Model support depends on the GPU and runtime library.
Local Beta configurations automatically fall back from Async Every Frame to Every Frame and from Async Alternating Frames to Alternating Frames, retaining valid update intervals, models and input scales. The separate Async pacing option remains available.
While editing running parameters, hold Ctrl and scroll the mouse wheel inside the overlay to zoom its interface: 50%–200%, in 10-percentage-point steps, default 100%. The choice is remembered.
Supported WGC + DLSSNR + Front Edge + VRR paths without frame generation can refresh completed pictures across short gaps. These refreshes reuse a completed picture without rerunning DLSSNR.

See RELEASE-NOTES.md for complete changes, requirements and limitations, and FRAME_SYNC_GUIDE.md for refresh controls.
The main package includes the community compatibility DLSSNR. DLSSNR-DLL-Options.zip retains the original RTX 50 / community RTX 20, 30, 40 choices; exit Magpie and back up the DLL before switching. NGX_OTA_Switch.bat is an optional administrator tool, unnecessary for normal installation.
Retain LICENSE-Magpie.txt, THIRD-PARTY-NOTICES.md, component licenses and build-manifest.json.
