# Magpie 实验分支文档索引

- [0.6.11 Beta1：DLSS 参数、SM86、VFX 浮窗与暂停截图](todos/20261022-dlss-runtime-presets-antiflicker-TODO.md)：四个独立分支已整合，0.6.11-beta1 已完成构建、包校验及本机部署启动；VFX 按用户意见暂定通过。[分支与验证记录](reviews/20261022-v0.6.11-beta1-implementation.md)。


## 0.6.11 待审核分发 / Distribution under review

[完整中英更新说明](../RELEASE_NOTES_v0.6.11-experimental.md)汇总 0.6.10 之后的变化。分发候选整合性能分析浮窗与双曲线、XeSS 输出间隔、DLSS 参数、SM86 0.4.0、默认社区 DLSSNR、暂停截图和时间戳命名。本地验证、部署和发布材料已完成；按用户后续授权创建的 [GitHub Draft](https://github.com/SAOG0721/Magpie/releases/tag/untagged-c23927d0f93baaa5e319) 已上传三个附件并核验，保持 Draft + Pre-release，等待审核。[本轮 TODO](todos/20261023-xess-output-interval-release-TODO.md)。

The 0.6.11 candidate integrates the resizable profiler and capture/output charts, XeSS output intervals, DLSS controls, SM86 0.4.0, the default community DLSSNR runtime, paused screenshots and timestamped filenames. The [GitHub Draft](https://github.com/SAOG0721/Magpie/releases/tag/untagged-c23927d0f93baaa5e319) contains the user-facing bilingual notes and three verified assets. It remains an unpublished Draft + Pre-release awaiting review.

- [错误状态与操作引导规范](../ERROR-REPORTING-GUIDELINES.md)：修改错误状态、提示或恢复入口时按需使用。
- [DLSSNR 状态与恢复](../DLSSNR-TROUBLESHOOTING.md)：检测结果、处理入口及恢复确认。
- [报错引导与 DLSSNR 状态细分 TODO](todos/20261013-error-guidance-and-dlssnr-status-TODO.md)：功能分支实现与独立编译验收交接。

- [2026-10-12 FG 设置与空闲重绘源码交接](reviews/20261012-fg-settings-idle-source-handoff.md)：FSR3 默认必选 AMDOF 质量，四类 FG 共用重复检测与空闲重绘。源码完成；本轮未编译部署，原生与 GPU 验收待执行。

帧率与刷新及配置页布局：原 `feature/0610-frame-refresh-rtss-auto` 分支在整页 280 DIP 布局基础上，完成模式外框圆角、10 处单位外置及 RTSS 独立分组。见[第五阶段实施与交接](reviews/20261011-rtss-grouped-units.md)及[TODO R01–R06](todos/20261011-frame-refresh-modes-TODO.md)。最新设计参照为 `profile-rtss-grouped.html`；R06 / S07 / V07 原生视觉与运行验收、产品编译部署交给后续会话。[第四阶段](reviews/20261011-profile-controls-unified.md)、[第三阶段](reviews/20261011-frame-refresh-visual-alignment.md)、[第二阶段](reviews/20261011-frame-refresh-rtss-auto.md)与[第一阶段](reviews/20261011-frame-refresh-modes.md)保留为历史。

当前公开版本为 [Magpie Experimental v0.6.10](https://github.com/SAOG0721/Magpie/releases/tag/v0.6.10-experimental)，其[中英 Release Note](../RELEASE_NOTES_v0.6.10-experimental.md)已按线上正文同步。`version.json` 和公开下载入口保持指向该已发布版本。0.6.11 Draft 审核及发布完成后再更新公开入口。

HDR 兼容模式更新开发中：见[功能文档](design/20261005-hdr-compatibility-update-functional-spec.md)和[开发需求与进度](todos/20261005-hdr-compatibility-update-development-TODO.md)。本轮进行源码和针对性回归，应用完整编译及部署留给后续会话。

帧同步当前状态：保留 Front Edge 默认，普通效果与 DLSS FG 支持 Async／Reflex 驱动基础限帧，XeSS FG 由 XeLL 接管；自动回退仅记简短日志，实时参数面板不显示运行状态。见 [使用指南](../FRAME_SYNC_GUIDE.md) 与 [实施记录](reviews/20260909-v0.6.7-frame-sync-modes.md)。

HDR 当前状态：旧配置页入口保持隐藏，新旧配置中的旧开关保持关闭；效果器选择器新增「HDR 组件」分类，仅显式转换链按节点启用 HDR 捕获／输出。见 [HDR 组件实施记录](reviews/20260908-v0.6.7-hdr-conversion-effects-plan.md)。

历史版本 [Magpie Experimental v0.6.8](https://github.com/SAOG0721/Magpie/releases/tag/v0.6.8-experimental.1) 的[更新说明](../RELEASE_NOTES_v0.6.8-experimental.md)与[分发准备记录](reviews/20260914-v0.6.8-release-preparation.md)保留供查阅。下方 Beta、TODO 与部署状态均为当时的历史记录，具体使用说明以当前版本 Release Note 为准。

当前功能已包含 Beta 1–6 的全部迭代，见 [Beta 6 TODO](todos/20260910-v0.6.7-beta6-TODO.md)、[Reflex 驱动限帧](reviews/20260910-beta6-reflex-pacing.md)、[HDR 组件](reviews/20260908-v0.6.7-hdr-conversion-effects-plan.md) 与 [参数输入验证](testing/PARAMETER-INPUT.md)。历史 [Beta 6 说明](../RELEASE_NOTES_v0.6.7-beta6.md)、[Beta 1 说明](../RELEASE_NOTES_v0.6.7-beta1.md)、[0.6.6 说明](../RELEASE_NOTES_v0.6.6-experimental.md) 保留。

- [0.6.6 参数交互与本地验证](reviews/20260906-v0.6.6-parameter-interaction.md)
- [0.6.6 停止缩放后的过期回调保护](reviews/20260906-v0.6.6-capture-shutdown.md)
- [0.6.6 NGX 异常后的死锁保护](reviews/20260906-v0.6.6-ngx-deadlock.md)

- [r10 DLSSNR 参数延迟重新启用](reviews/20260905-r10-dlssnr-parameter-restart.md)
- [Front Edge Sync 使用说明](../FRAME_SYNC_GUIDE.md)


本目录只收纳实验功能的路线图、TODO、测试矩阵和运行记录。上游通用 Wiki 文档继续保留在 `docs/` 根目录，避免实验说明与用户文档混在一起。

## 当前工作

- [效果器用户说明重写 TODO](todos/20261013-effect-description-user-copy-revision-TODO.md)／[样稿、全量审查与交接](reviews/20261013-effect-description-user-copy-revision.md)：已重写 165 项双语说明并整理 40 项分类／家族说明；减少防御句，聚焦自身用途，统一新旧表达。文案 review 与静态检查完成；编译部署及界面／GPU 验收待后续会话执行。
- [效果器说明维护规范](EFFECT-DESCRIPTION-GUIDE.md)：已修订为用户理解和选择优先，明确比较／组合／参数信息的归属；技术来源与证据要求继续保留。
- [上一轮效果器说明 TODO](todos/20261013-effect-descriptions-and-guidelines-TODO.md)／[逐项审查与交接](reviews/20261013-effect-description-standards.md)：保留 165 项事实核对、分类／进阶入口及 VFG RCAS ID 修正记录。本轮文案重写结果见上方；编译部署及实机验收交给独立会话。

- [DLSSFG sm86 启动修复源码交接](reviews/20261011-issue37-dlssfg-sm86-startup-fix.md)：原功能分支补齐显式 bundle 加载和 NGX 搜索目录；独立 loader 四场景通过，修改后的 Magpie 编译与真实补帧由后续会话验收。

- [DLSSFG Beta7 启用失败修复 TODO](todos/20261011-issue37-dlssfg-sm86-startup-failure-TODO.md)：`FeatureNotFound` 实机记录与独立加载探针；补齐 bundle 准备和 NGX 搜索路径。

- [Issue 37：DLSSFG 社区兼容接入开发交接](reviews/20261011-issue37-dlssfg-sm86-implementation.md)：默认统一接入、最高 4×；源码候选已写入，编译与实机验收交后续会话。
- [Issue 37：DLSSFG 社区兼容接入 TODO](todos/20261011-issue37-dlssfg-sm86-unified-TODO.md)：原需求与完整验收清单。
- **Issue 19：FSR 3 FG 源码集成**：独立分支 `feature/fsr3-frame-generation`，固定 2×、SDR、捕获时间戳优先及捕获接收间隔回退，未限制 WGC。见 [TODO](todos/20261011-issue19-fsr3-frame-generation-TODO.md)、[源码与编译交接](reviews/20261011-fsr3fg-source-development.md)、[效果说明](effects/FSR3FG.md)。本会话没有编译、部署或实机验收。

- **0.6.10 Beta5 本地整合**：继承 Beta4 49de3f25，合入 RTSS e830078a 的初始化／能力判断和启动更新、NVIDIA VFX 999e94e2 的统一套件、截图 abfce2ef 的内容选择。见[中英说明](../RELEASE_NOTES_v0.6.10-beta5.md)。完整构建与本地验证以 reports/0610-beta5-20261009 的记录及包内清单为准，真实 GPU／UI／游戏验收另行记录。

- **0.6.10 Beta4 本地整合**：继承 Beta3 `3c071cc6`，整合 HDR `fac29926` 的手动链和试用组迁移、RTSS `50144ea0` 的固定布局／同步帧率显示及保存／恢复修复、VRR `9e5f89f7` 的开销与诊断更新。见[本版中英说明](../RELEASE_NOTES_v0.6.10-beta4.md)。下方记录保留开发阶段边界，完整构建、包与配置保留以本轮审计为准；实际 UI、RTSS、HDR／RTX／FG 和 VRR 性能继续单独验收。

- **0.6.10 Beta3 本地整合**：`release/0.6.10-beta3` 继承 Beta2 `6e50812a`，增量合入 RTSS `f5f534c2`；保留统一 VRR、开关样式、UWP／参数焦点和渲染显卡选择。RTSS 设置 C++ 176 项与结构合同、原有 11 组相关软件回归通过；补齐同步夹具并验证实际基础帧率通知。完整构建及本地包以本轮整合审计和清单为准。真实 RTSS 事务、Hook、游戏帧率和原生界面验收仍待进行。见[本版中英说明](../RELEASE_NOTES_v0.6.10-beta3.md)。下方功能交接文档保留其开发阶段状态。

- [0.6.10 VRR GPU 占用与性能 TODO](todos/20261009-v0.6.10-vrr-performance-TODO.md)：已在原 VRR 分支精简前台查询、默认诊断与发布快照，缓存模块／输出资格，移除完整复制前的冗余 GPU 清空，并接入显式工作量统计。源码合同检查与夹具生成通过；C++ 回归／构建／部署和等帧率 GPU 对照交给独立会话，GPU 增量根因仍待确认。[源码实施与诊断交接](reviews/20261009-v0.6.10-vrr-performance-source-development.md)。

- [0.6.10 VRR 统一入口、FG 与前台关联修正 TODO](todos/20261004-v0.6.10-vrr-unified-fg-foreground-TODO.md)：用户已反馈双开关、无 FG 时输出指示器正常，DLSS／XeSS FG 都使其消失；部署日志确认共同 FG 资格排除。已在原功能分支合入 Beta2 基线，收敛为高级选项中的“VRR 可变刷新率”，解除 FG／窗口模式排除，接入实际交换链绑定与主界面身份更新。四项静态合同及资源检查通过；C++ 编译／执行、部署和实测交给独立会话。[源码交接](reviews/20261005-v0.6.10-vrr-unified-fg-source-development.md)。

- [0.6.10 G-SYNC 输出前台身份兼容 TODO](todos/20261004-v0.6.10-gsync-output-foreground-TODO.md)：第一版查询兼容、真实业务查询隔离、生命周期及短时真实前台参考入口已由单独会话集成到 Beta2。后续双开关与 FG 扩展按上方新 TODO 推进；[初次源码实施与交接](reviews/20261004-v0.6.10-gsync-foreground-source-development.md)保留原型阶段记录。

- **0.6.10 Beta2 本地整合**：`release/0.6.10-beta2` 继承 Beta1，保留参数焦点／输入所有权和“性能”分组渲染显卡选择；2026-10-05 增量合入 `df0e1ac8` 的统一 VRR／DLSS 与 XeSS 前台兼容，产品仅保留一个默认关闭的开关。修正前台测试夹具重名并完成 11 组软件回归。见 [本版中英说明](../RELEASE_NOTES_v0.6.10-beta2.md)。上下功能分支交接状态保留当时的开发记录；完整构建、部署与准确源码以本轮整合审计和包清单为准，实际游戏、输入和 G-SYNC 验收继续保留。
- [069 RTSS 目标应用限帧开发](todos/20261005-v0.6.9-rtss-target-frame-rate-TODO.md)：`feature/069-rtss-target-frame-rate` 已接入配置/四部分界面、工具入口、公开配置接口及会话同步与一次恢复；Beta3 动态文字读取错误资源表导致的配置页闪退已修复，209 项源码合同、325 项独立原生资源检查及原有刷新/GPU 位置合同通过。修复尚未编译部署到产品或进行真实写入与游戏验收，详见[闪退修复记录](reviews/20261005-rtss-profile-page-crash-fix.md)、[实施与编译交接](reviews/20261005-v0.6.9-rtss-target-frame-rate-implementation.md)和[功能文档](design/20261005-v0.6.9-rtss-target-frame-rate-plan.md)。

- [RTSS 能力判断与固定门槛整改 TODO](todos/20261009-rtss-capability-checks-TODO.md)：按实际接口、字段和操作条件判断可用性。第一阶段 10 项已完成：移除版本/名称及整项拒绝、解除同步模式的无关手动值依赖，开放可验证的离线配置操作，恢复只尝试一次且不阻断开始。源码与隔离回归通过，第二阶段 4 项安装发现、架构、格式和范围扩展后置；产品构建/UI/FPS 交给独立会话。[实施与交接](reviews/20261009-rtss-capability-checks.md)。

- [RTSS 状态分段与 Magpie 初始化及管理员启动 TODO](todos/20261009-rtss-status-magpie-init-and-admin-TODO.md)：已确认优先同行、按内容类型整段换行，三按钮统一普通操作样式，初始化 Magpie 独立不限帧配置且保留其他设置，主程序启动默认请求管理员权限。源码及隔离回归已完成，真实 UI、UAC/自启动与产品 FPS 待独立会话验收，见[实施与交接](reviews/20261009-rtss-status-magpie-init-and-admin.md)。

- [RTSS 配置保存失败 TODO](todos/20261009-rtss-save-failure-TODO.md)：普通 FPS 的 Apply/Restore 已按当前值跳过无效 setter，失败阶段与日志已拆分。216 项配置事务、64 项会话事务、55 项本机隔离原生事务及 358 项当前资源检查通过；非零帧时间兼容方案已设计，本次已完成 Magpie 初始化的清零文件路径和默认管理员启动；Global/目标转正 FPS、缺失字段恢复与其他版本验证仍待完成；本机普通权限无法写安装目录，已加入明确权限检测。产品构建、部署及目标应用 FPS 由独立会话验收。[修复与交接](reviews/20261009-rtss-save-failure-fix.md)。

- [RTSS 界面文案精简与控件样式 TODO](todos/20261009-rtss-ui-text-and-style-TODO.md)：固定帧率行、只读同步 FPS、停止策略直接排列和精简文案已在原分支完成，工具按钮局部对齐沿用原生样式。297 项合同、176 项设置、54 项生产代码显示回归、334 项当前资源原生检查通过；产品构建、主题/DPI 与真实交互交给独立会话。[实施与交接](reviews/20261009-rtss-ui-text-and-style.md)。

- [069 RTSS 目标应用限帧开发](todos/20261005-v0.6.9-rtss-target-frame-rate-TODO.md)：`feature/069-rtss-target-frame-rate` 已接入配置、工具入口、公开配置接口、会话同步与一次恢复，并修复配置页动态文字读取错误资源表的问题。原始源码及资源检查见[闪退修复记录](reviews/20261005-rtss-profile-page-crash-fix.md)和[实施记录](reviews/20261005-v0.6.9-rtss-target-frame-rate-implementation.md)；当前界面要求以[功能文档](design/20261005-v0.6.9-rtss-target-frame-rate-plan.md)和上方界面 TODO 为准，真实写入与游戏验收仍保留在交接清单。

- [069 渲染显卡常驻与选择状态整理 TODO](todos/20261004-v0.6.9-rendering-gpu-selection-TODO.md)：已在 `feature/069-rendering-gpu-selection` 实现“性能”分组中的常驻显卡选择、单卡自动／指定选项及设备缺失提示；79 项选择回归、51 项原有适配器回归、20 种语言资源合同和当前 IDL／XAML／页面语法检查通过。完整 Release 编译部署与实际 UI／热插拔／GPU 验收交给后续独立会话。[实施记录](reviews/20261004-v0.6.9-rendering-gpu-selection.md)。

- [参数焦点交接与会话窗口判定 TODO](todos/20261004-parameter-focus-coordination-TODO.md)：已在 `fix/069-uwp-window-owner` 完成根窗口判定、输入所有权、宿主层级及停止／重建协调，通过自动回归和测试窗口的原生层级验证；完整产品编译部署及游戏／GPU／VRR 验收待独立会话。

- [0.6.10 Beta1 VRR／G-SYNC 焦点诊断](reviews/20261004-v0.6.10-vrr-gsync-focus-diagnosis.md)：运行日志支持标准 DXGI 请求完整，但用户未看到指示器；非前台输出是首要待验证原因。真实前台对照、查询伪装／消息分离和 PresentMon 尚未执行，本轮仅调查与文档记录。

- **0.6.10 Beta1 本地整合**：`release/0.6.10-beta1` 以公开 0.6.9 源码为基线，合入当前 UWP 与标准 DXGI VRR 分支。见 [本版中英说明](../RELEASE_NOTES_v0.6.10-beta1.md)。下方 0.6.9 功能分支交接文档保留原开发阶段记录；本版构建与回归以整合审计及包清单为准，实机验收继续保留。

- [069 VRR／G-SYNC 标准呈现源码开发](reviews/20261004-v0.6.9-vrr-gsync-source-development.md)：`feature/069-vrr-gsync` 接入默认关闭的配置选项和共享 DXGI 策略。[TODO](todos/20261004-v0.6.9-vrr-gsync-nofocusloss-TODO.md) 保留 NoFocusLoss、最终输出预算及硬件验收；本轮未编译、部署或合并主线。

- **0.6.9 UWP 窗口兼容与死锁保护**：`fix/069-uwp-window-owner` 已实现两种协作模式、首帧准入与完整退役边界，保留 Win32 owner 和 NGX 防护，通过离线回归。完整构建／部署和实机验收待独立会话。[当前 TODO](todos/20261004-v0.6.9-uwp-window-coordination-and-lifecycle-TODO.md)、[第二阶段实施记录](reviews/20261004-v0.6.9-uwp-coordination-implementation.md)、[原定位](todos/20261004-v0.6.9-issues-65-67-uwp-window-owner-TODO.md)、[阶段 1 记录](reviews/20261004-v0.6.9-uwp-window-owner.md)。


- [069 “其他选项”按钮样式协调 TODO](todos/20261002-v0.6.9-effect-options-button-style-TODO.md)：已继承普通按钮的 14 号字与默认留白，保留 36 DIP 高度、纯文字、中性填充和零边框。144 组原生页头布局及页脚、容器和配置目录检查通过；单线程完整 Release 与包／编译资源校验通过。按用户明确授权关闭 Magpie 后，已归档旧完整目录、保留最新配置并替换至原 Beta3 路径，替换后的实际文件校验通过；Magpie 保持关闭，实际界面验收待完成。[实施与部署记录](reviews/20261002-v0.6.9-effect-options-button-style.md)。

- [069 效果组其他选项菜单与效果器说明 Review TODO](todos/20261002-v0.6.9-effect-options-and-description-review-TODO.md)：菜单源码与全部 162 项双语说明审查完成，入口沿用旧版 36 DIP 高度及当前普通按钮外观；目录生成、144 组原生页头布局、选择器模型与页面语法检查通过。已按单线程、低优先级和单逻辑核心完成 Beta3 构建部署及包／编译资源校验，游戏期间未启动应用；实际主窗口操作、完整 DPI 和安装后选择器验收仍待完成。[实施与验证](reviews/20261002-v0.6.9-effect-options-and-description-review.md)、[Beta3 部署记录](reviews/20261002-v0.6.9-beta3-deployment.md)。

- **Beta2 按钮与刷新控件样式对齐**：四个操作采用效果组／效果器的普通中性填充、零边框；六个下拉框与三组数值统一为 225 DIP，提供行内步进，并统一窄窗口换行与条件提示。完整 Release、原生标题／刷新控件测量及模型回归通过，已部署到原 Beta2；主窗口启动与三个现有配置保留检查通过。[TODO 与验证范围](todos/20261002-v0.6.9-beta2-ui-style-alignment-TODO.md)、[实施与部署记录](reviews/20261002-v0.6.9-beta2-ui-style-alignment.md)。

- **Beta2 刷新设置布局与文案**：重复帧检测移入“帧率与刷新 → 高级选项”，沿用全局设置；刷新设置说明改为简短陈述，移除重复保存提示。[修改与部署记录](reviews/20261002-v0.6.9-beta2-refresh-layout.md)。

- **Beta2 整数帧率修正**：实时面板与配置页的内容、光标、空闲三项均为 15–360 FPS、整数、普通步进 1；保留自动计算及旧配置浮点兼容。生产滑块 2,076 次编辑、统一刷新和工具栏回归通过，继续部署到原 Beta2。[修复与恢复记录](reviews/20261002-v0.6.9-beta2-integer-rates.md)。

- **0.6.9 Beta2 已部署**：全部 11 个功能分支已核对合入主线，包含 Beta1 fix1–3；完整 Release x64、19 组回归、45 组真实 NGX 合成场景、xBR 编译、资源／包校验和主窗口启动通过。保留旧配置语义及原 Toolbar 热键占用；实际游戏、Sunshine／外接显卡及完整 FG/HDR／多显示器验收待实测。[累计中英更新清单](../RELEASE_NOTES_v0.6.9-beta2.md)、[整合与部署记录](reviews/20261002-v0.6.9-beta2-integration.md)。

- [069 帧率、空闲重绘与光标刷新统一 TODO](todos/20261002-v0.6.9-unified-frame-refresh-TODO.md)：源码实现及离线验证完成，内容／光标／空闲 60／60／30 默认值和统一配置入口已接入；节奏方式位于内容选择与基础数值之间。保留旧配置语义，光标呈现独立于内容时钟；产品编译部署、原生 UI 与真实 GPU 验收交给 069 beta2。[实施记录](reviews/20261002-v0.6.9-unified-frame-refresh.md)。

- [069 效果组页面 TODO](todos/20261002-v0.6.9-effect-groups-page-TODO.md)：已在独立功能分支实现并合并回本地 experimental；标题右侧四个紧凑文字操作、新建按钮恢复及配置目录动作已实现，144 组标题、40 组新建、64 组隐藏容器、4 组目录处理回归和页面语法检查通过；完整编译部署及实际主窗口验收交给 069 beta2 会话。

- [069 PR #62 排除仅显示的间接适配器合并 TODO](todos/20261002-v0.6.9-pr62-display-only-adapters-TODO.md)：已在独立修复分支接入并合并回本地 experimental，保留原 PR 作者及历史；失败日志、51 项生产代码回归、3 个改动翻译单元语法检查和真实适配器查询通过。整项目构建、部署及 Sunshine DLSS NR／FG 实机验收交由 069 beta2 会话。关联 Issue #61。

- **0.6.9 Beta1 fix3 已部署**：消除工具栏 FPS 继承按钮基线导致的垂直下偏；128 组布局与既有交互回归、完整编译、包核验和主窗口启动检查通过。实际捕获视觉验收仍待实测；[更新说明](../RELEASE_NOTES_v0.6.9-beta1-fix3.md)、[实施与恢复记录](reviews/20261001-v0.6.9-beta1-fix3.md)。

- **0.6.9 Beta1 fix2 已部署**：[TODO](todos/20261001-v0.6.9-beta1-fix2-TODO.md)、[实施与验证](reviews/20261001-v0.6.9-beta1-fix2.md)、[参数文案 Review](reviews/20261001-v0.6.9-beta1-fix1-dlssnr-parameter-copy-review.md)。原生主窗口与配置迁移通过；完整 GUI／实际捕获验收待实测，保留两项快捷键注册冲突。

- [0.6.9 Beta1 fix1 TODO](todos/20261001-v0.6.9-beta1-fix1-TODO.md)：参数浮层、DLSSNR 进阶／名称提示与数值迁移、工具栏样式及居中吸附已部署到 Beta1。21 项完成，完整原生 GUI 与实际内容验收仍待完成；[实施与部署记录](reviews/20261001-v0.6.9-beta1-fix1.md)记录版本、包哈希、真实启动、三语提示、45 组 GPU 合成验证和恢复路径。

- [069 DLSSNR 细节控制 TODO](todos/20261001-v0.6.9-dlssnr-detail-control-TODO.md)：基础、保护／压缩、频率与色度时域控制已实施，旧模式兼容规则已由 Beta1 fix1 的统一迁移替代；[原始实施及验证记录](reviews/20261001-v0.6.9-dlssnr-detail-control.md)保留生产 shader 响应图与当时证据。

- [069 快捷键清除与“未设置”状态 TODO](todos/20261001-v0.6.9-shortcut-clear-TODO.md)：行内垃圾桶、弹窗清除、显式空绑定持久化与正常注销已实现，自动回归通过；真实界面验收和 069 beta1 编译部署交由独立会话。关联 Issue #34。
- [069 工具栏拖拽与上下停靠 TODO](todos/20261001-v0.6.9-toolbar-drag-TODO.md)：专用拖拽柄、上下蓝条、按应用 / 模式保存上下位置和会话期左右位置已实现，自动回归通过；真实界面 / GPU 验收与 069 beta1 编译部署交由独立会话。关联 Issue #50。
- [069 重复帧过滤优化 TODO](todos/20261001-v0.6.9-duplicate-frame-filter-optimization-TODO.md)：效果输出版本、旧帧复用、时序历史重置及 HDR 去重前置已实施，CPU / WARP 回归与源码语法检查通过；同步读回和 Dynamic 策略保留，069 beta1 产品编译部署与真实 SDK / 画质 / 性能验收交由独立会话。
- [069 光标刷新与主页选项 TODO](todos/20261001-v0.6.9-cursor-refresh-and-home-options-TODO.md)：应用配置光标策略、默认开启的 60 FPS 最低刷新、主页过滤及开发者入口已实施，386 项离线检查、相关回归和源码语法检查通过；[实施及 beta1 交接](reviews/20261001-v0.6.9-cursor-refresh-and-home-options.md)，产品编译部署与真实 UI / SDK / 性能验收交由独立会话。
- [069 DLSSNR 重复资源开销优化 To do](todos/20261001-v0.6.9-dlssnr-overhead-optimization-TODO.md)：入口共用缩放 / 引导、出口统一总残差、D3D12 集中调度与缓存优化已实施；自动回归及有限真实 NGX 冒烟通过，069 beta1 产品编译部署与完整画质 / 性能验收交由独立会话。
- [069 To do - xBR](todos/20260921-v0.6.9-xbr-TODO.md)：筛选标准 LV2 固定倍率、NoBlend 与 Hybrid 为首批范围；LV3、MLV4 和 Super-xBR 需经过画质/性能门槛后再决定是否进入 0.6.9。
- [DLSSNR Multi Pass 残差时域稳定路线](design/20260914-dlssnr-temporal-stabilization-routes.md)：比较无光流／光流累积、快慢历史、稳健统计、分频与联合升采样，说明当前 068 串联实现的接入边界和验证顺序。
- [v0.6.5 r8：参数状态、光流降级与 SR 入口收敛](todos/20260905-v0.6.5-r8-TODO.md)。
- [r8 本地版本说明](../RELEASE_NOTES_v0.6.5-r8-local.md)。

下列旧版条目保留历史；与 r8 记录不一致的方案已被取代。

- [v0.6.5 r4：统一光流方法与运动向量质量下拉菜单](todos/20260904-v0.6.5-r4-optical-flow-selection-TODO.md)：为 DLSS SR/FG/NR 定义 NVOF 4F/4M/4S/2M 四档，为 XeSSFG 增加 None/AMDOF/NVOF 方法选择、提供器专属质量档、能力错误与多消费者共享合同。
- [v0.6.5 r3 re fix2：恢复工具栏式的同帧浮窗呈现](todos/20260904-v0.6.5-r3-re-fix2-TODO.md)：移除提前生成 DrawData 和浮窗计时门控，让工具栏、参数窗与光标在实际前端帧内生成、绘制并统一 Present。
- [v0.6.5 r3 re fix1：浮窗与光标统一刷新调度](todos/20260904-v0.6.5-r3-re-fix1-TODO.md)：输入边沿即时推进，高频移动合并到正常前端 tick，并在交互期间使用约 8 ms 的按需刷新。
- [v0.6.5 r3：缩放期间调节效果参数](todos/20260904-v0.6.5-r3-TODO.md)：参数级区分实时应用与重新缩放后生效，并提供保存、还原和一次性完整重启流程。
- [v0.6.5 r2 fix4：参数列间距](todos/20260904-v0.6.5-r2-fix4-TODO.md)：将参数列间距增至 24 DIP，并保持分隔线居中。
- [v0.6.5 r2 fix3：参数列分隔线](todos/20260904-v0.6.5-r2-fix3-TODO.md)：在相邻可见参数列之间增加淡色细竖线，并随列显隐更新。
- [v0.6.5 r2 fix2：Residual 命名与 NR Style 换行](todos/20260904-v0.6.5-r2-fix2-TODO.md)：补齐两个 HSL Multiplier 名称，并让显式多行数值标签的说明行使用整列宽度。
- [v0.6.5 r2 fix1：参数列、残差语义与 DLSS 参数收敛](todos/20260904-v0.6.5-r2-fix1-TODO.md)：修正 Flyout 宽度、残差处理顺序、长标签、NR 范围与 Motion 开关。
- [v0.6.5 r2：参数列直展与 DLSSNR 残差精细控制](todos/20260904-v0.6.5-r2-TODO.md)：参数 Flyout 直接展开所有列，重排 DLSSNR 两列并增加 HSL 与有符号残差控制。
- [v0.6.5 r1：移除深度估算与效果参数多列分组](todos/20260904-v0.6.5-r1-TODO.md)：保留 Zero Depth 合同，移除学习型深度链，并为效果参数增加可选 GROUP 多列布局。
- [v0.6.1 Feature 4：缩放模式交互修复与引导性错误提示](todos/20260903-v0.6.1-feature4-interaction-errors-TODO.md)：修复拖拽闪退、重复新建不弹自动命名，并系统整理可恢复的失败提示与后续错误码拆分。
- [Frame Guidance 测试矩阵](testing/FRAME_GUIDANCE-TEST-MATRIX.md)：统一记录 Zero/Motion 两态、场景、性能和日志证据。

## 已完成的路线图

- [DLSS5 Frame Guidance 阶段 1–4](todos/completed/20260829-164413-DLSS5-FrameGuidance-short-term-TODO.md)：工程实现已完成，GPU 运行时验收转入当前 TODO。

## 已被 v0.6.5 取代

- [DLSS 组合兼容性短期 TODO](todos/20260830-DLSS-combination-compatibility-short-term-TODO.md)
- [DLSSNR 参数与性能短期 TODO](todos/20260829-DLSSNR-parameters-performance-short-term-TODO.md)
- [DLSSNR 性能与可观测性 TODO](todos/20260829-DLSSNR-performance-observability-TODO.md)

## 交接、发布与授权

- [实验分支 Git 工作流](GIT-WORKFLOW.md)
- [实验版发布规范](RELEASE-WORKFLOW.md)：版本与标签、双语 Release Note、附件布局、Draft 审核门禁及发布检查。
- [实验分支交接](../EXPERIMENTAL_HANDOFF_ZH.md)
- [v0.6.1 Hotfix 说明](../RELEASE_NOTES_v0.6.1-experimental-hotfix.md)
- [v0.6.1 已发布实验版说明](../RELEASE_NOTES_v0.6.1-experimental.md)
- [下一实验版计划](../RELEASE_NOTES_NEXT.md)
- [v0.6.0 Hotfix 说明](../RELEASE_NOTES_v0.6.0-experimental-hotfix.md)
- [v0.6.0 实验版说明](../RELEASE_NOTES_v0.6.0-experimental.md)
- [v0.5.9 未发布历史草稿](../RELEASE_NOTES_v0.5.9-experimental.md)
- [v0.5.8 实验版说明](../RELEASE_NOTES_v0.5.8-experimental.md)
- [v0.5.7 实验版说明](../RELEASE_NOTES_v0.5.7-experimental.md)
- [v0.5.6 实验版说明](../RELEASE_NOTES_v0.5.6-experimental.md)
- [v0.5.3 实验版说明](../RELEASE_NOTES_v0.5.3-experimental.md)
- [v0.5.2 实验版说明](../RELEASE_NOTES_v0.5.2-experimental.md)
- [实验包 README](../README-EXPERIMENTAL-RELEASE.txt)
- [第三方组件与再分发](../THIRD_PARTY_AND_REDISTRIBUTION.md)

## 目录约定

- `todos/`：按日期命名的执行清单；完成的阶段性清单归档到 `todos/completed/`。
- `testing/`：可复用测试矩阵、素材约定、截图/DDS/日志记录格式。
- 后续若增加设计说明，放入 `design/`；若增加一次性测试结果，放入 `results/YYYYMMDD/`。
