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
# 功能：复现 3×3 复数定点 Cholesky 分解核的仿真、综合、协同仿真和 RTL 导出。
# 使用：在本脚本所在目录执行 `vitis_hls -f run_hls.tcl`。
# 路径约定：XF_PROJ_ROOT 指向仓库中的 solver 目录，并从相邻 utils 目录引入
#           AMD Vitis 公共矩阵工具头文件。

set CSIM 1         ;# 运行 C 级数值正确性验证
set CSYNTH 1       ;# 将 HLS C++ 综合为 RTL
set COSIM 1        ;# 运行 C/RTL 协同仿真
set VIVADO_SYN 1   ;# 导出 Verilog 并执行 Vivado 综合
set VIVADO_IMPL 0  ;# 默认不执行耗时较长的布局布线
set CUR_DIR [pwd]
set XF_PROJ_ROOT $CUR_DIR/../../../..
set XPART xc7z020-clg484-1

set PROJ "cholesky_test.prj"
set SOLN "sol1"

if {![info exists CLKP]} {
  set CLKP 7.70
}

open_project -reset $PROJ

add_files "${XF_PROJ_ROOT}/L1/tests/cholesky/kernel/kernel_cholesky_0.cpp" -cflags "-DMATRIX_DIM=3 -DMATRIX_LOWER_TRIANGULAR=0 -DSEL_ARCH=0 -D_DATA_PATH=${XF_PROJ_ROOT}/L1/tests/cholesky/datas/ -I./ -I${XF_PROJ_ROOT}/L1/tests/cholesky/host/ -I${XF_PROJ_ROOT}/L1/tests/cholesky/kernel/ -I${XF_PROJ_ROOT}/L1/tests/cholesky/ -I${XF_PROJ_ROOT}/L1/tests/ -I${XF_PROJ_ROOT}/L1/include/ -I${XF_PROJ_ROOT}/L1/include/hw -I${XF_PROJ_ROOT}/L2/include -I${XF_PROJ_ROOT}/../utils/L1/include/"
add_files -tb "${XF_PROJ_ROOT}/L1/tests/cholesky/host/test_cholesky.cpp" -cflags "-DMATRIX_DIM=3 -DMATRIX_LOWER_TRIANGULAR=0 -DSEL_ARCH=0 -D_DATA_PATH=${XF_PROJ_ROOT}/L1/tests/cholesky/datas/ -I./ -I${XF_PROJ_ROOT}/L1/tests/cholesky/host/ -I${XF_PROJ_ROOT}/L1/tests/cholesky/kernel/ -I${XF_PROJ_ROOT}/L1/tests/cholesky/ -I${XF_PROJ_ROOT}/L1/tests/ -I${XF_PROJ_ROOT}/L1/include/ -I${XF_PROJ_ROOT}/L1/include/hw -I ./host -I${XF_PROJ_ROOT}/../utils/L1/include/"
set_top kernel_cholesky_0

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
