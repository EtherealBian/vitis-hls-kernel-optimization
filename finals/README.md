# 决赛：Zynq 实时频谱仪

本目录是 2025 年 FPGA 创新设计竞赛 AMD 赛道决赛作品。系统以
`xc7z020clg400-1` 为目标器件，用 32 MSPS、10 位并行 ADC 采集射频/低频信号，
在 PL 中执行数字下变频和 FFT，在 PS/PYNQ 中完成配置、DMA 传输与频谱显示。

## 功能组成

| 子系统 | 文件 | 功能 |
| --- | --- | --- |
| ADC 接口 | [`vivado/Qi.srcs/sources_1/new/adc_to_axis.v`](vivado/Qi.srcs/sources_1/new/adc_to_axis.v) | 偏移二进制转补码、ODDR 时钟转发、AXI4-Stream 帧输出 |
| DDC HLS | [`hls/ddc/ddc_processor.cpp`](hls/ddc/ddc_processor.cpp) | NCO、本振查表、复数混频、积分抽取、旁路 |
| FFT HLS | [`hls/fft/fft_spectrum_analyzer.cpp`](hls/fft/fft_spectrum_analyzer.cpp) | Hann 窗、1024~16384 点可配置 FFT、功率谱 |
| 系统连接 | [`vivado/Qi.srcs/sources_1/bd/design_1/design_1.bd`](vivado/Qi.srcs/sources_1/bd/design_1/design_1.bd) | PS7、时钟、复位、GPIO、AXIS FIFO/Switch、DMA、HLS IP 的 Block Design |
| 管脚约束 | [`vivado/Qi.srcs/constrs_1/new/QingLan.xdc`](vivado/Qi.srcs/constrs_1/new/QingLan.xdc) | ADC 数据/时钟管脚和异步时钟组 |
| PYNQ 软件 | [`pynq/spectrum_analyzer.py`](pynq/spectrum_analyzer.py) | 三模式控制、DMA、UDP 音频、频谱显示和峰值追踪 |
| 部署产物 | [`deploy/`](deploy/) | 与当前工程对应的 `FFT.bit`、`FFT.hwh` |

## 关键运行参数

- ADC 采样率：32 MHz；音频采样率：48 kHz。
- FFT 演示配置：4096 点；HLS 最大配置：16384 点。
- NCO 相位增量：`f_nco / Fs * 2^32`。
- DDC 输出采样率：`Fs / decimation`。
- 频率分辨率：`Fs_out / NFFT`。
- UDP 音频：端口 12345，每帧 4096 个 little-endian `int16` 采样。

源码构建与板端部署见 [`../docs/REPRODUCTION.md`](../docs/REPRODUCTION.md)。
