# 当前开发基线

核对日期：2026-11-24（Asia/Shanghai）。开发入口为主检出 `source/` 的 `experimental`，当前公开版本基线为 **Magpie Experimental v0.6.12**（已于 2026-11-06 发布，标签 `v0.6.12-experimental`）。

DLSSNR 处理模式仅保留逐帧／隔帧。旧配置中的异步逐帧自动回退为逐帧，异步隔帧自动回退为隔帧，保留已保存的有效更新间隔、模型、输入比例。独立帧同步节奏 Async 继续保留。

当前基线包含 OKLab 调色、DLSSNR 模型与自适应分辨率、参数面板缩放与布局改进、捕获恢复修复，以及受支持 WGC／DLSSNR／Front Edge＋VRR、无补帧路径的短暂空档补交。完整功能与使用限制见 [0.6.12 中英说明](../RELEASE_NOTES_v0.6.12-experimental.md)。

后续开发直接从当前主线开始；需要隔离时再建立对应功能工作树。当前公开版本及下一版跟踪见 [RELEASE_NOTES_NEXT](../RELEASE_NOTES_NEXT.md)。

## 构建与验证

- 本机 SDK 路径与构建选项放在不跟踪的 `src/BuildOptions.props.user`。
- 正式准备分发使用当前工作树的 `scripts/Build-Release.ps1 -Version <version> -ReleaseDirectory <dir> -PrepareRelease -IncludeSymbols -RequireMagpieClosed`，输出位于工作区 `release/<dir>/`。
- 现有本机运行目录继续按部署记录保留；准备正式附件不迁移运行目录或用户配置。
- 效果目录检查：`pwsh -NoProfile -File tests/Validate-EffectCatalog.ps1`；文档与一致性检查：`python -m unittest tests/validate_effect_descriptions.py` 与 `python tests/consistency/run.py`。提交前运行 `git diff --check`。
- 构建身份以包内 `build-manifest.json` 为准；本地审核与运行验收分别记录。发布操作遵循 [发布流程](RELEASE-WORKFLOW.md)。
