# DLSSNR 状态与恢复 / DLSSNR recovery

适用于包含“当前 DLSSNR 会话”状态区的 Magpie 版本。本说明位于程序目录，也可从问题详情中的“打开恢复说明”进入。

## 先看状态

- **正在启动**：尚未确认首个成功处理的画面。
- **正在运行**：DLSSNR 已成功处理画面。
- **已跳过**：效果组仍显示画面，DLSSNR 没有参与处理；有画面不代表 DLSSNR 正常。
- **效果组已停止**：整个效果组停止。多层 DLSSNR 失败采用此处理方式。
- **未运行（效果组已关闭）**：没有当前处理任务，不属于报错。
- **HDR 参数受限**：部分 SDR 参数不能用于 HDR，不代表 DLSSNR 停止。

浮层可打开主页状态与处理步骤。主页“最近一次问题 → 查看详情”可以复制诊断、打开日志、程序目录或本说明；当前状态与历史问题分别显示。

## 按检测结果处理

| 检测结果 | 下一步 | 恢复确认 |
| --- | --- | --- |
| 已确认缺少 `nvngx_dlssnr.dll` | 使用详情的“打开程序目录”。完全退出 Magpie，按当前版本运行包说明恢复完整运行文件；不要只从其他应用复制同名 DLL | 重开 Magpie，启用效果组，状态变为“正在运行” |
| 文件加载失败或接口不完整 | 核对诊断中的文件路径、版本及失败操作。将同一版本完整运行包解压到新目录比较，保留原目录和配置 | 新目录启用相同效果后成功运行；仍失败则提供两份诊断 |
| 选中的渲染显卡不是 NVIDIA | 打开本次配置的“渲染显卡”；只有确实存在兼容 NVIDIA 显卡时才改选，否则打开相关效果组移除 DLSSNR | 重新启用后查看实际选卡和状态 |
| 组合不支持，或要求更新驱动/功能库 | 对照当前版本运行包的兼容说明，核对实际 PCI ID、驱动和 DLL 版本。结果本身不能确定是哪一组件不兼容；没有明确要求时先复制诊断反馈 | 按已确认要求调整后重新打开 Magpie，查看状态 |
| 内存资源不足 | 关闭其他 GPU 高占用程序，降低源分辨率，或在相关效果组中把大于 1 的 Multi Pass 层数降至 1 | 重新启用，确认“正在运行” |
| 访问失败 | 查看操作和路径：文件读取失败检查完整可读文件；NGX 数据写入失败使用当前用户可写的程序目录。设备访问失败不等于文件权限问题 | 重新打开并重试；操作/对象不明确时复制诊断反馈 |
| NGX 已被进程保护禁用 | 在系统托盘中完全退出 Magpie，再重新打开。仅开关效果组不能解除保护 | 新进程启用后状态变为“正在运行” |
| 其他创建、处理或尺寸重建失败 | 核对本版本运行包说明后重新启用一次；再次失败就复制诊断及日志。可暂时移除 DLSSNR 使用其他效果 | 只以实际运行状态确认，不以有画面或参数可调确认 |

“打开相关效果组”会尝试展开对应组并定位效果；若组名、效果顺序已改变或出现重名，页面会说明情况并保留列表供手动查找。不会修改另一组配置。

## 反馈所需信息

复制问题详情，并附 `logs` 中对应时间的日志。详情包含会话代次、效果序号、阶段/操作、带类型的原始码、实际渲染 GPU/PCI ID、驱动和 DLL 路径/文件版本。写明是否单层、是否 HDR、是否在改变窗口尺寸后失败。不要仅提供“以前有画面”，这不能证明以前 DLSSNR 参与过处理。

当前实现没有可靠的逐 DLL 最低驱动和完整 GPU 支持矩阵，因此不会自动断言某个 NVIDIA 型号或某个驱动版本一定支持。不要将旧指南的 `615+` 或系列/DLL 对照当作本构建的判定规则。

## VRR 开启后帧率降低

先确认比较时实际 VRR 状态一致：切换到其他应用可能使实际 VRR 停用，不能只看 Magpie 的开关。捕获交付不足时，降低 NR 更新频率未必能恢复新画面的帧率。

在对应配置的“捕获方式”中，可以选择现有的 `DwmSharedSurface`，停止并重新启用效果组后比较。它是 SDR 窗口捕获的兼容路线；游戏是否提供可捕获的共享表面需实际测试。若捕获失败或画面不正常，在同一配置中改回 `Graphics Capture` 后重新启用。HDR 捕获继续使用 `Graphics Capture`，避免改变亮度和颜色语义。

启用重复帧检测时，Beta4 会在 DWM 等轮询捕获持续没有新内容后减少查重频率，恢复运动时继续正常捕获。关闭重复帧检测时，接纳帧率可能包含相同图像；应结合画面和功耗比较。

比较时保留 NR 模型、处理比例、抗闪烁和残差参数，使用相同场景和目标帧率。测试工具里的重复呈现次数与新内容帧率分别统计；仅提高重复呈现频率并不表示捕获到了更多新画面。

Beta4 的 Graphics Capture 在 Windows 25H2 及之后的 VRR 会话中请求 0 最小更新间隔，其他会话保留 1ms。0 设置或读回失败时，本次缩放会话回退到 1ms；停止并重新启用后才重新选择。若更新后出现 WGC 兼容性回归，可在启动进程前设置 `MAGPIE_WGC_MIN_UPDATE_INTERVAL_US=1000` 强制旧策略，再比较实际 VRR 和日志；只接受 `0` 或 `1000`，并在日志中记录选择和读回。该兼容开关不改变 NR 参数。

## English quick steps

The current DLSSNR status is separate from the last diagnostic record. **Skipped** means the group continues without DLSSNR; **stopped** means the whole group stopped. HDR-only parameter restrictions are not backend failures.

Open the details for the actual GPU/PCI ID, driver, DLL path/version and typed API result. For a missing or incompatible runtime, exit Magpie and restore the complete matching package in a new directory, preserving your original files. For insufficient memory, close other GPU-heavy programs or reduce source resolution/Multi Pass. A protected NGX process requires exiting Magpie from the tray and reopening it. Do not infer a GPU fault from `FeatureNotSupported`, or a driver-only fault from `OutOfDate`.

After the relevant change, enable the group and confirm **DLSSNR is running**. If it fails again, copy diagnostics and the matching logs. The effect-group action locates the recorded target when it still matches, and explains when the target has changed.

For a VRR-related frame-rate drop, compare with actual VRR active in both runs; switching focus to another application may deactivate it. For SDR window capture, select the existing `DwmSharedSurface` capture method in the matching profile and restart scaling. Compatibility depends on the game's shared surface. If capture fails or the image is incorrect, select `Graphics Capture` and restart. HDR capture continues to require `Graphics Capture`. Keep the NR model, resolution, anti-flicker and residual settings identical when comparing.

With duplicate-frame detection enabled, Beta4 reduces polling checks after a polled source such as DWM remains unchanged, then resumes normal capture when motion returns. Without duplicate detection, accepted-frame counts may include identical images; compare the visible motion and power as well.

Beta4 requests a zero WGC minimum update interval for VRR sessions on Windows 25H2 and later, retaining 1ms elsewhere. A zero setter/readback failure keeps that scaling session on 1ms. For a compatibility regression, set `MAGPIE_WGC_MIN_UPDATE_INTERVAL_US=1000` before launching Magpie to force the previous policy; only `0` and `1000` are accepted. Selection and readback are logged, and NR parameters are unaffected.
