# XeSS 输出帧间隔修复与 0.6.11 发布准备

## 2026-10-23 后续：用户说明整理与 Draft

按用户后续授权，合并重复的下载／升级说明，完整中英文 Release Notes 改为面向用户的功能和使用说明。正文提交为 `364951abad60b32e264ad63e7c40fdced4b0296d`，源码、包内说明和 GitHub 正文一致；程序仍来自 `4fc74a196993c883ce2f01e96e8ed8d81f1490e6`，全部非文档文件哈希不变。

[GitHub Draft](https://github.com/SAOG0721/Magpie/releases/tag/untagged-c23927d0f93baaa5e319) 已创建，状态为 Draft + Pre-release，三个附件的远端大小与 SHA-256 均匹配。主包重新打包后为 609,286,271 字节，SHA-256 为 `f24333239bb6f4f1b09eac996c71d2ada74f34e950547a18fae80633a440bc84`；两个可选附件保持原值。最新证据为发布目录中的 `GITHUB-DRAFT-VERIFICATION.json` 和工作区 `.tools/create-draft-0611-20261023/`。

源码已推送至 `experimental` 与 `release/0.6.11-preparation`；未公开发布，公开更新入口保持 0.6.10。下一步是用户审核 Draft。下文保留此前部署准备阶段的历史记录。

2026-10-23。产品构建提交 `4fc74a196993c883ce2f01e96e8ed8d81f1490e6`；北京时间 19:06 完成备份覆盖，版本保持 0.6.11。发布文件已准备，停止在创建或更新 GitHub Draft 之前，等待用户检查。[TODO](../todos/20261023-xess-output-interval-release-TODO.md)。

## 原因与修复

此前 XeSSFGPresenter 只返回 `xefgSwapChainGetLastPresentStatus` 的汇总帧数，没有给图表逐帧时间戳。SDK 一次应用 Present 可产生多个输出，不能把应用调用间隔或 FPS 倒数当成输出间隔。公共状态接口的语义与 [Intel 开发指南](https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md)一致；本次实际检查和测试使用本地固定 3.0.1 SDK 及随附 `libxess_fg.dll 1.3.1.78`。

现有兼容层已有逐次 provider 输出观测入口，但仅用于多倍兼容路径诊断，未接入分析器；原生 2× 没有安装该入口。本次复用该入口，在原生路径只安装被动观测，不启用多倍兼容修改和调度补丁。原始 DLL 文件保持不变，加载文件哈希、PE 身份、原始指令和页保护全部通过现有校验后才观测；未知原生运行库保留功能并显示缺测。入口只在上下文创建前安装、工作线程停止后恢复。

回调记录实际时间戳，只有成功输出形成区间。固定容量队列传送两个真实端点；回调不等待消费者、不分配内存、不格式化日志。失败、丢样、乱序完成、采样开关、空闲和新代际不会连接出虚假长区间。分析器将这些区间独立写入历史，SDK 汇总帧数继续负责输出 FPS，避免重复计数。它表示 provider 输出提交事件，不是显示器扫描时间。

## 验证

- 异步队列与生产历史：成功／失败、丢样、乱序、旧回调、开关、64 项容量回卷、重复消费、未知 SDK 计数、暂停／恢复和代际隔离通过；独立区间不增加 FPS。
- XeSS 兼容层：既有回滚故障、原生／兼容切换和还原检查通过；新增检查确认原生观测只改变一个已验证入口，其他兼容补丁位置保持原始字节，未知原生入口安全退回缺测。
- GPU SDK 探针：同一进程 8 轮原生 2×／兼容 3×／4×、诊断对照及销毁还原通过。最初连续采样探针中，60 次原生 2× 输入对应 119 次真实输出调用；正式带关闭／恢复采样的用例得到 107～212 个有效间隔，并进入生产历史类。数据来自实际回调，不由倍率生成。
- 硬件 D3D11 和 WARP 各 20 个 GPU 计时样本通过；445 项 FG 策略、普通／补帧去重策略、历史回归及 18 组真实 ImGui 语言／主题／DPI 布局和输入检查通过。
- 完整 Release x64 构建通过；246 个目录／ZIP 文件逐项哈希一致，英／简中／繁中编译资源分别 1,362／1,362／1,257 条逐项匹配，两个内嵌效果器目录各 165 项匹配。EXE/PDB GUID `21a75015-5ad3-574f-72e8-a0584dcfa3b2`，age 1，一致。

实际 Magpie 使用原程序名、专属便携配置和夹具自有动态图形源，两轮均执行普通缩放 → DLSS → XeSS 2× → 3× → 4×。所有场景包含分析器开关、源窗口尺寸变化、静止和恢复，正常退出且没有 GPU 查询失败。两轮各三个 XeSS 场景均记录到三组 `XeSSFG profiler output intervals`，证明真实输出区间已到达前端消费者。图形排版和输入由生产 ImGui 测试覆盖，没有把自动化测试写成物理鼠标、HDR／VRR 或长期游戏验收。

保留此前已获授权的 Magpie 专用 RTSS `ReflexSetLatencyMarker=0`。Magpie／Global 配置及 RTSSHooks64.dll 哈希均未变化。发布说明保留相关限制，不声称修复了 RTSS 内部问题。

## 部署、源码和发布材料

正式目录：工作区 `worktrees/release/v0.6.11-experimental/Magpie-Experimental-x64/`。先备份并核验上一版 `a72c2b70` 程序、ZIP、78 个用户配置文件、快捷方式、部署收据及当前 RTSS 配置，再覆盖并逐项复核。额外 18 个安装目录文件保留，配置未变，快捷方式指向原目录，最终检查时程序退出。备份及证据根目录为 `.tools/release-0611-xess-interval-20261023/`，备份位于其中 `backup/`。

本地 `release/0.6.11-preparation` 从 `07b1ad37` 快进到准确构建提交。主工作区 `source/experimental` 的已提交历史是候选祖先，其原有未提交内容没有移动、合并或覆盖。没有推送远端。

正式发布目录中的 `RELEASE-REVIEW.md` 是用户检查入口，另有完整双语 `RELEASE_BODY.md`、`DISTRIBUTION-MANIFEST.json`、`SHA256SUMS.txt`、`LOCAL-PREPARATION-AUDIT.json` 和本地 `DRAFT-PAYLOAD.json`。正文与源码／包内 RELEASE-NOTES 完全一致，涵盖相对 0.6.10 的全部本轮功能、DLSS A/C 备注、B 的限制和已知 RTSS 风险。

| 附件 | 字节 | SHA-256 |
|---|---:|---|
| Magpie-Experimental-x64.zip | 606,228,689 | `f95a1fda82b79743e16df067a4055dab05091830346ae45f9c00d766660e17e8` |
| DLSSNR-DLL-Options.zip | 379,128,973 | `1f900df1063e3ffca632220e7f1830ab0b22ea56aac629ad9e858c3a13115ec1` |
| NGX_OTA_Switch.bat | 4,294 | `d0f96b57f7e91101460eeb68cd618b2c11866a4dfc6901741c17acecac04c465` |

符号只留在 `worktrees/release/v0.6.11-final-candidate-20261023/Magpie-Experimental-x64-symbols/`。未执行 OTA 工具，未使用 Computer Use，未创建或修改 GitHub Draft／标签、上传、发布或更新公开入口。下一步是用户检查；确认文件后才进入仓库发布规范的 Draft 阶段。
