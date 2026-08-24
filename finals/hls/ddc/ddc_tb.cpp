/**
 * @file ddc_tb.cpp
 * @brief 数字下变频单元的 C 仿真测试平台。
 * @author Ethereal
 */

#include "ap_axi_sdata.h"
#include "hls_stream.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>

using AxisWord = ap_axiu<32, 1, 1, 1>;

void ddc_processor(hls::stream<AxisWord>& data_in,
                   hls::stream<AxisWord>& data_out, unsigned int nco_step,
                   unsigned int decimation, unsigned int gain_shift,
                   unsigned int bypass, int nfft_log2);

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

int main() {
  constexpr double kSampleRateHz = 32000000.0;
  constexpr double kTargetFrequencyHz = 4000000.0;
  constexpr int kNfftLog2 = 14;
  constexpr int kOutputPoints = 1 << kNfftLog2;
  constexpr int kDecimation = 100;
  constexpr int kInputPoints = kOutputPoints * kDecimation;

  hls::stream<AxisWord> input_stream("input_stream");
  hls::stream<AxisWord> output_stream("output_stream");

  // 构造 4 MHz、峰值 500 LSB 的 ADC 余弦波。抽取 100 倍时必须提供
  // kOutputPoints*kDecimation 个输入采样，才能产生完整的 FFT 输出帧。
  for (int sample_index = 0; sample_index < kInputPoints; ++sample_index) {
    const double time_s = static_cast<double>(sample_index) / kSampleRateHz;
    const std::int16_t adc_sample = static_cast<std::int16_t>(
        500.0 * std::cos(2.0 * kPi * kTargetFrequencyHz * time_s));

    AxisWord packet;
    packet.data = static_cast<std::uint16_t>(adc_sample);
    packet.keep = -1;
    packet.strb = -1;
    packet.user = 0;
    packet.id = 0;
    packet.dest = 0;
    packet.last = (sample_index == kInputPoints - 1) ? 1 : 0;
    input_stream.write(packet);
  }

  // 32 位 NCO 相位增量公式：f_nco/Fs*2^32。右移 6 位近似补偿 100 倍积分。
  const unsigned int nco_step = static_cast<unsigned int>(
      (kTargetFrequencyHz / kSampleRateHz) * 4294967296.0);
  constexpr unsigned int kGainShift = 6;
  constexpr unsigned int kBypass = 0;
  ddc_processor(input_stream, output_stream, nco_step, kDecimation,
                kGainShift, kBypass, kNfftLog2);

  std::ofstream output_file("ddc_iq_out.txt");
  int output_count = 0;
  bool last_is_correct = false;
  while (!output_stream.empty()) {
    const AxisWord packet = output_stream.read();
    const std::int16_t i_value =
        static_cast<std::int16_t>(packet.data.range(15, 0).to_uint());
    const std::int16_t q_value =
        static_cast<std::int16_t>(packet.data.range(31, 16).to_uint());
    output_file << i_value << ',' << q_value << '\n';
    ++output_count;
    last_is_correct = (output_count == kOutputPoints) && (packet.last == 1);
  }

  if (output_count != kOutputPoints || !last_is_correct) {
    std::cerr << "[FAILED] Output frame length or TLAST is incorrect.\n";
    return 1;
  }

  std::cout << "[PASSED] Generated " << output_count
            << " complex baseband samples.\n";
  return 0;
}
