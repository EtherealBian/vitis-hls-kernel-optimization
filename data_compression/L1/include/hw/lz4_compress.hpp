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
#ifndef _XFCOMPRESSION_LZ4_COMPRESS_HPP_
#define _XFCOMPRESSION_LZ4_COMPRESS_HPP_

/*
 * 项目：2025 年 FPGA 创新设计竞赛 AMD 赛道——LZ4 编码与数据流优化
 * 项目作者：Ethereal
 * 优化与中文注释维护：Ethereal
 * 上游来源：AMD Vitis Data Compression Library
 *
 * 维护说明：本文件保留 AMD/Xilinx 的原始版权与 Apache-2.0 许可声明。
 * 参赛工作优化 Token/长度/偏移编码状态机、阶段间 FIFO 和任务级 DATAFLOW；
 * 正文中文注释描述 LZ4 序列格式、各流的位域协议、状态迁移条件、外存搬运边界
 * 及 HLS pragma 对吞吐率和资源映射的影响。
 */

/**
 * @file lz4_compress.hpp
 * @brief Header for modules used in LZ4 compression kernel.
 *
 * This file is part of Vitis Data Compression Library.
 */

#include "hls_stream.h"
#include <ap_int.h>
#include <assert.h>
#include <stdint.h>
#include "lz_compress.hpp"
#include "lz_optional.hpp"
#include "mm2s.hpp"
#include "s2mm.hpp"
#include "stream_downsizer.hpp"
#include "stream_upsizer.hpp"

const int c_gmemBurstSize = 32;

/*
 * 中文说明：LZ4 格式编码与存储器接口（第二问优化版）
 *
 * 数据通路分为四层：
 * 1. lzCompress 产生“字面量/匹配长度/偏移”token；
 * 2. lz4CompressPart1 把 token 拆成字面量流和长度偏移流；
 * 3. lz4CompressPart2 按 LZ4 规范产生 token 字节、扩展长度、字面量和偏移；
 * 4. hlsLz4 / lz4CompressMM 负责分块、位宽转换、突发读写及压缩大小回传。
 *
 * 主要优化点：
 * - DATAFLOW 让匹配、拆分、格式编码和搬运阶段重叠执行；
 * - 核心编码状态机使用 PIPELINE II=1，降低逐 token 的控制开销；
 * - token_lookup 完全分割，避免小查找表的端口竞争；
 * - 对长度和偏移状态显式排除伪跨迭代依赖，帮助流水调度；
 * - 大字面量 FIFO 和长度偏移 FIFO 映射到 BRAM，短级间 FIFO 映射到 SRL；
 * - 多块索引、块大小和限制数组完全分割，使多个压缩单元可独立更新状态；
 * - 顶层采用宽总线突发访问，并在流接口处完成降宽/升宽，减少外存事务数。
 *
 * 注意：LZ4 token 的高、低 4 bit 分别只能直接表示 0~15，超过上限的长度必须
 * 继续输出若干 255 和最后的余数；偏移按 LZ4 规定使用两个字节编码。
 */

namespace xf {
namespace compression {
namespace details {

template <int MAX_LIT_COUNT, int PARALLEL_UNITS>
static void lz4CompressPart1(hls::stream<ap_uint<32> >& inStream,
                             hls::stream<uint8_t>& lit_outStream,
                             hls::stream<ap_uint<64> >& lenOffset_Stream,
                             uint32_t input_size,
                             uint32_t max_lit_limit[PARALLEL_UNITS],
                             uint32_t index) {
    if (input_size == 0) return;

    uint8_t match_len = 0;
    uint32_t lit_count = 0;
    uint32_t lit_count_flag = 0;

    // 预读一个 token，使循环内“使用当前 token”和“读取下一个 token”可以重叠。
    ap_uint<32> nextEncodedValue = inStream.read();
lz4_divide:
    for (uint32_t i = 0; i < input_size;) {
#pragma HLS PIPELINE II = 1 // 每周期解析一个输入 token；匹配 token 会按匹配长度推进位置
        ap_uint<32> tmpEncodedValue = nextEncodedValue;
        if (i < (input_size - 1)) nextEncodedValue = inStream.read();
        uint8_t tCh = tmpEncodedValue.range(7, 0);
        uint8_t tLen = tmpEncodedValue.range(15, 8);
        uint16_t tOffset = tmpEncodedValue.range(31, 16);
        uint32_t match_offset = tOffset;

        if (lit_count >= MAX_LIT_COUNT) {
            lit_count_flag = 1;
        } else if (tLen) {
            uint8_t match_len = tLen - 4; // LZ4 token 记录的是“匹配长度减 4”
            ap_uint<64> tmpValue;
            tmpValue.range(63, 32) = lit_count;
            tmpValue.range(15, 0) = match_len;
            tmpValue.range(31, 16) = match_offset;
            lenOffset_Stream << tmpValue;
            match_len = tLen - 1;
            lit_count = 0;
        } else {
            lit_outStream << tCh;
            lit_count++;
        }
        if (tLen)
            i += tLen;
        else
            i += 1;
    }
    if (lit_count) {
        ap_uint<64> tmpValue;
        tmpValue.range(63, 32) = lit_count;
        if (lit_count == MAX_LIT_COUNT) {
            lit_count_flag = 1;
            tmpValue.range(15, 0) = 777;
            tmpValue.range(31, 16) = 777;
        } else {
            tmpValue.range(15, 0) = 0;
            tmpValue.range(31, 16) = 0;
        }
        lenOffset_Stream << tmpValue;
    }
    max_lit_limit[index] = lit_count_flag;
}

/**
 * @brief 将字面量流和长度偏移流编码成标准 LZ4 字节序列。
 * @param in_lit_inStream 原始字面量字节流。
 * @param in_lenOffset_Stream 64 bit 描述流：`[63:32]` 字面量长度、`[31:16]`
 *        匹配偏移、`[15:0]` 匹配长度减 4。
 * @param outStream 压缩字节流。
 * @param endOfStream 与输出字节一一对应的结束标志流。
 * @param compressdSizeStream 最终压缩字节数。
 * @param input_size 当前块的原始字节数。
 * @details 编码顺序为：token、字面量扩展长度、字面量数据、2 字节偏移、匹配扩展长度。
 */
static void lz4CompressPart2(hls::stream<uint8_t>& in_lit_inStream,
                             hls::stream<ap_uint<64> >& in_lenOffset_Stream,
                             hls::stream<ap_uint<8> >& outStream,
                             hls::stream<bool>& endOfStream,
                             hls::stream<uint32_t>& compressdSizeStream,
                             uint32_t input_size) {

    // ------------------ LZ4 Compression State Definitions ------------------
    enum lz4CompressStates {
        WRITE_TOKEN,       // Write token byte
        WRITE_LIT_LEN,     // Write extended literal length
        WRITE_MATCH_LEN,   // Write extended match length
        WRITE_LITERAL,     // Write literal data
        WRITE_OFFSET0,     // Write offset low byte
        WRITE_OFFSET1      // Write offset high byte
    };

    // ------------------ State Machine and Data Processing Variables ------------------
    uint32_t lit_len = 0;              // Current literal length
    uint16_t outCntr = 0;              // Output counter (unused)
    uint32_t compressedSize = 0;       // Compressed data size
    enum lz4CompressStates next_state = WRITE_TOKEN;  // Current state
    uint16_t lit_length = 0;           // Literal length cache
    uint16_t match_length = 0;         // Match length cache
    uint16_t write_lit_length = 0;     // Literal length to write
    ap_uint<16> match_offset = 0;      // Match offset
    bool lit_ending = false;           // Literal ending flag
    bool extra_match_len = false;      // Extended match length required flag
    bool readOffsetFlag = true;        // Allow reading offset stream flag

    // ------------------ Pre-read Optimization Variables ------------------
    ap_uint<64> nextLenOffsetValue;    // Pre-read length offset value
    ap_uint<16> match_offset_plus_one = 0;  // Pre-calculated offset + 1

    // ------------------ Token Lookup Table ------------------
    // Pre-calculates all possible token values for LZ4 format
    // LZ4 Token Format: [Literal Length(high 4 bits) | Match Length(low 4 bits)]
    static const ap_uint<8> token_lookup[16][16] = {
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F},
        {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F},
        {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F},
        {0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F},
        {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F},
        {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F},
        {0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F},
        {0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F},
        {0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F},
        {0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F},
        {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF},
        {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF},
        {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF},
        {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF},
        {0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF},
        {0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF}
    };
    #pragma HLS ARRAY_PARTITION variable=token_lookup complete dim=0  // 小表寄存器化，允许组合并行查找

    // ------------------ Main Compression Loop ------------------
    lz4_compress:
    for (uint32_t inIdx = 0; (inIdx < input_size) || (!readOffsetFlag);) {
        #pragma HLS PIPELINE II = 1  // 状态机目标每周期输出或处理一个字节
        #pragma HLS DEPENDENCE variable=match_offset inter false  // 排除工具误判的跨迭代依赖
        #pragma HLS DEPENDENCE variable=lit_length inter false
        #pragma HLS DEPENDENCE variable=match_length inter false

        ap_uint<8> outValue = 0;  // Current output byte value

        // ------------------ Pre-read Optimization ------------------
        // Read next length offset value from stream when needed
        if (readOffsetFlag) {
            nextLenOffsetValue = in_lenOffset_Stream.read();
            readOffsetFlag = false;
        }

        // ------------------ Local Variable Caching ------------------
        // Use local variables for caching to reduce bit selection operation latency
        ap_uint<32> lit_len_tmp = nextLenOffsetValue.range(63, 32);  // Literal length
        ap_uint<16> match_len_tmp = nextLenOffsetValue.range(15, 0); // Match length
        ap_uint<16> match_off_tmp = nextLenOffsetValue.range(31, 16);// Match offset

        // ------------------ State Machine Processing ------------------
        if (next_state == WRITE_TOKEN) {
            // State: Write Token Byte
            lit_length = lit_len_tmp;    // Cache literal length
            match_length = match_len_tmp; // Cache match length
            match_offset = match_off_tmp; // Cache match offset

            // Update input index: literal length + match length + 4 (minimum match)
            uint32_t idx_increment = (uint32_t)match_length + (uint32_t)lit_length + 4;
            inIdx += idx_increment;

            // Check end conditions
            bool is_special_end = (match_length == 777) && (match_offset == 777);  // Special end marker
            bool is_normal_end = (match_offset == 0) && (match_length == 0);       // Normal end

            if (is_special_end) {
                inIdx = input_size;  // Immediately end processing
                lit_ending = true;
            }

            lit_len = lit_length;
            write_lit_length = lit_length;
            lit_ending = lit_ending || is_normal_end;  // Set end flag

            // Generate token using lookup table: high 4 bits = literal length, low 4 bits = match length
            ap_uint<4> lit_token = (lit_length >= 15) ? 15 : lit_length;
            ap_uint<4> match_token = (match_length >= 15) ? 15 : match_length;
            outValue = token_lookup[lit_token][match_token];

            // Handle literal length extension
            if (lit_length >= 15) {
                lit_length -= 15;        // Subtract 15 already encoded in token
                next_state = WRITE_LIT_LEN;  // Need to write extended length
                readOffsetFlag = false;  // Keep current data
            } else if (lit_length > 0) {
                lit_length = 0;
                next_state = WRITE_LITERAL;  // Directly write literal data
                readOffsetFlag = false;
            } else {
                next_state = WRITE_OFFSET0;  // No literal data, directly write offset
                readOffsetFlag = false;
            }

            // Handle match length extension
            if (match_length >= 15) {
                match_length -= 15;      // Subtract 15 already encoded in token
                extra_match_len = true;  // Need to write extended length
            } else {
                match_length = 0;
                extra_match_len = false; // No extension needed
            }

            // Pre-calculate offset + 1 (offset stored as actual value + 1 in LZ4 format)
            match_offset_plus_one = match_offset + 1;

        } else if (next_state == WRITE_LIT_LEN) {
            // State: Write Extended Literal Length
            bool lit_len_ge_255 = (lit_length >= 255);
            outValue = lit_len_ge_255 ? (ap_uint<8>)255 : (ap_uint<8>)lit_length;

            if (lit_len_ge_255) {
                lit_length -= 255;  // Continue writing extension bytes
            } else {
                next_state = WRITE_LITERAL;  // Extension complete, start writing literal data
                readOffsetFlag = false;
            }

        } else if (next_state == WRITE_LITERAL) {
            // State: Write Literal Data (uncompressed raw bytes)
            outValue = in_lit_inStream.read();  // Read from literal stream
            write_lit_length--;  // Decrement remaining literal length

            if (write_lit_length == 0) {
                // Literal data writing complete
                next_state = lit_ending ? WRITE_TOKEN : WRITE_OFFSET0;
                readOffsetFlag = lit_ending;  // If ending state, read new data
            }

        } else if (next_state == WRITE_OFFSET0) {
            // State: Write Offset Low Byte
            outValue = match_offset_plus_one.range(7, 0);  // Low 8 bits
            next_state = WRITE_OFFSET1;
            readOffsetFlag = false;

        } else if (next_state == WRITE_OFFSET1) {
            // State: Write Offset High Byte
            outValue = match_offset_plus_one.range(15, 8);  // High 8 bits
            next_state = extra_match_len ? WRITE_MATCH_LEN : WRITE_TOKEN;
            readOffsetFlag = !extra_match_len;  // If extended match length needed, don't read new data

        } else if (next_state == WRITE_MATCH_LEN) {
            // State: Write Extended Match Length
            bool match_len_ge_255 = (match_length >= 255);
            outValue = match_len_ge_255 ? (ap_uint<8>)255 : (ap_uint<8>)match_length;

            if (match_len_ge_255) {
                match_length -= 255;  // Continue writing extension bytes
            } else {
                next_state = WRITE_TOKEN;  // Extension complete, return to Token state
                readOffsetFlag = true;     // Allow reading new data
            }
        }

        // ------------------ Output Processing ------------------
        bool should_write = (compressedSize < input_size);  // Ensure not exceeding input size
        if (should_write) {
            outStream << outValue;      // Output compressed byte
            endOfStream << 0;           // Output non-end flag
            compressedSize++;           // Update compressed size counter
        }
    }

    // ------------------ Final Output ------------------
    compressdSizeStream << compressedSize;  // Output total compressed data size
    outStream << 0;                         // Output end byte
    endOfStream << 1;                       // Set stream end flag
}

} // namespace compression
} // namespace xf
} // namespace details

namespace xf {
namespace compression {

/**
 * @brief This is the core compression module which seperates the input stream into two
 * output streams, one literal stream and other offset stream, then lz4 encoding is done.
 *
 * @tparam PARALLEL_UNITS number of parallel units
 * @tparam MAX_LIT_COUNT encoded literal length count
 *
 * @param inStream Input data stream
 * @param outStream Output data stream
 * @param max_lit_limit Size for compressed stream
 * @param input_size Size of input data
 * @param endOfStream Stream indicating that all data is processed or not
 * @param compressdSizeStream Gives the compressed size for each 64K block
 * @param index index value
 *
 */
template <int MAX_LIT_COUNT, int PARALLEL_UNITS>
static void lz4Compress(hls::stream<ap_uint<32> >& inStream,
                                 hls::stream<ap_uint<8> >& outStream,
                                 uint32_t max_lit_limit[PARALLEL_UNITS],
                                 uint32_t input_size,
                                 hls::stream<bool>& endOfStream,
                                 hls::stream<uint32_t>& compressdSizeStream,
                                 uint32_t index) {
    #pragma HLS DATAFLOW // token 拆分与 LZ4 字节编码并行运行

    hls::stream<uint8_t, MAX_LIT_COUNT> lit_outStream;
    hls::stream<ap_uint<64>, c_gmemBurstSize> lenOffset_Stream;

    // 设置流通道的 FIFO 深度，用于吸收上下游速率差并降低数据流停顿风险。
    #pragma HLS STREAM variable=lit_outStream depth=MAX_LIT_COUNT
    #pragma HLS STREAM variable=lenOffset_Stream depth=c_gmemBurstSize

    // Improve parallel throughput
    #pragma HLS BIND_STORAGE variable=lit_outStream type=FIFO impl=BRAM // 大量字面量需要较深缓冲
    #pragma HLS BIND_STORAGE variable=lenOffset_Stream type=FIFO impl=BRAM // 解耦不规则 token 产生和消费速率

    details::lz4CompressPart1<MAX_LIT_COUNT, PARALLEL_UNITS>(inStream, lit_outStream, lenOffset_Stream, input_size,
                                                            max_lit_limit, index);

    details::lz4CompressPart2(lit_outStream, lenOffset_Stream, outStream, endOfStream, compressdSizeStream, input_size);
}

/**
 * @file hls_lz4_core.hpp
 * @brief Optimized LZ4 Compression Algorithm HLS Core Processing Module
 *
 * This module implements a complete LZ4 compression pipeline with four processing stages:
 * 1. LZ Compression - Basic pattern matching and dictionary lookup
 * 2. Best Match Filtering - Optimization of match results
 * 3. Match Enhancement - Compression ratio improvement
 * 4. LZ4 Format Encoding - Final compressed data generation
 *
 * The design features a fully pipelined architecture with dataflow optimization for
 * maximum throughput and hardware efficiency. Each stage operates concurrently on
 * different data segments to achieve high-performance compression.
 *
 * @tparam data_t Data type for stream elements
 * @tparam DATAWIDTH Data bus width, default 512-bit for optimal memory bandwidth
 * @tparam BURST_SIZE Burst transfer size, default 16 for efficient memory access
 * @tparam NUM_BLOCK Number of processing blocks, default 8 for parallel processing
 * @tparam M_LEN Minimum match length, default 6 bytes
 * @tparam MIN_MAT Minimum match threshold, default 4 bytes
 * @tparam LZ_MAX_OFFSET_LIM LZ maximum offset limit, default 65536 bytes
 * @tparam OFFSET_WIN Offset window size, default 65536 bytes
 * @tparam MAX_M_LEN Maximum match length, default 255 bytes (LZ4 standard limit)
 * @tparam MAX_LIT_CNT Maximum literal sequence length, default 4096 bytes
 * @tparam MIN_B_SIZE Minimum block size, default 128 bytes
 *
 * @param inStream Input data stream
 * @param outStream Output compressed data stream
 * @param outStreamEos Output stream end-of-stream flag
 * @param compressedSize Output compressed size stream
 * @param max_lit_limit Maximum literal length limits array for each block
 * @param input_size Total input data size
 * @param core_idx Core index identifier for multi-core implementations
 */
template <class data_t,
          int DATAWIDTH = 512,
          int BURST_SIZE = 16,
          int NUM_BLOCK = 8,
          int M_LEN = 6,
          int MIN_MAT = 4,
          int LZ_MAX_OFFSET_LIM = 65536,
          int OFFSET_WIN = 65536,
          int MAX_M_LEN = 255,
          int MAX_LIT_CNT = 4096,
          int MIN_B_SIZE = 128>
void hlsLz4Core(
    // Input data stream
    hls::stream<data_t>& inStream,
    // Output compressed data stream
    hls::stream<data_t>& outStream,
    // Output end-of-stream flag
    hls::stream<bool>& outStreamEos,
    // Output compressed size stream
    hls::stream<uint32_t>& compressedSize,
    // Maximum literal length limits array for each block
    uint32_t max_lit_limit[NUM_BLOCK],
    // Total input data size
    uint32_t input_size,
    // Core index identifier
    uint32_t core_idx) {

    // ------------------ Intermediate Data Stream Declarations ------------------
    // These streams connect the four pipeline stages in the compression flow
    hls::stream<ap_uint<32> > compressdStream("compressdStream");    // LZ compression results stream
    hls::stream<ap_uint<32> > bestMatchStream("bestMatchStream");    // Best match filtering stream
    hls::stream<ap_uint<32> > boosterStream("boosterStream");        // Enhanced match stream

    // ------------------ Stream Depth Optimization ------------------
    // Optimize FIFO depths for performance and resource utilization
    // 设置流通道的 FIFO 深度，用于吸收上下游速率差并降低数据流停顿风险。
    #pragma HLS STREAM variable = compressdStream depth = 8
    #pragma HLS STREAM variable = bestMatchStream depth = 8
    #pragma HLS STREAM variable = boosterStream depth = 8

    // ------------------ Storage Type Binding Optimization ------------------
    // Specify hardware implementation for FIFOs
    // 显式约束存储对象的端口类型和实现资源，保证并行访问能力可被综合实现。
    #pragma HLS BIND_STORAGE variable = compressdStream type = FIFO impl = SRL
    #pragma HLS BIND_STORAGE variable = boosterStream type = FIFO impl = SRL

    // ------------------ Dataflow Parallel Execution ------------------
    // Enable concurrent execution of all compression stages
    // Each stage operates independently on different data segments
    #pragma HLS dataflow // LZ 匹配、最佳匹配选择、匹配扩展和 LZ4 编码并发执行

    /**
     * @brief Stage 1: LZ Base Compression
     *
     * Performs dictionary-based pattern matching on input data to identify
     * repeated sequences. Generates preliminary match information including
     * literal lengths, match lengths, and match offsets using a sliding
     * window approach with hash-based dictionary lookup.
     *
     * Key Features:
     * - Sliding window with configurable size
     * - Hash-based dictionary for efficient pattern matching
     * - Parallel match candidate evaluation
     * - Configurable minimum match length and offset limits
     */
    xf::compression::lzCompress<M_LEN, MIN_MAT, LZ_MAX_OFFSET_LIM>(
        inStream, compressdStream, input_size);

    /**
     * @brief Stage 2: Best Match Filtering
     *
     * Filters and selects the optimal matches from preliminary results.
     * Evaluates multiple match candidates based on compression efficiency
     * criteria and selects the best combination of literal sequences and
     * matches to maximize compression ratio while maintaining processing
     * efficiency.
     *
     * Key Features:
     * - Multiple candidate evaluation and selection
     * - Offset window-based filtering
     * - Compression efficiency optimization
     * - Overlapping match resolution
     */
    xf::compression::lzBestMatchFilter<M_LEN, OFFSET_WIN>(
        compressdStream, bestMatchStream, input_size);

    /**
     * @brief Stage 3: Match Enhancement
     *
     * Optimizes match results to improve compression ratio through
     * techniques such as match length extension, overlap elimination,
     * and sequence recombination. Ensures compliance with LZ4 format
     * constraints while maximizing the benefits of identified matches.
     *
     * Key Features:
     * - Match length optimization
     * - Sequence boundary alignment
     * - Maximum match length enforcement
     * - Compression ratio improvement
     */
    xf::compression::lzBooster<MAX_M_LEN>(
        bestMatchStream, boosterStream, input_size);

    /**
     * @brief Stage 4: LZ4 Format Compression
     *
     * Encodes optimized match information into standard LZ4 compressed
     * format. Implements LZ4 block encoding with token generation,
     * literal length encoding, match length encoding, and offset
     * encoding according to LZ4 specification.
     *
     * Key Features:
     * - LZ4 standard compliant output
     * - Token-based encoding (literal length + match length)
     * - Extended length encoding for large sequences
     * - Multi-block processing support
     * - End-of-stream handling
     */
    xf::compression::lz4Compress<MAX_LIT_CNT, NUM_BLOCK>(
        boosterStream, outStream, max_lit_limit, input_size,
        outStreamEos, compressedSize, core_idx);
}

template <class data_t,
          int DATAWIDTH = 512,
          int BURST_SIZE = 16,
          int NUM_BLOCK = 8,
          int M_LEN = 6,
          int MIN_MAT = 4,
          int LZ_MAX_OFFSET_LIM = 65536,
          int OFFSET_WIN = 65536,
          int MAX_M_LEN = 255,
          int MAX_LIT_CNT = 4096,
          int MIN_B_SIZE = 128>
void hlsLz4(const data_t* in,
            data_t* out,
            const uint32_t input_idx[NUM_BLOCK],
            const uint32_t output_idx[NUM_BLOCK],
            const uint32_t input_size[NUM_BLOCK],
            uint32_t output_size[NUM_BLOCK],
            uint32_t max_lit_limit[NUM_BLOCK]) {
    hls::stream<ap_uint<8> > inStream[NUM_BLOCK];
    hls::stream<bool> outStreamEos[NUM_BLOCK];
    hls::stream<ap_uint<8> > outStream[NUM_BLOCK];
// 设置流通道的 FIFO 深度，用于吸收上下游速率差并降低数据流停顿风险。
#pragma HLS STREAM variable = outStreamEos depth = 2
#pragma HLS STREAM variable = inStream depth = c_gmemBurstSize
#pragma HLS STREAM variable = outStream depth = c_gmemBurstSize

// 显式约束存储对象的端口类型和实现资源，保证并行访问能力可被综合实现。
#pragma HLS BIND_STORAGE variable = outStreamEos type = FIFO impl = SRL
#pragma HLS BIND_STORAGE variable = inStream type = FIFO impl = SRL
#pragma HLS BIND_STORAGE variable = outStream type = FIFO impl = SRL

    hls::stream<uint32_t> compressedSize[NUM_BLOCK];

#pragma HLS dataflow // 外存读、压缩核心与外存写形成任务级流水
    xf::compression::details::mm2multStreamSize<8, NUM_BLOCK, DATAWIDTH, BURST_SIZE>(in, input_idx, inStream,
                                                                                     input_size);

    for (uint8_t i = 0; i < NUM_BLOCK; i++) {
// 展开循环迭代以形成并行硬件；展开因子越大，吞吐越高但资源与布线压力也越大。
#pragma HLS UNROLL
        // lz4Core is instantiated based on the NUM_BLOCK
        hlsLz4Core<ap_uint<8>, DATAWIDTH, BURST_SIZE, NUM_BLOCK>(inStream[i], outStream[i], outStreamEos[i],
                                                                 compressedSize[i], max_lit_limit, input_size[i], i);
    }

    xf::compression::details::multStream2MM<8, NUM_BLOCK, DATAWIDTH, BURST_SIZE>(
        outStream, outStreamEos, compressedSize, output_idx, out, output_size);
}

template <class data_t,
          int DATAWIDTH = 512,
          int BURST_SIZE = 16,
          int NUM_BLOCK = 8,
          int M_LEN = 6,
          int MIN_MAT = 4,
          int LZ_MAX_OFFSET_LIM = 65536,
          int OFFSET_WIN = 65536,
          int MAX_M_LEN = 255,
          int MAX_LIT_CNT = 4096,
          int MIN_B_SIZE = 128>
void lz4CompressMM(const data_t* in, data_t* out, uint32_t* compressd_size, const uint32_t input_size) {
    uint32_t block_idx = 0;
    uint32_t block_length = 64 * 1024;
    uint32_t no_blocks = (input_size - 1) / block_length + 1;
    uint32_t max_block_size = 64 * 1024;
    uint32_t readBlockSize = 0;

    bool small_block[NUM_BLOCK];
    uint32_t input_block_size[NUM_BLOCK];
    uint32_t input_idx[NUM_BLOCK];
    uint32_t output_idx[NUM_BLOCK];
    uint32_t output_block_size[NUM_BLOCK];
    uint32_t max_lit_limit[NUM_BLOCK];
    uint32_t small_block_inSize[NUM_BLOCK];
#pragma HLS ARRAY_PARTITION variable = input_block_size dim = 0 complete // 每个并行块独立保存输入大小
#pragma HLS ARRAY_PARTITION variable = input_idx dim = 0 complete        // 消除多单元访问同一数组端口的冲突
#pragma HLS ARRAY_PARTITION variable = output_idx dim = 0 complete
#pragma HLS ARRAY_PARTITION variable = output_block_size dim = 0 complete
#pragma HLS ARRAY_PARTITION variable = max_lit_limit dim = 0 complete    // 各块独立记录字面量上限状态

    // Figure out total blocks & block sizes
    for (uint32_t i = 0; i < no_blocks; i += NUM_BLOCK) {
        uint32_t nblocks = NUM_BLOCK;
        if ((i + NUM_BLOCK) > no_blocks) {
            nblocks = no_blocks - i;
        }

        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (uint32_t j = 0; j < NUM_BLOCK; j++) {
            if (j < nblocks) {
                uint32_t inBlockSize = block_length;
                if (readBlockSize + block_length > input_size) inBlockSize = input_size - readBlockSize;
                if (inBlockSize < MIN_B_SIZE) {
                    small_block[j] = 1;
                    small_block_inSize[j] = inBlockSize;
                    input_block_size[j] = 0;
                    input_idx[j] = 0;
                } else {
                    small_block[j] = 0;
                    input_block_size[j] = inBlockSize;
                    readBlockSize += inBlockSize;
                    input_idx[j] = (i + j) * max_block_size;
                    output_idx[j] = (i + j) * max_block_size;
                }
            } else {
                input_block_size[j] = 0;
                input_idx[j] = 0;
            }
            output_block_size[j] = 0;
            max_lit_limit[j] = 0;
        }

        // Call for parallel compression
        hlsLz4<data_t, DATAWIDTH, BURST_SIZE, NUM_BLOCK>(in, out, input_idx, output_idx, input_block_size,
                                                         output_block_size, max_lit_limit);

        // 逐项处理当前算法阶段的数据；循环边界定义有效规模，配套流水化或展开指令决定硬件并行度。
        for (uint32_t k = 0; k < nblocks; k++) {
            if (max_lit_limit[k]) {
                compressd_size[block_idx] = input_block_size[k];
            } else {
                compressd_size[block_idx] = output_block_size[k];
            }

            if (small_block[k] == 1) {
                compressd_size[block_idx] = small_block_inSize[k];
            }
            block_idx++;
        }
    }
}

} // namespace compression
} // namespace xf
#endif // _XFCOMPRESSION_LZ4_COMPRESS_HPP_
