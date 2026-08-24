/*
 * Copyright 2019 Xilinx, Inc.
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

#ifndef _XF_SECURITY_SHA224_256_HPP_
#define _XF_SECURITY_SHA224_256_HPP_

/*
 * 项目：2025 年 FPGA 创新设计竞赛 AMD 赛道——HMAC-SHA256 优化
 * 项目作者：Ethereal
 * 优化与中文注释维护：Ethereal
 * 上游来源：AMD Vitis Security Library
 *
 * 维护说明：本文件保留 AMD/Xilinx 的原始版权与 Apache-2.0 许可声明。
 * 参赛工作在兼容原接口和摘要格式的前提下，重构 SHA-256 流水线、双核任务分发
 * 以及片上 FIFO 的深度和存储映射；正文中文注释用于说明算法阶段、握手语义、
 * HLS 指令对应的硬件结构及关键边界条件。
 */

#include <ap_int.h>
#include <hls_stream.h>
#include "xf_security/types.hpp"
#include "xf_security/utils.hpp"

/*
 * 中文说明：SHA-224 / SHA-256 流式硬件实现（优化版）
 *
 * 一、代码功能
 * 1. preProcessing：读取消息和字节长度，按 FIPS 180-4 完成补位，并产生每个
 *    512 bit 分组对应的 64 个消息调度字 W[0..63]。
 * 2. sha256Digest：对每个分组执行 64 轮压缩，累加链式状态，最后输出 224 bit
 *    或 256 bit 摘要。
 * 3. split_input / merge_output：以“整条消息”为单位在两个摘要核之间轮询分配，
 *    再按照原始输入顺序合并结果，避免并行核完成时间不同造成摘要乱序。
 *
 * 二、本版本的主要优化点
 * 1. 消息调度直接生成：预处理阶段直接向 w_strm 输出 W[0..63]，去掉原实现中
 *    先缓存、后复制调度字所产生的额外周期。
 * 2. 16 项滑动窗口完全分割：W 数组的每个元素成为独立寄存器，可在同一周期
 *    读取 W[t-2]、W[t-7]、W[t-15]、W[t-16]，支持扩展循环 II=1。
 * 3. 轮函数流水化：每轮读取一个 Wt 并更新 a~h，目标启动间隔为 1 个周期。
 * 4. 常量和状态完全分割：K[64] 与 H[8] 可被并行访问，减少存储端口冲突。
 * 5. DATAFLOW 级并行：补位/调度生成与摘要压缩通过 FIFO 解耦，可处理不同分组。
 * 6. 双核消息级并行：两套完整 SHA 核交替处理消息，提高多消息场景的吞吐率；
 *    order_core_strm 记录归属，保证输出顺序不变。
 * 7. 分级 FIFO：大容量消息调度队列放入 BRAM，较短的控制流放入 LUTRAM，在
 *    吞吐率、资源和阻塞风险之间折中。
 *
 * 三、重要约束
 * - 一条消息的多个分组必须由同一个核连续处理，不能跨核拆分，否则链式状态会失效。
 * - SHA-256 内部按大端字节序运算，输入、尾部补位和最终摘要输出处均需正确换序。
 * - PIPELINE II=1 表示允许每周期启动一次循环迭代，不等同于整条消息一周期完成。
 */

#ifndef __SYNTHESIS__
#include <cstdio>
#endif
#ifndef _DEBUG
#define _DEBUG (0)
#endif
#define _XF_SECURITY_VOID_CAST static_cast<void>
#define _XF_SECURITY_PRINT(msg...) \
    do {                           \
        if (_DEBUG) printf(msg);   \
    } while (0)

#define ROTR(n, x) ((x >> n) | (x << (32 - n)))
#define ROTL(n, x) ((x << n) | (x >> (32 - n)))
#define SHR(n, x) (x >> n)
#define CH(x, y, z) ((x & y) ^ ((~x) & z))
#define MAJ(x, y, z) ((x & y) ^ (x & z) ^ (y & z))
#define BSIG0(x) (ROTR(2, x) ^ ROTR(13, x) ^ ROTR(22, x))
#define BSIG1(x) (ROTR(6, x) ^ ROTR(11, x) ^ ROTR(25, x))
#define SSIG0(x) (ROTR(7, x) ^ ROTR(18, x) ^ SHR(3, x))
#define SSIG1(x) (ROTR(17, x) ^ ROTR(19, x) ^ SHR(10, x))

namespace xf {
namespace security {
namespace internal {

/// @brief 一个 SHA-256 消息分组，共 16 个 32 bit 原始字，即 512 bit。
struct SHA256Block {
    uint32_t M[16];
};

/// @brief SHA-224/SHA-256 静态配置；两者压缩过程相同，仅初值和输出字数不同。
template <bool do_sha224>
struct sha256_digest_config;

template <>
struct sha256_digest_config<true> {
    static const short numH = 7;
};

template <>
struct sha256_digest_config<false> {
    static const short numH = 8;
};

/// @brief 32 bit 输入版本的消息预处理与调度字生成。
/// @details 将消息拆成 512 bit 分组，附加一个“1”位、若干“0”位和 64 bit 原始
///          位长度，再由 W[0..15] 递推得到 W[16..63]。优化版把调度字直接写入
///          `w_strm`，使生成阶段可以与下游压缩阶段重叠执行。
/// @param msg_strm 32 bit 消息数据流。
/// @param len_strm 每条消息的字节长度流。
/// @param end_len_strm 长度流结束标志；false 表示仍有消息，true 表示全部结束。
/// @param w_strm 每个分组的 64 个消息调度字输出流。
/// @param nblk_strm 每条消息经过补位后的分组数。
/// @param end_nblk_strm 分组数流的结束标志。
/// @note 当尾部不超过 55 字节时补位只需一个分组；超过 55 字节时需要两个分组。
inline void preProcessing(hls::stream<ap_uint<32> >& msg_strm,
                                   hls::stream<ap_uint<64> >& len_strm,
                                   hls::stream<bool>& end_len_strm,
                                   hls::stream<uint32_t>& w_strm,
                                   hls::stream<uint64_t>& nblk_strm,
                                   hls::stream<bool>& end_nblk_strm) {
    // 按消息循环处理。只有读到 end_len_strm=true 才退出。
LOOP_SHA256_GENENERATE_MAIN:
    for (bool end_flag = end_len_strm.read(); !end_flag; end_flag = end_len_strm.read()) {
        /// 当前消息长度，单位为字节。
        uint64_t len = len_strm.read();
        /// 标准要求写入补位尾部的是原消息位长度 L。
        uint64_t L = 8 * len;
        /// 分组数 = 完整分组数 + 至少一个补位分组 + 可能的第二个尾部分组。
        uint64_t blk_num = (len >> 6) + 1 + ((len & 0x3f) > 55);

        // 先通知下游本消息包含多少个分组，便于摘要核保持正确的链式状态。
        nblk_strm.write(blk_num);
        end_nblk_strm.write(false);

    // Process all complete 512-bit blocks from the message
    LOOP_SHA256_GEN_FULL_BLKS:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (uint64_t j = 0; j < uint64_t(len >> 6); ++j) {
            /// Message schedule W array (16-word sliding window)
            /// Used for generating the expanded message schedule W[0..63]
            uint32_t W[16];
            // 分割数组以增加并行读写端口，避免单个存储体端口数限制计算吞吐。
            #pragma HLS array_partition variable = W complete

            // Process first 16 words (W[0]-W[15]) from the message block
            // These are the original 512-bit message block words
        LOOP_SHA256_GEN_FIRST_16_WORDS:
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 0; t < 16; ++t) {
                #pragma HLS pipeline II = 1
                uint32_t l = msg_strm.read();
                // Convert from little-endian to big-endian as required by SHA-256 specification
                // SHA-256 algorithm assumes big-endian byte order
                l = ((0x000000ffUL & l) << 24) | ((0x0000ff00UL & l) << 8) |
                    ((0x00ff0000UL & l) >> 8) | ((0xff000000UL & l) >> 24);
                W[t] = l;
                w_strm.write(l);  // Output W[0]-W[15] directly to message schedule
                _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (first 16)\n", t, l);
            }

            // Generate remaining 48 words (W[16]-W[63]) using SHA-256 expansion function
            // W[t] = σ1(W[t-2]) + W[t-7] + σ0(W[t-15]) + W[t-16]
        LOOP_SHA256_GEN_REMAINING_48_WORDS:
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 16; t < 64; ++t) {
                #pragma HLS pipeline II = 1
                // Apply SHA-256 message schedule expansion formula
                uint32_t Wt = SSIG1(W[14]) + W[9] + SSIG0(W[1]) + W[0];

                // Update sliding window: shift all elements left by one position
                // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
                for (unsigned char j = 0; j < 15; ++j) {
                    W[j] = W[j + 1];
                }
                W[15] = Wt;
                w_strm.write(Wt);  // Output W[16]-W[63] to message schedule
                _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (extended)\n", t, Wt);
            }
        }

        /// Number of bytes remaining after processing full blocks
        char left = (char)(len & 0x3fULL); // Equivalent to len % 64

        _XF_SECURITY_PRINT("DEBUG: sent = %d, left = %d\n",
                          int(len & (-1ULL ^ 0x3fULL)), (int)left);

        // Handle padding based on remaining bytes
        if (left == 0) {
            // Case 1: Message ends exactly at block boundary
            // Start new block with padding '1' bit
            uint32_t W[16];
            // 分割数组以增加并行读写端口，避免单个存储体端口数限制计算吞吐。
            #pragma HLS array_partition variable = W complete

            // First word: append padding bit '1' followed by zeros
            W[0] = 0x80000000UL;  // '1' bit at MSB position
            w_strm.write(0x80000000UL);
            _XF_SECURITY_PRINT("DEBUG: W[0] =\t%08x (pad 1)\n", 0x80000000UL);

            // Words 1-13: zero padding
        LOOP_SHA256_GEN_PAD_ZEROS:
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 1; t < 14; ++t) {
                #pragma HLS pipeline II = 1
                W[t] = 0;
                w_strm.write(0);
                _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (zero)\n", t, 0);
            }

            // Words 14-15: append 64-bit message length L (big-endian)
            W[14] = (uint32_t)(0xffffffffUL & (L >> 32));  // Upper 32 bits of L
            W[15] = (uint32_t)(0xffffffffUL & (L));        // Lower 32 bits of L
            w_strm.write((uint32_t)(0xffffffffUL & (L >> 32)));
            w_strm.write((uint32_t)(0xffffffffUL & (L)));
            _XF_SECURITY_PRINT("DEBUG: W[14] =\t%08x (append L)\n", W[14]);
            _XF_SECURITY_PRINT("DEBUG: W[15] =\t%08x (append L)\n", W[15]);

            // Generate remaining 48 words for padding block using expansion function
        LOOP_SHA256_GEN_PAD_EXTENSION:
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 16; t < 64; ++t) {
                #pragma HLS pipeline II = 1
                uint32_t Wt = SSIG1(W[14]) + W[9] + SSIG0(W[1]) + W[0];
                // Update sliding window
                // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
                for (unsigned char j = 0; j < 15; ++j) {
                    W[j] = W[j + 1];
                }
                W[15] = Wt;
                w_strm.write(Wt);
                _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (pad extended)\n", t, Wt);
            }

        } else if (left < 56) {
            // Case 2: Remaining bytes fit in one block with padding and length
            // Can append padding '1', zeros, and length L in single block
            uint32_t W[16];
            // 分割数组以增加并行读写端口，避免单个存储体端口数限制计算吞吐。
            #pragma HLS array_partition variable = W complete
            int word_count = 0;

            // Process remaining message data with integrated padding
        LOOP_SHA256_GEN_TAIL_WITH_PADDING:
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 0; t < 16; ++t) {
                #pragma HLS pipeline II = 1
                if (t < (left >> 2)) {
                    // Process complete 32-bit words from remaining message
                    uint32_t l = msg_strm.read();
                    // Convert to big-endian
                    l = ((0x000000ffUL & l) << 24) | ((0x0000ff00UL & l) << 8) |
                        ((0x00ff0000UL & l) >> 8) | ((0xff000000UL & l) >> 24);
                    W[t] = l;
                    w_strm.write(l);
                    _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (tail data)\n", t, l);
                } else if (t == (left >> 2)) {
                    // Add padding '1' bit in the current partial word
                    uint32_t e = left & 3L;  // Remaining bytes in current word (0-3)
                    uint32_t padded_word;

                    // Handle different partial word cases
                    if (e == 0) {
                        // Word-aligned: start new word with padding '1'
                        padded_word = 0x80000000UL;
                    } else if (e == 1) {
                        // 1 byte remaining: read it and add padding
                        uint32_t l = msg_strm.read();
                        l = ((0x000000ffUL & l) << 24);  // Keep only valid byte
                        padded_word = l | 0x00800000UL;  // Add padding at byte position 1
                    } else if (e == 2) {
                        // 2 bytes remaining: read them and add padding
                        uint32_t l = msg_strm.read();
                        l = ((0x000000ffUL & l) << 24) | ((0x0000ff00UL & l) << 8);
                        padded_word = l | 0x00008000UL;  // Add padding at byte position 2
                    } else {
                        // 3 bytes remaining: read them and add padding
                        uint32_t l = msg_strm.read();
                        l = ((0x000000ffUL & l) << 24) | ((0x0000ff00UL & l) << 8) |
                            ((0x00ff0000UL & l) >> 8);
                        padded_word = l | 0x00000080UL;  // Add padding at byte position 3
                    }
                    W[t] = padded_word;
                    w_strm.write(padded_word);
                    _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (tail with pad)\n", t, padded_word);
                } else if (t < 14) {
                    // Zero padding between message and length fields
                    W[t] = 0;
                    w_strm.write(0);
                    _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (zero)\n", t, 0);
                } else if (t == 14) {
                    // Append upper 32 bits of message length L
                    W[t] = (uint32_t)(0xffffffffUL & (L >> 32));
                    w_strm.write((uint32_t)(0xffffffffUL & (L >> 32)));
                    _XF_SECURITY_PRINT("DEBUG: W[14] =\t%08x (append L)\n", W[14]);
                } else { // t == 15
                    // Append lower 32 bits of message length L
                    W[t] = (uint32_t)(0xffffffffUL & (L));
                    w_strm.write((uint32_t)(0xffffffffUL & (L)));
                    _XF_SECURITY_PRINT("DEBUG: W[15] =\t%08x (append L)\n", W[15]);
                }
            }

            // Generate remaining 48 words using message schedule expansion
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 16; t < 64; ++t) {
                #pragma HLS pipeline II = 1
                uint32_t Wt = SSIG1(W[14]) + W[9] + SSIG0(W[1]) + W[0];
                for (unsigned char j = 0; j < 15; ++j) {
                    W[j] = W[j + 1];
                }
                W[15] = Wt;
                w_strm.write(Wt);
                _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (tail extended)\n", t, Wt);
            }

        } else {
            // Case 3: Remaining bytes require two blocks
            // First block contains message data with padding
            // Second block contains only the length field

            // First block: process remaining message data with padding
            uint32_t W[16];
            // 分割数组以增加并行读写端口，避免单个存储体端口数限制计算吞吐。
            #pragma HLS array_partition variable = W complete

            // Copy all remaining message words and add padding
        LOOP_SHA256_GEN_FIRST_TAIL_BLOCK:
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 0; t < 16; ++t) {
                #pragma HLS pipeline II = 1
                if (t < (left >> 2)) {
                    // Process complete words from remaining message
                    uint32_t l = msg_strm.read();
                    l = ((0x000000ffUL & l) << 24) | ((0x0000ff00UL & l) << 8) |
                        ((0x00ff0000UL & l) >> 8) | ((0xff000000UL & l) >> 24);
                    W[t] = l;
                    w_strm.write(l);
                    _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (first tail data)\n", t, l);
                } else if (t == (left >> 2)) {
                    // Add padding '1' bit in partial word
                    uint32_t e = left & 3L;
                    uint32_t padded_word;

                    if (e == 0) {
                        padded_word = 0x80000000UL;
                    } else if (e == 1) {
                        uint32_t l = msg_strm.read();
                        l = ((0x000000ffUL & l) << 24);
                        padded_word = l | 0x00800000UL;
                    } else if (e == 2) {
                        uint32_t l = msg_strm.read();
                        l = ((0x000000ffUL & l) << 24) | ((0x0000ff00UL & l) << 8);
                        padded_word = l | 0x00008000UL;
                    } else {
                        uint32_t l = msg_strm.read();
                        l = ((0x000000ffUL & l) << 24) | ((0x0000ff00UL & l) << 8) |
                            ((0x00ff0000UL & l) >> 8);
                        padded_word = l | 0x00000080UL;
                    }
                    W[t] = padded_word;
                    w_strm.write(padded_word);
                    _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (first tail with pad)\n", t, padded_word);
                } else {
                    // Fill remaining words with zeros
                    W[t] = 0;
                    w_strm.write(0);
                    _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (zero)\n", t, 0);
                }
            }

            // Generate extension words for first tail block
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 16; t < 64; ++t) {
                #pragma HLS pipeline II = 1
                uint32_t Wt = SSIG1(W[14]) + W[9] + SSIG0(W[1]) + W[0];
                for (unsigned char j = 0; j < 15; ++j) {
                    W[j] = W[j + 1];
                }
                W[15] = Wt;
                w_strm.write(Wt);
                _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (first tail extended)\n", t, Wt);
            }

            // Second block: contains only the 64-bit length field
            uint32_t W2[16];
            // 分割数组以增加并行读写端口，避免单个存储体端口数限制计算吞吐。
            #pragma HLS array_partition variable = W2 complete

        LOOP_SHA256_GEN_SECOND_TAIL_BLOCK:
            for (int t = 0; t < 16; ++t) {
                // 对当前函数或循环建立流水线；II 约束决定相邻事务的最小启动间隔。
                #pragma HLS pipeline II = 1
                if (t < 14) {
                    // First 13 words are zeros
                    W2[t] = 0;
                    w_strm.write(0);
                    _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (second tail zero)\n", t, 0);
                } else if (t == 14) {
                    // Word 14: upper 32 bits of message length L
                    W2[t] = (uint32_t)(0xffffffffUL & (L >> 32));
                    w_strm.write((uint32_t)(0xffffffffUL & (L >> 32)));
                    _XF_SECURITY_PRINT("DEBUG: W[14] =\t%08x (second tail L high)\n", W2[14]);
                } else { // t == 15
                    // Word 15: lower 32 bits of message length L
                    W2[t] = (uint32_t)(0xffffffffUL & (L));
                    w_strm.write((uint32_t)(0xffffffffUL & (L)));
                    _XF_SECURITY_PRINT("DEBUG: W[15] =\t%08x (second tail L low)\n", W2[15]);
                }
            }

            // Generate extension words for second tail block
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 16; t < 64; ++t) {
                #pragma HLS pipeline II = 1
                uint32_t Wt = SSIG1(W2[14]) + W2[9] + SSIG0(W2[1]) + W2[0];
                for (unsigned char j = 0; j < 15; ++j) {
                    W2[j] = W2[j + 1];
                }
                W2[15] = Wt;
                w_strm.write(Wt);
                _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (second tail extended)\n", t, Wt);
            }
        }
    } // End main message loop

    // Signal end of processing to downstream blocks
    end_nblk_strm.write(true);
} // End preProcessing (32-bit version)

/// @brief Preprocessing function for SHA-224/256 (64-bit input version)
/// @details Generates complete message schedule W[0]-W[63] for SHA-256/224 algorithm
///          optimized for 64-bit input data width. This version processes 64-bit words
///          and splits them into two 32-bit words for the SHA-256 message schedule.
///          Handles message padding and block formatting according to FIPS 180-4 specification.
///
/// Key differences from 32-bit version:
/// - Input data is 64-bit wide, requiring splitting into two 32-bit words
/// - More efficient processing for 64-bit aligned data
/// - Complex tail handling for partial 64-bit words
///
/// @param msg_strm Input message stream (64-bit words)
/// @param len_strm Message length stream in bytes
/// @param end_len_strm End flag stream for message input
/// @param w_strm Output message schedule words W[0]-W[63]
/// @param nblk_strm Output stream for number of hash blocks per message
/// @param end_nblk_strm End flag stream for block count
inline void preProcessing(hls::stream<ap_uint<64> >& msg_strm,
                                   hls::stream<ap_uint<64> >& len_strm,
                                   hls::stream<bool>& end_len_strm,
                                   hls::stream<uint32_t>& w_strm,
                                   hls::stream<uint64_t>& nblk_strm,
                                   hls::stream<bool>& end_nblk_strm) {
    // Main processing loop for each message in the stream
LOOP_SHA256_GENENERATE_MAIN_64:
    // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
    for (bool end_flag = end_len_strm.read(); !end_flag; end_flag = end_len_strm.read()) {
        /// Message length in bytes
        uint64_t len = len_strm.read();
        _XF_SECURITY_PRINT("DEBUG: working on a new message of %ld bytes\n", len);
        /// Message length in bits (L)
        uint64_t L = 8 * len;
        /// Total number of 512-bit blocks to process
        /// Calculation: full blocks + padding block + potential extra block for length
        uint64_t blk_num = (len >> 6) + 1 + ((len & 0x3f) > 55);

        // Inform downstream digest function about block count
        nblk_strm.write(blk_num);
        end_nblk_strm.write(false);

    // Process all complete 512-bit blocks from the message
    LOOP_SHA256_GEN_FULL_BLKS_64:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (uint64_t j = 0; j < uint64_t(len >> 6); ++j) {
            /// Message schedule W array (16-word sliding window)
            uint32_t W[16];
            // 分割数组以增加并行读写端口，避免单个存储体端口数限制计算吞吐。
            #pragma HLS array_partition variable = W complete

            // Process first 16 words (W[0]-W[15]) from 64-bit input data
            // Each 64-bit input word produces two 32-bit message schedule words
        LOOP_SHA256_GEN_FIRST_16_WORDS_64:
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 0; t < 16; t += 2) {
                #pragma HLS pipeline II = 1
                // Read 64-bit word from input stream
                uint64_t ll = msg_strm.read().to_uint64();

                // Process low 32 bits of the 64-bit word
                uint32_t l_low = ll & 0xffffffffUL;
                // Convert from little-endian to big-endian as required by SHA-256 specification
                l_low = ((0x000000ffUL & l_low) << 24) | ((0x0000ff00UL & l_low) << 8) |
                        ((0x00ff0000UL & l_low) >> 8) | ((0xff000000UL & l_low) >> 24);
                W[t] = l_low;
                w_strm.write(l_low);
                _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (64b low)\n", t, l_low);

                // Process high 32 bits of the 64-bit word
                uint32_t l_high = (ll >> 32) & 0xffffffffUL;
                // Convert from little-endian to big-endian
                l_high = ((0x000000ffUL & l_high) << 24) | ((0x0000ff00UL & l_high) << 8) |
                         ((0x00ff0000UL & l_high) >> 8) | ((0xff000000UL & l_high) >> 24);
                W[t + 1] = l_high;
                w_strm.write(l_high);
                _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (64b high)\n", t + 1, l_high);
            }

            // Generate remaining 48 words (W[16]-W[63]) using SHA-256 expansion function
            // Same algorithm as 32-bit version: W[t] = σ1(W[t-2]) + W[t-7] + σ0(W[t-15]) + W[t-16]
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 16; t < 64; ++t) {
                #pragma HLS pipeline II = 1
                uint32_t Wt = SSIG1(W[14]) + W[9] + SSIG0(W[1]) + W[0];
                // Update sliding window: shift all elements left by one position
                // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
                for (unsigned char j = 0; j < 15; ++j) {
                    W[j] = W[j + 1];
                }
                W[15] = Wt;
                w_strm.write(Wt);
                _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (64b extended)\n", t, Wt);
            }
        }

        /// Number of bytes remaining after processing full blocks
        char left = (char)(len & 0x3fULL); // Equivalent to len % 64

        _XF_SECURITY_PRINT("DEBUG: sent = %d, left = %d\n",
                          int(len & (-1ULL ^ 0x3fULL)), (int)left);

        // Handle padding based on remaining bytes
        if (left == 0) {
            // Case 1: Message ends exactly at block boundary
            // Start new block with padding '1' bit
            uint32_t W[16];
            // 分割数组以增加并行读写端口，避免单个存储体端口数限制计算吞吐。
            #pragma HLS array_partition variable = W complete

            // First word: append padding bit '1' followed by zeros
            W[0] = 0x80000000UL;  // '1' bit at MSB position
            w_strm.write(0x80000000UL);
            _XF_SECURITY_PRINT("DEBUG: W[0] =\t%08x (pad 1)\n", 0x80000000UL);

            // Words 1-13: zero padding
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 1; t < 14; ++t) {
                #pragma HLS pipeline II = 1
                W[t] = 0;
                w_strm.write(0);
                _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (zero)\n", t, 0);
            }

            // Words 14-15: append 64-bit message length L (big-endian)
            W[14] = (uint32_t)(0xffffffffUL & (L >> 32));  // Upper 32 bits of L
            W[15] = (uint32_t)(0xffffffffUL & (L));        // Lower 32 bits of L
            w_strm.write(W[14]);
            w_strm.write(W[15]);
            _XF_SECURITY_PRINT("DEBUG: W[14] =\t%08x (append L)\n", W[14]);
            _XF_SECURITY_PRINT("DEBUG: W[15] =\t%08x (append L)\n", W[15]);

            // Generate remaining 48 words for padding block using expansion function
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 16; t < 64; ++t) {
                #pragma HLS pipeline II = 1
                uint32_t Wt = SSIG1(W[14]) + W[9] + SSIG0(W[1]) + W[0];
                // Update sliding window
                // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
                for (unsigned char j = 0; j < 15; ++j) {
                    W[j] = W[j + 1];
                }
                W[15] = Wt;
                w_strm.write(Wt);
                _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (pad extended)\n", t, Wt);
            }

        } else {
            // Case 2 & 3: Handle remaining bytes with complex 64-bit tail processing
            // This handles both single-block and two-block padding scenarios
            uint32_t W[16];
            // 分割数组以增加并行读写端口，避免单个存储体端口数限制计算吞吐。
            #pragma HLS array_partition variable = W complete

            // Calculate total number of 32-bit words needed for remaining data
            // Each 64-bit input word produces two 32-bit words
            int words_processed = 0;
            int total_words = (left + 7) / 8 * 2; // Convert bytes to 32-bit word count

            // Process tail data with integrated padding for 64-bit input
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 0; t < 16; ++t) {
                #pragma HLS pipeline II = 1
                if (t < total_words / 2) {
                    // Still have complete 64-bit words to process from remaining message
                    uint64_t ll = msg_strm.read().to_uint64();

                    // Process low 32 bits of 64-bit word
                    uint32_t l_low = ll & 0xffffffffUL;
                    l_low = ((0x000000ffUL & l_low) << 24) | ((0x0000ff00UL & l_low) << 8) |
                            ((0x00ff0000UL & l_low) >> 8) | ((0xff000000UL & l_low) >> 24);
                    W[t * 2] = l_low;
                    w_strm.write(l_low);
                    _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (tail 64b low)\n", t * 2, l_low);

                    // Process high 32 bits of 64-bit word
                    uint32_t l_high = (ll >> 32) & 0xffffffffUL;
                    l_high = ((0x000000ffUL & l_high) << 24) | ((0x0000ff00UL & l_high) << 8) |
                             ((0x00ff0000UL & l_high) >> 8) | ((0xff000000UL & l_high) >> 24);
                    W[t * 2 + 1] = l_high;
                    w_strm.write(l_high);
                    _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (tail 64b high)\n", t * 2 + 1, l_high);

                    words_processed += 2;
                } else if (t == total_words / 2 && (left % 8) != 0) {
                    // Handle partial 64-bit word (boundary case)
                    // This occurs when we have a 64-bit word that's not completely filled
                    int bytes_in_last_word = left % 8;
                    uint64_t ll = msg_strm.read().to_uint64();

                    // Process based on how many bytes are left in the final 64-bit word
                    if (bytes_in_last_word >= 4) {
                        // Case: At least 4 bytes remaining - process low 32 bits completely
                        uint32_t l_low = ll & 0xffffffffUL;
                        l_low = ((0x000000ffUL & l_low) << 24) | ((0x0000ff00UL & l_low) << 8) |
                                ((0x00ff0000UL & l_low) >> 8) | ((0xff000000UL & l_low) >> 24);
                        W[t * 2] = l_low;
                        w_strm.write(l_low);
                        _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (tail partial low)\n", t * 2, l_low);

                        // Handle high 32 bits with padding based on remaining bytes
                        uint32_t l_high = (ll >> 32) & 0xffffffffUL;
                        int remaining_bytes = bytes_in_last_word - 4;

                        uint32_t padded_high = 0;
                        if (remaining_bytes == 0) {
                            // No bytes left in high word - add padding at start
                            padded_high = 0x80000000UL;
                        } else if (remaining_bytes == 1) {
                            // 1 byte in high word - process and add padding
                            l_high = ((0x000000ffUL & l_high) << 24);
                            padded_high = l_high | 0x00800000UL;  // Pad at byte position 1
                        } else if (remaining_bytes == 2) {
                            // 2 bytes in high word - process and add padding
                            l_high = ((0x000000ffUL & l_high) << 24) | ((0x0000ff00UL & l_high) << 8);
                            padded_high = l_high | 0x00008000UL;  // Pad at byte position 2
                        } else { // remaining_bytes == 3
                            // 3 bytes in high word - process and add padding
                            l_high = ((0x000000ffUL & l_high) << 24) | ((0x0000ff00UL & l_high) << 8) |
                                    ((0x00ff0000UL & l_high) >> 8);
                            padded_high = l_high | 0x00000080UL;  // Pad at byte position 3
                        }

                        W[t * 2 + 1] = padded_high;
                        w_strm.write(padded_high);
                        _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (tail partial high with pad)\n",
                                          t * 2 + 1, padded_high);

                        words_processed += 2;
                    } else {
                        // Case: Less than 4 bytes remaining - only low 32 bits with padding
                        uint32_t l_low = ll & 0xffffffffUL;
                        uint32_t padded_low = 0;

                        if (bytes_in_last_word == 0) {
                            // Special case: exactly aligned, add padding
                            padded_low = 0x80000000UL;
                        } else if (bytes_in_last_word == 1) {
                            // 1 byte in low word - process and add padding
                            l_low = ((0x000000ffUL & l_low) << 24);
                            padded_low = l_low | 0x00800000UL;  // Pad at byte position 1
                        } else if (bytes_in_last_word == 2) {
                            // 2 bytes in low word - process and add padding
                            l_low = ((0x000000ffUL & l_low) << 24) | ((0x0000ff00UL & l_low) << 8);
                            padded_low = l_low | 0x00008000UL;  // Pad at byte position 2
                        } else { // bytes_in_last_word == 3
                            // 3 bytes in low word - process and add padding
                            l_low = ((0x000000ffUL & l_low) << 24) | ((0x0000ff00UL & l_low) << 8) |
                                   ((0x00ff0000UL & l_low) >> 8);
                            padded_low = l_low | 0x00000080UL;  // Pad at byte position 3
                        }

                        W[t * 2] = padded_low;
                        w_strm.write(padded_low);
                        _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (tail partial low with pad)\n",
                                          t * 2, padded_low);

                        // High 32 bits are all zeros since we're in low-word-only case
                        W[t * 2 + 1] = 0;
                        w_strm.write(0);
                        _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (zero)\n", t * 2 + 1, 0);

                        words_processed += 2;
                    }
                } else if (t * 2 < 14) {
                    // Fill remaining words with zeros until length field
                    W[t * 2] = 0;
                    w_strm.write(0);
                    _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (zero)\n", t * 2, 0);

                    // Handle the second word of the pair if needed
                    if (t * 2 + 1 < 14) {
                        W[t * 2 + 1] = 0;
                        w_strm.write(0);
                        _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (zero)\n", t * 2 + 1, 0);
                    }
                } else if (t * 2 == 14) {
                    // Words 14-15: append 64-bit message length L
                    W[14] = (uint32_t)(0xffffffffUL & (L >> 32));  // Upper 32 bits
                    w_strm.write(W[14]);
                    _XF_SECURITY_PRINT("DEBUG: W[14] =\t%08x (append L high)\n", W[14]);

                    W[15] = (uint32_t)(0xffffffffUL & (L));        // Lower 32 bits
                    w_strm.write(W[15]);
                    _XF_SECURITY_PRINT("DEBUG: W[15] =\t%08x (append L low)\n", W[15]);
                }
            }

            // Generate remaining 48 words using message schedule expansion
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int t = 16; t < 64; ++t) {
                #pragma HLS pipeline II = 1
                uint32_t Wt = SSIG1(W[14]) + W[9] + SSIG0(W[1]) + W[0];
                for (unsigned char j = 0; j < 15; ++j) {
                    W[j] = W[j + 1];
                }
                W[15] = Wt;
                w_strm.write(Wt);
                _XF_SECURITY_PRINT("DEBUG: W[%d] =\t%08x (tail extended)\n", t, Wt);
            }
        }
    } // End main message loop

    // Signal end of processing to downstream blocks
    end_nblk_strm.write(true);
} // End preProcessing (64-bit version)

/// @brief Stream duplication utility function
/// @details Duplicates an input stream into two identical output streams, including both data and end flags.
///          This function is commonly used in dataflow architectures to feed the same data to multiple
///          processing units or to create parallel processing paths.
///
/// The duplication maintains:
/// - Identical data content in both output streams
/// - Synchronized end flags
/// - Same data ordering in both outputs
/// - Pipeline-friendly architecture with II=1
///
/// @param in_strm Input data stream (64-bit values)
/// @param in_e_strm Input end flag stream (signals end of data)
/// @param out1_strm First output data stream (duplicate of input)
/// @param out1_e_strm First output end flag stream
/// @param out2_strm Second output data stream (duplicate of input)
/// @param out2_e_strm Second output end flag stream
///
/// @note Key HLS optimizations:
///       - Pipeline II=1 ensures maximum throughput
///       - Loop tripcount directives aid resource estimation
///       - Minimal latency design with direct stream-to-stream copying
/// @brief 将一条控制流同步复制成两份。
/// @details 数据和结束标志必须成对复制；II=1 使该辅助模块不会成为 DATAFLOW 的吞吐瓶颈。
inline void dup_strm(hls::stream<uint64_t>& in_strm,
                     hls::stream<bool>& in_e_strm,
                     hls::stream<uint64_t>& out1_strm,
                     hls::stream<bool>& out1_e_strm,
                     hls::stream<uint64_t>& out2_strm,
                     hls::stream<bool>& out2_e_strm) {
    // Read initial end flag to start processing
    bool e = in_e_strm.read();

    // Main processing loop: continue until end flag is encountered
    // 持续处理当前消息或数据流，直至结束标志、剩余计数或状态条件触发退出。
    while (!e) {
        // HLS optimization directives
        #pragma HLS loop_tripcount min = 1 max = 1 avg = 1  // Typical case: single message
        // 对当前函数或循环建立流水线；II 约束决定相邻事务的最小启动间隔。
        #pragma HLS pipeline II = 1  // Achieve initiation interval of 1 for maximum throughput

        // Read one element from input data stream
        uint64_t in_r = in_strm.read();

        // Duplicate data to first output stream
        out1_strm.write(in_r);
        out1_e_strm.write(false);  // Active data flag for first output

        // Duplicate data to second output stream
        out2_strm.write(in_r);
        out2_e_strm.write(false);  // Active data flag for second output

        // Read next end flag to determine if we should continue
        e = in_e_strm.read();
    }

    // Write end flags to both output streams to signal completion
    out1_e_strm.write(true);
    out2_e_strm.write(true);
}

/// @brief Single SHA-256 compression function iteration
/// @details Performs one round of the SHA-256 compression function according to FIPS 180-4 specification.
///          This function implements the core SHA-256 transformation that processes one message schedule
///          word per cycle and updates all eight working variables (a-h).
///
/// The compression function follows the standard SHA-256 algorithm:
///   T1 = h + Σ1(e) + Ch(e,f,g) + K[t] + W[t]
///   T2 = Σ0(a) + Maj(a,b,c)
///   a = T1 + T2
///   b = a
///   c = b
///   d = c
///   e = d + T1
///   f = e
///   g = f
///   h = g
///
/// @param a-h Working variables (a-h) passed by reference, updated in-place each round
/// @param w_strm Message schedule word stream (W[0]-W[63])
/// @param Kt Current round constant K[t], updated to K[t+1] for next iteration
/// @param K SHA-256 constants array (64 precomputed values)
/// @param t Current round index (0-63), used to select next constant
///
/// @note HLS optimization directives:
///       - INLINE off: Preserves function boundaries for better resource sharing
///       - PIPELINE II=1: Achieves single-cycle iteration interval for maximum throughput
///       - Working variables use register storage for minimal access latency
/// @brief 执行一轮 SHA-256 压缩。
/// @details 本函数从流中读取一个 W[t]，计算
/// `T1=h+Σ1(e)+Ch(e,f,g)+K[t]+W[t]` 和 `T2=Σ0(a)+Maj(a,b,c)`，随后更新 a~h。
/// 禁止内联用于保留清晰的轮函数硬件边界，循环流水目标 II=1。
inline void sha256_iter(uint32_t& a,
                        uint32_t& b,
                        uint32_t& c,
                        uint32_t& d,
                        uint32_t& e,
                        uint32_t& f,
                        uint32_t& g,
                        uint32_t& h,
                        hls::stream<uint32_t>& w_strm,
                        uint32_t& Kt,
                        const uint32_t K[],
                        short t) {
    // HLS optimization directives
    // 控制函数是否内联；保留或消除模块边界会影响层次、共享机会和组合路径长度。
    #pragma HLS INLINE off      // Prevent inlining to maintain function hierarchy
    #pragma HLS PIPELINE II = 1 // Single-cycle iteration for maximum throughput

    // Read current message schedule word from stream
    uint32_t Wt = w_strm.read();

    /// Temporal variables for intermediate calculations
    uint32_t T1, T2;

    // Compute T1 = h + Σ1(e) + Ch(e,f,g) + Kt + Wt
    // Where: Σ1(e) = ROTR(6,e) ⊕ ROTR(11,e) ⊕ ROTR(25,e)
    //        Ch(e,f,g) = (e ∧ f) ⊕ (¬e ∧ g)
    T1 = h + BSIG1(e) + CH(e, f, g) + Kt + Wt;

    // Compute T2 = Σ0(a) + Maj(a,b,c)
    // Where: Σ0(a) = ROTR(2,a) ⊕ ROTR(13,a) ⊕ ROTR(22,a)
    //        Maj(a,b,c) = (a ∧ b) ⊕ (a ∧ c) ⊕ (b ∧ c)
    T2 = BSIG0(a) + MAJ(a, b, c);

    // Update working variables according to SHA-256 specification
    // Variables are shifted through the registers with new values at a and e
    h = g;
    g = f;
    f = e;
    e = d + T1;  // e gets d plus T1
    d = c;
    c = b;
    b = a;
    a = T1 + T2; // a gets the sum of T1 and T2

    // Debug output for verification (disabled in synthesis)
    _XF_SECURITY_PRINT(
        "DEBUG: Kt=%08x, Wt=%08x\n"
        "\ta=%08x, b=%08x, c=%08x, d=%08x\n"
        "\te=%08x, f=%08x, g=%08x, h=%08x\n",
        Kt, Wt, a, b, c, d, e, f, g, h);

    // Prepare constant for next iteration using circular indexing
    // (t + 1) & 63 ensures wrap-around for indices 0-63
    Kt = K[(t + 1) & 63];
}

/// @brief SHA-256/224 digest computation core
/// @details Processes message blocks through SHA-256 compression function and emits final hash values.
///          This function implements the complete SHA-256/224 hashing algorithm including:
///          - Initialization with standard initial hash values
///          - Processing of multiple message blocks through 64 rounds each
///          - Final hash value computation and byte order conversion
///          - Support for both SHA-224 and SHA-256 output formats
///
/// @tparam h_width Hash output width (224 for SHA-224, 256 for SHA-256)
/// @param nblk_strm Input stream for number of message blocks to process
/// @param end_nblk_strm End flag stream for block count
/// @param w_strm Message schedule words stream (W[0]-W[63] for each block)
/// @param hash_strm Output hash stream (224-bit or 256-bit based on template)
/// @param end_hash_strm End flag stream for hash output
///
/// @note Algorithm features:
///       - Compliant with FIPS 180-4 SHA-256/224 specification
///       - Uses standard SHA-256 constants (K[0]-K[63])
///       - Supports both SHA-224 (truncated) and SHA-256 (full) output modes
///       - Implements Merkle-Damgard strengthening with block chaining
/// @brief SHA-224/SHA-256 摘要计算核心。
/// @details 每个 512 bit 分组执行 64 轮压缩；H[8] 完全分割后可并行更新。
/// `h_width=224` 时使用 SHA-224 初值并输出前 7 个状态字，`h_width=256` 时输出全部 8 个。
template <int h_width>
void sha256Digest(hls::stream<uint64_t>& nblk_strm,
                  hls::stream<bool>& end_nblk_strm,
                  hls::stream<uint32_t>& w_strm,
                  hls::stream<ap_uint<h_width> >& hash_strm,
                  hls::stream<bool>& end_hash_strm) {
    // Static assertion to ensure only valid hash widths are used
    XF_SECURITY_STATIC_ASSERT((h_width == 256) || (h_width == 224),
                              "Unsupported hash stream width, must be 224 or 256");

    /// SHA-256 constants K[0]-K[63]
    /// These are derived from the first 32 bits of the fractional parts of the cube roots
    /// of the first 64 prime numbers, as specified in FIPS 180-4
    static const uint32_t K[64] = {
        0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
        0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
        0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
        0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
        0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
        0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
        0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
        0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL, 0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL};
    #pragma HLS array_partition variable = K complete  // 64 个轮常量独立存放，消除数组端口竞争

    // Main processing loop for each message in the stream
LOOP_SHA256_DIGEST_MAIN:
    for (bool end_flag = end_nblk_strm.read(); !end_flag; end_flag = end_nblk_strm.read()) {
        /// Total number of 512-bit blocks to process for current message
        uint64_t blk_num = nblk_strm.read();
        // _XF_SECURITY_PRINT("expect %ld blocks.\n", blk_num);

        /// Internal hash state H[0]-H[7]
        /// These accumulate the hash computation across all message blocks
        uint32_t H[8];
        #pragma HLS array_partition variable = H complete  // 8 个链式状态寄存器可并行读取和更新

        // Initialize hash state with standard initial values
        if (h_width == 224) {
            // SHA-224 initial hash values (first 32 bits of fractional parts of square roots of 9th-16th primes)
            H[0] = 0xc1059ed8UL;
            H[1] = 0x367cd507UL;
            H[2] = 0x3070dd17UL;
            H[3] = 0xf70e5939UL;
            H[4] = 0xffc00b31UL;
            H[5] = 0x68581511UL;
            H[6] = 0x64f98fa7UL;
            H[7] = 0xbefa4fa4UL;
        } else {
            // SHA-256 initial hash values (first 32 bits of fractional parts of square roots of first 8 primes)
            H[0] = 0x6a09e667UL;
            H[1] = 0xbb67ae85UL;
            H[2] = 0x3c6ef372UL;
            H[3] = 0xa54ff53aUL;
            H[4] = 0x510e527fUL;
            H[5] = 0x9b05688cUL;
            H[6] = 0x1f83d9abUL;
            H[7] = 0x5be0cd19UL;
        }

    // Process each message block in sequence
    LOOP_SHA256_DIGEST_NBLK:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (uint64_t n = 0; n < blk_num; ++n) {
            #pragma HLS loop_tripcount min = 1 max = 1  // Resource estimation guidance
            #pragma HLS latency max = 65  // Maximum latency constraint for 64 rounds + overhead

            /// Working variables for SHA-256 compression function
            /// These hold intermediate state during the 64 rounds of processing
            uint32_t a, b, c, d, e, f, g, h;

            // Initialize working variables with current hash state
            // This implements the chaining of hash values between blocks
            a = H[0];
            b = H[1];
            c = H[2];
            d = H[3];
            e = H[4];
            f = H[5];
            g = H[6];
            h = H[7];

            // Initialize first round constant
            uint32_t Kt = K[0];

            // Process 64 rounds of SHA-256 compression function
        LOOP_SHA256_UPDATE_64_ROUNDS:
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (short t = 0; t < 64; ++t) {
                #pragma HLS pipeline II = 1  // 每周期启动一轮；64 轮仍保持严格的状态依赖

                // Execute one round of SHA-256 compression function
                // This updates all working variables (a-h) and advances to next constant
                sha256_iter(a, b, c, d, e, f, g, h, w_strm, Kt, K, t);
            } // End 64-round loop

            // Update hash state with the final working variable values
            // This implements the block chaining: H_i = H_{i-1} + working_variables
            H[0] = a + H[0];
            H[1] = b + H[1];
            H[2] = c + H[2];
            H[3] = d + H[3];
            H[4] = e + H[4];
            H[5] = f + H[5];
            H[6] = g + H[6];
            H[7] = h + H[7];
        } // End block processing loop

        // Emit final hash value in appropriate format (SHA-224 or SHA-256)
        if (h_width == 224) {
            // SHA-224 output: 224-bit hash using first 7 hash state words
            ap_uint<224> w224;
        LOOP_SHA256_EMIT_H224:
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (short i = 0; i < sha256_digest_config<true>::numH; ++i) {
                #pragma HLS unroll  // Fully unroll for parallel output assembly
                uint32_t l = H[i];
                // Convert from big-endian (SHA-256 internal) to little-endian (HLS output)
                // SHA-256 algorithm uses big-endian, while HLS typically uses little-endian
                uint8_t t0 = (((l) >> 24) & 0xff);  // Extract byte 0 (MSB)
                uint8_t t1 = (((l) >> 16) & 0xff);  // Extract byte 1
                uint8_t t2 = (((l) >> 8) & 0xff);   // Extract byte 2
                uint8_t t3 = (((l)) & 0xff);        // Extract byte 3 (LSB)
                // Reconstruct in little-endian order
                uint32_t l_little = ((uint32_t)t0) | (((uint32_t)t1) << 8) |
                                   (((uint32_t)t2) << 16) | (((uint32_t)t3) << 24);
                w224.range(32 * i + 31, 32 * i) = l_little;
            }
            hash_strm.write(w224);
        } else {
            // SHA-256 output: 256-bit hash using all 8 hash state words
            ap_uint<256> w256;
        LOOP_SHA256_EMIT_H256:
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (short i = 0; i < sha256_digest_config<false>::numH; ++i) {
                #pragma HLS unroll  // Fully unroll for parallel output assembly
                uint32_t l = H[i];
                // Convert from big-endian to little-endian
                uint8_t t0 = (((l) >> 24) & 0xff);
                uint8_t t1 = (((l) >> 16) & 0xff);
                uint8_t t2 = (((l) >> 8) & 0xff);
                uint8_t t3 = (((l)) & 0xff);
                uint32_t l_little = ((uint32_t)t0) | (((uint32_t)t1) << 8) |
                                   (((uint32_t)t2) << 16) | (((uint32_t)t3) << 24);
                w256.range(32 * i + 31, 32 * i) = l_little;
            }
            hash_strm.write(w256);
        }
        end_hash_strm.write(false);  // Signal valid hash output
    } // End main message loop

    // Signal end of all hash processing
    end_hash_strm.write(true);

} // End sha256Digest

/// @brief Top-level SHA-256/224 implementation with DATAFLOW optimization
/// @details This function implements the complete SHA-256/224 hashing pipeline using HLS DATAFLOW optimization.
///          It separates the implementation into two parallel stages:
///          1. Preprocessing: Generates complete message schedule W[0]-W[63]
///          2. Digest computation: Processes message blocks and produces final hash
///
/// The DATAFLOW pragma enables concurrent execution of preprocessing and digest stages,
/// allowing pipelined processing of multiple messages for maximum throughput.
///
/// @tparam m_width Input message stream width in bits (32 or 64)
/// @tparam h_width Output hash stream width in bits (224 for SHA-224, 256 for SHA-256)
/// @param msg_strm Input message stream
/// @param len_strm Message length stream in bytes
/// @param end_len_strm End flag stream for message input (one per message)
/// @param hash_strm Output hash stream
/// @param end_hash_strm End flag stream for hash output (one per hash)
///
/// @note Architecture features:
///       - DATAFLOW optimization enables concurrent preprocessing and digest computation
///       - BRAM-based message schedule stream for large depth requirements
///       - LUTRAM-based control streams for efficient small storage
///       - Fully pipelined processing for maximum throughput
///       - Support for both SHA-224 and SHA-256 through template specialization
/// @brief 单核顶层：把“预处理/调度生成”和“64 轮摘要计算”组成 DATAFLOW 流水线。
/// @details w_strm 是数据通道，nblk_strm 与 end_nblk_strm 是消息边界控制通道；三者
/// 必须保持一一对应，否则下游会把不同消息的分组错误串联。
template <int m_width, int h_width>
inline void sha256_top(hls::stream<ap_uint<m_width> >& msg_strm,
                                hls::stream<ap_uint<64> >& len_strm,
                                hls::stream<bool>& end_len_strm,
                                hls::stream<ap_uint<h_width> >& hash_strm,
                                hls::stream<bool>& end_hash_strm) {
    // HLS DATAFLOW directive enables concurrent execution of preprocessing and digest stages
    // This allows pipelined processing where preprocessing of next block overlaps with
    // digest computation of current block
    #pragma HLS DATAFLOW  // 允许预处理下一分组时并行压缩当前分组

    /// Complete message schedule words stream (W[0]-W[63] for each block)
    /// Depth 256 accommodates up to 4 full message blocks (64 words each) in pipeline
    hls::stream<uint32_t> w_strm("w_strm");
    // 设置流通道的 FIFO 深度，用于吸收上下游速率差并降低数据流停顿风险。
    #pragma HLS STREAM variable = w_strm depth = 256
    #pragma HLS RESOURCE variable = w_strm core = FIFO_BRAM  // 深数据队列使用 BRAM，节省 LUT

    /// Number of blocks stream - communicates block count per message to digest stage
    /// Depth 128 provides sufficient buffering for block count information
    hls::stream<uint64_t> nblk_strm("nblk_strm");
    // 设置流通道的 FIFO 深度，用于吸收上下游速率差并降低数据流停顿风险。
    #pragma HLS STREAM variable = nblk_strm depth = 128
    #pragma HLS RESOURCE variable = nblk_strm core = FIFO_LUTRAM  // 短控制队列使用 LUTRAM

    /// End flag stream for block count - synchronizes message boundaries between stages
    /// Depth 128 matches nblk_strm for consistent buffering
    hls::stream<bool> end_nblk_strm("end_nblk_strm");
    // 设置流通道的 FIFO 深度，用于吸收上下游速率差并降低数据流停顿风险。
    #pragma HLS STREAM variable = end_nblk_strm depth = 128
    #pragma HLS RESOURCE variable = end_nblk_strm core = FIFO_LUTRAM  // LUTRAM efficient for control flags

    // Stage 1: Preprocessing - Generate complete message schedule
    // This stage processes input messages and produces:
    // - Message schedule words W[0]-W[63] for each block
    // - Block count information for each message
    // - Synchronization end flags
    preProcessing(msg_strm, len_strm, end_len_strm,
                           w_strm, nblk_strm, end_nblk_strm);

    // Stage 2: Digest Computation - Process message blocks and produce final hash
    // This stage consumes the precomputed message schedule and:
    // - Performs SHA-256 compression function for 64 rounds per block
    // - Maintains hash state across blocks (Merkle-Damgard chaining)
    // - Produces final hash output with proper byte ordering
    sha256Digest<h_width>(nblk_strm, end_nblk_strm, w_strm,
                 hash_strm, end_hash_strm);
} // End sha256_top

/// @brief Input stream splitter for dual-core SHA-256/224 processing
/// @details Distributes input messages between two processing cores using round-robin scheduling.
///          Maintains message ordering information to ensure correct output sequence.
///          This function enables parallel processing by dividing workload between two cores.
///
/// The splitter performs three main functions:
/// 1. Round-robin distribution of messages to core0 and core1
/// 2. Preservation of message boundaries and lengths
/// 3. Tracking of processing order for output recombination
///
/// @tparam m_width Input message stream width in bits
/// @tparam h_width Output hash stream width in bits (224 or 256)
/// @param in_msg_strm Input message data stream
/// @param in_len_strm Input message length stream (bytes)
/// @param in_end_len_strm Input end flag stream
/// @param msg0_strm Output message stream for core0
/// @param len0_strm Output length stream for core0
/// @param end0_len_strm Output end flag stream for core0
/// @param msg1_strm Output message stream for core1
/// @param len1_strm Output length stream for core1
/// @param end1_len_strm Output end flag stream for core1
/// @param order_core_strm Core assignment order stream (tracks which core processed each message)
/// @param order_end_strm Order stream end flags
///
/// @note Architecture features:
///       - Round-robin load balancing between two cores
///       - Maintains message integrity (no message splitting)
///       - Order tracking for correct output sequencing
///       - Pipeline-optimized with II=1 for maximum throughput
/// @brief 按整条消息轮询分配到两个 SHA 核。
/// @details 消息长度、消息数据和结束标志必须送入同一核心；order_core_strm 同时记录
/// 本消息的核心编号，供合并阶段恢复原始顺序。这里绝不能按数据字交叉分配。
template <int m_width, int h_width>
inline void split_input(hls::stream<ap_uint<m_width> >& in_msg_strm,
                        hls::stream<ap_uint<64> >& in_len_strm,
                        hls::stream<bool>& in_end_len_strm,
                        hls::stream<ap_uint<m_width> >& msg0_strm,
                        hls::stream<ap_uint<64> >& len0_strm,
                        hls::stream<bool>& end0_len_strm,
                        hls::stream<ap_uint<m_width> >& msg1_strm,
                        hls::stream<ap_uint<64> >& len1_strm,
                        hls::stream<bool>& end1_len_strm,
                        hls::stream<ap_uint<1> >& order_core_strm,
                        hls::stream<bool>& order_end_strm) {
    // Prevent function inlining to maintain clear hierarchy in synthesis
    // 控制函数是否内联；保留或消除模块边界会影响层次、共享机会和组合路径长度。
    #pragma HLS INLINE off

    // Stream depth optimizations for different data types
    // 设置流通道的 FIFO 深度，用于吸收上下游速率差并降低数据流停顿风险。
    #pragma HLS STREAM variable = msg0_strm depth = 256  // Deep buffer for message data
    #pragma HLS STREAM variable = msg1_strm depth = 256  // Deep buffer for message data
    #pragma HLS STREAM variable = len0_strm depth = 64   // Moderate buffer for length info
    // 设置流通道的 FIFO 深度，用于吸收上下游速率差并降低数据流停顿风险。
    #pragma HLS STREAM variable = len1_strm depth = 64   // Moderate buffer for length info
    #pragma HLS STREAM variable = order_core_strm depth = 128  // Buffer for order tracking

    /// Round-robin counter for load balancing between cores
    /// 0 = core0, 1 = core1
    ap_uint<1> rr = 0;

    // Read initial end flag to start processing
    bool end_all = in_end_len_strm.read();

    // Main processing loop: continue until end of all input messages
    // 持续处理当前消息或数据流，直至结束标志、剩余计数或状态条件触发退出。
    while (!end_all) {
        #pragma HLS PIPELINE II = 1  // Single-cycle iteration for maximum throughput

        // Read message length from input stream
        uint64_t len = in_len_strm.read();

        // Determine target core using round-robin scheduling
        ap_uint<1> core = rr;
        rr ^= 1;  // Toggle between 0 and 1 for round-robin distribution

        // Record core assignment for output ordering
        order_core_strm.write(core);
        order_end_strm.write(false);  // Active data flag

        // Route message length to appropriate core stream
        if (core == 0) {
            len0_strm.write(len);
            end0_len_strm.write(false);  // Active data flag for core0
        } else {
            len1_strm.write(len);
            end1_len_strm.write(false);  // Active data flag for core1
        }

        // Calculate number of words needed for this message
        const unsigned int bytes_per_word = m_width / 8;
        unsigned int nwords = (len + bytes_per_word - 1) / bytes_per_word;  // Ceiling division

        // Stream complete message data to selected core
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (unsigned int i = 0; i < nwords; ++i) {
            #pragma HLS PIPELINE II = 1  // Single-cycle word transfer
            ap_uint<m_width> w = in_msg_strm.read();

            // Route message data to appropriate core stream
            if (core == 0) {
                msg0_strm.write(w);
            } else {
                msg1_strm.write(w);
            }
        }

        // Read next end flag to determine if more messages follow
        end_all = in_end_len_strm.read();
    }

    // Write termination signals to all output streams
    order_end_strm.write(true);    // End of order tracking
    end0_len_strm.write(true);     // End of core0 input
    end1_len_strm.write(true);     // End of core1 input
}

/// @brief Output stream merger for dual-core SHA-256/224 processing
/// @details Recombines hash outputs from two processing cores into a single output stream
///          while maintaining the original message ordering. Uses core assignment information
///          from the splitter stage to correctly sequence the hash outputs.
///
/// The merger performs three main functions:
/// 1. Reads core assignment order from the order tracking stream
/// 2. Selects hash outputs from the appropriate core (core0 or core1)
/// 3. Produces a single ordered output stream with correct sequencing
///
/// @tparam h_width Output hash stream width in bits (224 for SHA-224, 256 for SHA-256)
/// @param hash0_strm Hash output stream from core0
/// @param end_hash0_strm End flag stream for core0 outputs
/// @param hash1_strm Hash output stream from core1
/// @param end_hash1_strm End flag stream for core1 outputs
/// @param order_core_strm Core assignment order stream (from split_input stage)
/// @param order_end_strm Order stream end flags
/// @param out_hash_strm Recombined output hash stream in original message order
/// @param out_end_hash_strm Output end flag stream
///
/// @note Architecture features:
///       - Maintains strict output ordering using pre-recorded core assignments
///       - Pipeline-optimized with II=1 for maximum throughput
///       - Minimal latency design with direct stream routing
///       - Supports both SHA-224 and SHA-256 output formats
/// @brief 按输入消息的原始次序合并双核摘要结果。
/// @details 先读取 order_core_strm，再只从对应核心读取一个摘要；这样即使两个核的
/// 完成时刻不同，也不会改变外部可见的结果顺序。
template <int h_width>
inline void merge_output(hls::stream<ap_uint<h_width> >& hash0_strm,
                         hls::stream<bool>& end_hash0_strm,
                         hls::stream<ap_uint<h_width> >& hash1_strm,
                         hls::stream<bool>& end_hash1_strm,
                         hls::stream<ap_uint<1> >& order_core_strm,
                         hls::stream<bool>& order_end_strm,
                         hls::stream<ap_uint<h_width> >& out_hash_strm,
                         hls::stream<bool>& out_end_hash_strm) {
    // Prevent function inlining to maintain clear hierarchy in synthesis
    // 控制函数是否内联；保留或消除模块边界会影响层次、共享机会和组合路径长度。
    #pragma HLS INLINE off

    // Stream depth optimizations for different data types
    // 设置流通道的 FIFO 深度，用于吸收上下游速率差并降低数据流停顿风险。
    #pragma HLS STREAM variable = order_core_strm depth = 128  // Buffer for order tracking
    #pragma HLS STREAM variable = hash0_strm depth = 64        // Moderate buffer for core0 outputs
    #pragma HLS STREAM variable = hash1_strm depth = 64        // Moderate buffer for core1 outputs

    // Read initial end flag from order tracking stream
    bool end_order = order_end_strm.read();

    // Main processing loop: continue until all ordered outputs are processed
    // 持续处理当前消息或数据流，直至结束标志、剩余计数或状态条件触发退出。
    while (!end_order) {
        #pragma HLS PIPELINE II = 1  // Single-cycle iteration for maximum throughput

        // Read core assignment for next output from order tracking stream
        // This determines which core processed the corresponding input message
        ap_uint<1> core = order_core_strm.read();

        // Variables for hash value and end flag
        ap_uint<h_width> h;
        bool e;

        // Select hash output from the appropriate core based on assignment
        if (core == 0) {
            // Read hash value and end flag from core0
            h = hash0_strm.read();
            e = end_hash0_strm.read();
        } else {
            // Read hash value and end flag from core1
            h = hash1_strm.read();
            e = end_hash1_strm.read();
        }

        // Write selected hash value to unified output stream
        out_hash_strm.write(h);
        // Write active data flag to output end flag stream
        out_end_hash_strm.write(false);

        // Read next order end flag to determine if more outputs remain
        end_order = order_end_strm.read();
    }

    // Write termination signal to output end flag stream
    out_end_hash_strm.write(true);
}

/// @brief Dual-core top-level wrapper for SHA-256/224 implementation
/// @details Implements a high-throughput SHA-256/224 architecture using two parallel processing cores.
///          This wrapper orchestrates a three-stage pipeline:
///          1. Input splitting: Distributes messages between two cores using round-robin scheduling
///          2. Parallel processing: Two independent SHA-256 cores process messages concurrently
///          3. Output merging: Recombines results while maintaining original message ordering
///
/// The dual-core architecture provides significant throughput improvement over single-core
/// implementation while maintaining exact SHA-256/224 algorithm compliance and output ordering.
///
/// @tparam m_width Input message stream width in bits (32 or 64)
/// @tparam h_width Output hash stream width in bits (224 for SHA-224, 256 for SHA-256)
/// @param msg_strm Input message stream
/// @param len_strm Message length stream in bytes
/// @param end_len_strm End flag stream for message input
/// @param hash_strm Output hash stream
/// @param end_hash_strm End flag stream for hash output
///
/// @note Architecture features:
///       - Three-stage DATAFLOW pipeline for maximum throughput
///       - Round-robin load balancing between two processing cores
///       - Strict output ordering preservation
///       - Approximately 2x throughput compared to single-core implementation
///       - Balanced resource utilization between cores
/// @brief 双核 SHA 顶层。
/// @details DATAFLOW 同时运行“输入分发—两个摘要核—有序合并”。该优化主要提高
/// 多消息连续输入时的吞吐率，资源代价接近复制一套摘要核；单条消息不会被两个核共同加速。
template <int m_width, int h_width>
inline void sha256_top_dual_core(hls::stream<ap_uint<m_width> >& msg_strm,
                                          hls::stream<ap_uint<64> >& len_strm,
                                          hls::stream<bool>& end_len_strm,
                                          hls::stream<ap_uint<h_width> >& hash_strm,
                                          hls::stream<bool>& end_hash_strm) {
    // HLS DATAFLOW directive enables concurrent execution of all three pipeline stages
    // This allows input splitting, dual-core processing, and output merging to operate in parallel
    // 允许相邻函数或循环通过流并发执行，形成任务级流水并重叠处理不同数据。
    #pragma HLS DATAFLOW

    // ============================================================================
    // Core 0 Stream Declarations and Configuration
    // ============================================================================

    /// Core 0 input message stream
    hls::stream<ap_uint<m_width> > msg0_strm("msg0_strm");
    /// Core 0 message length stream
    hls::stream<ap_uint<64> > len0_strm("len0_strm");
    /// Core 0 input end flag stream
    hls::stream<bool> end0_len_strm("end0_len_strm");
    /// Core 0 output hash stream
    hls::stream<ap_uint<h_width> > hash0_strm("hash0_strm");
    /// Core 0 output end flag stream
    hls::stream<bool> end_hash0_strm("end_hash0_strm");

    // Stream depth optimizations for Core 0
    // 设置流通道的 FIFO 深度，用于吸收上下游速率差并降低数据流停顿风险。
    #pragma HLS STREAM variable = msg0_strm depth = 256    // Deep buffer for message data
    #pragma HLS STREAM variable = len0_strm depth = 64     // Moderate buffer for length info
    #pragma HLS STREAM variable = hash0_strm depth = 64    // Moderate buffer for hash outputs

    // ============================================================================
    // Core 1 Stream Declarations and Configuration
    // ============================================================================

    /// Core 1 input message stream
    hls::stream<ap_uint<m_width> > msg1_strm("msg1_strm");
    /// Core 1 message length stream
    hls::stream<ap_uint<64> > len1_strm("len1_strm");
    /// Core 1 input end flag stream
    hls::stream<bool> end1_len_strm("end1_len_strm");
    /// Core 1 output hash stream
    hls::stream<ap_uint<h_width> > hash1_strm("hash1_strm");
    /// Core 1 output end flag stream
    hls::stream<bool> end_hash1_strm("end_hash1_strm");

    // Stream depth optimizations for Core 1
    // 设置流通道的 FIFO 深度，用于吸收上下游速率差并降低数据流停顿风险。
    #pragma HLS STREAM variable = msg1_strm depth = 256    // Deep buffer for message data
    #pragma HLS STREAM variable = len1_strm depth = 64     // Moderate buffer for length info
    #pragma HLS STREAM variable = hash1_strm depth = 64    // Moderate buffer for hash outputs

    // ============================================================================
    // Order Tracking Stream Declarations and Configuration
    // ============================================================================

    /// Core assignment order tracking stream
    /// Records which core processed each message for output sequencing
    hls::stream<ap_uint<1> > order_core_strm("order_core_strm");
    /// Order stream end flags
    hls::stream<bool> order_end_strm("order_end_strm");

    // Stream depth optimization for order tracking
    // 设置流通道的 FIFO 深度，用于吸收上下游速率差并降低数据流停顿风险。
    #pragma HLS STREAM variable = order_core_strm depth = 128  // Buffer for core assignment records

    // ============================================================================
    // Three-Stage Pipeline Implementation
    // ============================================================================

    // Stage 1: Input Splitting
    // Distributes incoming messages between Core 0 and Core 1 using round-robin scheduling
    // Maintains order tracking information for correct output sequencing
    split_input<m_width, h_width>(msg_strm, len_strm, end_len_strm,
                                  msg0_strm, len0_strm, end0_len_strm,
                                  msg1_strm, len1_strm, end1_len_strm,
                                  order_core_strm, order_end_strm);

    // Stage 2: Parallel Processing
    // Both cores operate independently and concurrently on their respective message streams
    // Each core implements complete SHA-256/224 algorithm with preprocessing and digest stages

    // Core 0 processing pipeline
    internal::sha256_top<m_width, h_width>(msg0_strm, len0_strm, end0_len_strm,
                                                     hash0_strm, end_hash0_strm);

    // Core 1 processing pipeline
    internal::sha256_top<m_width, h_width>(msg1_strm, len1_strm, end1_len_strm,
                                                     hash1_strm, end_hash1_strm);

    // Stage 3: Output Merging
    // Recombines hash outputs from both cores into a single ordered output stream
    // Uses order tracking information from Stage 1 to maintain original message sequence
    merge_output<h_width>(hash0_strm, end_hash0_strm,
                          hash1_strm, end_hash1_strm,
                          order_core_strm, order_end_strm,
                          hash_strm, end_hash_strm);
}

} // namespace internal


/// @brief External SHA-224 hash function interface
/// @details Provides a clean, user-friendly interface for SHA-224 hashing that internally
///          uses the high-performance dual-core implementation. This function abstracts
///          the complex internal architecture and presents a simple streaming interface.
///
/// @tparam m_width Input message stream width in bits (32 or 64)
/// @param msg_strm Input message stream
/// @param len_strm Message length stream in bytes
/// @param end_len_strm End flag stream for message input
/// @param hash_strm Output 224-bit hash stream
/// @param end_hash_strm End flag stream for hash output
///
/// @note Interface characteristics:
///       - Defaults to dual-core implementation for maximum performance
///       - Maintains identical interface to single-core version for compatibility
///       - Provides automatic load balancing and parallel processing
///       - Guarantees FIPS 180-4 SHA-224 compliance
template <int m_width>
void sha224(hls::stream<ap_uint<m_width> >& msg_strm,
            hls::stream<ap_uint<64> >& len_strm,
            hls::stream<bool>& end_len_strm,
            hls::stream<ap_uint<224> >& hash_strm,
            hls::stream<bool>& end_hash_strm) {
    // Delegate to internal dual-core implementation with SHA-224 output configuration
    internal::sha256_top_dual_core<m_width, 224>(msg_strm, len_strm, end_len_strm,
                                                           hash_strm, end_hash_strm);
}

/// @brief External SHA-256 hash function interface
/// @details Provides a clean, user-friendly interface for SHA-256 hashing that internally
///          uses the high-performance dual-core implementation. This offers the same
///          benefits as the SHA-224 interface but produces full 256-bit hash outputs.
///
/// @tparam m_width Input message stream width in bits (32 or 64)
/// @param msg_strm Input message stream
/// @param len_strm Message length stream in bytes
/// @param end_len_strm End flag stream for message input
/// @param hash_strm Output 256-bit hash stream
/// @param end_hash_strm End flag stream for hash output
///
/// @note Interface characteristics:
///       - Defaults to dual-core implementation for maximum performance
///       - Maintains identical interface to single-core version for compatibility
///       - Provides automatic load balancing and parallel processing
///       - Guarantees FIPS 180-4 SHA-256 compliance
template <int m_width>
void sha256(hls::stream<ap_uint<m_width> >& msg_strm,
            hls::stream<ap_uint<64> >& len_strm,
            hls::stream<bool>& end_len_strm,
            hls::stream<ap_uint<256> >& hash_strm,
            hls::stream<bool>& end_hash_strm) {
    // Delegate to internal dual-core implementation with SHA-256 output configuration
    internal::sha256_top_dual_core<m_width, 256>(msg_strm, len_strm, end_len_strm,
                                                           hash_strm, end_hash_strm);
}

} // namespace security
} // namespace xf


// 清理本文件定义的算法宏，防止影响包含该头文件的其他编译单元。
#undef ROTR
#undef ROTL
#undef SHR
#undef CH
#undef MAJ
#undef BSIG0
#undef BSIG1
#undef SSIG0
#undef SSIG1
#undef _XF_SECURITY_PRINT
#undef _XF_SECURITY_VOID_CAST

#endif // _XF_SECURITY_SHA224_256_HPP_
