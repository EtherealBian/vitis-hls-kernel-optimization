# =============================================================================
# 文件名称：QingLan.xdc
# 功能说明：10 位 ADC 数据接口和 32 MHz 采样时钟输出的引脚及时钟域约束。
# 作者：Ethereal
# 目标器件：xc7z020clg400-1
#
# 注意：以下管脚按照决赛硬件接线确定。更换开发板、扩展接口或 ADC 模块时，
# 必须先核对原理图、电压域和 Bank VCCO，不可直接沿用 PACKAGE_PIN。
# =============================================================================

# 外部 ADC 采样时钟输出。
set_property -dict {PACKAGE_PIN V7 IOSTANDARD LVCMOS33} [get_ports {ad_clk}]

# 10 位并行 ADC 数据总线，D9 为最高位、D0 为最低位。
set_property -dict {PACKAGE_PIN U8  IOSTANDARD LVCMOS33} [get_ports {ad_data[9]}]
set_property -dict {PACKAGE_PIN Y7  IOSTANDARD LVCMOS33} [get_ports {ad_data[8]}]
set_property -dict {PACKAGE_PIN C20 IOSTANDARD LVCMOS33} [get_ports {ad_data[7]}]
set_property -dict {PACKAGE_PIN W6  IOSTANDARD LVCMOS33} [get_ports {ad_data[6]}]
set_property -dict {PACKAGE_PIN Y6  IOSTANDARD LVCMOS33} [get_ports {ad_data[5]}]
set_property -dict {PACKAGE_PIN U7  IOSTANDARD LVCMOS33} [get_ports {ad_data[4]}]
set_property -dict {PACKAGE_PIN V6  IOSTANDARD LVCMOS33} [get_ports {ad_data[3]}]
set_property -dict {PACKAGE_PIN Y18 IOSTANDARD LVCMOS33} [get_ports {ad_data[2]}]
set_property -dict {PACKAGE_PIN W19 IOSTANDARD LVCMOS33} [get_ports {ad_data[1]}]
set_property -dict {PACKAGE_PIN W18 IOSTANDARD LVCMOS33} [get_ports {ad_data[0]}]

# PS FCLK0 的 100 MHz 控制/DMA 时钟域与 Clocking Wizard 产生的 32 MHz ADC
# 采样域之间通过设计中的异步 FIFO 传输。声明两个时钟组异步，避免工具对
# 不存在固定相位关系的跨域路径执行普通同步时序分析；该约束不能替代 CDC 电路。
set control_clock [get_clocks -of_objects \
    [get_pins design_1_i/processing_system7_0/FCLK_CLK0]]
set sample_clock [get_clocks -of_objects \
    [get_pins design_1_i/clk_wiz_0/clk_out1]]
set_clock_groups -asynchronous -group $control_clock -group $sample_clock
