/**
 * @file fft_spectrum_analyzer.cpp
 * @brief 可配置窗函数与点数的流式 FFT 频谱功率计算单元。
 * @author Ethereal
 *
 * 数据格式：输入与输出均采用 32 位 AXI4-Stream。旁路 DDC 时，输入低 16 位
 * 为有符号实数采样；启用 DDC 时，低/高 16 位分别为有符号 I/Q。输出 32 位
 * 数据是单精度浮点功率值 |X[k]|^2 的位级表示，TLAST 标记一帧结束。
 *
 * 控制约束：nfft_log2 必须位于 FFT IP 支持范围内，当前最大值为 14，即
 * 16384 点；window_type=1 启用 Hann 窗，其余值使用矩形窗；ddc_en=1 表示
 * 输入是复数 I/Q。调用方必须保证每次启动前送入恰好 2^nfft_log2 个采样点。
 */

#include "ap_axi_sdata.h"
#include "hls_fft.h"
#include "hls_math.h"
#include "hls_stream.h"

#include <complex>
#include <cstdint>

namespace {

constexpr int kMaxNfftLog2 = 14;
constexpr float kTwoPi = 6.28318530717958647692F;

/** FFT LogiCORE 的综合期参数。 */
struct FftParams : hls::ip_fft::params_t {
  // 输出按 0、1、...、N-1 自然顺序排列，便于 PS 端直接建立频率坐标。
  static const unsigned ordering_opt = hls::ip_fft::natural_order;
  static const unsigned max_nfft = kMaxNfftLog2;
  static const bool has_nfft = true;

  // 输入输出均使用单精度复数；AXI 配置与状态通道按字节对齐。
  static const unsigned input_width = 32;
  static const unsigned output_width = 32;
  static const unsigned config_width = 24;
  static const unsigned status_width = 8;
  static const unsigned phase_factor_width = 24;

  // 基 4 Burst I/O 在吞吐率和资源占用之间折中，复乘优先映射到 DSP。
  static const unsigned arch_opt = hls::ip_fft::radix_4_burst_io;
  static const unsigned complex_mult_type =
      hls::ip_fft::use_mults_performance;
};

using FftConfig = hls::ip_fft::config_t<FftParams>;
using FftStatus = hls::ip_fft::status_t<FftParams>;
using ComplexFloat = std::complex<float>;
using AxisWord = ap_axiu<32, 1, 1, 1>;

/**
 * @brief 将打包的实数或 I/Q 采样转换为复数，并按需乘 Hann 窗。
 *
 * Hann 系数为 w[n]=0.5*(1-cos(2*pi*n/(N-1)))。循环 II=1，使输入端在
 * 下游允许时每周期接收一个采样。函数不依赖输入 TLAST，而以控制寄存器中的
 * 帧长为准，因此软件、ADC 帧发生器和本模块的点数配置必须保持一致。
 */
void ApplyWindow(hls::stream<AxisWord>& data_in,
                 hls::stream<ComplexFloat>& fft_in, int nfft_log2,
                 int window_type, int ddc_en) {
  const int points = 1 << nfft_log2;

  for (int sample_index = 0; sample_index < points; ++sample_index) {
#pragma HLS PIPELINE II = 1
    const AxisWord input_packet = data_in.read();
    const ap_int<16> i_sample = input_packet.data.range(15, 0);
    const ap_int<16> q_sample = input_packet.data.range(31, 16);

    const ComplexFloat input_value =
        (ddc_en == 0)
            ? ComplexFloat(static_cast<float>(i_sample), 0.0F)
            : ComplexFloat(static_cast<float>(i_sample),
                           static_cast<float>(q_sample));

    float window_coefficient = 1.0F;
    if (window_type == 1 && points > 1) {
      const float phase =
          kTwoPi * static_cast<float>(sample_index) /
          static_cast<float>(points - 1);
      window_coefficient = 0.5F * (1.0F - hls::cos(phase));
    }

    fft_in.write(input_value * window_coefficient);
  }
}

/**
 * @brief 消耗 FFT 状态字，并把复数频谱转换为功率谱 AXI4-Stream。
 *
 * FFT IP 每帧同时产生数据流和状态流。即使上层暂不使用溢出状态，也必须读走
 * 状态字以免内部 FIFO 堵塞。输出浮点数通过 union 做位级封装，不执行数值型
 * 整数转换；PS 端应使用 float32 视图解释该 32 位字。
 */
void CalculatePower(hls::stream<ComplexFloat>& fft_out,
                    hls::stream<FftStatus>& fft_status,
                    hls::stream<AxisWord>& data_out, int nfft_log2) {
  const int points = 1 << nfft_log2;
  const FftStatus frame_status = fft_status.read();
  (void)frame_status;

  for (int bin_index = 0; bin_index < points; ++bin_index) {
#pragma HLS PIPELINE II = 1
    const ComplexFloat spectrum_value = fft_out.read();
    const float power = spectrum_value.real() * spectrum_value.real() +
                        spectrum_value.imag() * spectrum_value.imag();

    union FloatBits {
      float value;
      std::uint32_t bits;
    } converter;
    converter.value = power;

    AxisWord output_packet;
    output_packet.data = converter.bits;
    output_packet.keep = -1;
    output_packet.strb = -1;
    output_packet.user = 0;
    output_packet.id = 0;
    output_packet.dest = 0;
    output_packet.last = (bin_index == points - 1) ? 1 : 0;
    data_out.write(output_packet);
  }
}

}  // namespace

/**
 * @brief FFT 频谱分析 HLS 顶层函数。
 *
 * AXI4-Lite 的 ctrl 地址空间同时承载块级 ap_start/ap_done 控制和三个运行时
 * 参数；DATAFLOW 使加窗、FFT、功率计算三个阶段并行工作。内部 FIFO 深度用于
 * 吸收各阶段突发访问差异，实际流控仍由 AXI4-Stream 握手保证。
 */
void fft_spectrum_analyzer(hls::stream<AxisWord>& data_in,
                           hls::stream<AxisWord>& data_out, int nfft_log2,
                           int window_type, int ddc_en) {
#pragma HLS INTERFACE axis port = data_in
#pragma HLS INTERFACE axis port = data_out
#pragma HLS INTERFACE s_axilite port = nfft_log2 bundle = ctrl
#pragma HLS INTERFACE s_axilite port = window_type bundle = ctrl
#pragma HLS INTERFACE s_axilite port = ddc_en bundle = ctrl
#pragma HLS INTERFACE s_axilite port = return bundle = ctrl

  static hls::stream<ComplexFloat> fft_input_stream("fft_input_stream");
  static hls::stream<ComplexFloat> fft_output_stream("fft_output_stream");
  static hls::stream<FftConfig> fft_config_stream("fft_config_stream");
  static hls::stream<FftStatus> fft_status_stream("fft_status_stream");

#pragma HLS STREAM variable = fft_input_stream depth = 4100
#pragma HLS STREAM variable = fft_output_stream depth = 4100
#pragma HLS STREAM variable = fft_config_stream depth = 4
#pragma HLS STREAM variable = fft_status_stream depth = 4
#pragma HLS DATAFLOW

  FftConfig frame_config;
  frame_config.setDir(1);  // 1 为正向 FFT：时域/复基带转换到频域。
  frame_config.setNfft(nfft_log2);
  fft_config_stream.write(frame_config);

  ApplyWindow(data_in, fft_input_stream, nfft_log2, window_type, ddc_en);
  hls::fft<FftParams>(fft_input_stream, fft_output_stream, fft_status_stream,
                      fft_config_stream);
  CalculatePower(fft_output_stream, fft_status_stream, data_out, nfft_log2);
}
