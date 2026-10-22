# VFX VSR 参数浮窗：隔离原生验证

2026-10-22，Windows，本机实际 DPI 96（100%）。对应 TODO V01–V06。

结论：按用户最新指示，**暂定测试通过，未复现由调参引起的内部关闭故障**。没有提交浮窗行为修复，也没有改变 LightDismiss、父浮窗约束、选择同步或窗口尺寸策略。150%/200% DPI、键盘/Esc、页面切换及运行缩放时的完整验收仍未完成。

## 实际模板测试

测试使用 0.6.11-beta1 候选的真实 `ScalingModesPage`、编译的 XAML 模板、参数 ViewModel、配置保存和 `MainWindow/XamlWindow`，包含诊断提交 `47834ddd`。只为测试生成独立 `App.cpp`，使用独立 mutex/messages、新建便携配置和复制的运行文件。未启动快捷键、缩放运行时、RTSS、更新检查、托盘、提权重启或自启动同步，未向当前 Magpie 广播消息。

| 运行 | 观察 |
|---|---|
| 程序化选择，原始 `ShouldConstrainToRootBounds=false` | 连续 66 次完成；前 16 次为同类型品质，其后循环全部五种类型并逐类切换合法品质。四次真实 Choices Reset；UI 与配置值逐次一致，测试配置落盘。仅启动时一次 WM_SIZE，父浮窗只在测试最终主动 Hide 时 Closing。 |
| 原生鼠标选择，原始约束 | 连续 63 次完成，包含全部类型与品质、四次品质列表 Reset。第 64 次选择已提交、子下拉正常关闭后，外部窗口获得前台，父浮窗随后关闭。没有参数引起的内部关闭证据。 |
| 测试对照 `ShouldConstrainToRootBounds=true` | 外部窗口获得前台，选择未提交，比较不成立；未据此修改产品。 |
| 最后一次鼠标对照 | 指针目标属于另一进程，双重 PID 检查拒绝发送输入。即时只读查询确认该 PID 为 Chrome。未操作该窗口。 |

原生鼠标测试中主窗口位于 `(52,52)`，尺寸 `980×690`；合法点击的屏幕 Y 最低为 32，已经覆盖子下拉伸出宿主上边界的情况。这一现象本身没有造成关闭，不能据此全局启用父浮窗约束。

首次实验的 UI Automation peer 在本机返回空，原始探针因此失败；后续探针明确使用 `SelectedIndex` 程序接口并单独进行原生鼠标测试。程序化证据日志旧版 PASS 行写了 UIA，但其 66 条 `probe.programmaticSelection` 明确记录了实际路径；以事件记录及上表为准，不将它视作 UIA/键盘验收。

## 第一次原生父浮窗关闭的事件链

见 [鼠标运行事件](evidence/20261022-vfx-flyout/actual-template-mouse.txt)，探针 PID 47712：

- `19:05:33.807`：品质选择提交。
- `.814`：子 ComboBox DropDownClosed，父浮窗仍在。
- `.929`：宿主收到 `WA_INACTIVE`，目标 HWND `0x2312f8` 属于另一 PID 51480。
- `.932`：前台事件确认该外部窗口。
- `.938`：父 Flyout Closing。
- `.970`：父 Flyout Closed。

期间没有 WM_SIZE 或 Choices Reset。该外部 PID 在事后查询前已退出，未确认其程序身份；不能套用先前其他 PID 的进程名。这证明了这次探针关闭是外部焦点切换路径，不能证明用户历史故障也由同一外部程序引起。正常外部失焦的关闭行为应保留。

## 复跑与诊断

- `tests/Run-EffectParameterFlyoutProbe.ps1` 是不加载产品服务/配置的缩小版 XAML Island 对照，支持系统 XAML 或候选的 WinUI 2.8.7。
- `tests/Build-EffectParameterAppProbe.ps1` 从已完成的候选编译中读取对象、生成头和 linker tlog，只替换测试 App 对象，所有链接输出写到新的测试目录。`-CoreLibrary` 可使用打包清理前复制的私有静态库。不要在候选仍编译时链接。
- 该脚本参数为 `-SourceRoot`、`-RuntimeDirectory`、`-IntermediateDirectory`（包含 `Magpie.tlog` 的目录）、`-OutputDirectory`（必须不存在），可加 `-Run`。默认程序化选择；测试子进程环境 `MAGPIE_FLYOUT_PROBE_INTERACTION=mouse` 启用指针模式，`MAGPIE_FLYOUT_PROBE_CONSTRAIN=1` 只控制测试对照。
- 原生鼠标仅向两次检查均属于探针 PID 的屏幕目标发送点击；不发送键盘输入。窗口被覆盖时应记录跳过，不能取消 PID 保护来完成测试。
- 产品诊断通过测试进程环境 `MAGPIE_TRACE_PARAMETER_FLYOUT=1` 开启；不写配置。记录父/子事件、模型编辑、Choices Reset、锚点生命周期、宿主大小/失活、排队重定位及明确关闭入口。默认关闭，无行为变更。
- 普通便携 Magpie 仍共享生产 mutex，不能直接在正在运行的用户实例旁启动来做隔离测试；测试生成器特意使用独立启动路径。

相关源码经过 MSVC `/W4 /WX` 语法检查，真实候选完成了 XAML/C++ 编译与链接，独立 App 探针完成编译/链接/运行。用户配置 SHA-256 在测试前后均为 `F13C1FB4B3118FB8213EF574C84B2AB847CCFA30EE0A1B1F11325F4AB7F3BC57`。

完整事件文件另见 [程序化运行](evidence/20261022-vfx-flyout/actual-template-programmatic.txt)、[约束对照](evidence/20261022-vfx-flyout/actual-template-constrained.txt)、[输入保护](evidence/20261022-vfx-flyout/actual-template-input-guard.txt)。它们仅保留各自进程运行段的诊断事件，不包含用户配置或窗口标题。
