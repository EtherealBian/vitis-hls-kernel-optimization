# 开源范围与许可边界

## 已公开

- 初赛三项实际优化源码、测试数据、Tcl 脚本和关键报告。
- 决赛 FFT/DDC HLS 源码、测试平台、NCO 查找表和组件配置。
- 决赛手写 ADC RTL、XDC、Vivado 工程/Block Design 和 PYNQ 软件。
- 与归档工程对应的 `FFT.bit`、`FFT.hwh`，用于板端快速复现。
- 中文架构、算法索引、构建部署和注释规范。

## 未收录

- `.Xil`、`.cache`、Vivado `.runs/.gen/.ip_user_files`、HLS 工作目录和日志备份。
- 自动生成的数百个 Verilog/VHDL、IP 驱动、DCP、XSA 和平台导出副本。
- 与代码复现无关的临时文件、IDE 状态和绝对路径配置。

上述文件可由已公开源码和 AMD 工具重新生成。排除生成物使仓库从原始约
655 MB/2561 个文件缩减为可审查、可维护的参赛源码归档。

## 第三方内容

初赛代码源自 AMD/Xilinx Vitis Libraries，继续遵守其 Apache License 2.0 和
原始版权声明。Vivado/Vitis 自带 IP、PYNQ 运行库及工具本身不因本仓库而重新
授权；使用者需自行满足相应软件许可。决赛部署文件只适用于对应器件和硬件连接。
