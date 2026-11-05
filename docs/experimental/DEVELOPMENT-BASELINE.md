# 当前开发基线

核对日期：2026-11-06（Asia/Shanghai）。开发入口为主检出 `source/` 的 `experimental`，当前本地候选为 **0.6.12 Beta 5**。

Beta 5 的 DLSSNR 处理模式仅保留逐帧／隔帧。旧配置中的异步逐帧自动回退为逐帧，异步隔帧自动回退为隔帧，保留已保存的有效更新间隔、模型、输入比例。独立帧同步节奏 Async 继续保留。

本版保留 DLSSNR 自适应分辨率和捕获恢复修复，新增受支持 WGC／DLSSNR／Front Edge＋VRR、无补帧路径的短暂空档补交。启用条件和使用限制见 [Beta 5 中英说明](../RELEASE_NOTES_v0.6.12-beta5.md)。

后续开发直接从当前主线开始；需要隔离时再建立对应功能工作树。当前公开版本及下载入口以 [RELEASE_NOTES_NEXT](../RELEASE_NOTES_NEXT.md) 为准。

## 构建与验证

- 本机 SDK 路径与构建选项放在不跟踪的 `src/BuildOptions.props.user`。
- 使用当前工作树的 `scripts/Build-Release.ps1 -Version 0.6.12-beta5 -IncludeSymbols -RequireMagpieClosed` 构建本地运行目录；明确准备分发文件时加 `-PrepareRelease`。
- 效果目录检查：`pwsh -NoProfile -File tests/Validate-EffectCatalog.ps1`。其余回归按修改范围选择；提交前运行 `git diff --check`。
- 构建身份以包内 `build-manifest.json` 为准；本地审核与运行验收分别记录。发布操作遵循 [发布流程](RELEASE-WORKFLOW.md)。
