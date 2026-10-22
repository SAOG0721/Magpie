# DLSS SR / NR 参数实施记录

基线：`d6879977`；分支：`feature/dlss-models-antiflicker`。所有控件沿用效果器现有参数浮窗和实时参数面板。

- SR 新增 `renderPreset`：自动、A–F、J/K/L/M，默认 K。保存稳定 NGX 数值；缺字段或非法值回到 K，有效旧值保留。SDR/HDR 都传入 Balanced hint，自动不写 hint；隐藏旧 HDR 适配器保持 J。更换预设沿用重启缩放，Resize 保留设置。日志区分请求与未知的实际模型，并记录可观察到的已加载 DLL 路径和版本。
- NR 新增 `antiFlickerTimeMs`：5–200 ms、步进 5、默认 80，关闭抗闪烁时隐藏。它仅属于链末端，不复制到 Pass 2/3。统一规范化 UI/后端值；只改时间不调用 NR 参数更新、不清空历史、不改变当前帧 revision。下一有效捕获帧采用新值；250 ms 断流重置规则保持不变。
- Choice 参数拒绝非整数及超出 int 范围的值，回到描述符默认值，避免导入值经四舍五入成为另一个有效预设。

已通过：`Run-DLSSParametersTests.ps1`、`Run-DLSSNRMultiPassTests.ps1`、`Run-DLSSNRTemporalTests.ps1`（WARP / D3D11 debug 无警告）、`Run-DlssOpticalFlowTests.ps1`。覆盖实际 HLSL 描述、UI/后端规范化、缺字段/旧值、条件显示、时间尺度、四模式、1/2/3 Pass、时间单改及混合修改、Resize、重复帧与历史保留。

整合后执行全量候选编译、目录文案与资源检查。真实 NVIDIA SR 模型采用情况、驱动覆盖、性能/画质、NR SDR/HDR 实际画面、配置导入导出及完整页面交互验收尚未以这些单元/WARP 测试代替；未部署或修改用户配置。

映射来源：[现行 NVIDIA NGX 头文件](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs.h)、[历史 A–F 枚举](https://github.com/NVIDIA-RTX/Streamline/blob/v2.7.32/include/sl_dlss.h)。只引用数值契约，不引入 Streamline。
