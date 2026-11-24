# Magpie Experimental v0.6.14

本版为 v0.6.13 的修复版本，重点修复效果组参数面板中的滑动条重置与浮窗收回问题，并修正若干前端异步回调的稳定性隐患。

## 修复内容

### 效果组参数面板与界面交互

- **修复非零边界滑动条打开弹窗时重置的问题**：
  - 在效果组中配置 **DLSS Frame Generation**（倍率 **2–4x**）、**XeSS Frame Generation**（倍率 **2–4x**）、**NVIDIA VFX VFG**（倍率 **2–8x**）等效果器时，此前重新打开参数弹窗会触发 XAML `Slider` 初始化钳位回写，导致已保存的倍率（如 3x / 4x / 8x）被立即重置回最小值（2x）并覆盖配置。
  - 同时修复了其他同类非精确输入滑条（`MIN > 0` 或 `MAX < 0`，如部分 CRT、Anime4K、xBR、MLAA、DLSSNR 参数）在打开参数弹窗时被钳位到边界值的问题。
  - 全部离散步进滑条现统一映射到 0 基刻度空间，重开弹窗保持已保存数值。
- **修复参数浮窗拖出窗外松手后滑条粘连及浮窗不收回的问题**：
  - 修复在效果参数浮窗（以及效果选择器等窗口化浮层）内拖动滑条或 OKLab 曲线／色轮并把指针移出浮窗外松手后，移回浮窗时滑条／画布继续跟随光标拖动，以及再次点击喜鹊主界面或标题栏时浮窗无法自动收回的问题；切出到其他应用窗口时也会正确收回。
- **前端异步回调安全性修复**：
  - 修复“新建档案”候选窗口异步解析名称时回调持有裸指针的潜在崩溃风险。
  - 修复 OKLab 导入 `.cube` LUT 返回主线程时未先校验效果组／效果索引有效性的越界风险。
  - 修复 Toast 弹窗跨线程主题切换回调的弱引用保护。

本版包含 v0.6.13 的全部功能更新，详见 [v0.6.13 完整更新说明](RELEASE_NOTES_v0.6.13-experimental.md)。

## 安装、升级与回退

首次安装与升级均使用完整主包 **`Magpie-Experimental-x64.zip`**。先备份设置并从系统托盘完全退出旧版，将压缩包解压到新目录后运行 `Magpie.exe`。

保留旧版目录和设置备份即可随时回退：完全退出当前版本，恢复设置备份后从旧目录启动。

## 附件用途

| 文件 | 用途 |
| --- | --- |
| `Magpie-Experimental-x64.zip` | 完整独立安装包，内置 310.9.16 社区兼容 DLSSNR（适用于 RTX 20 / 30 / 40 系列）。 |
| `DLSSNR-DLL-Options.zip` | 包含 NVIDIA 原版（RTX 50）与社区兼容版（RTX 20 / 30 / 40）两套 DLL。替换前先退出 Magpie 并备份原文件，将所选 `nvngx_dlssnr.dll` 放到 `Magpie.exe` 同级目录，步骤见包内说明。 |
| `NGX_OTA_Switch.bat` | 可选的 NVIDIA NGX OTA 更新排错工具，需要管理员权限，`Restore default` 可还原默认。常规安装无需运行。 |

---

# Magpie Experimental v0.6.14 User Guide

This bugfix release for v0.6.13 fixes scaling-mode parameter slider resets and windowed flyout light-dismiss failures, along with several frontend async callback safety issues.

## Fixes

### Scaling-mode effect parameters and UI

- **Fixed slider value reset on flyout reopen for non-zero bounds**:
  - When configuring **DLSS Frame Generation** (multiplier **2–4x**), **XeSS Frame Generation** (multiplier **2–4x**), or **NVIDIA VFX VFG** (multiplier **2–8x**) in a scaling mode, reopening the parameter flyout previously triggered XAML `Slider` range coercion writeback, immediately resetting saved multipliers (such as 3x, 4x, or 8x) back to the minimum (2x) and overwriting saved settings.
  - Also fixes the same coercion reset for other non-precise sliders with `MIN > 0` or `MAX < 0` (such as certain CRT, Anime4K, xBR, MLAA, and DLSSNR parameters).
  - All discrete-step parameter sliders now map uniformly onto a 0-based tick range, preserving saved values across flyout reopens.
- **Fixed stuck slider drags and parameter flyout light-dismiss after dragging outside its bounds**:
  - Fixed a XAML Islands windowed-popup desync where dragging a slider (or OKLab curve/wheel) past the flyout edge and releasing left the slider/canvas stuck following the cursor upon re-entry and prevented subsequent clicks on the main Magpie window or title bar from dismissing the flyout. Switching to another application now also dismisses open windowed flyouts cleanly.
- **Frontend async lifetime and bounds safety**:
  - Fixed a potential use-after-free when resolving candidate window names asynchronously in the New Profile flyout.
  - Added removed-state and index bounds checks after resuming on the UI thread in OKLab `.cube` LUT import.
  - Guarded cross-thread theme-change callbacks in `ToastPage` with a weak reference.

This release includes all changes from v0.6.13. See the [v0.6.13 release notes](RELEASE_NOTES_v0.6.13-experimental.md) for the full list.

## Installation, upgrades and rollback

Use the full **`Magpie-Experimental-x64.zip`** package for fresh installs and upgrades. Back up your settings, fully exit the previous version from the system tray, extract the ZIP into a new folder, and run `Magpie.exe`.

Keep your previous program folder and settings backup to roll back at any time: fully exit this version, restore your backup settings, and launch from the older folder.

## Assets

| File | Purpose |
| --- | --- |
| `Magpie-Experimental-x64.zip` | Complete standalone package bundling the 310.9.16 community compatibility DLSSNR (RTX 20 / 30 / 40 series). |
| `DLSSNR-DLL-Options.zip` | Includes both the NVIDIA original (RTX 50) and community compatibility (RTX 20 / 30 / 40) DLLs. Exit Magpie and back up your current DLL before placing the chosen `nvngx_dlssnr.dll` beside `Magpie.exe`; see the included README for steps. |
| `NGX_OTA_Switch.bat` | Optional NVIDIA NGX OTA troubleshooting script requiring administrator privileges (`Restore default` restores defaults). Not needed for normal installation. |
