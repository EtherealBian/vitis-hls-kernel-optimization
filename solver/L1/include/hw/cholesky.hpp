/*
 * Copyright 2021 Xilinx, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file cholesky.hpp
 * @brief This file contains cholesky functions
 *   - cholesky                 : Entry point function
 *   - choleskyTop             : Top level function that selects implementation architecture and internal types based
 * on a traits class.
 *   - choleskyBasic           : Basic implementation requiring lower resource
 *   - choleskyAlt             : Lower latency architecture requiring more resources
 *   - choleskyAlt2            : Further improved latency architecture requiring higher resource
 */

#ifndef _XF_SOLVER_CHOLESKY_HPP_
#define _XF_SOLVER_CHOLESKY_HPP_

/*
 * 项目：2025 年 FPGA 创新设计竞赛 AMD 赛道——Cholesky 分解优化
 * 项目作者：Ethereal
 * 优化与中文注释维护：Ethereal
 * 上游来源：AMD Vitis Solver Library
 *
 * 维护说明：本文件保留 AMD/Xilinx 的原始版权与 Apache-2.0 许可声明。
 * 参赛工作针对 3×3 复数定点矩阵，通过数组完全分割、循环展开、部分和并行归约
 * 和分段初值配合 Newton-Raphson 迭代降低延迟；正文中文注释同时标明数据依赖、
 * 精度与资源权衡、上下三角输出约定以及每条 HLS 指令的硬件含义。
 */

#include "ap_fixed.h"
#include "hls_x_complex.h"
#include <complex>
#include "utils/std_complex_utils.h"
#include "utils/x_matrix_utils.hpp"
#include "hls_stream.h"

/*
 * 中文说明：Cholesky 分解硬件实现（第三问优化版）
 *
 * 数学目标：对 Hermitian 正定矩阵 A 求下三角 L，使 A=L·L^H；当模板参数
 * LowerTriangularL=false 时输出上三角 U=L^H，使 A=U^H·U。
 *
 * 本题实际使用的 choleskyAlt 优化点：
 * 1. 内部结果由压缩一维数组改为完整二维数组，并在两个维度完全分割，使历史
 *    元素成为独立寄存器，满足同周期多点读取。
 * 2. 当前行的 j 循环和每个内积的 k 循环完全展开，把串行复数乘加变为并行硬件。
 * 3. 每个 j 使用独立的 square_partial，最后并行归约，消除多个 j 对同一
 *    square_sum 的循环携带依赖。
 * 4. 对角线保存倒数，非对角元素用乘法代替除法，缩短关键路径。
 * 5. 定点倒数平方根采用“分段常数初值 + Newton-Raphson 迭代”，替代通用
 *    sqrt 后再除法；关键乘法绑定到 DSP。
 * 6. 初始化和局部循环使用 PIPELINE/UNROLL，减少固定小矩阵的循环控制开销。
 *
 * 资源与精度权衡：完全分割、完全展开会显著增加 DSP/LUT/寄存器；近似倒数
 * 平方根会引入数值误差。因此该结构更适合本题 3×3 小矩阵，不宜不加评估地扩展
 * 到大矩阵。外层行循环存在真实数据依赖，必须保持顺序执行。
 */

namespace xf {
namespace solver {

// ===================================================================================================================
// 默认 traits：集中定义中间精度、架构编号、流水间隔和展开参数。
// 定点特化会扩展乘积/累加位宽，避免中间乘加过早截断或溢出。
template <bool LowerTriangularL, int RowsColsA, typename InputType, typename OutputType>
struct choleskyTraits {
    typedef InputType PROD_T;
    typedef InputType ACCUM_T;
    typedef InputType ADD_T;
    typedef InputType DIAG_T;
    typedef InputType RECIP_DIAG_T;
    typedef InputType OFF_DIAG_T;
    typedef OutputType L_OUTPUT_T;
    static const int ARCH =
        1; // 架构选择：0=低资源 Basic，1=低延迟 Alt，2=更高并行度 Alt2
    static const int INNER_II = 1; // 内层循环目标启动间隔
    static const int UNROLL_FACTOR =
        1; // Alt2 的内层展开因子；增大可提高并行度，也会增加计算和存储端口资源
    static const int UNROLL_DIM = (LowerTriangularL == true ? 1 : 2); // 根据输出三角方向选择分割维度
    static const int ARCH2_ZERO_LOOP =
        true; // Alt2 是否用独立循环清零无效三角区，以免拖慢主计算循环
};

// Specialization for complex
template <bool LowerTriangularL, int RowsColsA, typename InputBaseType, typename OutputBaseType>
struct choleskyTraits<LowerTriangularL, RowsColsA, hls::x_complex<InputBaseType>, hls::x_complex<OutputBaseType> > {
    typedef hls::x_complex<InputBaseType> PROD_T;
    typedef hls::x_complex<InputBaseType> ACCUM_T;
    typedef hls::x_complex<InputBaseType> ADD_T;
    typedef hls::x_complex<InputBaseType> DIAG_T;
    typedef InputBaseType RECIP_DIAG_T;
    typedef hls::x_complex<InputBaseType> OFF_DIAG_T;
    typedef hls::x_complex<OutputBaseType> L_OUTPUT_T;
    static const int ARCH = 1;
    static const int INNER_II = 1;
    static const int UNROLL_FACTOR = 1;
    static const int UNROLL_DIM = (LowerTriangularL == true ? 1 : 2);
    static const int ARCH2_ZERO_LOOP = true;
};

// Specialization for std complex
template <bool LowerTriangularL, int RowsColsA, typename InputBaseType, typename OutputBaseType>
struct choleskyTraits<LowerTriangularL, RowsColsA, std::complex<InputBaseType>, std::complex<OutputBaseType> > {
    typedef std::complex<InputBaseType> PROD_T;
    typedef std::complex<InputBaseType> ACCUM_T;
    typedef std::complex<InputBaseType> ADD_T;
    typedef std::complex<InputBaseType> DIAG_T;
    typedef InputBaseType RECIP_DIAG_T;
    typedef std::complex<InputBaseType> OFF_DIAG_T;
    typedef std::complex<OutputBaseType> L_OUTPUT_T;
    static const int ARCH = 1;
    static const int INNER_II = 1;
    static const int UNROLL_FACTOR = 1;
    static const int UNROLL_DIM = (LowerTriangularL == true ? 1 : 2);
    static const int ARCH2_ZERO_LOOP = true;
};

// Specialization for ap_fixed
template <bool LowerTriangularL,
          int RowsColsA,
          int W1,
          int I1,
          ap_q_mode Q1,
          ap_o_mode O1,
          int N1,
          int W2,
          int I2,
          ap_q_mode Q2,
          ap_o_mode O2,
          int N2>
struct choleskyTraits<LowerTriangularL, RowsColsA, ap_fixed<W1, I1, Q1, O1, N1>, ap_fixed<W2, I2, Q2, O2, N2> > {
    typedef ap_fixed<W1 + W1, I1 + I1, AP_RND_CONV, AP_SAT, 0> PROD_T;
    typedef ap_fixed<(W1 + W1) + BitWidth<RowsColsA>::Value,
                     (I1 + I1) + BitWidth<RowsColsA>::Value,
                     AP_RND_CONV,
                     AP_SAT,
                     0>
        ACCUM_T;
    typedef ap_fixed<W1 + 1, I1 + 1, AP_RND_CONV, AP_SAT, 0> ADD_T;
    typedef ap_fixed<(W1 + 1) * 2, I1 + 1, AP_RND_CONV, AP_SAT, 0> DIAG_T;     // Takes result of sqrt
    typedef ap_fixed<(W1 + 1) * 2, I1 + 1, AP_RND_CONV, AP_SAT, 0> OFF_DIAG_T; // Takes result of /
    typedef ap_fixed<2 + (W2 - I2) + W2, 2 + (W2 - I2), AP_RND_CONV, AP_SAT, 0> RECIP_DIAG_T;
    typedef ap_fixed<W2, I2, AP_RND_CONV, AP_SAT, 0>
        L_OUTPUT_T; // Takes new L value.  Same as L output but saturation set
    static const int ARCH = 1;
    static const int INNER_II = 1;
    static const int UNROLL_FACTOR = 1;
    static const int UNROLL_DIM = (LowerTriangularL == true ? 1 : 2);
    static const int ARCH2_ZERO_LOOP = true;
};

// Further specialization for hls::complex<ap_fixed>
template <bool LowerTriangularL,
          int RowsColsA,
          int W1,
          int I1,
          ap_q_mode Q1,
          ap_o_mode O1,
          int N1,
          int W2,
          int I2,
          ap_q_mode Q2,
          ap_o_mode O2,
          int N2>
struct choleskyTraits<LowerTriangularL,
                      RowsColsA,
                      hls::x_complex<ap_fixed<W1, I1, Q1, O1, N1> >,
                      hls::x_complex<ap_fixed<W2, I2, Q2, O2, N2> > > {
    typedef hls::x_complex<ap_fixed<W1 + W1, I1 + I1, AP_RND_CONV, AP_SAT, 0> > PROD_T;
    typedef hls::x_complex<ap_fixed<(W1 + W1) + BitWidth<RowsColsA>::Value,
                                    (I1 + I1) + BitWidth<RowsColsA>::Value,
                                    AP_RND_CONV,
                                    AP_SAT,
                                    0> >
        ACCUM_T;
    typedef hls::x_complex<ap_fixed<W1 + 1, I1 + 1, AP_RND_CONV, AP_SAT, 0> > ADD_T;
    typedef hls::x_complex<ap_fixed<(W1 + 1) * 2, I1 + 1, AP_RND_CONV, AP_SAT, 0> > DIAG_T;     // Takes result of sqrt
    typedef hls::x_complex<ap_fixed<(W1 + 1) * 2, I1 + 1, AP_RND_CONV, AP_SAT, 0> > OFF_DIAG_T; // Takes result of /
    typedef ap_fixed<2 + (W2 - I2) + W2, 2 + (W2 - I2), AP_RND_CONV, AP_SAT, 0> RECIP_DIAG_T;
    typedef hls::x_complex<ap_fixed<W2, I2, AP_RND_CONV, AP_SAT, 0> >
        L_OUTPUT_T; // Takes new L value.  Same as L output but saturation set
    static const int ARCH = 1;
    static const int INNER_II = 1;
    static const int UNROLL_FACTOR = 1;
    static const int UNROLL_DIM = (LowerTriangularL == true ? 1 : 2);
    static const int ARCH2_ZERO_LOOP = true;
};

// Further specialization for std::complex<ap_fixed>
template <bool LowerTriangularL,
          int RowsColsA,
          int W1,
          int I1,
          ap_q_mode Q1,
          ap_o_mode O1,
          int N1,
          int W2,
          int I2,
          ap_q_mode Q2,
          ap_o_mode O2,
          int N2>
struct choleskyTraits<LowerTriangularL,
                      RowsColsA,
                      std::complex<ap_fixed<W1, I1, Q1, O1, N1> >,
                      std::complex<ap_fixed<W2, I2, Q2, O2, N2> > > {
    typedef std::complex<ap_fixed<W1 + W1, I1 + I1, AP_RND_CONV, AP_SAT, 0> > PROD_T;
    typedef std::complex<ap_fixed<(W1 + W1) + BitWidth<RowsColsA>::Value,
                                  (I1 + I1) + BitWidth<RowsColsA>::Value,
                                  AP_RND_CONV,
                                  AP_SAT,
                                  0> >
        ACCUM_T;
    typedef std::complex<ap_fixed<W1 + 1, I1 + 1, AP_RND_CONV, AP_SAT, 0> > ADD_T;
    typedef std::complex<ap_fixed<(W1 + 1) * 2, I1 + 1, AP_RND_CONV, AP_SAT, 0> > DIAG_T;     // Takes result of sqrt
    typedef std::complex<ap_fixed<(W1 + 1) * 2, I1 + 1, AP_RND_CONV, AP_SAT, 0> > OFF_DIAG_T; // Takes result of /
    typedef ap_fixed<2 + (W2 - I2) + W2, 2 + (W2 - I2), AP_RND_CONV, AP_SAT, 0> RECIP_DIAG_T;
    typedef std::complex<ap_fixed<W2, I2, AP_RND_CONV, AP_SAT, 0> >
        L_OUTPUT_T; // Takes new L value.  Same as L output but saturation set
    static const int ARCH = 1;
    static const int INNER_II = 1;
    static const int UNROLL_FACTOR = 1;
    static const int UNROLL_DIM = (LowerTriangularL == true ? 1 : 2);
    static const int ARCH2_ZERO_LOOP = true;
};

// ===================================================================================================================
// Helper functions

// Square root
// o Overloaded versions of the sqrt function
// o The square root of a complex number is expensive.  However, the diagonal values of a Cholesky decomposition are
// always
//   real so we don't need a full complex square root.
template <typename T_IN, typename T_OUT>
int cholesky_sqrt_op(T_IN a, T_OUT& b) {
Function_cholesky_sqrt_op_real:;
    const T_IN ZERO = 0;
    if (a < ZERO) {
        b = ZERO;
        return (1);
    }
    b = x_sqrt(a);
    return (0);
}
template <typename T_IN, typename T_OUT>
int cholesky_sqrt_op(hls::x_complex<T_IN> din, hls::x_complex<T_OUT>& dout) {
Function_cholesky_sqrt_op_complex:;
    const T_IN ZERO = 0;
    T_IN a = din.real();
    dout.imag(ZERO);

    if (a < ZERO) {
        dout.real(ZERO);
        return (1);
    }

    dout.real(x_sqrt(a));
    return (0);
}
template <typename T_IN, typename T_OUT>
int cholesky_sqrt_op(std::complex<T_IN> din, std::complex<T_OUT>& dout) {
Function_cholesky_sqrt_op_complex:;
    const T_IN ZERO = 0;
    T_IN a = din.real();
    dout.imag(ZERO);

    if (a < ZERO) {
        dout.real(ZERO);
        return (1);
    }

    dout.real(x_sqrt(a));
    return (0);
}

// Reciprocal square root.
template <typename InputType, typename OutputType>
void cholesky_rsqrt(InputType x, OutputType& res) {
Function_cholesky_rsqrt_default:;
    res = x_rsqrt(x);
}

template <int W1, int I1, ap_q_mode Q1, ap_o_mode O1, int N1,
          int W2, int I2, ap_q_mode Q2, ap_o_mode O2, int N2>
/**
 * @brief 计算定点数倒数平方根 `1/sqrt(x)`。
 * @param x 正的对角剩余量。
 * @param res 倒数平方根近似值。
 * @details 先按输入范围选择分段常数初值，再执行
 * `y <- y*(1.5-0.5*x*y*y)`。该方法以若干 DSP 乘法替代通用平方根加除法，
 * 可降低延迟，但结果是近似值；较大输入通过增加迭代次数改善收敛精度。
 */
void cholesky_rsqrt(ap_fixed<W1, I1, Q1, O1, N1> x,
                    ap_fixed<W2, I2, Q2, O2, N2>& res) {
    // 控制函数是否内联；保留或消除模块边界会影响层次、共享机会和组合路径长度。
    #pragma HLS inline

    // 中间乘积使用加宽类型，减少定点乘法中途截断造成的误差。
    ap_fixed<W2, I2, Q2, O2, N2> initial_approx;        // 当前迭代值 y
    ap_fixed<W2+W2, I2+I2, Q2, O2, N2> approx_square;   // y*y
    ap_fixed<W2+W2, I2+I2, Q2, O2, N2> product_approx_square; // x*y*y

    // 一般输入迭代 3 次；大输入的初值较粗，增加到 6 次。
    int num_iterations = 3;

    #pragma HLS bind_op variable=approx_square op=mul impl=dsp          // y*y 使用 DSP
    #pragma HLS bind_op variable=product_approx_square op=mul impl=dsp  // x*y*y 使用 DSP
    #pragma HLS bind_op variable=initial_approx op=mul impl=dsp         // Newton 更新乘法使用 DSP

    // 分段常数初值并非 ROM 查找表；区间越贴近真实函数，Newton 收敛越快。
    if (x < 0.0625) {
        initial_approx = 5.6569;
    } else if (x < 0.25) {
        initial_approx = 2.6667;
    } else if (x < 1) {
        initial_approx = 1.3333;
    } else if (x < 4) {
        initial_approx = 0.66667;
    } else if (x < 9) {
        initial_approx = 0.33333;
    } else if (x < 16) {
        initial_approx = 0.16667;
    } else {
        initial_approx = 0.125;
        num_iterations = 6;  // 大输入增加迭代次数
    }

    // Newton-Raphson 迭代。相邻迭代有真实数据依赖，PIPELINE 只能优化迭代内部数据通路。
    for (int i = 0; i < num_iterations; i++) {
        #pragma HLS PIPELINE II=1

        // Compute square of current approximation
        approx_square = initial_approx * initial_approx;

        // Compute product of input and squared approximation
        product_approx_square = x * approx_square;

        // Newton-Raphson update: y = y * (1.5 - 0.5 * x * y^2)
        initial_approx = initial_approx *
                        (ap_fixed<W2+W2, I2+I2, Q2, O2, N2>(1.5) -
                         ap_fixed<W2+W2, I2+I2, Q2, O2, N2>(0.5) * product_approx_square);
    }

    // Output the final computed result
    res = initial_approx;
}

// Local multiplier to handle a complex case currently not supported by the hls::x_complex class
// - Complex multiplied by a real of a different type
// - Required for complex fixed point implementations
template <typename AType, typename BType, typename CType>
void cholesky_prod_sum_mult(AType A, BType B, CType& C) {
Function_cholesky_prod_sum_mult_real:;
    C = A * B;
}
template <typename AType, typename BType, typename CType>
void cholesky_prod_sum_mult(hls::x_complex<AType> A, BType B, hls::x_complex<CType>& C) {
Function_cholesky_prod_sum_mult_complex:;
    C.real(A.real() * B);
    C.imag(A.imag() * B);
}
template <typename AType, typename BType, typename CType>
void cholesky_prod_sum_mult(std::complex<AType> A, BType B, std::complex<CType>& C) {
Function_cholesky_prod_sum_mult_complex:;
    C.real(A.real() * B);
    C.imag(A.imag() * B);
}

// ===================================================================================================================
// choleskyBasic
template <bool LowerTriangularL, int RowsColsA, typename CholeskyTraits, class InputType, class OutputType>
int choleskyBasic(const InputType A[RowsColsA][RowsColsA], OutputType L[RowsColsA][RowsColsA]) {
    int return_code = 0;

    // Use the traits struct to specify the correct type for the intermediate variables. This is really only needed for
    // fixed point.
    typename CholeskyTraits::PROD_T prod;
    typename CholeskyTraits::ACCUM_T sum[RowsColsA];
    typename CholeskyTraits::ACCUM_T A_cast_to_sum;    // A with the same dimensions as sum.
    typename CholeskyTraits::ACCUM_T prod_cast_to_sum; // prod with the same dimensions as sum.

    typename CholeskyTraits::ADD_T A_minus_sum;
    typename CholeskyTraits::DIAG_T new_L_diag;         // sqrt(A_minus_sum)
    typename CholeskyTraits::OFF_DIAG_T new_L_off_diag; // sum/L
    typename CholeskyTraits::OFF_DIAG_T L_cast_to_new_L_off_diag;

    typename CholeskyTraits::L_OUTPUT_T new_L;
    OutputType retrieved_L;
    // Internal memory used to aviod read access from function output argument L.
    // NOTE: The internal matrix only needs to be triangular but optimization using a 1-D array it will require addition
    // logic to generate the indexes. Refer to the choleskyAlt function.
    OutputType L_internal[RowsColsA][RowsColsA];

col_loop:
    // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
    for (int j = 0; j < RowsColsA; j++) {
        sum[j] = 0;

    // Calculate the diagonal value for this column
    diag_loop:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (int k = 0; k < RowsColsA; k++) {
            if (k <= (j - 1)) {
                if (LowerTriangularL == true) {
                    retrieved_L = L_internal[j][k];
                } else {
                    retrieved_L = L_internal[k][j];
                }
                sum[j] = hls::x_conj(retrieved_L) * retrieved_L;
            }
        }
        A_cast_to_sum = A[j][j];

        A_minus_sum = A_cast_to_sum - sum[j];

        if (cholesky_sqrt_op(A_minus_sum, new_L_diag)) {
#ifndef __SYNTHESIS__
            printf("ERROR: Trying to find the square root of a negative number\n");
#endif
            return_code = 1;
        }

        // Round to target format using method specifed by traits defined types.
        new_L = new_L_diag;

        if (LowerTriangularL == true) {
            L_internal[j][j] = new_L;
            L[j][j] = new_L;
        } else {
            L_internal[j][j] = hls::x_conj(new_L);
            L[j][j] = hls::x_conj(new_L);
        }

    // Calculate the off diagonal values for this column
    off_diag_loop:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (int i = 0; i < RowsColsA; i++) {
            if (i > j) {
                if (LowerTriangularL == true) {
                    sum[j] = A[i][j];
                } else {
                    sum[j] = hls::x_conj(A[j][i]);
                }

            sum_loop:
                // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
                for (int k = 0; k < RowsColsA; k++) {
#pragma HLS PIPELINE II = CholeskyTraits::INNER_II
                    if (k <= (j - 1)) {
                        if (LowerTriangularL == true) {
                            prod = -L_internal[i][k] * hls::x_conj(L_internal[j][k]);
                        } else {
                            prod = -hls::x_conj(L_internal[k][i]) * (L_internal[k][j]);
                        }

                        prod_cast_to_sum = prod;
                        sum[j] += prod_cast_to_sum;
                    }
                }

                new_L_off_diag = sum[j];

                L_cast_to_new_L_off_diag = L_internal[j][j];

                // Diagonal is always real, avoid complex division
                new_L_off_diag = new_L_off_diag / hls::x_real(L_cast_to_new_L_off_diag);

                // Round to target format using method specifed by traits defined types.
                new_L = new_L_off_diag;

                if (LowerTriangularL == true) {
                    L[i][j] = new_L;
                    L_internal[i][j] = new_L;
                } else {
                    L[j][i] = hls::x_conj(new_L);
                    L_internal[j][i] = hls::x_conj(new_L);
                }
            } else if (i < j) {
                if (LowerTriangularL == true) {
                    L[i][j] = 0;
                } else {
                    L[j][i] = 0;
                }
            }
        }
    }
    return (return_code);
}

// ===================================================================================================================
/**
 * @brief 面向固定小矩阵的低延迟 Cholesky 分解核心。
 * @tparam LowerTriangularL true 输出下三角 L，false 输出上三角 L^H。
 * @tparam RowsColsA 方阵维度。
 * @tparam CholeskyTraits 中间位宽、舍入方式和架构参数。
 * @param A Hermitian 正定输入矩阵。
 * @param L 输出的下三角或上三角分解因子。
 * @return 0 表示完成；1 表示对角剩余量为负，输入不满足正常分解条件。
 * @details 第 i 行先并行计算所有 j<i 的非对角元素：
 * `L(i,j)=(A(i,j)-sum_{k<j}L(i,k)conj(L(j,k)))/L(j,j)`；随后计算
 * `L(i,i)=sqrt(A(i,i)-sum_{j<i}|L(i,j)|^2)`。行内完全并行，行间保持顺序。
 */
template <bool LowerTriangularL, int RowsColsA, typename CholeskyTraits, class InputType, class OutputType>
int choleskyAlt(const InputType A[RowsColsA][RowsColsA], OutputType L[RowsColsA][RowsColsA]) {
    #pragma HLS PIPELINE II = 1 // 尝试流水化函数内可重叠操作，不会消除行间真实依赖
    #pragma HLS INLINE off      // 保持独立层次，防止顶层展开导致面积失控
    int return_code = 0;        // 0：成功；1：出现负数开方

    // 完整二维工作矩阵避免压缩下三角的地址计算；对角数组保存 1/L(j,j)。
    OutputType L_internal[RowsColsA][RowsColsA];
    typename CholeskyTraits::RECIP_DIAG_T diag_internal[RowsColsA];

    // 两维完全分割后，各元素独立存放，为 j/k 循环完全展开提供足够读端口。
    #pragma HLS ARRAY_PARTITION variable = L_internal complete dim = 1
    #pragma HLS ARRAY_PARTITION variable = L_internal complete dim = 2
    #pragma HLS ARRAY_PARTITION variable = diag_internal complete

    // Local variables using CholeskyTraits defined types for precision control
    typename CholeskyTraits::ACCUM_T A_cast_to_sum;        // Input matrix element cast to accumulation type
    typename CholeskyTraits::DIAG_T new_L_diag;           // New diagonal element
    typename CholeskyTraits::RECIP_DIAG_T new_L_diag_recip; // New diagonal element reciprocal
    typename CholeskyTraits::PROD_T prod;                 // Product intermediate result
    typename CholeskyTraits::ACCUM_T prod_cast_to_sum;    // Product cast to accumulation type
    typename CholeskyTraits::ACCUM_T product_sum;         // Product accumulation sum
    typename CholeskyTraits::OFF_DIAG_T prod_cast_to_off_diag; // Product cast to off-diagonal type
    typename CholeskyTraits::OFF_DIAG_T new_L_off_diag;   // New off-diagonal element
    typename CholeskyTraits::L_OUTPUT_T new_L;            // New L matrix element

    // 显式清零完整矩阵，保证未使用三角区和尚未计算的历史位置具有确定值。
    for (int r = 0; r < RowsColsA; ++r) {
        #pragma HLS PIPELINE II = 1
        for (int c = 0; c < RowsColsA; ++c) {
            // 展开循环迭代以形成并行硬件；展开因子越大，吞吐越高但资源与布线压力也越大。
            #pragma HLS UNROLL
            L_internal[r][c] = 0;
            L[r][c] = 0;
        }
        diag_internal[r] = 0;
    }

// 外层按行执行：第 i 行依赖 0..i-1 行，属于算法真实依赖，不能完全展开。
row_loop:
    for (int i = 0; i < RowsColsA; ++i) {
        // 当前行所有 j<i 的元素可并行计算；每个元素内部的 k<j 内积也完全展开。

        // square_sum = sum_{j=0..i-1} conj(new_L_{i,j}) * new_L_{i,j}
        typename CholeskyTraits::ACCUM_T square_partial[RowsColsA]; // 每个 j 独占一个模平方部分和
        #pragma HLS ARRAY_PARTITION variable = square_partial complete

        // Initialize partial accumulators to zero
        for (int jj = 0; jj < RowsColsA; ++jj) {
            // 展开循环迭代以形成并行硬件；展开因子越大，吞吐越高但资源与布线压力也越大。
            #pragma HLS UNROLL
            square_partial[jj] = 0;
        }

        // Compute new_L(i,j) elements for each j in parallel
    col_loop_unrolled:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (int j = 0; j < RowsColsA; ++j) {
            #pragma HLS UNROLL
            if (j < i) {
                // Initialize product_sum with A[i][j] or conj(A[j][i])
                if (LowerTriangularL == true) {
                    product_sum = A[i][j];  // Lower triangular: directly take A[i][j]
                } else {
                    product_sum = hls::x_conj(A[j][i]);  // Upper triangular: take conjugate of A[j][i]
                }

                // sum_loop: product_sum += - sum_k L_internal[i][k] * conj(L_internal[j][k]) (k from 0 to j-1)
            sum_loop_unrolled:
                // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
                for (int k = 0; k < RowsColsA; ++k) {
                    #pragma HLS UNROLL
                    if (k < j) {
                        // Directly read computed L_internal elements (row progression ensures data readiness)
                        prod = -L_internal[i][k] * hls::x_conj(L_internal[j][k]);
                        prod_cast_to_sum = prod;
                        product_sum += prod_cast_to_sum;
                    }
                }

                // Convert product_sum to off-diagonal type and multiply by diag_internal[j] (stored reciprocal)
                prod_cast_to_off_diag = product_sum;
                typename CholeskyTraits::RECIP_DIAG_T L_diag_recip = diag_internal[j];
                cholesky_prod_sum_mult(prod_cast_to_off_diag, L_diag_recip, new_L_off_diag);

                // Truncate/format to target L type
                new_L = new_L_off_diag;

                // Store in L_internal and write back to output matrix L (lower or upper triangular format)
                L_internal[i][j] = new_L;
                if (LowerTriangularL == true) {
                    L[i][j] = new_L;    // Lower triangular: direct storage
                    L[j][i] = 0;        // Upper triangular part set to zero
                } else {
                    L[j][i] = hls::x_conj(new_L);  // Upper triangular: store conjugate
                    L[i][j] = 0;        // Lower triangular part set to zero
                }

                // Accumulate square contribution to square_partial[j] (parallel accumulation)
                square_partial[j] = square_partial[j] + hls::x_conj(new_L) * new_L;
            } // End j < i condition
        }     // End j loop

        // 完全展开归约各分支的 |L(i,j)|^2，避免所有 j 串行写同一累加器。
        typename CholeskyTraits::ACCUM_T square_sum = {};
        for (int s = 0; s < RowsColsA; ++s) {
            #pragma HLS UNROLL
            square_sum += square_partial[s];
        }

        // Diagonal element (i,i) computation
        A_cast_to_sum = A[i][i];
        typename CholeskyTraits::ADD_T A_minus_sum = A_cast_to_sum - square_sum;

        // Compute square root, check for positive definiteness
        if (cholesky_sqrt_op(A_minus_sum, new_L_diag)) {
            #ifndef __SYNTHESIS__
            printf("Error: Attempted square root of negative number\n");
            #endif
            return_code = 1;  // Set error return code
        }
        new_L = new_L_diag;

        // 保存 1/L(i,i)，后续非对角计算用乘法替代除法。
        typename CholeskyTraits::DIAG_T A_minus_sum_cast_diag = A_minus_sum;
        cholesky_rsqrt(hls::x_real(A_minus_sum_cast_diag), new_L_diag_recip);
        diag_internal[i] = new_L_diag_recip;

        // Store diagonal element to output L matrix
        if (LowerTriangularL == true) {
            L[i][i] = new_L;  // Lower triangular: direct storage
        } else {
            L[i][i] = hls::x_conj(new_L);  // Upper triangular: store conjugate
        }
    } // End main row loop

    return return_code;
}
// ===================================================================================================================
// choleskyAlt2: Further improved latency architecture requiring higher resource
template <bool LowerTriangularL, int RowsColsA, typename CholeskyTraits, class InputType, class OutputType>
int choleskyAlt2(const InputType A[RowsColsA][RowsColsA], OutputType L[RowsColsA][RowsColsA]) {
    int return_code = 0;

    // To avoid array index calculations every iteration this architecture uses a simple 2D array rather than a
    // optimized/packed triangular matrix.
    OutputType L_internal[RowsColsA][RowsColsA];
    OutputType prod_column_top;
    typename CholeskyTraits::ACCUM_T square_sum_array[RowsColsA];
    typename CholeskyTraits::ACCUM_T A_cast_to_sum;
    typename CholeskyTraits::ADD_T A_minus_sum;
    typename CholeskyTraits::DIAG_T A_minus_sum_cast_diag;
    typename CholeskyTraits::DIAG_T new_L_diag;
    typename CholeskyTraits::RECIP_DIAG_T new_L_diag_recip;
    typename CholeskyTraits::PROD_T prod;
    typename CholeskyTraits::ACCUM_T prod_cast_to_sum;
    typename CholeskyTraits::ACCUM_T product_sum;
    typename CholeskyTraits::ACCUM_T product_sum_array[RowsColsA];
    typename CholeskyTraits::OFF_DIAG_T prod_cast_to_off_diag;
    typename CholeskyTraits::OFF_DIAG_T new_L_off_diag;
    typename CholeskyTraits::L_OUTPUT_T new_L;

// 分割数组以增加并行读写端口，避免单个存储体端口数限制计算吞吐。
#pragma HLS ARRAY_PARTITION variable = A cyclic dim = CholeskyTraits::UNROLL_DIM factor = CholeskyTraits::UNROLL_FACTOR
#pragma HLS ARRAY_PARTITION variable = L cyclic dim = CholeskyTraits::UNROLL_DIM factor = CholeskyTraits::UNROLL_FACTOR
#pragma HLS ARRAY_PARTITION variable = L_internal cyclic dim = CholeskyTraits::UNROLL_DIM factor = \
    CholeskyTraits::UNROLL_FACTOR
// 分割数组以增加并行读写端口，避免单个存储体端口数限制计算吞吐。
#pragma HLS ARRAY_PARTITION variable = square_sum_array cyclic dim = 1 factor = CholeskyTraits::UNROLL_FACTOR
#pragma HLS ARRAY_PARTITION variable = product_sum_array cyclic dim = 1 factor = CholeskyTraits::UNROLL_FACTOR

col_loop:
    // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
    for (int j = 0; j < RowsColsA; j++) {
        // Diagonal calculation
        A_cast_to_sum = A[j][j];
        if (j == 0) {
            A_minus_sum = A_cast_to_sum;
        } else {
            A_minus_sum = A_cast_to_sum - square_sum_array[j];
        }
        if (cholesky_sqrt_op(A_minus_sum, new_L_diag)) {
#ifndef __SYNTHESIS__
            printf("ERROR: Trying to find the square root of a negative number\n");
#endif
            return_code = 1;
        }
        // Round to target format using method specifed by traits defined types.
        new_L = new_L_diag;
        // Generate the reciprocal of the diagonal for internal use to aviod the latency of a divide in every
        // off-diagonal calculation
        A_minus_sum_cast_diag = A_minus_sum;
        cholesky_rsqrt(hls::x_real(A_minus_sum_cast_diag), new_L_diag_recip);
        // Store diagonal value
        if (LowerTriangularL == true) {
            L[j][j] = new_L;
        } else {
            L[j][j] = hls::x_conj(new_L);
        }

    sum_loop:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (int k = 0; k <= j; k++) {
// Define average trip count for reporting, loop reduces in length for every iteration of col_loop
#pragma HLS loop_tripcount max = 1 + RowsColsA / 2
            // Same value used in all calcs
            // o Implement -1* here
            prod_column_top = -hls::x_conj(L_internal[j][k]);

        // NOTE: Using a fixed loop length combined with a "if" to implement reducing loop length
        // o Ensures the inner loop can achieve the maximum II (1)
        // o May introduce a small overhead resolving the "if" statement but HLS struggled to schedule when the variable
        //   loop bound expression was used.
        // o Will report inaccurate trip count as it will reduce by one with the col_loop
        // o Variable loop bound code: row_loop: for(int i = j+1; i < RowsColsA; i++) {
        row_loop:
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int i = 0; i < RowsColsA; i++) {
// IMPORTANT: row_loop must not merge with sum_loop as the merged loop becomes variable length and HLS will struggle
// with scheduling
// 该 HLS 指令约束综合调度或硬件映射；修改时必须重新检查时序、吞吐和资源报告。
#pragma HLS LOOP_FLATTEN off
#pragma HLS PIPELINE II = CholeskyTraits::INNER_II
#pragma HLS UNROLL FACTOR = CholeskyTraits::UNROLL_FACTOR

                if (i > j) {
                    prod = L_internal[i][k] * prod_column_top;
                    prod_cast_to_sum = prod;

                    if (k == 0) {
                        // Prime first sum
                        if (LowerTriangularL == true) {
                            A_cast_to_sum = A[i][j];
                        } else {
                            A_cast_to_sum = hls::x_conj(A[j][i]);
                        }
                        product_sum = A_cast_to_sum;
                    } else {
                        product_sum = product_sum_array[i];
                    }

                    if (k < j) {
                        // Accumulate row sum of columns
                        product_sum_array[i] = product_sum + prod_cast_to_sum;
                    } else {
                        // Final calculation for off diagonal value
                        prod_cast_to_off_diag = product_sum;
                        // Diagonal is stored in its reciprocal form so only need to multiply the product sum
                        cholesky_prod_sum_mult(prod_cast_to_off_diag, new_L_diag_recip, new_L_off_diag);
                        // Round to target format using method specifed by traits defined types.
                        new_L = new_L_off_diag;
                        // Build sum for use in diagonal calculation for this row.
                        if (k == 0) {
                            square_sum_array[j] = hls::x_conj(new_L) * new_L;
                        } else {
                            square_sum_array[j] = hls::x_conj(new_L) * new_L;
                        }
                        // Store result
                        L_internal[i][j] = new_L;
                        // NOTE: Use the upper/lower triangle zeroing in the subsequent loop so the double memory access
                        // does not
                        // become a bottleneck
                        // o Results in a further increase of DSP resources due to the higher II of this loop.
                        // o Retaining the zeroing operation here can give this a loop a max II of 2 and HLS will
                        // resource share.
                        if (LowerTriangularL == true) {
                            L[i][j] = new_L;                                   // Store in lower triangle
                            if (!CholeskyTraits::ARCH2_ZERO_LOOP) L[j][i] = 0; // Zero upper
                        } else {
                            L[j][i] = hls::x_conj(new_L);                      // Store in upper triangle
                            if (!CholeskyTraits::ARCH2_ZERO_LOOP) L[i][j] = 0; // Zero lower
                        }
                    }
                }
            }
        }
    }
    // Zero upper/lower triangle
    // o Use separate loop to ensure main calcuation can achieve an II of 1
    // o As noted above this may increase the DSP resources.
    // o Required when unrolling the inner loop due to array dimension access
    if (CholeskyTraits::ARCH2_ZERO_LOOP) {
    zero_rows_loop:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (int i = 0; i < RowsColsA - 1; i++) {
        zero_cols_loop:
            for (int j = i + 1; j < RowsColsA; j++) {
// Define average trip count for reporting, loop reduces in length for every iteration of zero_rows_loop
// 提供循环迭代次数估计，仅用于报告分析，不改变循环的实际功能边界。
#pragma HLS loop_tripcount max = 1 + RowsColsA / 2
#pragma HLS PIPELINE
                if (LowerTriangularL == true) {
                    L[i][j] = 0; // Zero upper
                } else {
                    L[j][i] = 0; // Zero lower
                }
            }
        }
    }
    return (return_code);
}

// ===================================================================================================================
// choleskyTop: Top level function that selects implementation architecture and internal types based on the
// traits class provided via the CholeskyTraits template parameter.
// o Call this function directly if you wish to override the default architecture choice or internal types
template <bool LowerTriangularL, int RowsColsA, typename CholeskyTraits, class InputType, class OutputType>
int choleskyTop(const InputType A[RowsColsA][RowsColsA], OutputType L[RowsColsA][RowsColsA]) {
    switch (CholeskyTraits::ARCH) {
        case 0:
            return choleskyBasic<LowerTriangularL, RowsColsA, CholeskyTraits, InputType, OutputType>(A, L);
        case 1:
            return choleskyAlt<LowerTriangularL, RowsColsA, CholeskyTraits, InputType, OutputType>(A, L);
        case 2:
            return choleskyAlt2<LowerTriangularL, RowsColsA, CholeskyTraits, InputType, OutputType>(A, L);
        default:
            return choleskyBasic<LowerTriangularL, RowsColsA, CholeskyTraits, InputType, OutputType>(A, L);
    }
}

/**
* @brief cholesky
*
* @tparam LowerTriangularL   When false generates the result in the upper triangle
* @tparam RowsColsA          Defines the matrix dimensions
* @tparam InputType          Input data type
* @tparam OutputType         Output data type
* @tparam TRAITS             choleskyTraits class
*
* @param matrixAStrm         Stream of Hermitian/symmetric positive definite input matrix
* @param matrixLStrm         Stream of Lower or upper triangular output matrix
*
* @return                    An integer type. 0=Success. 1=Failure. The function attempted to find the square root of
* a negative number i.e. the input matrix A was not Hermitian/symmetric positive definite.
*/
template <bool LowerTriangularL,
          int RowsColsA,
          class InputType,
          class OutputType,
          typename TRAITS = choleskyTraits<LowerTriangularL, RowsColsA, InputType, OutputType> >
int cholesky(hls::stream<InputType>& matrixAStrm, hls::stream<OutputType>& matrixLStrm) {
    InputType A[RowsColsA][RowsColsA];
    OutputType L[RowsColsA][RowsColsA];

    // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
    for (int r = 0; r < RowsColsA; r++) {
#pragma HLS PIPELINE
        for (int c = 0; c < RowsColsA; c++) {
            matrixAStrm.read(A[r][c]);
        }
    }

    int ret = 0;
    ret = choleskyTop<LowerTriangularL, RowsColsA, TRAITS, InputType, OutputType>(A, L);

    // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
    for (int r = 0; r < RowsColsA; r++) {
#pragma HLS PIPELINE
        for (int c = 0; c < RowsColsA; c++) {
            matrixLStrm.write(L[r][c]);
        }
    }
    return ret;
}

} // end namespace solver
} // end namespace xf
#endif
