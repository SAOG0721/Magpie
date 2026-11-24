Magpie Experimental v0.6.14 x64

安装或升级
备份设置并从托盘完全退出旧版 Magpie，将完整 Magpie-Experimental-x64.zip 解压到新目录，再运行 Magpie.exe。
普通配置：%LOCALAPPDATA%\Magpie\config\v4e\config.json。
便携配置：程序目录内的 config\v4e\config.json。
保留旧程序目录与升级前的设置备份。回退前完全退出当前版本，恢复原设置后从旧版目录运行。

使用要点
侧栏应用档案的“更多”菜单支持上移与下移（支持键盘操作），调整后即时保存。
DLSSNR 自适应分辨率（SDR）每档 5%，结合帧率目标区分游戏掉帧与显卡负载，减少误降档，未带来明显改善时自动恢复；手动比例范围 25%–100%，步进 1%。
DLSSNR 提供逐帧（默认）与隔帧（SDR，每 2 / 3 / 4 个新捕获帧更新一次完整 NR，默认每 2 帧）。SDR 下 NVIDIA 光流＋隔帧改善了完整帧与复用帧切换的亮度与细节闪烁，剧烈运动或切场时可能出现局部拖影或增强减弱。
在效果器列表搜索 OKLab 添加调色效果器，常规数值实时生效，导入或清除 LUT 后重新开始缩放；运行参数浮窗内按 Ctrl＋滚轮可缩放界面（50%–200%，每档 10%）。
VRR 默认关闭，更改 DLSSNR 模型／处理模式／分辨率，以及捕获、帧同步或 VRR 设置后需重新开始缩放。
RTSS 联动会校验安装路径和数字签名。若无法自动启动，请从安装目录手动打开 RTSS 或 MSI Afterburner 后再点“重新检测”。

完整更新、适用条件与限制见 RELEASE-NOTES.md；刷新设置见 FRAME_SYNC_GUIDE.md。
主包内置 310.9.16 社区兼容 DLSSNR。DLSSNR-DLL-Options.zip 包含原版（RTX 50）与社区兼容版（RTX 20 / 30 / 40）；替换前退出 Magpie 并备份原 DLL。NGX_OTA_Switch.bat 为可选管理员排错工具，常规安装无需运行。
保留 LICENSE-Magpie.txt、THIRD-PARTY-NOTICES.md、组件许可证及 build-manifest.json。

English

Install or upgrade
Back up settings and fully exit the previous Magpie version from the tray. Extract the complete Magpie-Experimental-x64.zip into a new folder, then run Magpie.exe.
Normal settings: %LOCALAPPDATA%\Magpie\config\v4e\config.json.
Portable settings: config\v4e\config.json under the program folder.
Keep the previous program folder and a pre-upgrade settings backup. Before rolling back, fully exit the current version, restore the previous settings, and run Magpie from the older folder.

Essentials
Each profile's More menu in the sidebar supports Move up and Move down (keyboard-accessible), saving immediately.
DLSSNR Adaptive resolution (SDR) adjusts in 5% steps, distinguishing game frame drops from GPU load to reduce false downshifts and reverting automatically if a downshift doesn't help. Manual scale covers 25%–100% in 1% steps.
DLSSNR offers Every Frame (default) and Alternating Frames (SDR, full NR every 2 / 3 / 4 new captures, default 2). SDR + NVIDIA Optical Flow Alternating Frames reduces brightness and detail flicker between full and reused frames, though fast motion or scene cuts can still show local trails or softer enhancement.
Search for OKLab in the effect list to add color grading (numeric changes apply live; restart scaling after importing or clearing a LUT). Hold Ctrl and scroll the mouse wheel inside the live parameter overlay to zoom its UI (50%–200%, 10% steps).
VRR defaults to Off; restart scaling after changing DLSSNR model/processing mode/resolution or capture, frame-sync, or VRR settings.
RTSS integration verifies install paths and digital signatures. If auto-launch is blocked, start RTSS or MSI Afterburner from its install folder and click Recheck.

See RELEASE-NOTES.md for complete changes, requirements, and limitations, and FRAME_SYNC_GUIDE.md for refresh controls.
The main package bundles the 310.9.16 community compatibility DLSSNR. DLSSNR-DLL-Options.zip includes both the original (RTX 50) and community compatibility (RTX 20 / 30 / 40) options; exit Magpie and back up the DLL before switching. NGX_OTA_Switch.bat is an optional administrator troubleshooting tool, unnecessary for normal installation.
Retain LICENSE-Magpie.txt, THIRD-PARTY-NOTICES.md, component licenses, and build-manifest.json.
