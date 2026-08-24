# 项目架构

## 初赛与决赛关系

初赛是三项独立的 HLS 算子优化任务，重点在循环、存储、数据流和资源映射；
决赛是完整的软硬件协同频谱仪，重点在实时采集、数字信号处理、AXI 数据通路
和 PYNQ 交互。两阶段共同构成全国二等奖项目，但代码与工具版本分别归档。

## 决赛硬件数据流

1. Clocking Wizard 产生 32 MHz 采样时钟，并由 `adc_to_axis_v1_0` 中的 ODDR
   转发到外部 ADC。
2. ADC 的 10 位偏移二进制被转换为以零为中心的补码，符号扩展为 32 位 AXIS。
3. 异步 AXIS FIFO 完成 32 MHz 采样域与 100 MHz 处理域之间的跨时钟传输。
4. `ddc_processor` 用 32 位 NCO 和 1024 点 Q1.15 正弦表执行复数混频，并对
   每 `decimation` 个输入积分后输出一个 I/Q 样本；旁路模式直接输出实数。
5. `fft_spectrum_analyzer` 对实数或复数输入按需加 Hann 窗，调用 FFT IP 后
   输出 `real^2 + imag^2` 单精度浮点功率。
6. AXI DMA 把功率谱写入 PS 的连续内存。AXIS Switch 允许 UDP 音频经 MM2S
   进入相同 DDC 旁路/FFT 通路。

## 软件状态机

| 模式 | 数据源 | DDC 配置 | 频谱坐标 |
| --- | --- | --- | --- |
| HIGH | 32 MSPS ADC | NCO 锁定中心频率，按带宽抽取 | 中心频率附近的频偏 |
| LOW | 32 MSPS ADC | NCO=0，按带宽抽取 | 0~低频观察上限 |
| AUDIO | UDP 48 kHz PCM | bypass=1 | 0~24 kHz |

跨射频/音频模式时，PYNQ 软件重新下载 Overlay，以清除 HLS 静态状态、DMA 和
AXI 流中可能残留的数据。频谱显示采用指数移动平均抑制抖动，并在当前观察窗内
追踪最大功率频点。

## 接口契约

- DDC 输入：`TDATA[15:0]` 为有符号 ADC；输出 `TDATA[15:0]` 为 I，
  `TDATA[31:16]` 为 Q。
- FFT 输入：`ddc_en=0` 时只解释低 16 位；`ddc_en=1` 时解释 I/Q 两路。
- FFT 输出：32 位 IEEE-754 单精度功率的原始位模式，PS 端必须 `view(float32)`。
- ADC、DDC、FFT 和 DMA 的帧点数必须一致，否则会等待不足数据或产生帧错位。
