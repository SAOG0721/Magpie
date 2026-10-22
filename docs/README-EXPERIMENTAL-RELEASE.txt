Magpie Experimental v0.6.11 x64

安装或升级
备份设置并从托盘完全退出 Magpie，将 Magpie-Experimental-x64.zip 完整解压到新目录，再运行 Magpie.exe。
普通配置：%LOCALAPPDATA%\Magpie\config\v4e\config.json。没有 v4e 时导入旧 v4，后续保存到 v4e。
便携配置：程序目录内的 config\v4e\config.json。保留旧程序和原配置，以便回退。

0.6.11 使用要点
在效果组页面的效果器参数中选择 DLSS SR／FG 模型预设。SR 默认 K，A–F 为 CNN，J/K 为 Transformer 1代，L/M 为 Transformer 2代；FG 默认自动，可选 A/B/C。修改模型后重新开始缩放。
DLSSNR 抗闪烁响应时间为 5–200 ms，步进 5 ms，默认 80 ms；关闭抗闪烁时隐藏并保留数值。下一有效捕获帧生效，较大值更稳定、响应更慢。
主包默认携带 310.9.16 社区兼容 DLSSNR；可选包原样保留 NVIDIA 原版 RTX 50 和社区兼容 RTX 20/30/40 两个选项。
截图可选输入＋输出、仅输出（默认）或仅输入；暂停时通过保留画面保存快照。
截图名为 Magpie_YYYYMMDD_HHmmss_SSS_input.png / _output.png，时间取请求时的本地时间，精确到毫秒；同组共用时间戳，重名加短序号。右键导出带效果／Pass／纹理后缀，PNG／DDS 格式保持原样。

完整更新、安装与限制见 RELEASE-NOTES.md；帧率说明见 FRAME_SYNC_GUIDE.md。
保留 LICENSE-Magpie.txt、THIRD-PARTY-NOTICES.md、组件许可证及 build-manifest.json。
可选附件：DLSSNR-DLL-Options.zip、NGX_OTA_Switch.bat。普通安装无需运行 OTA 工具。

English

Install or upgrade
Back up settings, fully exit Magpie from the tray, extract the complete Magpie-Experimental-x64.zip into a new folder and run Magpie.exe.
Normal settings: %LOCALAPPDATA%\Magpie\config\v4e\config.json. If v4e is absent, import v4 and save subsequent changes to v4e.
Portable settings: config\v4e\config.json under the program folder. Keep the previous program and settings for rollback.

0.6.11 essentials
Choose DLSS SR/FG model presets in each effect's parameter settings on the Scaling modes page. SR defaults to K: A–F are CNN, J/K are Transformer 1, and L/M are Transformer 2. FG defaults to Auto with A/B/C choices. Restart scaling after changing a model preset.
DLSSNR anti-flicker response time ranges from 5 to 200 ms in 5-ms steps, defaulting to 80 ms. Turning anti-flicker Off hides the control and retains its value. Changes apply on the next valid captured frame; higher values favor stability with slower adaptation.
The main package includes the 310.9.16 community compatibility DLSSNR. The unchanged optional ZIP contains the NVIDIA original for RTX 50 and the community version for RTX 20/30/40.
Screenshots offer input + output, output only (default), or input only, with snapshots of retained content while paused.
Files use Magpie_YYYYMMDD_HHmmss_SSS_input.png / _output.png with the request's local time to millisecond precision. Pairs share a timestamp; collisions add a short sequence number. Right-click exports include effect/pass/texture suffixes and keep their PNG/DDS formats.

See RELEASE-NOTES.md for updates, installation and limitations, and FRAME_SYNC_GUIDE.md for refresh settings.
Retain LICENSE-Magpie.txt, THIRD-PARTY-NOTICES.md, component licenses and build-manifest.json.
Optional assets: DLSSNR-DLL-Options.zip and NGX_OTA_Switch.bat. Normal installation does not require the OTA tool.
