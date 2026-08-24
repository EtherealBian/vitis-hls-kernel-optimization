`timescale 1ns / 1ps

// ============================================================================
// 模块名称：adc_to_axis_v1_0
// 功能说明：将 10 位并行 ADC 采样转换为带帧边界的 32 位 AXI4-Stream。
// 作者：Ethereal
//
// 数据约定：
//   1. 外部 ADC 输出 0~1023 的偏移二进制，512 对应模拟零点。
//   2. 本模块翻转最高位，把偏移二进制转换为 -512~511 的补码，再符号扩展
//      到 32 位 m_axis_tdata；下游 DDC 仅使用低 16 位。
//   3. fft_pts 指定每帧采样数。最后一个成功握手的采样同时置高 TLAST。
//
// AXI4-Stream 规则：
//   当 TVALID=1 且 TREADY=0 时，TDATA/TKEEP/TLAST 必须保持稳定。本模块使用
//   单级弹性寄存器，只有当前寄存器为空或下游已接收当前字时才装载新采样。
//   外部 ADC 本身没有反压接口，因此下游长时间拉低 TREADY 会造成物理采样
//   丢失；系统通过 AXIS Data FIFO 吸收短时背压。
// ============================================================================

module adc_to_axis_v1_0 (
    input  wire        clk,            // 32 MHz 采样域时钟。
    input  wire        rst_n,          // 异步低有效复位。
    input  wire [15:0] fft_pts,        // PS 配置的输出帧长，0 按 1 点处理。

    input  wire [9:0]  ad_data,        // 外部 10 位 ADC 偏移二进制数据。

    // 将内部采样时钟经 ODDR 转发给外部 ADC。接口属性帮助 Vivado 在 Block
    // Design 中识别时钟端口及其频率，便于时钟传播和接口连接检查。
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 ad_clk CLK" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 32000000" *)
    output wire        ad_clk,

    output wire [31:0] m_axis_tdata,   // 符号扩展后的 ADC 补码采样。
    output wire [3:0]  m_axis_tkeep,   // 每一位对应一个有效字节，恒为 4'b1111。
    output wire        m_axis_tvalid,  // 本拍输出数据有效。
    output wire        m_axis_tlast,   // 当前拍为一帧最后一个采样。
    input  wire        m_axis_tready   // 下游能够接收当前拍。
);

  // 防止 fft_pts=0 时执行 frame_length-1 产生 16 位下溢。
  wire [15:0] frame_length = (fft_pts == 16'd0) ? 16'd1 : fft_pts;

  // 翻转偏移二进制最高位即可得到位宽不变的二进制补码：
  // 0 -> 10'b1000000000(-512)，512 -> 0，1023 -> 10'b0111111111(511)。
  wire [9:0] signed_sample = {~ad_data[9], ad_data[8:0]};

  reg [31:0] data_reg;
  reg        valid_reg;
  reg        last_reg;
  reg [15:0] sample_index_reg;

  // valid_reg=0 表示输出寄存器为空；m_axis_tready=1 表示当前字会在本拍被
  // 接收。两种情况下均可在时钟沿装载下一采样而不违反 AXI 稳定性要求。
  wire load_next_sample = (~valid_reg) | m_axis_tready;

  // ODDR 在上升沿输出 0、下降沿输出 1，形成占空比接近 50% 的转发时钟。
  // 使用专用 I/O 时钟资源可避免普通组合逻辑转发造成的毛刺和额外偏斜。
  ODDR #(
      .DDR_CLK_EDGE("OPPOSITE_EDGE"),
      .INIT(1'b0),
      .SRTYPE("SYNC")
  ) u_adc_clock_forwarder (
      .Q  (ad_clk),
      .C  (clk),
      .CE (1'b1),
      .D1 (1'b0),
      .D2 (1'b1),
      .R  (~rst_n),
      .S  (1'b0)
  );

  // 单级 AXI4-Stream 源寄存器。计数器只在装载新样本时推进，因此下游
  // 背压期间 TLAST 和帧内位置均保持不变。
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      data_reg         <= 32'd0;
      valid_reg        <= 1'b0;
      last_reg         <= 1'b0;
      sample_index_reg <= 16'd0;
    end else if (load_next_sample) begin
      data_reg  <= {{22{signed_sample[9]}}, signed_sample};
      valid_reg <= 1'b1;
      last_reg  <= (sample_index_reg == frame_length - 16'd1);

      if (sample_index_reg == frame_length - 16'd1)
        sample_index_reg <= 16'd0;
      else
        sample_index_reg <= sample_index_reg + 16'd1;
    end
  end

  assign m_axis_tdata  = data_reg;
  assign m_axis_tkeep  = 4'hF;
  assign m_axis_tvalid = valid_reg;
  assign m_axis_tlast  = last_reg;

endmodule
