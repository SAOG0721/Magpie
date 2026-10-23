# DLSS SR 预设质量档路由修复

分支：`fix/dlss-sr-preset-quality-routing`；修复前基线：`07b1ad37`。

用户以注册表开启的 DLSS 画面角落指示器确认，重启缩放后 SR 仍为 K。用户随后实测 FG 切换到 C 后 GPU 占用下降，确认 FG 可用；本次仅修改 SR。0.6.11 发布继续取消，未部署或更新 GitHub。

## 根因与修改

用户实际尺寸为 1620×947 → 2463×1440。UI 保存的 L 和后端日志中的 Balanced App hint 均正确，但旧代码仅设置 Balanced 预设；运行库会根据实际缩放比例选择其他质量档，因此接受 Balanced hint 不代表最终执行该预设。

此前单变量诊断已验证：此尺寸下 Quality-only L 与全部质量档 L 的六帧输出逐帧相同；640×360 → 1280×720 则消费 Performance hint。单纯将 SetI 改为 SetUI 不改变输出。

`DLSSSRUpscaler::Initialize` 现在将显式选择的同一预设提交到 DLAA、Quality、Balanced、Performance、UltraPerformance、UltraQuality 六个 hint。Auto 仍不提交 hint。默认 K、创建时的 Balanced 请求、输入输出尺寸及帧引导契约保持原样；FG 没有修改。

## 实际 GPU 回归

- GPU：RTX 5070 Ti；驱动：617.42。
- SR DLL：310.7.0.0；SHA-256：`BE6E434A94CA32499515EB62CA0E6C274526055D568D0426E4C652DCDFB6EE6E`。
- 直接编译本分支实际 `DLSSSRUpscaler.cpp`，其对象文件优先于既有 Core 静态库链接；链接 MAP 确认 Initialize 来自该对象。没有用测试内的替代实现模拟修复。
- 运行 12 个独立进程，每个创建 SR Feature、处理六帧并从真实 GPU 读回 PNG，共 72 帧。隔离缓存，不启动正式 Magpie，不修改用户配置、注册表或驱动设置。
- 检查 PNG 中实际 Render Preset 指示，并排除底部 160 行指示区域比较场景像素。

| 尺寸 | 旧代码请求 L | 修复后请求 K / L / M | Auto 前后 |
|---|---|---|---|
| 1620×947 → 2463×1440 | 实际 K | 实际 K / L / M | 均为 K，六帧全图逐像素相同 |
| 640×360 → 1280×720 | 实际 M | 实际 K / L / M | 均为 M，六帧全图逐像素相同 |

两种尺寸下 K↔L、L↔M 的六帧场景像素均有差异，不只是指示文字变化。第六帧排除指示区域后，用户尺寸的 K/L 相差 65,491 个像素，L/M 相差 43,593 个；另一尺寸分别为 5,375 和 4,656 个。

## 其他验证与边界

- `tests/Run-DLSSParametersTests.ps1` 通过：预设数值、默认值、UI/后端规范化及 SDR/HDR 接线未退化。
- `tests/Validate-EffectCatalog.ps1` 通过：165 个效果与描述审查一致，更新了本次修改的 SR 运行源码证据哈希。
- 实际生产 C++ 编译、原生测试链接和 `git diff --check` 通过。
- 本次实际模型验收覆盖 K/L/M 和 Auto、两种 SDR 缩放比例。未把结果扩展为其他预设、其他 GPU、HDR 实际模型或完整播放器交互验收；没有重建或部署完整应用。

本地完整证据位于工作树 `.tools/20261023-sr-quality-routing/`：`build.ps1` / `build.json` / `production.map`、`run.py` / `results.json`、`analyze.py` / `comparisons.json`、`indicators.png`，以及 `runs/` 下各场景的六帧 PNG 和日志。旧代码对照程序及此前单变量诊断保留在相邻工作树 `1022-paused-screenshots/.tools/sr-preset-diagnostic/`。

修复源码的 UTF-8/LF SHA-256：`2ddd49182da6d979a3c3eb935de0e6dce268013be1ffd615c69470b16fd8b06d`。

## 后续授权：覆盖部署 0.6.11

用户随后要求覆盖目前的 0.6.11 正式版。已从提交 `05f243e38809b596de83ab91dd5270b4b7aeffe5` 完整构建 Release x64，版本保持 `0.6.11`，覆盖既有 `worktrees/release/v0.6.11-experimental/Magpie-Experimental-x64` 及其 ZIP。246 个运行文件和 ZIP 条目逐项校验通过；与旧正式版实际不同的运行文件只有 `Magpie.exe` 和 `build-manifest.json`。SR/FG/NR DLL 与其他运行文件保持相同，社区 NR 和可选包身份已核验。

原程序目录、原 ZIP、全部配置及开始菜单快捷方式已备份到工作区 `.tools/deploy-0611-sr-fix-20261023/backup`。覆盖前后配置逐文件哈希相同；现有开始菜单 `Magpie.lnk` 从 beta1 切换至上述正式版。部署时程序已退出，本次没有启动；未执行远端发布。完整应用内的模型切换仍由用户实际使用验收。

部署收据：`worktrees/release/v0.6.11-experimental/deployment-state.json`；构建、核验脚本与日志：工作区 `.tools/deploy-0611-sr-fix-20261023/`。新 ZIP 为 606,205,112 字节，SHA-256：`2B646A75C11FA60E917F19798EA14501A77ACEE700504F06F213C14613F979D3`。
