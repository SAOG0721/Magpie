# FG 参数顺序与 B 备注：0.6.11 验证及覆盖部署

2026-10-23，Windows，本机 DPI 96（100%）。按用户授权完成本地覆盖部署，GitHub 发布继续取消。本轮不使用 Computer Use。

## 实现及构建身份

- 分支：`ui/dlss-fg-parameter-order`；干净构建提交：`2345b1487004677c506af6e4689e67ca2f51f201`。
- 已确认包含 `9b99180c`（顺序）、`30652c32`（B 标签）及 `05f243e3`（此前已部署的 SR 路由修复）。
- 参数顺序：帧数倍率 → 模型预设 → 光流方法 → 对应光流质量；参数名称、保存值及默认值保持原样。
- B 标签：简繁中文为 `B（UI 重建 - 不生效）`，英文及描述符回退为 `B (UI Recomposition - No Effect)`。A/C 继续仅显示字母，默认仍为自动；未添加额外说明。
- 完整 Release x64 构建版本 `0.6.11`；候选位于工作区 `worktrees/release/v0.6.11-fg-ui-candidate-20261023/`。复用原会话已完成的构建，没有因会话切换重新构建产品。

## 原生 XAML 验收

从该候选的实际编译对象、XAML、`resources.pri` 和效果描述符链接隔离测试程序，只替换测试启动入口。测试使用独立 mutex/messages 和独立便携配置，不启用正式产品的缩放、快捷键、托盘、自动更新、提权重启或自启动服务，不发送真实鼠标／键盘输入，不操作用户配置。

简中、繁中和英文分别完成以下检查：

- 真实参数浮窗打开；控件相对 Y 坐标为倍率 19、模型 82、光流方法 145，顺序正确。
- 实际选择框和下拉 B 项的 `TextBlock` 均已创建，`IsTextTrimmed=false`。中文文字实际宽度 136.47，英文 199.68，均在控件布局内。
- `HasDescriptionLine=false`，模型选择框下没有额外备注行。
- 从测试配置读取倍率 3、B、NVOF；切换 Auto/A/B/C 与无光流／AMDOF／NVOF 后，ViewModel、模型参数和品质项可见性一致，父浮窗保持打开。
- 修改倍率为 4、NVOF 品质为 4 后保存；逐项核对落盘 JSON，模型 B、光流 NVOF、AMDOF 原品质值也均正确。
- MakePri 从本次编译 PRI 导出的三个语言候选与定稿标签完全一致。

首次新建测试配置没有 `experimentalOpticalFlowDefaultsVersion`，正常旧配置迁移将测试中的 NVOF 重置为无，因此首轮读取断言失败。核实生产迁移源码后，仅在测试配置补齐版本 1，三语言均通过；没有修改产品行为。

这是实际原生控件的程序化布局、截断、状态和保存验收。RenderTargetBitmap 生成的控件图片不完整，未将这些图片作为目视证据。先前 Computer Use 被用户停止后，本轮未继续调用；未扩展为人工鼠标／键盘、其他 DPI 或真实播放器/GPU 画质与性能验收。

## 备份及覆盖部署

用户退出旧进程后，于北京时间约 11:18 完成部署，未覆盖运行中的程序。备份位置：工作区 `.tools/deploy-0611-fg-ui-20261023/backup/`，包含旧程序目录、旧 ZIP、全部 78 个配置文件、开始菜单 `Magpie.lnk` 和旧部署收据；备份均经过哈希核验。旧构建提交为 `05f243e38809b596de83ab91dd5270b4b7aeffe5`。

安装位置继续为工作区 `worktrees/release/v0.6.11-experimental/Magpie-Experimental-x64/`，ZIP 位于同级。候选目录、ZIP 和安装目录的 246 个包文件逐项一致；实际改变的运行文件只有：

- `Magpie.exe`
- `resources.pri`
- `effects/DLSSFG/DLSS_FrameGeneration.hlsl`
- `build-manifest.json`

原目录额外的 7 个文件保持原样；所有 78 个用户配置文件在备份、覆盖及最终核验时逐文件哈希一致。现有快捷方式目标及工作目录已经正确，无需改写，最终核对仍指向该正式版。SR/FG/NR 运行 DLL 与既有包身份相同，未改驱动设置或 SR/FG 后端。

新 ZIP：606,205,178 字节；SHA-256：`2D2ACEA01AB0B94076E18DF029F27DCD640FB1BA36774157B89BD7D672E28BE5`。

主包社区 DLSSNR SHA-256：`A8A237A967469C455A84654932B513AB656FBAA1E0C44504B1E1BF0890F902B8`。可选 `DLSSNR-DLL-Options.zip` SHA-256：`1F900DF1063E3FFCA632220E7F1830AB0B22EA56AAC629AD9E858C3A13115EC1`，继续复用指定来源。

## 最终状态与证据

- 部署收据：工作区 `worktrees/release/v0.6.11-experimental/deployment-state.json`，状态 `deployed-package-and-shortcut-verified`。
- 完整构建、测试、资源、备份和部署证据：工作区 `.tools/deploy-0611-fg-ui-20261023/` 下的 `build.log`、`candidate-validation.json`、`probe-zh-hans.log`、`probe-zh-hant.log`、`probe-en-us.log`、`ui-validation.json`、`deploy.log`、`deployment-state.json` 及 `finalize.log`。
- 正式程序保持退出，本次没有重新启动。GitHub push、标签、Draft 和公开发布均未执行。

需要回退时，先确认程序已退出，再使用上述备份中的旧程序和 ZIP；配置只在确有需要时恢复，避免覆盖用户后续修改。
