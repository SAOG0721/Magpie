# Magpie Experimental v0.6.13

本版改进 DLSSNR 自适应分辨率与光流隔帧效果，降低渲染开销，完善多款原生效果器的 GPU 同步与恢复，并新增侧栏档案排序与繁体中文效果目录。

## 功能更新

### DLSSNR 与效果器

- **DLSSNR 自适应分辨率**：改进 SDR 下的自适应调档逻辑（每档 **5%**；手动比例范围 **25%–100%**，步进 **1%**），结合当前帧率目标（跟随源或固定目标）区分游戏本身掉帧与显卡负载，减少误降档。降档后若未带来明显的负载减轻或帧率改善，会自动恢复原档位；负载平稳后再逐步升回。
- **DLSSNR 光流隔帧**：改善 SDR 下 **NVIDIA 光流（NVOF）＋隔帧**（每 **2 / 3 / 4** 个新捕获帧更新一次完整 NR，默认每 **2** 帧）的帧间衔接，减轻完整推理帧与复用帧交替时的亮度与细节闪烁，低输入比例下改善更明显。剧烈运动、大面积遮挡或切换场景时可能出现局部拖影或增强减弱。
- **降低 DLSSNR 与光流开销**。用纹理交换省去呈现参考帧的整帧复制。逐帧模式使用手动比例时跳过多余的 GPU 计时查询。受支持的 SDR 路径减少提交画面时的 CPU 等待，AMD 光流还可复用有效的置信度纹理。
- **FSR 2 / FSR 3 / XeSS / RTX Video 稳定性与加载**：
  - 完善 FSR 3、XeSS SR / XeSS FG 及呈现器的 GPU 同步等待、超时处理与设备移除恢复，遇到无法安全释放的后端资源时提示重启 Magpie。
  - FSR 2 与 RTX Video（VSR）直接从程序目录加载运行库，避免与系统中的同名 DLL 冲突。
- **内联效果参数修复**。开启**内联效果参数**时，DLSSNR 与 NVIDIA VFX VSR 的已保存运行状态可正确加载，避免相关着色器编译失败，并修正缓存键。

更改 DLSSNR 的模型、处理模式、分辨率设置，以及捕获、帧同步或 VRR 选项后，需重新开始缩放生效。

### 界面与常规改进

- **侧栏档案排序**：左侧栏应用档案的“更多”菜单新增**上移**与**下移**，支持键盘操作，调整后即时保存。
- **繁体中文目录与参数说明**：补全繁体中文（`zh-Hant`）的效果名称、分类、简介与提示，搜索支持繁简关键词；补充部分 CRT 与 OKLab 参数说明。
- **窗口切换与退出**：源窗口最大化或还原时等待尺寸更新完成，避免误打断缩放；改进尺寸调整失败与停止缩放时的资源清理，减少界面卡顿。
- **配置、开机自启与 RTSS**：
  - `config.json` 的读写上限为 **16 MiB**，超限时会提示。主配置缺失或无效时尝试从 `.bak` 恢复。
  - 收紧开机自启动计划任务的用户权限，启动时自动迁移旧任务。
  - 核验 RTSS／MSI Afterburner 的安装路径和签名，自动启动要求目录具备写入权限保护。如提示手动启动，从安装目录打开对应程序后点**重新检测**。

## 安装、升级与回退

首次安装与升级均使用完整主包 **`Magpie-Experimental-x64.zip`**。先备份设置并从系统托盘完全退出旧版，将压缩包解压到新目录后运行 `Magpie.exe`。

保留旧版目录和设置备份即可随时回退：完全退出当前版本，恢复设置备份后从旧目录启动。

## 附件用途

| 文件 | 用途 |
| --- | --- |
| `Magpie-Experimental-x64.zip` | 完整独立安装包，内置 310.9.16 社区兼容 DLSSNR（适用于 RTX 20 / 30 / 40 系列）。 |
| `DLSSNR-DLL-Options.zip` | 包含 NVIDIA 原版（RTX 50）与社区兼容版（RTX 20 / 30 / 40）两套 DLL。替换前先退出 Magpie 并备份原文件，将所选 `nvngx_dlssnr.dll` 放到 `Magpie.exe` 同级目录，步骤见包内说明。 |
| `NGX_OTA_Switch.bat` | 可选的 NVIDIA NGX OTA 更新排错工具，需要管理员权限，`Restore default` 可还原默认。常规安装无需运行。 |

## 致谢

- 感谢 [onlinenow81](https://github.com/onlinenow81) 报告 [issue #75](https://github.com/SAOG0721/Magpie/issues/75) 并提供复现记录。

---

# Magpie Experimental v0.6.13 User Guide

This release improves DLSSNR adaptive resolution and optical-flow alternating frames, cuts rendering overhead, strengthens GPU sync and recovery across native effects, and adds sidebar profile reordering and Traditional Chinese catalog support.

## Feature updates

### DLSSNR and effects

- **DLSSNR adaptive resolution**: Improved SDR adaptive scaling (**5%** per step; manual scale **25%–100%** in **1%** steps). Uses the active frame-rate target (Follow source or fixed target) to tell game-side frame drops apart from GPU load, reducing false downshifts. If a downshift doesn't meaningfully ease GPU load or improve frame rate, it reverts automatically; once load settles, it steps back up gradually.
- **DLSSNR optical-flow alternating frames**: Improved frame-to-frame transitions for **SDR + NVIDIA Optical Flow (NVOF) + Alternating Frames** (full NR every **2 / 3 / 4** new captures, default **2**), reducing brightness and detail flicker between full and reused frames—especially at lower input scales. Fast motion, large occlusions, or scene cuts can still show local trails or softer enhancement.
- **Lower DLSSNR and optical-flow overhead.** Texture swaps remove full-frame reference copies. Every Frame mode skips unnecessary GPU timing queries when using a manual input scale. Supported SDR paths reduce CPU waits during submission, and AMD Optical Flow can reuse valid confidence textures.
- **FSR 2 / FSR 3 / XeSS / RTX Video stability and loading**:
  - Improved GPU sync waits, timeout handling, and device-removal recovery across FSR 3, XeSS SR / XeSS FG, and presenters, prompting for a Magpie restart if backend resources cannot be safely released.
  - Loads FSR 2 and RTX Video (VSR) runtimes directly from the program folder to avoid conflicts with same-named DLLs elsewhere.
- **Inline effect parameters fix.** When **Make effect parameters inline** is enabled, saved DLSSNR and NVIDIA VFX VSR runtime state now loads without triggering the affected shader compilation failure. The fix also corrects the shader cache key.

Restart scaling after changing DLSSNR model, processing mode, or resolution settings, as well as capture, frame-sync, or VRR options.

### Interface and general improvements

- **Sidebar profile reordering**: Added **Move up** and **Move down** to each profile's **More** menu in the left sidebar, with full keyboard access. Changes save immediately.
- **Traditional Chinese catalog and parameter hints**: Completed Traditional Chinese (`zh-Hant`) effect names, categories, summaries, and hints, with search supporting both Traditional and Simplified Chinese keywords. Added missing CRT and OKLab parameter descriptions.
- **Window transitions and shutdown**: Waits for window dimensions to update during source maximize/restore so scaling isn't cut short, and improves resource cleanup on resize failures or exit to keep the UI responsive.
- **Config, auto-start, and RTSS**:
  - Caps `config.json` reads and writes at **16 MiB** and shows a prompt when the limit is exceeded. If the primary file is missing or invalid, Magpie tries the `.bak` backup.
  - Tightens user permissions on the Task Scheduler auto-start task and migrates legacy tasks automatically on startup.
  - Verifies the installation path and signature for RTSS / MSI Afterburner and requires a write-protected install directory for automatic launch. If prompted to start the tool manually, open it from its installation directory and select **Recheck**.

## Installation, upgrades and rollback

Use the full **`Magpie-Experimental-x64.zip`** package for fresh installs and upgrades. Back up your settings, fully exit the previous version from the system tray, extract the ZIP into a new folder, and run `Magpie.exe`.

Keep your previous program folder and settings backup to roll back at any time: fully exit this version, restore your backup settings, and launch from the older folder.

## Assets

| File | Purpose |
| --- | --- |
| `Magpie-Experimental-x64.zip` | Complete standalone package bundling the 310.9.16 community compatibility DLSSNR (RTX 20 / 30 / 40 series). |
| `DLSSNR-DLL-Options.zip` | Includes both the NVIDIA original (RTX 50) and community compatibility (RTX 20 / 30 / 40) DLLs. Exit Magpie and back up your current DLL before placing the chosen `nvngx_dlssnr.dll` beside `Magpie.exe`; see the included README for steps. |
| `NGX_OTA_Switch.bat` | Optional NVIDIA NGX OTA troubleshooting script requiring administrator privileges (`Restore default` restores defaults). Not needed for normal installation. |

## Acknowledgments

- Thanks to [onlinenow81](https://github.com/onlinenow81) for reporting [issue #75](https://github.com/SAOG0721/Magpie/issues/75) and providing reproduction records.
