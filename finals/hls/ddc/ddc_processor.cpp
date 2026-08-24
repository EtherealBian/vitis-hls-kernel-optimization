/**
 * @file ddc_processor.cpp
 * @brief 基于 NCO 查找表、复数混频和积分抽取的数字下变频单元。
 * @author Ethereal
 *
 * 输入为低 16 位有效的有符号 ADC 采样。DDC 模式下，每个输入采样依次与
 * Q1.15 格式的 cos/sin 系数相乘，在 decimation 个采样上积分，再输出一组
 * 低 16 位 I、高 16 位 Q 的复基带数据；旁路模式下直接输出 I，Q 固定为 0。
 * 输出帧长始终为 2^nfft_log2，输入需求量分别为 N 或 N*decimation。
 */

#include "ap_axi_sdata.h"
#include "ap_int.h"
#include "ddc_lut1024.h"
#include "hls_stream.h"

namespace {

using AxisWord = ap_axiu<32, 1, 1, 1>;
using Sample = ap_int<16>;
using Coefficient = ap_int<16>;
using Accumulator = ap_int<48>;
using Phase = ap_uint<32>;

/**
 * @brief 填充 AXI4-Stream 侧带信号并写出一个 I/Q 数据字。
 *
 * TLAST 由输出样本序号重新生成，不透传 ADC 输入的 TLAST。这样，DDC 的抽取
 * 倍数变化不会破坏下游 FFT 所需的固定帧边界。
 */
void WriteIqPacket(hls::stream<AxisWord>& data_out, Sample i_value,
                   Sample q_value, bool is_last) {
#pragma HLS INLINE
  AxisWord output_packet;
  output_packet.data.range(15, 0) = static_cast<ap_uint<16>>(i_value);
  output_packet.data.range(31, 16) = static_cast<ap_uint<16>>(q_value);
  output_packet.keep = -1;
  output_packet.strb = -1;
  output_packet.user = 0;
  output_packet.id = 0;
  output_packet.dest = 0;
  output_packet.last = is_last ? 1 : 0;
  data_out.write(output_packet);
}

}  // namespace

/**
 * @brief 数字下变频 HLS 顶层函数。
 *
 * @param data_in      32 位 AXI4-Stream 输入，低 16 位为 ADC 补码采样。
 * @param data_out     32 位 AXI4-Stream 输出，低/高 16 位分别为 I/Q。
 * @param nco_step     32 位相位增量，满足 nco_step=f_nco/Fs*2^32。
 * @param decimation   积分抽取倍数；0 和 1 均按 1 处理，最大有效值为 65535。
 * @param gain_shift   附加右移位数，范围被限制到 0~31。
 * @param bypass       非 0 时旁路混频与抽取，Q 通道清零。
 * @param nfft_log2    输出帧点数的二进制指数，例如 12 表示 4096 点。
 *
 * 相位累加器声明为 static，使连续启动的相邻帧保持 NCO 相位连续。若系统需要
 * 确定相位起点，应通过复位 IP 清零静态状态。乘积为 Q1.15 系数，因此固定先
 * 右移 15 位；gain_shift 再补偿积分抽取带来的幅度增长。
 */
void ddc_processor(hls::stream<AxisWord>& data_in,
                   hls::stream<AxisWord>& data_out, unsigned int nco_step,
                   unsigned int decimation, unsigned int gain_shift,
                   unsigned int bypass, int nfft_log2) {
#pragma HLS INTERFACE axis port = data_in
#pragma HLS INTERFACE axis port = data_out
#pragma HLS INTERFACE s_axilite port = nco_step bundle = ctrl
#pragma HLS INTERFACE s_axilite port = decimation bundle = ctrl
#pragma HLS INTERFACE s_axilite port = gain_shift bundle = ctrl
#pragma HLS INTERFACE s_axilite port = bypass bundle = ctrl
#pragma HLS INTERFACE s_axilite port = nfft_log2 bundle = ctrl
#pragma HLS INTERFACE s_axilite port = return bundle = ctrl

  static Phase phase_accumulator = 0;
  const int output_points = 1 << nfft_log2;

  // 两张 1024×16 位表分别推断为单端口 Block ROM，降低 LUT 逻辑占用。
#pragma HLS BIND_STORAGE variable = cos_lut_1024 type = rom_1p impl = bram
#pragma HLS BIND_STORAGE variable = sin_lut_1024 type = rom_1p impl = bram

  const ap_uint<16> decimation_factor =
      (decimation <= 1) ? static_cast<ap_uint<16>>(1)
                        : static_cast<ap_uint<16>>(decimation);
  const ap_uint<5> compensation_shift =
      (gain_shift > 31) ? static_cast<ap_uint<5>>(31)
                        : static_cast<ap_uint<5>>(gain_shift);

  if (bypass != 0) {
    // 旁路路径维持一拍一采样，供直接 ADC 或 UDP 音频频谱分析使用。
    for (int output_index = 0; output_index < output_points; ++output_index) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 1024 max = 16384
      const AxisWord input_packet = data_in.read();
      const Sample adc_sample = input_packet.data.range(15, 0);
      WriteIqPacket(data_out, adc_sample, 0,
                    output_index == output_points - 1);
    }
    return;
  }

  // DDC 路径：NCO 混频后采用矩形窗积分抽取，相当于一级 CIC 的积分/抽取。
  for (int output_index = 0; output_index < output_points; ++output_index) {
#pragma HLS LOOP_TRIPCOUNT min = 1024 max = 16384
    Accumulator i_accumulator = 0;
    Accumulator q_accumulator = 0;

    for (ap_uint<16> input_index = 0; input_index < decimation_factor;
         ++input_index) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 1 max = 16384
      const AxisWord input_packet = data_in.read();
      const Sample adc_sample = input_packet.data.range(15, 0);

      // 取相位累加器高 10 位寻址 1024 点正弦表；更新后自然按 2^32 回绕。
      const ap_uint<10> lut_index = phase_accumulator.range(31, 22);
      phase_accumulator += nco_step;
      const Coefficient cosine = cos_lut_1024[lut_index];
      const Coefficient sine = sin_lut_1024[lut_index];

      i_accumulator += static_cast<Accumulator>(adc_sample) * cosine;
      q_accumulator -= static_cast<Accumulator>(adc_sample) * sine;
    }

    const Sample i_output =
        static_cast<Sample>(i_accumulator >> (15 + compensation_shift));
    const Sample q_output =
        static_cast<Sample>(q_accumulator >> (15 + compensation_shift));
    WriteIqPacket(data_out, i_output, q_output,
                  output_index == output_points - 1);
  }
}
