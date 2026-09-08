# HDR 色彩边界修正：SDR 复刻合同 + 锚定肩部曲线设计记录

日期：2026-09-08
分支：local-0.6.5-hdr
范围：`HdrColorTransform`（CPU）、`HdrSurfaceAdapter`（GPU mode 0/1/2/3）、`FrameSourceBase`（SDR 白测量告警）、`Renderer`（DLSSNR boundary 日志）、`tests/HdrMechanicalTests.cpp`

## 1. 问题背景

v0.6.6-hdr-fp16compat 实机验证中，DLSS SR / DLSSFG / XeSSFG 颜色正常，DLSSNR 两种模式（R8 用户路径与 FP16 实验路径）均出现偏移；上一轮有理曲线重写后偏移形态变为偏灰偏蓝，且调整残差参数时误差被放大。其余 native 路线走 DirectFP16 / 纯线性 bounded / terminal 路由，完全不经过色调映射；DLSSNR 是唯一同时挂在 SDRCompatible（mode 0/1）与 BoundedHDR（mode 2/3）上的 backend，因此偏移源锁定在曲线与归一化环节。

三个已确认的偏移源：

1. 旧实现的有理曲线 `v·(peak+shoulder)/(peak·(v+shoulder))` 在 SDR 白以下偏离恒等：以 peak=12.5、shoulder=1.0 计，SDR 白 1.0 被压到 0.54，模型输入整体压暗，逆曲线放大残差参数误差（灰蓝偏移与"动参数更严重"的直接机制）。
2. mode 2 去掉负值钳制后，scRGB 越界负通道原样进入 DLSSNR 模型；模型消费域是 [0, ∞)，负线性输入没有训练契约（FP16 路径偏蓝的主要嫌疑）。
3. `GetMonitorSdrWhiteNits` 失败时静默回退 80 nit，归一化分母与显示器真实 SDR 白（如 360 nit）差 4.5 倍，且无任何日志提示。

## 2. 两条路由、两份合同

设计过程中确立的关键约束：**恒等段（SDR 白以下逐位通过）与 T < 1 的压缩目标在数学上不相容**。恒等段强制 f(1)=1，单调性禁止 f 在 1 之后回落，而 UNORM8 存储又没有 1 以上的码空间——数值验证确认了这一点（target=0.95 被 clamp 推回 1.000001，k 顶到 0.999999，肩部退化为阶跃）。因此两条路由采用两份各自正确的合同：

### 2.1 SDRCompatible（mode 0/1，R8 复刻路径）：恒等 + saturate 复刻合同

```text
前向：y = min(max(v·exposure / (sdrWhite/80), 0), 1)
逆向：v' = y < 1 ? y·(sdrWhite/80)/exposure : framePeak
```

SDR 白以下恒等（捕获的 SDR 游戏帧逐位进入 backend，与线上 0.6.5 完全一致）；白点以上直接 saturate，逆方向把饱和白恢复到帧声明峰值（高光在峰值档位存活，虽然失去白上细节——这正是 8bit U8 复刻路线的物理上限，也是"复刻原路径"语义的正确代价）。该合同同时服务 DLSSNR R8、GroupA shader 效果与 RTX Video 等 SDRCompatible 路由。

### 2.2 BoundedHDR（mode 2/3，DLSSNR FP16 实验路径）：锚定肩部曲线族

以归一化输入 `x`（canonical ÷ sdrWhite，SDR 白 = 1.0）定义前向 `f`：

```text
x <= 1        f(x) = x                          恒等段
1 < x <= p    f(x) = x / (1 + k(x - 1))         锚定 Reinhard 肩部
x > p         f(x) = T + f'(p)·(x - p)          线性尾
```

参数：`p = hdrPeakNits / sdrWhiteNits`，`T = f(p)`（设计锚点，恒 > 1），
`k = (p - T) / (T(p - 1))`，`f'(p) = (1-k)T²/p²`。

配对逆函数：

```text
y <= 1        g(y) = y
1 < y <= T    g(y) = y(1 - k) / (1 - k·y)       肩部代数精确逆（g(T) = p）
y > T         g(y) = p + (y - T) / f'(p)        尾部逆
```

性质：`f(1)=1` 与恒等段 C0 连续；`k ∈ (0,1)` 保证肩部严格单调；尾部斜率 `f'(p) > 0` 保证任意高 nits 输入不逆转；逆增益全域有界（肩部 ≤ 1/(1-k)，尾部 = 1/f'(p)）。T 取 2.5：实验矩阵显示 scale=2 接近基线、4.5 爆炸，2.5 落在安全带上沿，`f(∞)=1/k ≈ 2.9` 有界。

## 3. 各 mode 的最终行为

- **mode 0（HDR→SDR）**：复刻合同前向（恒等 + saturate）。
- **mode 1（SDR→HDR）**：复刻合同逆向；饱和白恢复到帧峰值。
- **mode 2（canonical→bounded，DLSSNR 输入）**：`max(v,0)` 钳掉越界负值（模型消费域契约），`·normalizationScale/(sdrWhite/80)` 归一，再肩部曲线（T=2.5）压入模型验证带。
- **mode 3（bounded→canonical，DLSSNR 输出）**：肩部精确逆 + 反归一，全程无钳制。
- mode 4/5/6（scRGB / HDR10 terminal）不变。

CPU（`HdrColorTransform`）与 GPU（`HdrSurfaceAdapter`）共用同一组系数（常量缓冲携带 peak/target/shoulderK/tailSlope，112 字节含对齐 padding），机械测试保证两者实现一致。

## 4. 配套改动

- `FrameSourceBase.cpp`：SDR 白测量失败且显示为 HDR 时输出 Warn（含峰值与色彩空间），消除静默 4.5 倍错位。
- `Renderer.cpp` DLSSNR boundary 日志扩展为 `sdrWhite / peak / curvePeak / curveTarget / curveK / curveTailSlope / colorInferred`，实机一条日志即可核对归一化与曲线参数。
- `tests/HdrMechanicalTests.cpp`：`TestHdrToneMapRoundTrip` 改为复刻合同语义（白点以下精确往返、饱和峰恢复）；新增 `TestShoulderCurveFamily`（恒等段、复刻合同饱和/恢复、肩部严格单调至 2.25p、锚点与配对逆含尾上点、T 取值域、bounded 编解码往返 + 带内断言）。全部通过（1174 PASS / 0 FAIL）。
- `Run-HdrMechanicalValidation.ps1` 锚点更新为复刻合同 + 肩部族符号，全部通过。

## 5. 预期与验证

预期效果：R8 复刻路径在 HDR 开关下回到逐位级一致（这是本设计的硬保证），灰蓝偏移消失；FP16 路径高光被压回模型验证过的 [0, ~2.5] 邻域，负通道不再进入模型。

实机验证顺序：

1. HDR on + DLSSNR R8 路径：SDR 画面应与 HDR off 基线逐位一致；日志核对 `sdrWhite` 与显示器设置相符。
2. HDR on + FP16 实验路径：高光不再偏色；调整残差参数不再放大误差。
3. 高亮 HDR 测试图：确认无色阶乱序（逆转）与带状。
4. 若仍有残余偏移，日志中 `curveK/curveTarget` 与 `HDR adapter dispatch: white= peak=` 可直接定位是曲线参数还是元数据错位。

## 6. 已知边界

- R8 复刻合同在白点以上丢弃细节（saturate 后逆恢复到峰值档）；这是 8bit U8 复刻语义的物理上限，HDR 高光保真由 FP16 路径承担。
- 复刻合同逆向把 1.0 码值解释为帧峰值；若后端恰好输出 1.0 的"真 SDR 白"，会被提升到峰值档。DLSSNR 输出域按 saturate 语义设计，该解释与其一致。
- 恒等段与肩部在 x=1 处 C0 连续但导数有跳变；FP16 路径无碍。若需要 C1 平滑可引入 [1-ε, 1+ε] 混合，当前刻意不加以保持逆函数代数精确。
- 尾部线性外推超出 p 的输入在逆变换后可超过 hdrPeakNits；canonical 保留原始值域，由 presenter 合同承接。
- `BoundedRouteHighlightTarget = 2.5` 是基于社区 DLL 实验的工程取值；后续 harness A/B 发现更优锚点时只改这一个常量（CPU/GPU/测试自动跟随）。

