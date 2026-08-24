# 构建、部署与验证

## 环境

| 阶段 | 推荐环境 |
| --- | --- |
| 初赛 | AMD Vitis HLS 2024.2，目标器件 `xc7z020-clg484-1` |
| 决赛 HLS/Vivado | AMD Vitis/Vivado 2025.2，目标器件 `xc7z020clg400-1` |
| 决赛板端 | 能加载 Zynq-7020 Overlay 的 PYNQ Linux、Python 3、NumPy、Matplotlib、ipywidgets |

不同版本可能升级 FFT、DMA 或 Block Design IP。若 Vivado 提示升级 IP，应先保存
原工程副本，并重新执行 C 仿真、综合、时序和板端验证。

## 初赛复现

进入 `preliminary/` 后按其 README 执行三套 `run_hls.tcl`。历史综合与协同仿真
报告已随各测试目录保留，便于在无法安装 Vitis 2024.2 时核对结果。

## 决赛 HLS IP

在 Vitis Unified IDE 2025.2 中分别导入以下组件描述：

- `finals/hls/ddc/vitis-comp.json`
- `finals/hls/fft/vitis-comp.json`

对每个组件依次执行 C Simulation、C Synthesis 和 Package。配置文件已指定
100 MHz 时钟、`xc7z020clg400-1` 和 IP Catalog 输出。命令行环境中也可在各
组件目录执行：

```bash
vitis-run --mode hls --csim --config hls_config.cfg --work_dir hls_component
vitis-run --mode hls --synthesis --config hls_config.cfg --work_dir hls_component
vitis-run --mode hls --package --config hls_config.cfg --work_dir hls_component
```

DDC 目录的工作目录名使用 `hls_component_ddc`，与其 `vitis-comp.json` 一致。
若当前安装版本的参数名称不同，以 `vitis-run --help` 为准；不要提交生成的
`.Xil`、`.cache`、`logs`、`hls/` 或打包中间目录。

## Vivado 系统工程

1. 完成两项 HLS IP 的 Package，确认 `finals/hls/fft` 和 `finals/hls/ddc`
   下生成可被 Vivado 扫描的 IP Catalog。
2. 用 Vivado 2025.2 打开 `finals/vivado/Qi.xpr`。工程的 IP Repository 已改为
   相对路径，便于仓库移动。
3. 在 IP Catalog 中刷新仓库，确认 `ddc_processor:1.0` 和
   `fft_spectrum_analyzer:1.0` 均可解析。
4. 打开 `design_1`，执行 Validate Design、Generate Output Products 和
   Create HDL Wrapper。
5. 依次运行 Synthesis、Implementation、Generate Bitstream，并检查无关键
   时序违例和未约束端口。
6. 导出 `.bit` 与 `.hwh`，保持相同基本文件名。仓库 `finals/deploy/` 中提供了
   决赛归档版本，可用于快速板端复现。

## PYNQ 部署

把以下三个文件复制到板端同一 Notebook 目录：

```text
FFT.bit
FFT.hwh
spectrum_analyzer.py
```

在 Jupyter 中执行 `%run spectrum_analyzer.py`，或把文件内容作为一个代码单元
运行。射频模式需要连接 10 位 ADC；音频模式需要向板端 UDP 12345 端口发送
每包 4096 个 little-endian `int16` PCM 采样。停止时点击界面按钮，等待程序
释放 DMA 连续内存和套接字后再重新运行。

## 验证清单

- DDC C 仿真输出 16384 个 I/Q 点，最后一点 TLAST=1。
- FFT C 仿真能在双音输入的半谱中检测到超过阈值的峰值。
- Vivado Block Design Validate Design 通过，HLS IP 和时钟/复位均已解析。
- 射频输入单音时，主峰随 NCO 中心频率和抽取倍数正确移动。
- UDP 音频单音时，0~24 kHz 范围内主峰与发送频率一致。
