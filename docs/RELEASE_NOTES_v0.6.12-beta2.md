# Magpie Experimental 0.6.12 Beta 2

本地内测版本，包含 2026-10-31 后续修订。开发分支：`feature/0612-beta2-dlssnr-performance`。

## 中文

- 修复选择部分 OKLab 编辑区域、打开 VFG、RTXVideo HDR 和部分 CRT 效果参数时的闪退。
- 修复打开效果组参数页、展开进阶参数时，模型预设和处理模式等选项可能被隐藏控件改写的问题。
- 恢复并排勾选项中“性能进阶”和各 Pass“UI 修正”的文字，以及相关参数的悬停提示；处理模式统一命名为“逐帧处理／隔帧处理／异步处理”。
- DLSSNR 性能进阶增加“分辨率控制”：新建效果默认自适应，支持切回手动并恢复保存的比例；旧配置保留手动设置。自适应以 5 个百分点调整，只重建发生变化的完整或轻量 NR 分支，升档等待更稳定的余量；首次创建新尺寸仍有开销。
- 移除“光流计算顺序”选项及预缩放路径，旧配置统一迁回先估算光流、再适配运动矢量。AMD 性能档保留自身的半分辨率估算。
- 性能进阶按“分辨率控制 → 输入比例 → 处理模式 → 残差复用方式 → 光流方法 → 光流质量”排列。
- 异步处理使用三槽增强池和一个最新输入待办；完整推理仍最多一个任务在途，残差有效年龄仍为一个捕获帧。
- 修复异步模式的同帧质量补刷占用新内容时隙：运动期间随新捕获合成增强，确认静止后补刷最终结果；补刷不推进内容限速时钟。非插帧的帧率主值采用新内容帧率。
- 增加光流、NR、增强采用比例、结果过期和切档耗时记录，便于对照性能。

## English

- Fixed crashes when selecting certain OKLab editor sections or opening parameters for VFG, RTXVideo HDR and several CRT effects.
- Fixed hidden parameter controls rewriting model presets, processing modes and other values when opening or expanding effect parameters.
- Restored labels for the paired Performance and per-pass UI fix checkboxes and related parameter tooltips. Processing modes are named Every Frame, Alternating Frames and Asynchronous Processing.
- Added Adaptive / Manual resolution control to DLSSNR Performance Advanced Adjustments. New effects use Adaptive; existing configurations retain Manual. Adaptive uses five percentage point steps and preserves the saved manual value. Resolution changes rebuild only the affected full or lightweight NR branch, and upgrades require sustained headroom. Creating a new size still has a cost.
- Removed optical flow calculation order and its input pre-scaling path. Existing configurations migrate to estimating optical flow before adapting motion vectors. AMD Performance retains its internal half-resolution estimation.
- Ordered performance controls as Resolution control → Input percentage → Processing mode → Residual reuse → Optical flow method → Optical flow quality.
- Asynchronous Processing uses a three-slot enhancement pool and a latest-input mailbox, with one full inference in flight and a one-capture residual age limit.
- Fixed same-capture quality refreshes consuming new-content presentation slots. Moving sources consume enhancements with new captures; static sources receive a final quality refresh without advancing the content pacing clock. The non-frame-generation FPS value counts new content.
- Added optical flow, NR, enhancement coverage, expiration and resolution-switch timing measurements.
