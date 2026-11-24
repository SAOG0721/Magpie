# Magpie Experimental v0.6.14

本版为 v0.6.13 的快速修复版本，修复效果组参数面板中 FG 补帧倍率等非零下限（或负上限）滑动条在重新打开弹窗时被重置回边界值的问题。

## 修复内容

### 效果组参数面板

- **修复非零边界滑动条打开弹窗时重置的问题**：
  - 在效果组中配置 **DLSS Frame Generation**（倍率 **2–4x**）、**XeSS Frame Generation**（倍率 **2–4x**）、**NVIDIA VFX VFG**（倍率 **2–8x**）等效果器时，此前重新打开参数弹窗会触发 XAML `Slider` 初始化钳位回写，导致已保存的倍率（如 3x / 4x / 8x）被立即重置回最小值（2x）并覆盖配置。
  - 同时修复了其他同类非精确输入滑条（`MIN > 0` 或 `MAX < 0`，如部分 CRT、Anime4K、xBR、MLAA、DLSSNR 参数）在打开参数弹窗时被钳位到最小值或最大值的问题。
  - 全部离散步进滑条现统一映射到 0 基刻度空间，不再受控件初始值钳位影响，调整后即时保存且重开弹窗保持原值。

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

This hotfix release for v0.6.13 fixes an issue in the scaling-mode effect parameter flyout where sliders with a non-zero minimum (or negative maximum)—such as Frame Generation multipliers—reset back to their boundary value when reopening the flyout.

## Fixes

### Scaling-mode effect parameters

- **Fixed slider value reset on flyout reopen for non-zero bounds**:
  - When configuring **DLSS Frame Generation** (multiplier **2–4x**), **XeSS Frame Generation** (multiplier **2–4x**), or **NVIDIA VFX VFG** (multiplier **2–8x**) in a scaling mode, reopening the parameter flyout previously triggered XAML `Slider` range coercion writeback, immediately resetting saved multipliers (such as 3x, 4x, or 8x) back to the minimum (2x) and overwriting saved settings.
  - Also fixes the same coercion reset for other non-precise sliders with `MIN > 0` or `MAX < 0` (such as certain CRT, Anime4K, xBR, MLAA, and DLSSNR parameters).
  - All discrete-step parameter sliders now map uniformly onto a 0-based tick range, preventing initialization clamping writeback and preserving saved values across flyout reopens.

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
