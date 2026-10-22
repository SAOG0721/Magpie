<p align="center"><img src="./src/Magpie/Icons/SVG/Magpie Icon Full Disabled.svg" width="150" height="150" alt="Magpie"></p>
<h1 align="center">Magpie Experimental</h1>

🌍 [English](./README.md) | **简体中文**

**当前版本：[Magpie Experimental v0.6.10](https://github.com/SAOG0721/Magpie/releases/tag/v0.6.10-experimental)** · [完整更新说明](docs/RELEASE_NOTES_v0.6.10-experimental.md)

**0.6.11 待审核草稿：** [完整更新说明](docs/RELEASE_NOTES_v0.6.11-experimental.md)。源码已包含 0.6.11 候选版；审核完成前，上方公开下载入口保留 0.6.10。

Magpie Experimental 是面向 Windows 的窗口画面处理工具，也是 [Blinue/Magpie](https://github.com/Blinue/Magpie) 的非官方实验分支。它在 Magpie 的窗口缩放与效果系统基础上，探索 DLSS、XeSS、FSR、RTX Video 等技术在游戏、视频和其他窗口内容中的应用。

程序捕获目标窗口的画面，按用户配置的效果组处理，再以全屏或窗口形式显示。目标应用无需为这些效果单独集成接口。本项目不代表 Magpie 官方，实验分支的问题请在本仓库反馈。

## 主要功能

### 图像处理与帧生成

| 功能 | 效果示例 | 用途 |
| --- | --- | --- |
| 空间缩放与锐化 | Lanczos、FSR 及其他 MagpieFX 效果 | 放大窗口内容，调整清晰度与细节表现 |
| 实验性时序超分辨率 | DLSS SR、FSR 2/3/4、XeSS SR | 探索从捕获画面和估算运动信息进行图像重建 |
| AI 画面调整 | DLSSNR | 同分辨率 SDR 画面处理，调整色调、结构、阴影、反射与辉光 |
| 视频增强 | RTX Video | 视频超分辨率与降噪，改善低分辨率或压缩内容的观感 |
| 帧生成 | DLSSFG、XeSSFG、FSR3FG、NVIDIA VFG | 在真实帧之间生成中间画面，提高视觉流畅度 |

实验效果的可用性取决于显卡、驱动、运行组件和具体效果组合。各版本包含的效果及硬件要求见 [Release 说明](https://github.com/SAOG0721/Magpie/releases)。

### 效果组与参数调节

一个**效果组**可以按顺序组合多个效果器，并保存各自的参数和缩放设置。可以使用内置效果组，也可以为不同应用创建自己的组合。

当前分支的补帧组合、HDR 转换和共用设置见[内置效果使用说明](docs/Built-in%20effects.md#current-branch-setup)。

工具栏中的参数面板支持边看画面边调节，修改自动保存。控件会标明参数是实时生效，还是需要重新启用效果组；双击滑条可恢复效果器自身的默认值。参数支持分组、下拉选项和中文翻译。

同一面板在所有效果器之前提供 Front Edge Sync 开关和目标帧率滑条（15–360 FPS，步进 1），方便集中调节帧节奏与画面效果。修改的生效方式以各控件的 Live（实时）或 Restart（重新启用）标记为准。参数浮窗会记住位置和大小，并在捕获区域变化时自动调整到可见范围。

### 对比、性能监测与帧同步

工具栏提供原图／处理后对比、各效果器耗时与帧率监测、截屏和参数调节，并支持自定义快捷键。启用帧生成时，可以分别查看输出帧率和真实帧率，帮助判断效果与性能开销。

Front Edge Sync 用于控制 Magpie 的帧提交节奏，启用 FG 时控制补帧前的真实帧输入。源程序仍需配合限帧，同步等待可能增加延迟；设置方法见 [帧同步使用说明](docs/FRAME_SYNC_GUIDE.md)。

## 下载与安装

1. 从 [GitHub Releases](https://github.com/SAOG0721/Magpie/releases) 选择版本，阅读该版本说明并下载主包 `Magpie-Experimental-x64.zip`。
2. 完全退出正在运行的 Magpie，将主包完整解压到一个新目录。
3. 运行其中的 `Magpie.exe`，选择效果组和目标窗口，使用主页所示快捷键启用效果。

升级前请将需要保留的设置和截图备份到程序目录之外，并按对应 Release 的说明处理旧安装和配置。可选运行组件、DLL 选项及辅助工具的用途和使用方法也以该版本说明为准。GitHub 自动生成的源码压缩包用于开发，不是可直接运行的程序包。

## 使用与兼容性

- 发布主包面向 Windows x64，需要支持 DirectX 11 的显卡；具体 AI 效果可能有更高要求。
- Magpie 从完整窗口画面进行处理，无法取得游戏引擎原生的完整运动矢量、深度、曝光和独立 UI 信息。估算光流可以辅助部分效果，但不能等同于游戏原生 DLSS／FSR／XeSS 集成。
- 图像处理可能同时影响文字和 UI；时序效果也可能产生拖影或其他瑕疵。可通过“对比”判断效果是否适合当前内容。
- 同一效果组使用一种帧生成效果，避免与其他补帧系统叠加。生成帧率不等于游戏的真实渲染帧率，也不代表输入响应速度同比提升。

## 问题反馈

遇到问题时，先查看主页“最近一次问题”的处理建议和详细信息，或打开日志目录。向 [Issues](https://github.com/SAOG0721/Magpie/issues) 反馈时，请提供程序版本、显卡与驱动、效果组、输入／输出分辨率、复现步骤和相关日志。

通用使用问题也可参考 [Magpie 上游 FAQ](https://github.com/Blinue/Magpie/wiki/FAQ)；本分支特有的实验效果和兼容性问题请在本仓库讨论。

## 开发与文档

项目包含 Magpie 应用、MagpieFX 效果及实验性原生效果后端。源码构建默认关闭可选专有后端，相关 SDK 和运行组件需要另行准备；本机路径通过不入库的 `src/BuildOptions.props.user` 配置。

- [第三方依赖、许可与构建边界](docs/THIRD_PARTY_AND_REDISTRIBUTION.md)
- [实验功能的设计与开发记录](docs/experimental/README.md)
- [构建和打包脚本](scripts/Build-Release.ps1)
- [MagpieFX 效果格式](docs/MagpieFX.md)

## 贡献与致谢

本项目建立在 [Blinue/Magpie](https://github.com/Blinue/Magpie) 及其贡献者的工作之上，也感谢提交代码、翻译、设计建议和测试反馈的参与者。

- [HexBen123](https://github.com/HexBen123)：早期深度估算性能优化与 TensorRT 集成指导，以及 [DLSSNR 参数本地化提案](https://github.com/SAOG0721/Magpie/pull/16)。
- [Kristijan1001](https://github.com/Kristijan1001)：[缩放期间实时编辑效果参数的参考实现](https://github.com/SAOG0721/Magpie/pull/4)，为本项目参数面板的设计与实现提供了参考。
- [LCPD15](https://github.com/LCPD15)：[窗口模式 1.0× 倍率修复](https://github.com/SAOG0721/Magpie/pull/3)，允许同分辨率效果按原尺寸显示，避免输出被额外放大。
- [TurnX-alt](https://github.com/TurnX-alt)：[构建、界面与仓库一致性改进](https://github.com/SAOG0721/Magpie/pull/23)，包括 ClangCL 无 SDK 构建修复、预设参数与反馈链接修正、窄窗口文字换行优化、缩放开始提示，以及仓库一致性检查套件。
- [konodiodaaaaa1](https://github.com/konodiodaaaaa1)：[HDR 支持与兼容性适配](https://github.com/SAOG0721/Magpie/pull/24)。

欢迎通过 Issues 和 Pull Requests 参与；代码贡献记录见 [Contributors](https://github.com/SAOG0721/Magpie/graphs/contributors)。

## 许可

Magpie 派生源码采用 [GPLv3](LICENSE)。第三方 SDK、模型和运行组件适用各自的许可证，详见 [第三方组件与再分发说明](docs/THIRD_PARTY_AND_REDISTRIBUTION.md)。
