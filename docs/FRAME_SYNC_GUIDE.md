# Frame Rate & Refresh Guide / 帧率与刷新使用指南

🌍 **English** | [简体中文](#简体中文)

---

## Quick Start (Recommended Setups)

If you just want smooth results without digging into every toggle, start with **Simple** mode and pick the scenario closest to yours:

- **Gaming without Frame Generation**: Set **Content rate** to **Follow source** (or **Custom** matching your game's frame-rate cap, e.g. `60 FPS`).
- **Gaming with Frame Generation (e.g. 2× FG on a 120 Hz monitor)**: Set **Content rate** to your **base real frame rate** (e.g. **Custom `60 FPS`**, *not* `120 FPS`), and cap the game to the same `60 FPS` (in-game limiter or Magpie's built-in **RTSS** sync). Frame generation multiplies the base rate automatically.
- **Video playback (24 / 30 / 60 FPS)**: Use **Follow source** and keep **Static-picture redraw** on (`30 FPS`). Normal 24/30 FPS video will not falsely trigger idle mode.

> [!NOTE]
> Most changes under **Frame rate & refresh** save automatically and take effect after you **restart scaling** (or click **Save and restart** in the live parameter overlay).

---

## Simple vs. Advanced Mode

- **Where settings are stored**: The Default profile and each application profile save their own refresh settings. New profiles copy the Default profile (or your chosen template) and then evolve independently. You can view and edit the Default profile summary right from **Home**.
- **Simple mode (default)**: Shows the essential controls (**Content rate**, **Static-picture redraw**, and **VRR**). Under the hood, it uses **Front Edge** pacing, disables general duplicate detection, and keeps cursor supplementation synced to your custom content rate (or `60 FPS` when following source).
- **Advanced mode**: Unlocks **Pacing method**, **Cursor refresh**, and **General duplicate detection**. Switching back to Simple hides those options without erasing your saved Advanced preferences.
- **Resetting defaults**: Clicking Reset in Simple mode only resets the visible fields and preserves your Advanced preferences and other profiles.

### All Settings & Defaults

| Setting | Options & Default | What It Controls |
| --- | --- | --- |
| **Content rate** | `Follow source` / `Custom` *(default: Custom 60 FPS)* | Sets Magpie's base processing rate for new frames (`15–360 FPS`, integer, step `1`). |
| **Static-picture redraw** | `Off` / `Custom` *(default: On, 30 FPS)* | Periodically re-presents the cached image when the source window stops updating (`15–360 FPS`). |
| **Pacing** *(Advanced)* | `Front Edge` *(default)* / `Async` / `NVIDIA Reflex` | How Magpie times frame capture and submission. |
| **Cursor refresh** *(Advanced)* | `Response first` / `Original frames only` / `Prefer original frames with supplementation` *(default)* | How smoothly Magpie updates the system cursor independently of game/video frames. |
| **Cursor supplementation** *(Advanced)* | `Auto (display refresh)` / `Custom` *(default: Custom 60 FPS)* | Minimum cursor refresh target (`15–360 FPS`) when supplementation is active. |
| **VRR (Experimental)** | `Off` *(default)* / `On` | Requests Variable Refresh Rate / tearing support on eligible output paths. |
| **General duplicate detection** *(Advanced)* | `Never` *(default)* / `Dynamic` / `Always` | Shared duplicate-frame filter used across all four frame generators (DLSSFG, XeSSFG, FSR3FG, VFG). |
| **RTSS management** | `Off` *(default)* / Global or Target app | Automatically configures RivaTuner Statistics Server (RTSS) to cap the source app for you. |

---

## How Pacing Works (and Which One to Pick)

When **Content rate** is set to **Follow source**, Magpie processes every new captured frame as it arrives. When set to **Custom**, Magpie paces its **base content rate** to that target (your frame-generation multiplier does **not** change this base limit).

If your source app, capture pipeline, or heavy effects cannot reach the target, the actual frame rate will naturally be lower.

- **Front Edge *(Default — smoothest frame times)***: Aligns frames right before submission for even pacing. Because it waits to hit the timing grid, it can add a bit of latency.
- **Async *(Lower wait, less strict grid)***: Limits how often Magpie starts processing a new frame before capture, without trying to "catch up" to missed intervals. Good if Front Edge feels sluggish.
- **NVIDIA Reflex**: Lets the NVIDIA driver pace the base target on supported paths. If Reflex cannot run on the current path, Magpie falls back or stops safely.
- **XeSS (XeLL)**: When using XeSS with XeLL active, XeLL handles base pacing on its own without stacking another limiter on top.

---

## Capping the Source App & RTSS Integration

For smooth frame pacing—especially with **Frame Generation**—the source game should not run wildly faster than Magpie's base content rate. You can cap the game in its own settings, or let Magpie manage **RTSS** for you:

- When scaling starts, Magpie ensures its own RTSS profile is **unlimited**, then writes your target cap to RTSS (either globally or for the target app).
- **Sync mode** uses your effective base content rate (`15–360 FPS`). Note that Sync mode does **not** apply a cap when Content rate is set to **Follow source**.
- **Manual target**: You can also specify a custom cap (`15–360 FPS`, step `1`) directly in the RTSS group, even when sync is off.
- **Cleanup on exit**: When scaling stops normally, Magpie tries to restore your previous RTSS limit. If the app crashes or is force-killed, restoration cannot be guaranteed.
- The **"Written"** indicator only confirms that the RTSS config file was updated and verified—it does not guarantee the game obeyed RTSS. **Recheck** is read-only; **Retry** applies to the current session.
- If Magpie asks you to start RTSS or MSI Afterburner manually, open it from its installation directory and select **Recheck**. Version 0.6.13 checks the installed executables and requires a protected directory for automatic launch.

---

## Cursor Refresh, Static Pictures & Frame Generation

### System Cursor vs. In-Game Cursor
**Cursor refresh** controls the Windows system cursor that Magpie renders on top of the output. If a game draws its own cursor directly into the game image, that cursor moves at the game's content rate. Cursor supplementation is a minimum target (limited by your display refresh rate) and does not waste GPU work when the mouse isn't moving.

### Static-Picture Redraw & Idle Behavior
When the source window has no new frames (such as a paused video or static menu):
- **Normal paths (no FG)**: Follow Magpie's standard cached-redraw rules.
- **Frame Generation paths (DLSSFG / XeSSFG / FSR3FG / VFG)**: After roughly **0.5 seconds** with no new content, Magpie refreshes the completed cached frame at the redraw rate **without** re-running effects or optical flow. This 0.5 s delay prevents normal 24/30 FPS video from being mistaken for an idle screen. As soon as new content resumes, FG history resets cleanly.
- If **Static-picture redraw** is turned **Off**, Magpie simply holds the last frame until new content arrives.
- Effective redraw rate is capped by your content rate and display refresh rate, and is **not** multiplied by FG.

### Reading the Toolbar Metrics
- **`R/s`**: Counts cached static-picture redraws separately from real new frames.
- **`FSR3 Submitted`**: Counts frames successfully submitted to FSR3 FG; it is not necessarily equal to physical monitor FPS or unique interpolated frames.
- **Duplicate detection**: Shared across DLSSFG, XeSSFG, FSR3FG, and VFG in Advanced mode.

---

## Experimental VRR

**VRR** is **Off** by default. Turning it on requests variable refresh rate (tearing) after you restart scaling, provided your output path, GPU, driver, and monitor all support it.

- On supported **WGC + DLSSNR + Front Edge + VRR (no FG)** paths (v0.6.12+), Magpie can also resubmit the most recently completed frame to smooth out brief gaps (~2 frame periods without new content, stopping after ~250 ms) without re-running DLSSNR.
- **How to verify**: A toggled switch or successful present does not prove physical VRR is active—check your monitor's built-in refresh-rate OSD or frame-interval graph.

---

## Troubleshooting Checklist

1. **Start simple**: Switch to **Simple** mode with a minimal effect group (no FG, no VRR, no RTSS) to confirm clean baseline capture and output.
2. **Add one feature at a time**: Enable RTSS, VRR, or Frame Generation one by one so you can immediately spot what introduced stutter or latency.
3. **Keep comparisons fair**: Always compare using the same in-game FPS cap, scene, and effect parameters.
4. **Collect diagnostics**: If you hit an issue, disable the suspected option to confirm, then include your logs, capture method, GPU driver version, and reproduction steps when filing an issue.

---

<a id="简体中文"></a>

# 帧率与刷新使用指南（简体中文）

[English](#frame-rate--refresh-guide--帧率与刷新使用指南) | **简体中文**

---

## 快速上手（推荐配置）

如果你不想研究每个选项，直接保持默认的**简单模式**，按场景选即可：

- **玩游戏（不开补帧）**：将**内容帧率**设为**跟随源**，或设为**自定义**并与游戏内限帧保持一致（例如 `60 FPS`）。
- **玩游戏（开补帧，例如 120 Hz 显示器开 2× 补帧）**：将**内容帧率**设为**基础真实帧率**（例如自定义 **`60 FPS`**，**不要**填 `120 FPS`），同时在游戏内或用 Magpie 的 **RTSS 联动**把游戏也限在 `60 FPS`。补帧倍率会自动乘在基础帧率上。
- **看视频（24 / 30 / 60 FPS）**：用**跟随源**，保持**静态画面重绘**开启（`30 FPS`）。正常的 24/30 FPS 视频不会被误判成静止画面。

> [!NOTE]
> **帧率与刷新**中的大多数修改会自动保存，并在**重新开始缩放**后生效（运行期间可在参数浮窗点**保存并重启**）。

---

## 简单模式 vs 高级模式

- **配置保存位置**：默认配置和每个应用配置各自独立保存刷新设置。新建应用配置时会复制默认配置（或所选模板），之后互不影响。主页可以直接查看和编辑默认配置。
- **简单模式（默认）**：只展示最常用的三项（**内容帧率**、**静态画面重绘**、**VRR**）。底层固定使用 **Front Edge** 节奏、关闭通用重复检测，并将光标补充率自动对齐到你的自定义内容帧率（跟随源时为 `60 FPS`）。
- **高级模式**：展开**节奏方式**、**光标刷新**与**通用重复检测**。切回简单模式时不会清空你之前在高级模式里调好的偏好。
- **恢复默认**：在简单模式下点“恢复默认”只会重置当前可见选项，保留高级偏好和其他应用配置。

### 全部选项与默认值速查

| 选项 | 可选值与默认值 | 作用说明 |
| --- | --- | --- |
| **内容帧率** | `跟随源` / `自定义` *（默认：自定义 60 FPS）* | 控制 Magpie 处理新内容的基础目标帧率（`15–360 FPS`，整数，步进 `1`）。 |
| **静态画面重绘** | `关闭` / `自定义` *（默认：开启，30 FPS）* | 画面长时间没变化时，按该频率重新提交缓存画面（`15–360 FPS`）。 |
| **节奏方式** *（高级）* | `Front Edge` *（默认）* / `Async` / `NVIDIA Reflex` | 控制 Magpie 取帧与提交画面的时机策略。 |
| **光标刷新** *（高级）* | `响应优先` / `仅随原始内容帧` / `原始帧优先并补充` *（默认）* | 控制 Magpie 绘制的系统鼠标光标刷新流畅度。 |
| **光标补充率** *（高级）* | `自动（跟随显示刷新率）` / `自定义` *（默认：自定义 60 FPS）* | 光标补充生效时的最低刷新目标（`15–360 FPS`）。 |
| **VRR（实验）** | `关闭` *（默认）* / `开启` | 在支持的输出路径上请求可变刷新率（VRR / tearing）。 |
| **通用重复检测** *（高级）* | `从不` *（默认）* / `动态` / `总是` | 四种补帧后端（DLSSFG / XeSSFG / FSR3FG / VFG）共用的重复帧过滤。 |
| **RTSS 接管** | `不管理` *（默认）* / 全局或目标应用 | 自动通过 RivaTuner Statistics Server（RTSS）替你给源程序限帧。 |

---

## 三种节奏方式怎么选？

- 设为**跟随源**时，源程序来一帧新画面，Magpie 就处理一帧。
- 设为**自定义**时，Magpie 按设定的**基础内容帧率**控制节奏（补帧倍率不会改变这个基础目标）。如果游戏本身、捕获或重负载效果器跑不到目标帧率，实际帧率自然会更低。

各节奏方式的特点：

- **Front Edge *（默认，帧间隔最稳）***：在画面提交前对齐时间网格，节奏最均匀，但等待对齐可能会略微增加延迟。
- **Async *（等待更少）***：在取帧前控制处理启动间隔，不追赶过期的时间网格。如果觉得 Front Edge 有点粘手可以试试它。
- **NVIDIA Reflex**：在支持的路径上交由 NVIDIA 驱动控制基础节奏；不支持或失败时会自动安全回退或停止。
- **XeSS（XeLL）**：使用带 XeLL 的 XeSS 路径时，由 XeLL 自行限帧，不会再叠加一套基础限帧器。

---

## 源程序限帧与 RTSS 联动

想要画面顺滑（尤其是**开补帧**时），源程序的帧率最好不要大幅高于 Magpie 的基础内容帧率。你可以直接在游戏里限帧，也可以开启 Magpie 的 **RTSS 接管**：

- 开始缩放时，Magpie 会先把**自身**在 RTSS 中设为不限帧，再将目标上限写入 RTSS（可选全局或仅目标应用）。
- **同步限帧**会自动使用当前有效的基础内容帧率（`15–360 FPS`）；注意**内容帧率为“跟随源”时不会启用同步限帧**。
- **手动指定**：即使关闭同步，也可以在 RTSS 面板里直接指定 `15–360 FPS` 的限帧值。
- **退出恢复**：正常停止缩放时会尝试恢复你原来的 RTSS 限帧；如果程序崩溃或被强制结束，无法保证恢复。
- 状态显示**“已写入”**仅代表 RTSS 配置文件写入并通过核验，不代表游戏一定服从 RTSS。**重新检测**只读状态，**重试**仅针对当前会话。
- 如果 Magpie 提示手动启动 RTSS 或 MSI Afterburner，请从对应安装目录启动，再点**重新检测**。0.6.13 会核验已安装的程序，并要求自动启动的安装目录受到写入权限保护。

---

## 光标、静态画面与补帧细节

### 系统光标 vs 游戏内光标
**光标刷新**只作用于 Magpie 叠加发布的 Windows 系统光标；如果游戏把准星或鼠标直接画进了游戏画面里，它属于内容的一部分，随游戏帧率刷新。光标补充率只是最低目标（受显示器刷新率限制），鼠标静止时不会做无意义的空刷。

### 静态画面重绘与空闲逻辑
当源窗口没有新画面变化（比如暂停视频或静止菜单）时：
- **无补帧路径**：沿用常规缓存重绘规则。
- **补帧路径（DLSSFG / XeSSFG / FSR3FG / VFG）**：连续约 **0.5 秒**没有新内容后，才开始按重绘率刷新已完成缓存，**不会**重跑效果器或光流。这个 0.5 秒缓冲专门用来避免把正常的 24/30 FPS 视频误判为空闲；一旦新内容恢复，补帧历史会自动重置。
- 如果关闭**静态画面重绘**，Magpie 会保持最后一帧不动，直到有新画面到达。
- 实际重绘率受内容帧率和显示刷新率限制，**不会**乘以补帧倍率。

### 工具栏指标怎么看
- **`R/s`**：单独统计静态缓存重绘的提交次数，不混入真实新内容帧率。
- **`FSR3 Submitted`**：表示成功提交给 FSR3 补帧的次数，不能直接等同于显示器物理刷新帧率或有效插值帧数。
- **通用重复检测**：在高级模式下由四类补帧共用（旧版效果器内的独立重复检测选项已迁移合并）。

---

## 实验性 VRR

**VRR** 默认关闭，开启并重新开始缩放后，会按当前输出路径、显卡、驱动与显示器能力请求可变刷新率。

- 在 v0.6.12+ 受支持的 **WGC + DLSSNR + Front Edge + VRR（无补帧）**路径上，如果连续约两个目标帧周期没有新内容提交，Magpie 会按目标节奏补交最近完成的画面（不重跑 DLSSNR，连续约 250 ms 无新内容后停止），用于平滑短暂空档。
- **如何确认真正生效**：软件开关打开或提交成功并不能证明显示器物理 VRR 正在工作，请通过显示器自带的实时刷新率 OSD 或帧间隔曲线确认。

---

## 问题排查步骤

1. **回到基线**：先切回**简单模式**，只保留最简单的效果链（关掉补帧、VRR 和 RTSS），确认基础捕获和画面正常。
2. **逐项叠加**：再依次单独开启 RTSS、VRR 或补帧，方便第一时间定位引起卡顿或延迟的选项。
3. **控制变量**：对比效果时，保持相同的游戏限帧、场景和效果参数。
4. **保留日志**：遇到问题先关闭对应选项复核，反馈时附上日志文件、捕获方式、显卡驱动版本与复现步骤。
