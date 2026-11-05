# 下一版与公开版本 / Next and public release

## 下一版 / Next release

本地开发候选为 **0.6.12 Beta 5**。DLSSNR 仅保留逐帧／隔帧，旧配置中的异步逐帧／异步隔帧分别自动回退为逐帧／隔帧，保留有效更新间隔、模型、输入比例；独立 Async 节奏继续保留。新增受支持 WGC／DLSSNR／Front Edge＋VRR、无补帧路径的短暂空档补交，并继承此前捕获与恢复修复。构建身份与验收结果以对应候选包的构建清单和审核记录为准。

The local development candidate is **0.6.12 Beta 5**. DLSSNR now offers only Every Frame and Alternating Frames. Old configurations automatically fall back from Async Every Frame to Every Frame and from Async Alternating Frames to Alternating Frames, retaining valid update intervals, models and input scales. The separate Async pacing option remains available. This version also adds refreshes across short gaps on supported WGC/DLSSNR/Front Edge + VRR paths without frame generation and includes preceding capture and recovery fixes. Refer to the candidate package manifest and review record for its build identity and validation results.

- [0.6.12 Beta 5 中英说明 / Bilingual notes](RELEASE_NOTES_v0.6.12-beta5.md)
- [0.6.12 Beta 3 中英说明 / Bilingual notes](RELEASE_NOTES_v0.6.12-beta3.md)
- [开发基线 / Development baseline](experimental/DEVELOPMENT-BASELINE.md)

## 当前公开版本 / Current public release

[Magpie Experimental v0.6.11](https://github.com/SAOG0721/Magpie/releases/tag/v0.6.11-experimental) 已于 2026-10-23 公开，标记为 Pre-release，标签指向 `4fc74a196993c883ce2f01e96e8ed8d81f1490e6`。`version.json` 与公开下载入口保持指向这个已发布版本。

[Magpie Experimental v0.6.11](https://github.com/SAOG0721/Magpie/releases/tag/v0.6.11-experimental) was published on 2026-10-23 as a Pre-release. Its tag points to `4fc74a196993c883ce2f01e96e8ed8d81f1490e6`. `version.json` and public download links refer to this published version.

[完整中英更新说明 / Complete bilingual release notes](RELEASE_NOTES_v0.6.11-experimental.md) · [历史开发记录 / Development history](experimental/README.md)
