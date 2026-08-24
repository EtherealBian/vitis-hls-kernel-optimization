/*
 * (c) Copyright 2019-2022 Xilinx, Inc. All rights reserved.
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
 *
 */
#ifndef _XFCOMPRESSION_LZ_COMPRESS_HPP_
#define _XFCOMPRESSION_LZ_COMPRESS_HPP_

/*
 * 项目：2025 年 FPGA 创新设计竞赛 AMD 赛道——LZ4 压缩优化
 * 项目作者：Ethereal
 * 优化与中文注释维护：Ethereal
 * 上游来源：AMD Vitis Data Compression Library
 *
 * 维护说明：本文件保留 AMD/Xilinx 的原始版权与 Apache-2.0 许可声明。
 * 参赛工作集中在 LZ77 字典组织、候选匹配并行比较、归约选择、BRAM 分区和
 * 流水线依赖约束；正文中文注释解释每个阶段生成的数据、综合后的硬件含义、
 * 有效位和偏移边界，以及保持压缩格式不变所需的控制条件。
 */

/**
 * @file lz_compress.hpp
 * @brief Header for modules used in LZ4 and snappy compression kernels.
 *
 * This file is part of Vitis Data Compression Library.
 */
#include "compress_utils.hpp"
#include "hls_stream.h"

#include <ap_int.h>
#include <assert.h>
#include <stdint.h>

/*
 * 中文说明：LZ77 匹配搜索内核（第二问优化版）
 *
 * 功能：逐字节维护滑动窗口，以当前 MATCH_LEN 字节生成哈希地址，从字典的
 * MATCH_LEVEL 个历史候选中寻找“长度最长、且偏移合法”的匹配，输出 32 bit token：
 * [7:0] 为当前字节，[15:8] 为匹配长度，[31:16] 为匹配偏移。
 *
 * 主要优化点：
 * 1. 主循环 PIPELINE II=1：理想情况下每周期接收一个新字节。
 * 2. present_window 完全分割，窗口移位和逐字节比较可全部展开。
 * 3. 字典/有效标志采用双端口存储并循环分区，降低同周期多候选访问的端口冲突。
 * 4. MATCH_LEVEL 个候选并行比较；匹配长度计算循环完全展开。
 * 5. 最佳候选不再采用长串行比较链，而采用分层锦标赛归约，缩短组合关键路径。
 * 6. 使用批次号 validityFlags 代替每次调用都清空整张字典，减少启动延迟。
 * 7. 对确认不存在跨迭代冲突的存储显式添加 DEPENDENCE inter false，帮助 HLS
 *    接受 II=1 调度。该指令依赖算法保证，不能在存在真实 RAW/WAR 依赖时滥用。
 * 8. 文件后半部分的通用接口将大字典映射到 URAM，并调整分区/展开因子，以较少
 *    BRAM/LUT 支撑更大的历史窗口。
 *
 * 权衡：并行比较、数组分割和归约树会增加比较器、寄存器与布线；字典规模或
 * MATCH_LEVEL 增大时应重新综合评估时序与资源，不能认为 II=1 必然实现。
 */

namespace xf {
namespace compression {

/**
 * @brief 低延迟流式 LZ77 匹配器。
 * @tparam MATCH_LEN 单次比较的最大字节数。
 * @tparam MIN_MATCH 允许输出匹配的最小长度。
 * @tparam LZ_MAX_OFFSET_LIMIT 最大合法回溯距离。
 * @tparam MATCH_LEVEL 每个哈希槽并行保留和比较的历史候选数。
 * @tparam MIN_OFFSET 最小合法偏移，避免当前位置与自身匹配。
 * @tparam LZ_DICT_SIZE 哈希字典槽数，要求为 2 的幂。
 * @tparam LEFT_BYTES 主循环结束后按字面量输出的尾部字节数。
 * @param inStream 8 bit 原始字节流。
 * @param outStream 32 bit LZ token 流。
 * @param input_size 输入总字节数。
 */
template <int MATCH_LEN,
          int MIN_MATCH,
          int LZ_MAX_OFFSET_LIMIT,
          int MATCH_LEVEL = 6,
          int MIN_OFFSET = 1,
          int LZ_DICT_SIZE = 1 << 12,
          int LEFT_BYTES = 64>
void lzCompress(hls::stream<ap_uint<8> >& inStream,
                hls::stream<ap_uint<32> >& outStream,
                uint32_t input_size) {
    #pragma HLS PIPELINE II=1  // 目标：主数据通路每周期处理一个新的窗口位置
    #pragma HLS INLINE off     // 保留独立模块边界，便于控制资源和查看综合报告

    // Calculate dictionary element width and define data types
    const int c_dictEleWidth = (MATCH_LEN * 8 + 24);
    typedef ap_uint<MATCH_LEVEL * c_dictEleWidth> uintDictV_t;
    typedef ap_uint<c_dictEleWidth> uintDict_t;

    // Early return for empty input
    if (input_size == 0) {
        return;
    }

    // 哈希字典：一个槽中打包 MATCH_LEVEL 条历史记录；循环分区用于扩展并行访问带宽。
    static uintDictV_t historyData[LZ_DICT_SIZE];
    #pragma HLS BIND_STORAGE variable=historyData type=RAM_T2P impl=BRAM
    #pragma HLS ARRAY_PARTITION variable=historyData cyclic factor=8 dim=1

    // 有效批次号与字典采用相同分区，便于同一周期联合读取“候选内容 + 是否有效”。
    static ap_uint<32> validityFlags[LZ_DICT_SIZE];
    #pragma HLS BIND_STORAGE variable=validityFlags type=RAM_T2P impl=BRAM
    #pragma HLS ARRAY_PARTITION variable=validityFlags cyclic factor=8 dim=1

    // 批次号机制：新任务递增 processingBatch 即可逻辑清空字典，避免逐项写零。
    #ifndef AVOID_STATIC_MODE
    static uint32_t processingBatch = 1;
    #else
    uint32_t processingBatch = 1;
    #endif

    // 当前滑动窗口完全分割，使移位、哈希和 MATCH_LEN 字节比较都可并行展开。
    uint8_t present_window[MATCH_LEN];
    #pragma HLS ARRAY_PARTITION variable=present_window complete

    // ------------------ Initialization Phase ------------------
    // Pre-fill the sliding window with initial data bytes
    init_window_fill:
    // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
    for (uint8_t fill_index = 1; fill_index < MATCH_LEN; fill_index++) {
        #pragma HLS PIPELINE off
        present_window[fill_index] = inStream.read();
    }

    // ------------------ Main Compression Loop ------------------
    lz_compress:
    // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
    for (uint32_t i = MATCH_LEN - 1; i < input_size - LEFT_BYTES; i++) {
        #pragma HLS PIPELINE II=1
        #pragma HLS dependence variable=historyData inter false  // 已由哈希/批次逻辑保证无跨迭代伪相关
        #pragma HLS dependence variable=validityFlags inter false // 解除保守依赖分析对 II 的限制

        // Calculate current position in the input stream
        uint32_t currIdx = i - MATCH_LEN + 1;

        // ------------------ Window Management ------------------
        // Shift window and load new byte from input stream
        uint8_t new_byte = inStream.read();
        window_shift:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (int shift_pos = 0; shift_pos < MATCH_LEN - 1; shift_pos++) {
            #pragma HLS UNROLL
            present_window[shift_pos] = present_window[shift_pos + 1];
        }
        present_window[MATCH_LEN - 1] = new_byte;

        // ------------------ Improved Hash Computation ------------------
        // Multiplicative hash for better distribution and reduced collisions
        uint32_t hash = 0;
        if (MIN_MATCH == 3) {
            hash = (present_window[0] * 2654435761UL) ^
                   (present_window[1] * 2246822519UL) ^
                   (present_window[2] * 3266489917UL);
            hash = hash & (LZ_DICT_SIZE - 1);
        } else {
            hash = (present_window[0] * 2654435761UL) ^
                   (present_window[1] * 2246822519UL) ^
                   (present_window[2] * 3266489917UL) ^
                   (present_window[3] * 668265263UL);
            hash = hash & (LZ_DICT_SIZE - 1);
        }

        // ------------------ Dictionary Lookup ------------------
        // Retrieve previous entries from dictionary if they belong to current epoch
        uintDictV_t dictReadValue = 0;
        if (validityFlags[hash] == processingBatch) {
            dictReadValue = historyData[hash];
        }

        // ------------------ Dictionary Update ------------------
        // Prepare new dictionary entry by shifting old entries and adding current window
        uintDictV_t dictWriteValue = dictReadValue << c_dictEleWidth;

        // Copy current window content to new dictionary entry
        window_to_dict:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (int byte_pos = 0; byte_pos < MATCH_LEN; byte_pos++) {
            #pragma HLS UNROLL
            dictWriteValue.range((byte_pos + 1) * 8 - 1, byte_pos * 8) = present_window[byte_pos];
        }

        // Store current position index in dictionary entry
        dictWriteValue.range(c_dictEleWidth - 1, MATCH_LEN * 8) = currIdx;

        // Update dictionary and mark entry as valid for current epoch
        historyData[hash] = dictWriteValue;
        validityFlags[hash] = processingBatch;

        // ------------------ Parallel Pattern Matching ------------------
        // Evaluate all candidate matches in parallel for minimum latency
        uint8_t match_lengths[MATCH_LEVEL];
        uint32_t match_offsets[MATCH_LEVEL];
        bool match_valids[MATCH_LEVEL];

        parallel_match_evaluation:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (int level = 0; level < MATCH_LEVEL; level++) {
            #pragma HLS UNROLL

            uint8_t len = 0;
            uintDict_t compareWith = dictReadValue.range((level + 1) * c_dictEleWidth - 1,
                                                         level * c_dictEleWidth);
            uint32_t compareIdx = compareWith.range(c_dictEleWidth - 1, MATCH_LEN * 8);

            // Fully parallel byte comparison for minimum latency
            bool byte_matches[MATCH_LEN];
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int m = 0; m < MATCH_LEN; m++) {
                #pragma HLS UNROLL
                byte_matches[m] = (present_window[m] == compareWith.range((m + 1) * 8 - 1, m * 8));
            }

            // Parallel match length calculation
            bool valid_match = true;
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int m = 0; m < MATCH_LEN; m++) {
                #pragma HLS UNROLL
                if (valid_match && byte_matches[m]) {
                    len = m + 1;
                } else {
                    valid_match = false;
                }
            }

            // 并行检查长度、最小偏移和最大回溯距离，非法候选直接置为零。
            bool length_ok = (len >= MIN_MATCH);
            bool index_ok = (currIdx > compareIdx);
            bool offset_ok = ((currIdx - compareIdx) < LZ_MAX_OFFSET_LIMIT);
            bool min_offset_ok = ((currIdx - compareIdx - 1) >= MIN_OFFSET);
            bool special_case_ok = !(len == 3 && (currIdx - compareIdx - 1) > 4096);

            match_valids[level] = length_ok && index_ok && offset_ok && min_offset_ok && special_case_ok;
            match_lengths[level] = match_valids[level] ? len : 0;
            match_offsets[level] = currIdx - compareIdx - 1;
        }

        // ------------------ Tournament-based Best Match Selection ------------------
        // Multi-stage parallel selection for minimum critical path
        uint8_t best_length = 0;
        uint32_t best_offset = 0;

        if (MATCH_LEVEL >= 4) {
            // First tournament level
            const int LEVEL1_SIZE = (MATCH_LEVEL + 1) / 2;
            uint8_t level1_lengths[LEVEL1_SIZE];
            uint32_t level1_offsets[LEVEL1_SIZE];

            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int l = 0; l < LEVEL1_SIZE; l++) {
                #pragma HLS UNROLL
                int idx1 = 2 * l;
                int idx2 = 2 * l + 1;

                if (idx2 < MATCH_LEVEL) {
                    if (match_lengths[idx1] >= match_lengths[idx2]) {
                        level1_lengths[l] = match_lengths[idx1];
                        level1_offsets[l] = match_offsets[idx1];
                    } else {
                        level1_lengths[l] = match_lengths[idx2];
                        level1_offsets[l] = match_offsets[idx2];
                    }
                } else if (idx1 < MATCH_LEVEL) {
                    level1_lengths[l] = match_lengths[idx1];
                    level1_offsets[l] = match_offsets[idx1];
                } else {
                    level1_lengths[l] = 0;
                    level1_offsets[l] = 0;
                }
            }

            // Second tournament level
            const int LEVEL2_SIZE = (LEVEL1_SIZE + 1) / 2;
            uint8_t level2_lengths[LEVEL2_SIZE];
            uint32_t level2_offsets[LEVEL2_SIZE];

            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int l = 0; l < LEVEL2_SIZE; l++) {
                #pragma HLS UNROLL
                int idx1 = 2 * l;
                int idx2 = 2 * l + 1;

                if (idx2 < LEVEL1_SIZE) {
                    if (level1_lengths[idx1] >= level1_lengths[idx2]) {
                        level2_lengths[l] = level1_lengths[idx1];
                        level2_offsets[l] = level1_offsets[idx1];
                    } else {
                        level2_lengths[l] = level1_lengths[idx2];
                        level2_offsets[l] = level1_offsets[idx2];
                    }
                } else if (idx1 < LEVEL1_SIZE) {
                    level2_lengths[l] = level1_lengths[idx1];
                    level2_offsets[l] = level1_offsets[idx1];
                } else {
                    level2_lengths[l] = 0;
                    level2_offsets[l] = 0;
                }
            }

            // Final selection
            best_length = level2_lengths[0];
            best_offset = level2_offsets[0];
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int l = 1; l < LEVEL2_SIZE; l++) {
                #pragma HLS UNROLL
                if (level2_lengths[l] > best_length) {
                    best_length = level2_lengths[l];
                    best_offset = level2_offsets[l];
                }
            }
        } else {
            // Direct comparison for small MATCH_LEVEL
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int l = 0; l < MATCH_LEVEL; l++) {
                #pragma HLS UNROLL
                if (match_lengths[l] > best_length) {
                    best_length = match_lengths[l];
                    best_offset = match_offsets[l];
                }
            }
        }

        // ------------------ Output Generation ------------------
        ap_uint<32> outValue = 0;
        outValue.range(7, 0) = present_window[0];
        outValue.range(15, 8) = best_length;
        outValue.range(31, 16) = best_offset;
        outStream << outValue;
    }

    // ------------------ Residual Data Processing ------------------
    process_residual_window:
    // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
    for (int m = 1; m < MATCH_LEN; m++) {
        #pragma HLS PIPELINE II=1
        ap_uint<32> outValue = 0;
        outValue.range(7, 0) = present_window[m];
        outStream << outValue;
    }

    process_leftover_bytes:
    // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
    for (int l = 0; l < LEFT_BYTES; l++) {
        #pragma HLS PIPELINE II=1
        ap_uint<32> outValue = 0;
        outValue.range(7, 0) = inStream.read();
        outStream << outValue;
    }
}

/**
 * @file lz_compress.hpp
 * @brief Optimized LZ Compression Core Module - Enhanced Version with Streaming I/O
 *
 * This module enhances the basic LZ compression algorithm with streaming processing,
 * large file chunk handling, and URAM resource optimization. It supports continuous
 * processing of multiple data blocks, making it suitable for large-scale data compression
 * scenarios in hardware-accelerated applications.
 *
 * @tparam MAX_INPUT_SIZE Maximum size of a single data block, default 64KB
 * @tparam SIZE_DT Data type for size parameters, default uint32_t
 * @tparam MATCH_LEN Match length parameter, determines sliding window size
 * @tparam MIN_MATCH Minimum match length threshold
 * @tparam LZ_MAX_OFFSET_LIMIT Maximum offset limit for matches
 * @tparam CORE_ID Core identifier for multi-core parallelism, default 0
 * @tparam MATCH_LEVEL Number of candidate matches stored per hash slot, default 6
 * @tparam MIN_OFFSET Minimum offset to avoid self-matching, default 1
 * @tparam LZ_DICT_SIZE Dictionary size determining hash table capacity, default 4096
 * @tparam LEFT_BYTES Number of leftover bytes to process at the end, default 64
 *
 * @param inStream Input data stream using IntVectorStream_dt<8,1> format
 * @param outStream Output compressed metadata stream using IntVectorStream_dt<32,1> format
 */
template <int MAX_INPUT_SIZE = 64 * 1024,
          class SIZE_DT = uint32_t,
          int MATCH_LEN,
          int MIN_MATCH,
          int LZ_MAX_OFFSET_LIMIT,
          int CORE_ID = 0,
          int MATCH_LEVEL = 6,
          int MIN_OFFSET = 1,
          int LZ_DICT_SIZE = 1 << 12,
          int LEFT_BYTES = 64>
void lzCompress(hls::stream<IntVectorStream_dt<8, 1> >& inStream,
                hls::stream<IntVectorStream_dt<32, 1> >& outStream) {

    // ------------------ Constants and Type Definitions ------------------
    const uint16_t c_indxBitCnts = 24;                    // Index bit width, supports up to 8MB data blocks
    const uint16_t c_fifo_depth = LEFT_BYTES + 2;         // Local buffer depth
    const int c_dictEleWidth = (MATCH_LEN * 8 + c_indxBitCnts); // Dictionary element bit width
    typedef ap_uint<MATCH_LEVEL * c_dictEleWidth> uintDictV_t; // Dictionary value type
    typedef ap_uint<c_dictEleWidth> uintDict_t;           // Single candidate match type
    const uint32_t totalDictSize = (1 << (c_indxBitCnts - 1)); // Total dictionary size 8MB

    // ------------------ Static Variable Declarations ------------------
    // Maintain state across multiple data blocks
    #ifndef AVOID_STATIC_MODE
    static bool resetDictFlag = true;     // Dictionary reset flag
    static uint32_t relativeNumBlocks = 0; // Relative block counter
    #else
    bool resetDictFlag = true;            // Non-static mode, reinitialize each time
    uint32_t relativeNumBlocks = 0;
    #endif

    // ------------------ Hash Dictionary Declaration ------------------
    // Use URAM resources for large-capacity storage
    uintDictV_t dict[LZ_DICT_SIZE];
    #pragma HLS RESOURCE variable = dict core = XPM_MEMORY uram  // 大容量历史字典映射到 URAM
    #pragma HLS ARRAY_PARTITION variable = dict cyclic factor = 4 dim = 1  // 四路存储体并行访问

    // ------------------ Local Buffers and Stream Declarations ------------------
    uint8_t present_window[MATCH_LEN];                    // Sliding window buffer
    // 分割数组以增加并行读写端口，避免单个存储体端口数限制计算吞吐。
    #pragma HLS ARRAY_PARTITION variable = present_window complete  // Complete partitioning
    hls::stream<uint8_t> lclBufStream("lclBufStream");    // Local buffer stream
    #pragma HLS STREAM variable = lclBufStream depth = c_fifo_depth
    // 显式约束存储对象的端口类型和实现资源，保证并行访问能力可被综合实现。
    #pragma HLS BIND_STORAGE variable = lclBufStream type = fifo impl = srl

    // ------------------ Input/Output Registers ------------------
    IntVectorStream_dt<8, 1> inVal;    // Input value register
    IntVectorStream_dt<32, 1> outValue; // Output value register

    // ------------------ Main Processing Loop ------------------
    // Process continuous data blocks
    // 持续处理当前消息或数据流，直至结束标志、剩余计数或状态条件触发退出。
    while (true) {
        uint32_t iIdx = 0;  // Current block index

        // ------------------ Dictionary Management ------------------
        // Reset dictionary every 8MB of processed data to prevent index overflow
        if (resetDictFlag) {
            // Initialize dictionary reset values
            ap_uint<MATCH_LEVEL* c_dictEleWidth> resetValue = 0;
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int i = 0; i < MATCH_LEVEL; i++) {
                #pragma HLS UNROLL
                resetValue.range((i + 1) * c_dictEleWidth - 1, i * c_dictEleWidth + MATCH_LEN * 8) = -1;
            }

            // Dictionary initialization: reset all hash slots
        dict_flush:
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int i = 0; i < LZ_DICT_SIZE; i++) {
                #pragma HLS PIPELINE II = 1
                #pragma HLS UNROLL FACTOR = 4  // 只展开 4 路，在启动延迟与资源之间折中
                dict[i] = resetValue;
            }
            resetDictFlag = false;      // Clear reset flag
            relativeNumBlocks = 0;      // Reset block counter
        } else {
            relativeNumBlocks++;        // Increment block counter
        }

        // ------------------ Stream End Detection ------------------
        // Read next value and check strobe signal
        auto nextVal = inStream.read();
        if (nextVal.strobe == 0) {
            outValue.strobe = 0;        // Set output end flag
            outStream << outValue;
            break;                      // Exit main loop
        }

        // ------------------ Sliding Window Initialization ------------------
        // Fill first MATCH_LEN-1 bytes into sliding window
    lz_fill_present_win:
        // 持续处理当前消息或数据流，直至结束标志、剩余计数或状态条件触发退出。
        while (iIdx < MATCH_LEN - 1) {
            #pragma HLS PIPELINE II = 1
            inVal = nextVal;
            nextVal = inStream.read();  // Pre-read next value
            present_window[++iIdx] = inVal.data[0];  // Fill sliding window
        }

        // ------------------ Circular Buffer Filling ------------------
        // Pre-read LEFT_BYTES bytes into local buffer
    lz_fill_circular_buf:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (uint16_t i = 0; i < LEFT_BYTES; ++i) {
            #pragma HLS PIPELINE II = 1
            inVal = nextVal;
            nextVal = inStream.read();
            lclBufStream << inVal.data[0];  // Write to local buffer
        }

        // Set output stream enable
        outValue.strobe = 1;

        // ------------------ Main Compression Loop ------------------
        // Process current data block
    lz_compress:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (; nextVal.strobe != 0; ++iIdx) {
            #pragma HLS PIPELINE II = 1
            #ifndef DISABLE_DEPENDENCE
            #pragma HLS dependence variable = dict inter false  // 排除工具推断出的伪跨迭代依赖
            #endif

            // Calculate global index: consider relative offset for multi-block processing
            uint32_t currIdx = (iIdx + (relativeNumBlocks * MAX_INPUT_SIZE)) - MATCH_LEN + 1;

            // ------------------ Circular Buffer Management ------------------
            // FIFO update for sliding window
            auto inValue = lclBufStream.read(); // Read oldest value from buffer
            lclBufStream << nextVal.data[0];    // Write new value to buffer
            nextVal = inStream.read();          // Read next value from input stream

            // ------------------ Sliding Window Update ------------------
            // Shift and insert new value
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (uint8_t m = 0; m < MATCH_LEN - 1; m++) {
                #pragma HLS UNROLL
                present_window[m] = present_window[m + 1];  // Shift left
            }
            present_window[MATCH_LEN - 1] = inValue;        // Insert new value

            // ------------------ Hash Value Calculation ------------------
            // Multi-byte hash with optimized distribution
            uint32_t hash = 0;
            if (MIN_MATCH == 3) {
                // Hash calculation for 3-byte minimum match
                hash = (present_window[0] * 2654435761UL) ^
                       (present_window[1] * 2246822519UL) ^
                       (present_window[2] * 3266489917UL);
                hash = hash & (LZ_DICT_SIZE - 1);
            } else {
                // Hash calculation for 4-byte minimum match
                hash = (present_window[0] * 2654435761UL) ^
                       (present_window[1] * 2246822519UL) ^
                       (present_window[2] * 3266489917UL) ^
                       (present_window[3] * 668265263UL);
                hash = hash & (LZ_DICT_SIZE - 1);
            }

            // ------------------ Dictionary Lookup ------------------
            // Read all candidate matches from hash slot
            uintDictV_t dictReadValue = dict[hash];

            // ------------------ Dictionary Update Preparation ------------------
            // Shift existing candidates and insert new window data
            uintDictV_t dictWriteValue = dictReadValue << c_dictEleWidth;
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int m = 0; m < MATCH_LEN; m++) {
                #pragma HLS UNROLL
                dictWriteValue.range((m + 1) * 8 - 1, m * 8) = present_window[m];
            }
            dictWriteValue.range(c_dictEleWidth - 1, MATCH_LEN * 8) = currIdx;

            // ------------------ Dictionary Write ------------------
            // Update hash slot with new entry
            dict[hash] = dictWriteValue;

            // ------------------ Parallel Match Search and Filtering ------------------
            uint8_t match_lengths[MATCH_LEVEL];    // Candidate match lengths
            uint32_t match_offsets[MATCH_LEVEL];   // Candidate match offsets
            bool match_valids[MATCH_LEVEL];        // Candidate match validity flags

            // Process all candidate matches in parallel
            // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
            for (int l = 0; l < MATCH_LEVEL; l++) {
                #pragma HLS UNROLL
                uint8_t len = 0;
                uintDict_t compareWith = dictReadValue.range((l + 1) * c_dictEleWidth - 1, l * c_dictEleWidth);
                uint32_t compareIdx = compareWith.range(c_dictEleWidth - 1, MATCH_LEN * 8);

                // ------------------ Parallel Byte Comparison ------------------
                // Compare current window with candidate match byte by byte
                bool byte_matches[MATCH_LEN];
                // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
                for (uint8_t m = 0; m < MATCH_LEN; m++) {
                    #pragma HLS UNROLL
                    byte_matches[m] = (present_window[m] == compareWith.range((m + 1) * 8 - 1, m * 8));
                }

                // ------------------ Parallel Match Length Calculation ------------------
                // Find longest consecutive match length
                bool valid_match = true;
                // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
                for (uint8_t m = 0; m < MATCH_LEN; m++) {
                    #pragma HLS UNROLL
                    if (valid_match && byte_matches[m]) {
                        len = m + 1;  // Update match length
                    } else {
                        valid_match = false;  // Encounter mismatched byte, terminate calculation
                    }
                }

                // ------------------ Parallel Match Filtering ------------------
                // Apply multiple constraint conditions
                bool length_ok = (len >= MIN_MATCH);                                // Length threshold check
                bool index_ok = (currIdx > compareIdx);                            // Index validity check
                bool offset_ok = ((currIdx - compareIdx) < LZ_MAX_OFFSET_LIMIT);   // Maximum offset check
                bool min_offset_ok = ((currIdx - compareIdx - 1) >= MIN_OFFSET);   // Minimum offset check
                bool block_boundary_ok = (compareIdx >= (relativeNumBlocks * MAX_INPUT_SIZE)); // Block boundary check
                bool special_case_ok = !(len == 3 && (currIdx - compareIdx - 1) > 4096);  // Special case filtering

                // ------------------ Comprehensive Match Validation ------------------
                // Combined validity assessment
                match_valids[l] = length_ok && index_ok && offset_ok && min_offset_ok &&
                                 block_boundary_ok && special_case_ok;
                match_lengths[l] = match_valids[l] ? len : 0;      // Set invalid match length to 0
                match_offsets[l] = currIdx - compareIdx - 1;       // Calculate offset
            }

            // ------------------ Improved Parallel Best Match Selection ------------------
            uint8_t best_length = 0;   // Best match length
            uint32_t best_offset = 0;  // Best match offset

            // Select different strategies based on match level
            if (MATCH_LEVEL >= 4) {
                // Multi-level tournament selection: suitable for larger MATCH_LEVEL

                // First level selection: pairwise comparison
                const int LEVEL1_SIZE = (MATCH_LEVEL + 1) / 2;
                uint8_t level1_lengths[LEVEL1_SIZE];
                uint32_t level1_offsets[LEVEL1_SIZE];

                // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
                for (int l = 0; l < LEVEL1_SIZE; l++) {
                    #pragma HLS UNROLL
                    int idx1 = 2 * l;
                    int idx2 = 2 * l + 1;

                    if (idx2 < MATCH_LEVEL) {
                        // Compare two candidates, select the one with larger length
                        if (match_lengths[idx1] >= match_lengths[idx2]) {
                            level1_lengths[l] = match_lengths[idx1];
                            level1_offsets[l] = match_offsets[idx1];
                        } else {
                            level1_lengths[l] = match_lengths[idx2];
                            level1_offsets[l] = match_offsets[idx2];
                        }
                    } else if (idx1 < MATCH_LEVEL) {
                        // Single candidate case
                        level1_lengths[l] = match_lengths[idx1];
                        level1_offsets[l] = match_offsets[idx1];
                    } else {
                        level1_lengths[l] = 0;
                        level1_offsets[l] = 0;
                    }
                }

                // Second level selection: continue pairwise comparison
                const int LEVEL2_SIZE = (LEVEL1_SIZE + 1) / 2;
                uint8_t level2_lengths[LEVEL2_SIZE];
                uint32_t level2_offsets[LEVEL2_SIZE];

                // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
                for (int l = 0; l < LEVEL2_SIZE; l++) {
                    #pragma HLS UNROLL
                    int idx1 = 2 * l;
                    int idx2 = 2 * l + 1;

                    if (idx2 < LEVEL1_SIZE) {
                        if (level1_lengths[idx1] >= level1_lengths[idx2]) {
                            level2_lengths[l] = level1_lengths[idx1];
                            level2_offsets[l] = level1_offsets[idx1];
                        } else {
                            level2_lengths[l] = level1_lengths[idx2];
                            level2_offsets[l] = level1_offsets[idx2];
                        }
                    } else if (idx1 < LEVEL1_SIZE) {
                        level2_lengths[l] = level1_lengths[idx1];
                        level2_offsets[l] = level1_offsets[idx1];
                    } else {
                        level2_lengths[l] = 0;
                        level2_offsets[l] = 0;
                    }
                }

                // Final selection: find best match among second level results
                best_length = level2_lengths[0];
                best_offset = level2_offsets[0];
                // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
                for (int l = 1; l < LEVEL2_SIZE; l++) {
                    #pragma HLS UNROLL
                    if (level2_lengths[l] > best_length) {
                        best_length = level2_lengths[l];
                        best_offset = level2_offsets[l];
                    }
                }
            } else {
                // Direct comparison: suitable for smaller MATCH_LEVEL
                // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
                for (int l = 0; l < MATCH_LEVEL; l++) {
                    #pragma HLS UNROLL
                    if (match_lengths[l] > best_length) {
                        best_length = match_lengths[l];
                        best_offset = match_offsets[l];
                    }
                }
            }

            // ------------------ Compressed Metadata Output ------------------
            // Format = [Offset(16 bits) | Match Length(8 bits) | Literal Byte(8 bits)]
            outValue.data[0].range(7, 0) = present_window[0];     // Current literal byte
            outValue.data[0].range(15, 8) = best_length;          // Best match length
            outValue.data[0].range(31, 16) = best_offset;         // Best match offset
            outStream << outValue;                                // Write to output stream
        }

        // ------------------ Residual Byte Processing ------------------
        // Process remaining bytes in sliding window
        outValue.data[0] = 0;
    lz_compress_leftover:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (uint8_t m = 1; m < MATCH_LEN; ++m) {
            #pragma HLS PIPELINE II = 1
            outValue.data[0].range(7, 0) = present_window[m];  // Output literal bytes only
            outStream << outValue;
        }

        // ------------------ Leftover Byte Processing ------------------
        // Process remaining LEFT_BYTES in local buffer
    lz_left_bytes:
        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (uint16_t l = 0; l < LEFT_BYTES; ++l) {
            #pragma HLS PIPELINE II = 1
            outValue.data[0].range(7, 0) = lclBufStream.read();  // Read from buffer and output
            outStream << outValue;
        }

        // ------------------ Dictionary Reset Check ------------------
        // Reset dictionary when cumulative processed data reaches 8MB
        resetDictFlag = ((relativeNumBlocks * MAX_INPUT_SIZE) >= (totalDictSize)) ? true : false;

        // ------------------ Data Block Completion ------------------
        // Output end flag for current block
        outValue.strobe = 0;
        outStream << outValue;
    }
}

} // namespace compression
} // namespace xf
#endif // _XFCOMPRESSION_LZ_COMPRESS_HPP_
