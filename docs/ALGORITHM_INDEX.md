# 文件与算法功能索引

## 初赛

| 文件 | 算法功能 |
| --- | --- |
| [`preliminary/security/L1/include/xf_security/sha224_256.hpp`](../preliminary/security/L1/include/xf_security/sha224_256.hpp) | SHA-224/SHA-256 补位、消息调度、64 轮压缩及任务流组织；为 HMAC-SHA256 提供散列内核 |
| [`preliminary/data_compression/L1/include/hw/lz_compress.hpp`](../preliminary/data_compression/L1/include/hw/lz_compress.hpp) | LZ77 哈希字典、滑动窗口、并行候选匹配和最长匹配选择 |
| [`preliminary/data_compression/L1/include/hw/lz4_compress.hpp`](../preliminary/data_compression/L1/include/hw/lz4_compress.hpp) | 将 LZ77 匹配结果编码为 LZ4 Token、Literal、Offset 和 Match Length |
| [`preliminary/solver/L1/include/hw/cholesky.hpp`](../preliminary/solver/L1/include/hw/cholesky.hpp) | 对称/厄米正定矩阵 Cholesky 分解、部分和并行与三角矩阵输出 |

初赛的完整依赖和报告索引见
[`preliminary/docs/ALGORITHM_INDEX.md`](../preliminary/docs/ALGORITHM_INDEX.md)。

## 决赛 HLS/RTL

| 文件 | 算法或硬件功能 |
| --- | --- |
| [`finals/vivado/Qi.srcs/sources_1/new/adc_to_axis.v`](../finals/vivado/Qi.srcs/sources_1/new/adc_to_axis.v) | 10 位 ADC 偏移二进制转补码、32 位符号扩展、AXI4-Stream 单级弹性寄存器、TLAST 组帧和 ODDR 时钟转发 |
| [`finals/hls/ddc/ddc_processor.cpp`](../finals/hls/ddc/ddc_processor.cpp) | 32 位相位累加器、1024 点 NCO 查表、实数乘复本振、48 位积分累加、整数抽取和旁路 |
| [`finals/hls/ddc/ddc_lut1024.h`](../finals/hls/ddc/ddc_lut1024.h) | Q1.15 格式正弦/余弦系数，综合为两块单端口 Block ROM |
| [`finals/hls/ddc/ddc_tb.cpp`](../finals/hls/ddc/ddc_tb.cpp) | 生成 32 MSPS/4 MHz 余弦测试信号，验证抽取输出点数和 TLAST |
| [`finals/hls/fft/fft_spectrum_analyzer.cpp`](../finals/hls/fft/fft_spectrum_analyzer.cpp) | 实数/IQ 解包、Hann 窗、动态点数正向 FFT、复数模平方和浮点位级封装 |
| [`finals/hls/fft/tb_fft.cpp`](../finals/hls/fft/tb_fft.cpp) | 生成 50.4/120.4 Hz 双音，验证半谱存在显著峰值 |
| [`finals/vivado/Qi.srcs/sources_1/bd/design_1/design_1.bd`](../finals/vivado/Qi.srcs/sources_1/bd/design_1/design_1.bd) | PS7、AXI Interconnect/SmartConnect、DMA、GPIO、AXIS FIFO/Switch、DDC/FFT IP 与 ADC RTL 的系统互连 |
| [`finals/vivado/Qi.srcs/constrs_1/new/QingLan.xdc`](../finals/vivado/Qi.srcs/constrs_1/new/QingLan.xdc) | ADC 管脚、电气标准以及 32/100 MHz 异步时钟组约束 |

## 决赛 PYNQ 软件

| 文件 | 软件功能 |
| --- | --- |
| [`finals/pynq/spectrum_analyzer.py`](../finals/pynq/spectrum_analyzer.py) | Overlay 下载、IP 寄存器配置、DMA 缓冲区生命周期、UDP PCM 接收、功率转 dB、FFT shift、指数平均、自动缩放和主峰 Marker |
| [`finals/deploy/FFT.bit`](../finals/deploy/FFT.bit) | 对应决赛 Vivado 工程的 FPGA 配置比特流 |
| [`finals/deploy/FFT.hwh`](../finals/deploy/FFT.hwh) | PYNQ 解析 IP、地址空间和寄存器映射所需的硬件描述 |
