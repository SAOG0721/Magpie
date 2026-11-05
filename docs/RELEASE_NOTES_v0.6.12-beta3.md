# Magpie Experimental 0.6.12 Beta 3

本地内测版本，2026-10-31。开发分支：`feature/0612-beta3-async-composition-ab`。

## 中文

- 新增“异步残差合成”对照选项：差量叠加（Beta 2）／总残差混合（Beta 3），默认总残差混合。位于性能进阶中残差复用方式之后，仅异步处理时显示；可在运行中切换。
- 两项共用当前 Beta 3 的模型、提交顺序和恢复策略，仅比较残差合成公式。首次启用差量叠加时准备同源轻量残差缓存，之后在本次缩放中保留。建议先固定手动分辨率和其他参数进行 A/B，再根据实际画面决定最终保留方案。
- 调整异步 NR 合成：可信区域复用完整 NR 总残差，失效区域使用当前轻量 NR，减少两份轻量残差差异引起的画面变化。
- 完整 NR 后台任务在当前画面发布后提交；保留一个在途任务和一个最新输入，避免后台续投提前挤占输出。
- 补齐完整与轻量 NR 的 GPU 队列依赖，避免后台任务延迟时内部资源被另一条 NR 队列提前使用；源停止更新后仍能完成最后一帧增强。
- 完整结果暂时失效后增加稳定恢复过程；局部失配仍按像素处理，残差有效年龄保持一个捕获帧。
- 新增“流水线处理”：复用或重投影上一捕获帧的同分辨率 NR 结果，输出后准备下一次结果。失配时以同一分辨率处理当前帧，该帧需要等待补算。
- 自适应分辨率分别判断完整 NR、轻量 NR、合成与固定开销，并参考结果的实际采用率和可信度；保持快降慢升与 5 个百分点的档位。
- 自适应启动时预热一个邻接档，主动升档使用已准备的档位，切回时复用模型资源。启动时间和显存占用相应增加；紧急降低到尚未准备的档位仍有创建开销。
- 保留 Beta 2 的参数保存、并排勾选项文字以及 OKLab／VFG 参数页崩溃修复。

## English

- Added an Async Residual Composition comparison: Delta Add (Beta 2) / Total Residual Blend (Beta 3), with Total Residual Blend as the default. It appears after Residual Reuse in advanced performance controls for asynchronous processing and supports live switching.
- Both choices share the current Beta 3 models, submission order and recovery policy; only the composition formula changes. The first use of Delta Add prepares source-matched lightweight residual snapshots, retained for the scaling session. Start comparisons with a fixed manual resolution and identical settings, then choose the final behavior from the observed image quality.
- Revised asynchronous NR composition: trusted regions reuse the complete NR residual, while invalid regions use current lightweight NR. This removes variation caused by differences between current and historical lightweight residuals.
- Submit full NR work after publishing the current output. Keep one inference in flight and one latest-input mailbox.
- Order GPU evaluation across the full and lightweight NR queues to prevent internal resource conflicts when background work is delayed. Complete the final enhancement even after the source stops updating.
- Stabilize recovery after full results become temporarily unavailable. Local rejection remains per pixel, with a one-capture residual age limit.
- Added Pipelined Processing: reuse or reproject the preceding capture's NR result at the same resolution, then prepare the next result after output publication. Invalid reuse triggers current-frame inference at that same resolution and adds a wait to that frame.
- Adaptive resolution separates full NR, lightweight NR, composition and fixed costs, and checks result adoption and confidence. It retains faster reductions, slower upgrades and five percentage point tiers.
- Prewarm one adjacent tier at adaptive startup. Proactive upgrades use prepared tiers, and switching back reuses model allocations. Startup time and memory usage increase; emergency changes to an unprepared tier still incur creation cost.
- Includes the Beta 2 parameter persistence, paired checkbox labels and OKLab/VFG parameter-page crash fixes.
