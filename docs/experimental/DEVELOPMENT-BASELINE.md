# 当前开发基线

核对日期：2026-11-24（Asia/Shanghai）。GitHub 当前公开版本基线仍为 **Magpie Experimental v0.6.12**（已于 2026-11-06 发布，标签 `v0.6.12-experimental`，`version.json` 保持 `0.6.12`）；**Magpie Experimental v0.6.13** 当前在 `release/0.6.13-preparation` 上作为本地候选进行发布准备（功能来自 0.6.13 Beta 3）。

DLSSNR 处理模式继续保留逐帧／同步隔帧（每 2／3／4 帧）。旧本地 Beta 配置中的异步逐帧自动回退为逐帧，异步隔帧自动回退为同步隔帧，保留已保存的有效更新间隔、模型、输入比例；独立帧同步节奏 Async 继续保留。受支持的 WGC＋DLSSNR＋Front Edge＋VRR、无补帧路径的短暂空档补交继续保留。

0.6.13 改进 DLSSNR 自适应分辨率和 SDR NVIDIA 光流同步隔帧，完善窗口恢复、效果编译与配置保护。详见 [0.6.13 候选说明](../RELEASE_NOTES_v0.6.13-experimental.md)及 [0.6.12 中英说明](../RELEASE_NOTES_v0.6.12-experimental.md)。

后续开发直接从当前主线开始；需要隔离时再建立对应功能工作树。当前公开版本及下一版跟踪见 [RELEASE_NOTES_NEXT](../RELEASE_NOTES_NEXT.md)。

## 构建与验证

- 本机 SDK 路径与构建选项放在不跟踪的 `src/BuildOptions.props.user`。
- 在发布工作树运行 `scripts/Build-Release.ps1 -Version 0.6.13 -ReleaseDirectory v0.6.13-experimental -PrepareRelease -IncludeSymbols -RequireMagpieClosed`，输出位于工作区 `release/v0.6.13-experimental/`。
- 现有本机运行目录继续按部署记录保留；准备正式附件不迁移运行目录或用户配置。
- 运行 `pwsh -NoProfile -File tests/Validate-EffectCatalog.ps1 -CatalogOnly` 检查效果目录，运行 `python tests/validate_effect_descriptions.py` 和 `python tests/consistency/run.py` 检查效果说明和仓库一致性。提交前运行 `git diff --check`。
- 构建身份以包内 `build-manifest.json` 为准；本地审核与运行验收分别记录。发布操作遵循 [发布流程](RELEASE-WORKFLOW.md)。
