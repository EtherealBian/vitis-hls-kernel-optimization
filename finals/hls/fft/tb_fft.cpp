/**
 * @file tb_fft.cpp
 * @brief FFT 频谱分析单元的 C 仿真测试平台。
 * @author Ethereal
 */

#include "ap_axi_sdata.h"
#include "hls_stream.h"

#include <cmath>
#include <cstdint>
#include <iostream>

using AxisWord = ap_axiu<32, 1, 1, 1>;

void fft_spectrum_analyzer(hls::stream<AxisWord>& data_in,
                           hls::stream<AxisWord>& data_out, int nfft_log2,
                           int window_type, int ddc_en);

namespace {

constexpr float kPi = 3.14159265358979323846F;

float BitsToFloat(std::uint32_t bits) {
  union FloatBits {
    float value;
    std::uint32_t bits;
  } converter;
  converter.bits = bits;
  return converter.value;
}

}  // namespace

int main() {
  constexpr int kNfftLog2 = 10;
  constexpr int kPoints = 1 << kNfftLog2;
  constexpr float kSampleRateHz = 1024.0F;

  hls::stream<AxisWord> input_stream("input_stream");
  hls::stream<AxisWord> output_stream("output_stream");

  // 生成两个非整周期正弦分量，用于检查 Hann 窗对频谱泄漏的抑制效果。
  for (int sample_index = 0; sample_index < kPoints; ++sample_index) {
    const float time_s = static_cast<float>(sample_index) / kSampleRateHz;
    const float sample =
        1000.0F * std::sin(2.0F * kPi * 50.4F * time_s) +
        500.0F * std::sin(2.0F * kPi * 120.4F * time_s);

    AxisWord packet;
    packet.data = static_cast<std::uint16_t>(static_cast<std::int16_t>(sample));
    packet.keep = -1;
    packet.strb = -1;
    packet.user = 0;
    packet.id = 0;
    packet.dest = 0;
    packet.last = (sample_index == kPoints - 1) ? 1 : 0;
    input_stream.write(packet);
  }

  // window_type=1：Hann 窗；ddc_en=0：输入仅使用低 16 位实数通道。
  fft_spectrum_analyzer(input_stream, output_stream, kNfftLog2, 1, 0);

  int detected_peak_bins = 0;
  for (int bin_index = 0; bin_index < kPoints; ++bin_index) {
    const AxisWord packet = output_stream.read();
    const float power = BitsToFloat(packet.data.to_uint());
    const float normalized_magnitude =
        std::sqrt(power) / (static_cast<float>(kPoints) / 2.0F);

    if (bin_index < kPoints / 2 && normalized_magnitude > 50.0F) {
      ++detected_peak_bins;
      std::cout << "Peak bin: " << bin_index
                << ", normalized magnitude: " << normalized_magnitude << '\n';
    }
  }

  if (detected_peak_bins == 0) {
    std::cerr << "[FAILED] No spectral peak above threshold.\n";
    return 1;
  }

  std::cout << "[PASSED] Detected " << detected_peak_bins
            << " bins above threshold.\n";
  return 0;
}
