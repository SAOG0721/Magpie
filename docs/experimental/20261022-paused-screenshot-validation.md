# 暂停截图：独立输入交接验证记录

日期：2026-10-22。基线：`d687997780ac8d6e54d8b21a0c6b2be70f5eb401`。对应本次总 TODO 的 P01–P07。

## 已确认问题与修改

原路径在请求包含输入时设置强制重绘，并将原生输入快照附在 FG 呈现槽上。无新捕获帧的软件 FG 会在发布前返回；代理 FG 的 idle 重绘又不满足 `contentFrame` 条件。因此成功创建输入快照不等于它能到达保存路径。

现在请求在前端固定已成功呈现的输出及其身份；后端 dispatcher 直接复制保留的原生输入，以请求自身的一格交接通知前端。请求不再设置 `_forceNextRender`，不调用效果 Draw/Evaluate、不推进捕获 ID、FG 队列或 FPS 计数。

- 原生输入与固定输出的 `frameId / captureSequence / resourceGeneration` 三元组完全一致时，交接完成即可保存，不需要再提交一帧。
- 后端已经领先时，只接受该候选随后实际成功呈现的真实输出；候选若已被跳过，重新取得当前保留输入。不把不同帧或生成帧硬配成原始输入对照。
- InputOnly 在输入交接后直接保存，不要求已有输出或新呈现。OutputOnly 保留已呈现输出直接复制到 staging 的路径。
- NT 共享输入资源以 keyed mutex 转交。生产复制函数主动 Flush，暂停后不依赖下一次发布提交 GPU 工作；后端在复制及 ReleaseSync 后发出 fence/event。后台只限时等待 Win32 event；回到前端 dispatcher 后才 OpenSharedResource、AcquireSync(1, 0)、复制 staging 和 ReleaseSync，再以 fence 等待读取完成。
- 停止、资源重建及捕获中断明确取消未完成请求；已进入读取的不可变快照遵循会话停止检查。等待超时仍是兜底。请求开始、输入交接、身份/捕获错误和落盘结果均有请求 ID 日志。
- 保持同编号 input/output、独占新文件、部分失败、原生输入尺寸与 SDR/HDR 色彩导出。对照开关不参与截图选择。

## 自动验证

命令：`tests/Run-ScreenshotContentTests.ps1`。

新增 `ScreenshotHandoffTests.cpp` 实际执行生产 `SceneScreenshotFrame::CopyInput`，在两个独立 D3D11 WARP 设备间传递 37×19 输入，覆盖 BGRA8 和 FP16。生产者复制后不再渲染、发布或额外 Flush；前端设备明确使用 D3D11_CREATE_DEVICE_SINGLETHREADED；后台仅等待生产函数创建的完成事件，同时前端提交 128 次独立 context 复制，然后在前端取得 keyed ownership。消费者复制后经过 D3D11 fence，逐行验证原始像素。它覆盖暂停后独立 GPU 交接与尺寸保持，不只是源码字符串断言。

同一测试执行生产候选选择函数：无后续 Present 的已固定配对、InputOnly 没有输出、后端领先、生成帧拒绝、真实帧到达、候选被跳过后重试、sequence/generation 不一致。另覆盖 Pending 已被 Reading 领取后的取消，以及 256 次 Pending→Reading 与 Pending→Finished 并发竞争，确保只有一个状态转移胜出。

既有验证继续覆盖：截图内容配置恢复、文件编号/溢出、并发成对保存、部分失败、不覆盖已有文件、临时文件清理、真实 WIC PNG 编码/解码、HDR 导出。脚本对 RendererScreenshot、Renderer、ScalingRuntime、ScreenshotHelper、TextureHelper 五个生产编译单元执行 MSVC 语法检查。

## 开销与边界

没有新增持续输入缓存或逐帧复制。请求进行时最多保留一个输入候选和一个固定输出；进入 readback 后各增加对应 staging。替换固定输出时会短暂持有旧/新两个输出。分配字节随原生/输出尺寸和格式变化，驱动分配及实际显存峰值仍须实机测量。

WARP 验证不等于 NVIDIA 硬件、PotPlayer 或真实窗口操作验收。尚未部署、重启正在使用的 Magpie/PotPlayer 或更改用户配置。无 FG 的用户现场失败原因仍未证实；新增入口/交接/取消日志用于区分请求未到达、捕获重启、资源变化和 GPU 读取失败。

P01 的现场版本/捕获方式复现、P07 原生验收矩阵仍待执行：播放、暂停超过 10 秒、拖动后暂停；普通缩放/VSR/DLSSNR/各可用 FG；按钮和快捷键 × 三种内容；连续截图、工具栏与光标变化、停启、分辨率变化、重复帧过滤。检查暂停无需恢复播放即可落盘，配对三元身份一致，截图不新增时序 Evaluate 或生成帧计数。正常软件/VFX FG 序列以真实帧结尾；请求发生在生成帧期间仍需等该序列的真实帧实际提交，不能以自动测试替代这一原生接受条件。


## 审查补充：线程归属与取消竞争

初版把前端共享资源的 AcquireSync 放到后台，忽略了前端设备可能使用 SINGLETHREADED。现已改为后台仅等待后端 fence 的 Win32 event，前端 DXGI/context 操作与正常资源释放均留在 dispatcher。输入和 staging 的超时结果也在回到 dispatcher 后处理，避免在后台错误返回时释放前端资源。

捕获中断及后端快照失败使用原子的 Pending→Finished 取消；如果前端已进入 Reading，取消不会改变状态、报告伪失败或提前解除 Busy。读取协程自己的失败仍能结束 Reading。此修正来自源码审查，不代表用户现场曾触发该竞争。


## 0.6.11 截图时间戳命名

截图按钮和快捷键在请求进入时记录本地墙钟，异步等待及编码不重新取时间。示例为 `Magpie_20261022_194530_123_input.png` / `Magpie_20261022_194530_123_output.png`；单图同样保留 `_input` 或 `_output`。同一时间戳已有任一侧文件或同名目录时，整组使用 `_01`、`_02` 等，例如 `Magpie_20261022_194530_123_01_input.png`。旧数字编号文件保留且不影响新命名。

右击效果导出共用时间戳与碰撞分配器：效果输出使用 `_effect_01_output.png`；通道纹理使用 `_effect_01_pass_02_texture_03.png` 或 `.dds`。效果/通道编号从 1 开始，texture 为效果内部原始纹理编号。PNG 最终输出与 DDS 中间格式及原有导出内容不变。

实际目录测试覆盖八个相同毫秒请求的并发配对、部分成功后的整组避让、单图及右击 DDS 名称占用、大小写碰撞、旧文件共存和独占保存不覆盖；暂停 GPU 交接、色彩及配置测试继续运行。保存互斥覆盖名称选择到所有侧写入，独占文件提交仍是最终防覆盖边界。
