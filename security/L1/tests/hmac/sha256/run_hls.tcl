# Copyright (C) 2019-2022, Xilinx, Inc.
# Copyright (C) 2022-2023, Advanced Micro Devices, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# vitis hls makefile-generator v2.0.0

# 项目作者：Ethereal
# 功能：复现 HMAC-SHA256 的 C 仿真、C 综合、C/RTL 协同仿真和 RTL 导出。
# 使用：在本脚本所在目录执行 `vitis_hls -f run_hls.tcl`。
# 路径约定：XF_PROJ_ROOT 指向仓库中的 security 目录，测试将自动使用优化后的
#           L1/include/xf_security/sha224_256.hpp。

set CSIM 1         ;# 运行 C 级功能仿真
set CSYNTH 1       ;# 将 HLS C++ 综合为 RTL
set COSIM 1        ;# 运行 C/RTL 协同仿真
set VIVADO_SYN 1   ;# 导出 Verilog 并执行 Vivado 综合
set VIVADO_IMPL 0  ;# 默认不执行耗时较长的布局布线
set CUR_DIR [pwd]
set XF_PROJ_ROOT $CUR_DIR/../../../..
set XPART xc7z020-clg484-1

set PROJ "hmac_sha256_test.prj"
set SOLN "solution1"

if {![info exists CLKP]} {
  set CLKP 14.0
}

open_project -reset $PROJ

add_files "test.cpp" -cflags "-I${XF_PROJ_ROOT}/L1/include"
add_files -tb "test.cpp gld.dat" -cflags "-I${XF_PROJ_ROOT}/L1/include"
set_top test_hmac_sha256

open_solution -reset $SOLN



set_part $XPART
create_clock -period $CLKP
set_clock_uncertainty 10%

if {$CSIM == 1} {
  csim_design
}

if {$CSYNTH == 1} {
  csynth_design
}

if {$COSIM == 1} {
  cosim_design
}

if {$VIVADO_SYN == 1} {
  export_design -flow syn -rtl verilog
}

if {$VIVADO_IMPL == 1} {
  export_design -flow impl -rtl verilog
}

exit
