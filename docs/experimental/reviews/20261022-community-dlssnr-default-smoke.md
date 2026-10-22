# 默认社区 DLSSNR 310.9.16：独立原生 smoke

日期：2026-10-22。目标整合提交：`0c12e3fe1edb7b0b57d557cdcaf2d6716b83aad7`。本次只验证主包新默认 DLL，未修改主程序、打包脚本、配置、驱动或远端。

## 身份与隔离

- 默认文件：`dependencies/dlssnr-runtime/nvngx_dlssnr.dll`，276,162,560 bytes，完整 FileVersion **310.9.16.0**，SHA-256 **`a8a237a967469c455a84654932b513ab656fbaa1e0c44504b1e1bf0890f902b8`**。本机 Authenticode 为 **NotSigned**，无签名者。
- 配套可选包：`dependencies/dlssnr-runtime/DLSSNR-DLL-Options.zip`，SHA-256 **`1f900df1063e3ffca632220e7f1830ab0b22ea56aac629ad9e858c3a13115ec1`**，与指定的 `release/v0.6.10-experimental/updated-20261017-two-options` 来源身份一致。
- 使用此前已经通过测试的 `DLSSPresetSmoke.exe`，SHA-256 `f88d277ad18d9728f5194e0a4ec99a439fa2b86fba283d59e3eef818a771d129`。它链接的原生对象来自 `247158ef7e89e6c7720c6ebc296c89cc89ead673`；逐文件比较确认 NR Filter/MultiPass/Temporal/参数与时间状态代码到目标整合提交没有变化。本次复用该二进制，不声称重新验证了整合版 GUI 或正式包整体。
- 预先审阅 runner 与 native fixture：只创建 D3D 设备、调用生产 NR 封装并启动独立子进程；不调用 ScalingWindow/UI，不读写用户效果配置，不结束现有 Magpie。SM86 KnownFolder 测试替身只指向任务内 audit 缓存。未更改 NVIDIA profile。运行前后的只读进程枚举均未发现名为 Magpie 的进程；没有停止或启动 Magpie 的操作。
- 原 `.sm86-audit/native-smoke` 证据未覆盖。另建 `.sm86-audit/community-nr-310916-default/runtime`，复制测试执行环境，只将这个新目录内的 NR DLL 换为上述精确身份。执行后再次核对源文件及隔离 DLL，哈希均未改变。

## 实测结果

物理 GPU：**NVIDIA GeForce RTX 5070 Ti**；驱动：**617.42**（本轮 `nvidia-smi` 只读查询）。两个独立进程分别执行 SDR RGBA8 和 FP16，512×288、单 Pass、抗闪烁模式 1、零光流/零深度契约。

| 项目 | SDR | FP16 |
|---|---:|---:|
| 实际 Loaded DLSSNR 版本 | 310.9.16.0 | 310.9.16.0 |
| Feature 18 创建 | 成功 | 成功 |
| 新合成捕获帧 Draw/Evaluate 返回成功 | 6 | 6 |
| fence 完成后 GPU staging 读取 | 8 | 8 |
| 时间参数 | 5 → 80 → 200 ms | 5 → 80 → 200 ms |
| 单改时间后的重复 Draw 输出哈希不变 | 2 次 | 2 次 |
| 新输入产生变化且有限的空间输出 | 是 | 是 |
| Drain、D3D11/D3D12 设备健康 | 成功／S_OK | 成功／S_OK |
| 子进程退出码 | 0 | 0 |

完整生产日志确认隔离 `nvngx_dlssnr.dll` 被加载，`Feature=18 created=true`，初始化 `tau=5ms`，然后一次记录 `80ms`、一次记录 `200ms (next capture)`。每次时间修改后，对上一捕获身份再次调用 Draw，读取的完整输出哈希与修改前相同；下一新捕获帧继续处理。

**没有观测内部 NGX Evaluate 计数器。** 此次原生检查确认重复输出不变，不能单凭相同像素断言未执行网络。此前独立单元/源代码测试对无额外 Evaluate 的结论不被扩大为本次计数器实测。

输出范围与旧 310.8 测试不同：SDR RGB 范围达到 0–255；FP16 最高值随序列达到约 1.045，均为有限数。测试没有画质真值或视觉比较阈值，因此这些结果只证明原生执行与读回成功，**不证明新版画质正确、优于旧版或无伪影**。

## 日志解释与边界

通用 NGX signed discovery 在启动时记录了无签名 snippet 校验失败及 `0xBAD00000`；随后 Magpie 原有直接 snippet 加载路径加载上述 DLL，创建 Feature 并成功执行。保留这些早期错误，不能将它们省略后称日志完全无错误，也不能忽略后续成功阶段而称初始化失败。现有生产日志中的 `path=signed-snippet` 是历史内部路径名称，**并不表示社区 DLL 通过 Authenticode**。

本次没有重跑未修改的 SR/FG，也没有为这一次 DLL 切换扩展测试框架。2/3 Pass、抗闪烁模式 2/3/4、真实输入光流、长时间运行、画质/性能、GUI 停启/调整尺寸和 PotPlayer 暂停截图均不属于这次通过结论。**RTX 20/30/40 没有本轮实卡证据**，不从 RTX 5070 Ti 的结果推断其兼容性。

## 命令与证据

在本工作树执行：

```powershell
python tests/run_dlss_preset_smoke.py --source . --build-root ../release/v0.6.11-beta1-candidate --output .sm86-audit/community-nr-310916-default --skip-build --case nr 0 1
python tests/run_dlss_preset_smoke.py --source . --build-root ../release/v0.6.11-beta1-candidate --output .sm86-audit/community-nr-310916-default --skip-build --case nr 1 1
```

上述命令依赖预先复制并校验好的新隔离 runtime；`--skip-build` 使用原已验证测试程序，并不读取/更改候选包的运行库。

[机器可读记录](20261022-community-dlssnr-default-smoke.json) 保存精确文件身份、生产日志摘录、每次输出哈希、命令结果与证据文件摘要。完整 stdout/stderr 和生产日志分别保留于：

- `C:/Users/81443/Documents/Magpie enhance/worktrees/1022-sm86-fg/.sm86-audit/community-nr-310916-default/nr-0-1/`
- `C:/Users/81443/Documents/Magpie enhance/worktrees/1022-sm86-fg/.sm86-audit/community-nr-310916-default/nr-1-1/`
