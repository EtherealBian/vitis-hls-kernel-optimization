# Zynq Spectrum & HLS

[中文](#中文) · [English](#english)

![Zynq spectrum analyzer and HLS kernels](docs/project-overview.png)

## 中文

2025 FPGA 创新设计竞赛 AMD 赛道全国二等奖项目。初赛针对三项 HLS 算子优化计算和存储结构。决赛将 ADC、数字下变频、FFT 与 PYNQ 集成到一套 Zynq-7000 频谱仪。

### 决赛频谱仪

1. **RF 采集与跨时钟域**。10 位 ADC 以 32 MSPS 采集。RTL 将偏移二进制转换为补码并封装成 AXI4-Stream，异步 FIFO 将数据送入 100 MHz 处理域。
2. **数字下变频与抽取**。32 位 NCO 查 Q1.15 正弦表生成本振，实数输入与复本振混频。对 I/Q 样本分别积分，每达到设定抽取倍数输出一组结果。
3. **UDP 音频旁路**。PYNQ 接收 48 kHz PCM，经 DMA MM2S 与 AXIS Switch 送入同一处理链。DDC 在音频模式下旁路混频和抽取，FFT 使用实数输入。
4. **功率谱与显示**。HLS 核可启用 Hann 窗，执行可配置点数的 FFT 并输出模平方。DMA S2MM 回传浮点功率，PYNQ 完成 dB 转换、指数平均和主峰标记。

| 模式 | 数据源 | 频谱视图 |
| --- | --- | --- |
| HIGH | ADC，NCO 调至观察中心 | 观察中心附近的频偏 |
| LOW | ADC，NCO 为零 | 从直流起的低频频段 |
| AUDIO | UDP 48 kHz PCM | 0～24 kHz |

### 初赛 HLS 算子

| 算子 | 实现关注点 |
| --- | --- |
| HMAC-SHA256 | 消息补位、消息调度和 64 轮压缩的流式组织 |
| LZ4 | LZ77 滑窗候选匹配、最长匹配选择和 LZ4 序列编码 |
| Cholesky | 正定矩阵的部分和并行计算与三角矩阵输出 |

[初赛源码、基线与报告](preliminary/) · [算子与文件索引](docs/ALGORITHM_INDEX.md)

### 部署和复现

- **板端试跑**。将 [FFT.bit](finals/deploy/FFT.bit)、[FFT.hwh](finals/deploy/FFT.hwh) 和 [spectrum_analyzer.py](finals/pynq/spectrum_analyzer.py) 放在板端同一 Jupyter 目录，运行 `%run spectrum_analyzer.py`。音频模式接收发往板端 UDP 12345 的小端 `int16` PCM，每包 4096 个样本。
- **重建决赛系统**。使用 Vitis/Vivado 2025.2，对 [DDC](finals/hls/ddc/) 和 [FFT](finals/hls/fft/) 分别执行 C 仿真、综合和 IP 打包。在 [Qi.xpr](finals/vivado/Qi.xpr) 中刷新 IP、验证 Block Design，再生成配套的 bit/HWH。
- **复现初赛算子**。用 Vitis HLS 2024.2 执行各算子的 `run_hls.tcl`，并核对归档的综合及协同仿真报告。

[完整命令与验证清单](docs/REPRODUCTION.md) · [AXIS 接口与模式切换](docs/ARCHITECTURE.md) · [归档范围](docs/OPEN_SOURCE_SCOPE.md)

## English

This 2025 AMD-track FPGA competition entry won national second prize. The preliminary round optimized three HLS kernels. The finals integrated ADC capture, digital downconversion, FFT, and a PYNQ interface on Zynq-7000.

### Finals spectrum analyzer

1. **RF capture.** A 10-bit ADC runs at 32 MSPS. RTL converts offset binary to signed AXI4-Stream, and an asynchronous FIFO bridges the sampling and 100 MHz processing domains.
2. **Downconversion.** A 32-bit NCO uses a Q1.15 sine table to generate the complex local oscillator. The DDC mixes the real input and integrates each I/Q channel before emitting one pair per decimation interval.
3. **Audio bypass.** PYNQ receives 48 kHz PCM over UDP. DMA MM2S and an AXIS switch feed the same pipeline, with DDC mixing and decimation bypassed for real-valued audio.
4. **Spectrum display.** Optional Hann windowing precedes the configurable FFT. The kernel computes magnitude squared, DMA S2MM returns floating-point power, and PYNQ converts it to dB, smooths the trace, and marks the strongest peak.

HIGH mode tunes the NCO to the center of the band of interest. LOW mode sets the NCO to zero. AUDIO mode displays the 0–24 kHz half-spectrum.

### Preliminary HLS kernels

The [preliminary archive](preliminary/) includes source, baselines, tests, and reports. HMAC-SHA256 covers stream organization around padding, message scheduling, and the 64-round compression core. LZ4 covers sliding-window match selection and sequence encoding. Cholesky uses parallel partial sums to produce a triangular factor. [Source and algorithm index](docs/ALGORITHM_INDEX.md)

### Deploy and reproduce

- **Run the archived overlay.** Copy the matching [bitstream](finals/deploy/FFT.bit), [HWH](finals/deploy/FFT.hwh), and [PYNQ script](finals/pynq/spectrum_analyzer.py) into one Jupyter directory on the board. Run `%run spectrum_analyzer.py`. The audio input accepts UDP on port 12345 with 4096 little-endian `int16` PCM samples per packet.
- **Rebuild the finals system.** Use Vitis/Vivado 2025.2 to simulate, synthesize, and package the [DDC](finals/hls/ddc/) and [FFT](finals/hls/fft/) IP. Refresh the IP catalog in [Qi.xpr](finals/vivado/Qi.xpr), validate the block design, and export a matching bit/HWH pair.
- **Reproduce the preliminary kernels.** Use Vitis HLS 2024.2 and the per-kernel `run_hls.tcl` scripts. Check the archived synthesis and co-simulation reports alongside the new runs.

[Reproduction and verification guide](docs/REPRODUCTION.md) · [AXIS interfaces and modes](docs/ARCHITECTURE.md) · [Archive scope](docs/OPEN_SOURCE_SCOPE.md)
