# XeSS 输出帧间隔修复与 0.6.11 发布准备

后续状态（2026-10-23）：用户已授权创建 Draft；用户说明、下载段落和包内文档已同步，三个附件远端核验通过。[Draft 待审核](https://github.com/SAOG0721/Magpie/releases/tag/untagged-c23927d0f93baaa5e319)，尚未公开发布。以下保留原始准备范围及完成记录。

用户要求定位、修复并部署到 0.6.11，收尾开发并准备正式发布，在创建 Draft 前停止等待检查。沿用不使用 Computer Use；保留用户配置和现有 RTSS `ReflexSetLatencyMarker=0`。

## 已确认原因与实现边界

XeSSFGPresenter 仅返回 SDK 的汇总 `framesPresented`，没有给分析器单帧时间戳。XeSS SDK 公共状态接口不包含逐帧时间；应用一次 Present 可包含多个输出，不能直接用它的调用间隔或 `1000/FPS` 代替。现有 XeSSFGPacing 的逐次 provider 输出采样仅用于兼容模式诊断，未接入图表，原生 2× 也未安装该观测入口。

复用已有固定运行库身份／原始指令验证和生命周期租约，在原生 2× 仅安装被动输出观测入口，不启用多倍兼容补丁或调度调整。未知运行库保留原生功能及缺测降级。输出间隔独立于汇总 FPS 入库，成功的真实事件才构成样本；停止、空闲、丢样和新代际均断开间隔。需验证入口实际回调、返回值和 2×／3×／4× 的样本数，不能仅以图表出现数字验收。

## TODO

- [x] 完成真实输出事件接入及受保护的原生 2× 观测；保留既有 SDK 调度行为。
- [x] 验证异步样本传递、丢样、失败提交、开关、重建及运行库恢复；18 组 ImGui 布局与历史回归通过。实际 Magpie 闲置和跨效果链测试随完整构建继续。
- [x] 实际 GPU 验证 2×／3×／4× 及分析器历史数据：8 轮 SDK 上下文、两轮五场景 Magpie 运行通过，三种 XeSS 倍率每轮均有三组实际间隔被消费者接收；GPU／FG 回归通过。
- [x] 汇总相对 0.6.10 的功能，补齐完整中英文发布说明与已知限制。
- [x] 从冻结提交 `4fc74a19` 构建 0.6.11：246 个文件、3,981 条三语言资源、两个内嵌目录、EXE/PDB 和可选附件通过校验；19:06 备份覆盖正式目录。
- [x] 准备 RELEASE_BODY、分发清单、校验值、Draft 参数和维护者检查步骤；已停止在 Draft 创建前，没有远端写入。

发布沿用 `v0.6.11-experimental` / `Magpie Experimental v0.6.11` / `Magpie-Experimental-x64.zip`。本轮不推送、不创建远端标签、Draft 或上传附件，也不更新公开更新入口；后续流程按 `docs/experimental/RELEASE-WORKFLOW.md`。

结果见 [修复、验证与发布准备记录](../reviews/20261023-xess-output-interval-release-preparation.md)。用户检查是下一阶段；未授权自动继续 Draft 或发布。
