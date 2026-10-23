# Magpie 实验版发布规范

本文规定 `SAOG0721/Magpie` 实验版 Release 的准备、审核、发布说明与附件结构。目标是让源码、标签、Release Note 和二进制附件能够相互核对，并防止未经审核的 Draft 被意外发布。

## 1. 版本与命名

常规实验版统一使用以下格式：

| 项目 | 格式 | 示例 |
| --- | --- | --- |
| 版本号 | `X.Y.Z` | `0.6.0` |
| Git 标签 | `vX.Y.Z-experimental` | `v0.6.0-experimental` |
| Release 标题 | `Magpie Experimental vX.Y.Z` | `Magpie Experimental v0.6.0` |
| 主包 | `Magpie-Experimental-x64.zip` | `Magpie-Experimental-x64.zip` |
| 仓库说明快照 | `docs/RELEASE_NOTES_vX.Y.Z-experimental.md` | `docs/RELEASE_NOTES_v0.6.0-experimental.md` |

- GitHub Release 始终标记为 **Pre-release**。Draft 只是审核状态，不是版本类型。
- 首个紧急修复可使用 `vX.Y.Z-experimental-hotfix`；同一基础版本继续修复时在末尾增加序号，例如 `.2`。
- 标签必须指向完成构建和运行验证的准确提交。已推送或已发布的标签不得移动、覆盖或重建。
- 常规版的 `version.json`、程序“关于”页、包内 `build-manifest.json`、标签和 Release 标题中的版本必须一致。Hotfix 必须明确继承的基础版本，并在标签、标题和附件名中使用一致的 Hotfix 后缀。

## 2. 发布门禁

发布过程分为四个阶段，每个阶段的授权不能自动延伸到下一阶段。

### 阶段 A：准备源码和文件

1. 在功能分支完成实现和测试，再合并到 `experimental`。
2. 更新版本说明、包内 README、第三方声明及相关测试文档。
3. 从准备发布的提交构建完整主包和全部可选附件。
4. 核对发布目录的绝对路径，并按分发清单逐项检查本地文件名、文件是否存在、大小、SHA-256 和安装说明。
5. 文件全部准备完成后停止，交由维护者进行本地检查。

在维护者明确确认“文件检查通过”前，不创建 GitHub Release Draft，也不上传附件。

### 阶段 B：创建 Draft

只有在维护者明确批准后才执行：

1. 将准备发布的源码和文档推送到 GitHub。
2. 确认远端提交与本地构建清单记录的提交一致。
3. 创建 GitHub Release，设置 `draft: true`、`prerelease: true`。
4. 上传全部附件，但保持 Draft，不发布。
5. 将 GitHub Draft 的正文、附件名称、大小和哈希发给维护者复核。

“创建 Draft”“上传 Release”或“提交文件”均不等于授权发布。

### 阶段 C：审核 Release Note

- 中文与英文内容必须完整对应；不能只给英文摘要代替完整英文说明。
- 维护者在线修改正文后，应将最终内容同步回仓库中的版本说明文件。
- 仅删除维护者明确指定的过期 Draft。不得顺带删除其他 Draft、Release 或标签。
- 修改 Draft 正文或附件后仍保持 `draft: true`、`prerelease: true`。

### 阶段 D：发布

只有收到明确的“发布该版本”指令后，才将已审核的 Draft 发布。发布前最后确认：

- 标签、目标提交和版本号正确；
- `draft` 将从 `true` 变为 `false`，`prerelease` 仍为 `true`；
- 正文为最终中英文版本；
- 附件齐全且哈希没有变化；
- 没有同时发布其他 Draft。

## 3. Release Note 结构

常规版本使用“完整中文在前、分隔线、完整英文在后”的结构：

```markdown
# Magpie Experimental vX.Y.Z

## 功能更新

### Magpie 本体易用性更新

- 界面、配置管理、默认行为、错误引导和兼容性变化。

### 效果器更新

- 效果器功能、参数、画质、性能和稳定性变化。

## 应该下载哪个文件？

首次安装和常规升级使用完整主包 `Magpie-Experimental-x64.zip`；升级前备份设置并完全退出旧版，完整解压到新目录后运行。

### 可选组件（如有）

### 排错工具（如有）

Contributor: [GitHub 用户名](https://github.com/用户名) 具体贡献。

---

# Magpie Experimental vX.Y.Z User Guide

<!-- 与中文信息等价的完整英文版本，使用相同顺序。 -->
```

写作要求：

- 正文面向用户，说明功能变化、操作入口、生效方式和必要限制。实现过程、调试经验、测试轮次及本机验证覆盖范围记录在 `RELEASE-REVIEW.md` 或 `docs/experimental/reviews/`。
- 功能变化按用户能识别的功能分组，可使用“Magpie 本体易用性”和“效果器”等分组。只有下载内容或升级步骤确实不同时才按版本分流，相同则合并。
- 新增或改变的参数写明范围、默认值、步进与生效方式；仅在升级行为有变化时补充旧配置处理，完整技术解释放在对应使用文档。
- 画质、性能或稳定性存在取舍时必须明确写出，不能只描述收益。
- 性能数字必须同时给出测试 GPU、分辨率、测试条件、单位和近似范围，并说明实际结果会因环境而异。
- 安装说明使用用户在压缩包中实际看到的文件名和目录，不使用内部构建目录名称。
- 已知问题应提供可操作的排查或回退方法。
- Contributor 使用准确的 GitHub 用户名、主页链接和具体贡献内容，不用含糊的“感谢贡献”。README 和 `.all-contributorsrc` 的展示与 GitHub 自动统计是独立事项。
- 面向用户的 Release Note 不重复列出附件哈希；GitHub 附件界面提供公开校验信息。本地准备仍使用 `SHA256SUMS.txt` 和审计记录逐项核对，附件更新后必须同步更新本地校验值。
- 发布目录的 `RELEASE_BODY.md` 为审核主稿；定稿后同步到仓库版本说明快照和包内 `RELEASE-NOTES.md`，三份正文保持一致。包内 README 保留简短安装说明和使用要点。若重新打包，更新本地文件清单与校验值。

## 4. 附件与目录结构

### 完整主包

- `Magpie-Experimental-x64.zip` 是所有用户可独立安装的完整包。
- 本地主发布文件名与分发清单、`SHA256SUMS.txt` 一致。网盘需要带版本号的名称时，另建渠道副本并记录对应关系，保留主发布文件名和原内容。
- 用户必须完全退出 Magpie，将 ZIP 完整解压到新目录后运行。
- 不允许让用户在压缩包内直接运行，也不能把“只替换 `Magpie.exe`”写成常规更新方式。
- `Magpie.exe` 与 `resources.pri` 必须来自同一次完整构建。涉及 UI 资源或索引映射时，两者必须一起更新。
- 包内保留 `LICENSE-Magpie.txt`、`THIRD-PARTY-NOTICES.md`、`README-Experimental.txt` 和 `build-manifest.json`。

### DLSSNR DLL 选项包

- 默认来源固定为维护者指定的 `release/v0.6.10-experimental/updated-20261017-two-options/DLSSNR-DLL-Options.zip`。主包使用其中 `Community-Patched-310.9.16-RTX20-RTX30-RTX40-Series/nvngx_dlssnr.dll`；分发用的可选 ZIP 原样复制，不重新压缩或恢复已移除的 SF-v2。
- 准备依赖时，将上述社区 DLL 提取为 `dependencies/dlssnr-runtime/nvngx_dlssnr.dll`，同时把原 ZIP 复制为该目录下的 `DLSSNR-DLL-Options.zip`。普通构建和 Release 使用同一个 `DLSSNRRuntimeDir`；独立 worktree 的本地 props 可将它设为工作区内该目录的绝对路径。第三方二进制不提交 Git。
- 已选输入的 SHA-256：社区 DLL `A8A237A967469C455A84654932B513AB656FBAA1E0C44504B1E1BF0890F902B8`；可选 ZIP `1F900DF1063E3FFCA632220E7F1830AB0B22EA56AAC629AD9E858C3A13115EC1`。构建复制时校验完整性；开发者仍可显式指定其它运行库目录。
- `Build-Release.ps1`（含 `-SkipBuild`）从同一依赖目录刷新主包 DLL，并将可选包放在主 ZIP 旁。缺少输入文件时直接报错，不能沿用旧包冒充新的默认来源。这里的可选包 README 中保留原有 0.6.10 来源版本标记。
- 使用 `DLSSNR-DLL-Options.zip` 命名；包内只保留 NVIDIA 原版 RTX 50 和社区兼容 RTX 20/30/40 两个目录，以及安装说明、文件版本和 SHA-256。
- 包内明确区分 NVIDIA 原版与社区兼容版本，并附带切换说明、文件版本和 SHA-256。
- 替换前必须完全退出 Magpie。第三方 DLL 不进入源码仓库或 GitHub 自动生成的源码归档。

### 最小更新与 Hotfix

- 最小更新包不是完整安装包，标题和正文开头必须明确写出这一点。
- 必须说明适用的准确基础版本、受影响用户、需要替换的全部文件以及恢复完整包的方法。
- 不得静默替换已发布附件；使用新的 Hotfix 标签和独立 Pre-release，保留原版本记录与哈希。
- 如果修复涉及 UI 资源，最小包必须同时包含匹配的 `Magpie.exe` 和 `resources.pri`。

## 5. 构建与附件检查

发布构建使用所选源码工作树中的 `scripts/Build-Release.ps1`。下文 `release/` 均相对于工作区根目录，即主检出 `source/` 的父目录；不相对于当前工作树或终端目录。

源码可位于 `source/`、`worktrees/<名称>/`、`.worktrees/<名称>/source/` 等已登记的 Git 工作树。脚本通过 Git 共享目录确定主检出和工作区根目录，解析失败即报错；构建前输出源码、工作区和发布文件的绝对路径。正式分发始终写入工作区根目录的 `release/<版本>/Magpie-Experimental-x64/`，中间文件归入该版本的 `obj/`，同级保存 ZIP 和独立符号目录。

现有本机运行目录和快捷方式以对应部署记录为准，准备分发文件不迁移或覆盖它们。`scripts/Deploy-Local067.ps1` 是历史 `0.6.7-local` 部署入口；正式打包脚本拒绝将该历史运行目录作为可清空的打包目标。本地更新没有创建 GitHub Draft、上传或发布的含义。

正式附件至少满足：

- 构建所用的源码工作树无未提交修改，`build-manifest.json` 中 `sourceDirty` 为 `false`；
- Release x64 构建为 0 error，运行时必需文件齐全；
- 清单中的版本、完整提交 SHA、平台、功能开关与文件哈希正确；
- 主包能够解压到新目录并启动，关键功能完成一次目标 GPU 验收；
- 可选组件能够从第一分卷完整解压，根目录和覆盖路径符合本规范；
- ZIP/7z 内没有 `bin/`、`obj/`、PDB、LIB、EXP、缓存、日志、本机路径、凭据或未授权 SDK 文件；
- 每个第三方文件已按 `docs/THIRD_PARTY_AND_REDISTRIBUTION.md` 完成再分发审查；
- 上传后的附件名称、字节数和 SHA-256 与本地记录逐项一致。

## 6. 发布记录维护

- `docs/RELEASE_NOTES_NEXT.md` 只描述下一版计划和当前公开版本入口。
- 每个已发布常规版本和 Hotfix 都保留对应的本地 Markdown 快照。
- 未发布的历史草稿必须在文档标题和索引中明确标记，不能伪装成已发布版本。
- GitHub 上最终正文发生修改时，应追加文档提交同步本地快照，不改写已经推送的历史。
- 发布完成后更新 `docs/experimental/README.md` 的索引，并检查 `version.json` 指向当前常规版本或维护者指定的更新入口。

## 7. 历史发布经验

| 版本 | 形成的规范 |
| --- | --- |
| v0.5.7 | 多附件 Release 必须逐项说明用途并记录校验值；专有 DLL 选项包与源码分离。 |
| v0.5.8 | 最小更新必须限定准确基础版本；`Magpie.exe` 与 `resources.pri` 的 UI/逻辑映射不能拆开更新。 |
| v0.5.9 未发布草稿 | 删除 Draft 不等于删除源码标签；未发布文档必须明确标记，Draft 也不能被当作已发布版本。 |
| v0.6.0 | Release Note 使用完整中英双语；TensorRT 分卷解压后直接得到需要合并的全部组件，不增加 `256`、`runtime` 或版本包装目录；Contributor 写明具体贡献。 |
| v0.6.0 Hotfix | 已发布版本的紧急修复使用独立 Pre-release；最小包必须声明不能独立安装，并说明基础版本和完整替换步骤。 |

## 8. 最终检查清单

- [ ] 功能和 GPU 验收通过。
- [ ] 发布目录的绝对路径位于工作区根目录 `release/<版本>/`，分发清单中的每个文件均按原名存在。
- [ ] 源码、Release Note、包内 README 和版本信息一致。
- [ ] 主包及所有可选附件已在本地准备并由维护者检查。
- [ ] 远端源码提交与 `build-manifest.json` 一致。
- [ ] Draft 同时设置 `draft: true` 和 `prerelease: true`。
- [ ] 中英文正文信息完整对应，只含用户功能、操作和必要限制；相同的下载／升级步骤已合并。
- [ ] 安装路径、默认值、范围、步进、风险和回退方法准确。
- [ ] Contributor 的用户名、链接和贡献内容准确。
- [ ] 每个附件的名称、大小和 SHA-256 已复核。
- [ ] 收到明确发布授权，并且只发布指定 Draft。
