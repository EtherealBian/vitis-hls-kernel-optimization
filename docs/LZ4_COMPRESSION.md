# 第二问：LZ4 压缩原代码功能与优化说明

## 1. 项目与文件说明

第二问针对 Vitis HLS 的 **LZ4 压缩算子**进行优化。测试顶层函数为 `lz4CompressEngineRun`，目标器件为 `xc7z020-clg484-1`。

本题的核心对比文件为：

- 基线匹配查找代码：`baselines/data_compression/lz_compress.hpp`
- 优化后匹配查找代码：`data_compression/L1/include/hw/lz_compress.hpp`
- 基线 LZ4 编码代码：`baselines/data_compression/lz4_compress.hpp`
- 优化后 LZ4 编码代码：`data_compression/L1/include/hw/lz4_compress.hpp`
- 综合报告：`data_compression/L1/tests/lz4_compress/reports/csynth.xml`
- C 仿真日志：`data_compression/L1/tests/lz4_compress/reports/csim.log`
- C/RTL 协同仿真报告：`data_compression/L1/tests/lz4_compress/reports/cosim.rpt`

本题测试数据为 `sample.txt`。最终 C 仿真得到的压缩比为 **2.09396**。

需要说明的是，`baselines/data_compression/lz4_compress.hpp` 已经包含长度/偏移预读取和局部变量缓存代码，而实验记录将它们列为第一阶段优化。因此下文会同时区分：

1. 原版本目录中可以直接看到的原始架构；
2. 实验记录反映的完整优化过程；
3. 原版本与最终版本之间能够由代码直接确认的差异。

---

## 2. 原代码实现了什么功能

### 2.1 LZ4 压缩的基本思想

LZ4 将输入数据表示成若干个序列。每个序列通常包含：

```text
Token + 扩展字面量长度 + Literal 字节 + 2字节 Offset + 扩展匹配长度
```

其中：

- **Literal**：无法用历史数据替代、需要原样输出的字节；
- **Match**：与前面已经出现的数据相同的连续字节；
- **Offset**：当前数据与历史匹配位置之间的距离；
- **Match Length**：匹配数据的长度；
- **Token**：高 4 位记录字面量长度的短编码，低 4 位记录匹配长度的短编码。

当字面量长度或匹配长度大于 15 时，LZ4 会在 Token 后继续使用若干个扩展长度字节表示剩余长度。

### 2.2 原代码的整体流水线

测试顶层 `lz4CompressEngineRun()` 使用 HLS `DATAFLOW`，依次连接四个处理阶段：

```text
8位输入字节流
     │
     ▼
lzCompress
哈希字典查找，产生初步匹配长度和偏移
     │
     ▼
lzBestMatchFilter
过滤冲突、重叠或不合适的匹配
     │
     ▼
lzBooster
继续向后扩展匹配，提高匹配长度
     │
     ▼
lz4Compress
将 Literal、Length、Offset 编码成标准 LZ4 字节流
```

各阶段通过 `hls::stream` 相连，可以在不同输入位置上并行工作。

### 2.3 `lzCompress()`：滑动窗口与哈希字典匹配

`lz_compress.hpp` 中的 `lzCompress()` 是压缩前端。它逐字节滑动一个长度为 `MATCH_LEN` 的窗口，并为当前位置寻找历史匹配。

#### 维护滑动窗口

代码将新字节读入 `present_window[MATCH_LEN-1]`，同时把窗口中其余字节向前移动。窗口使用完全数组分区，使各字节可以被独立访问。

#### 计算哈希地址

原代码根据窗口开头的 3 个或 4 个字节，通过移位和异或计算哈希值。哈希值用于索引字典：

```cpp
hash = (present_window[0] << 4) ^
       (present_window[1] << 3) ^
       (present_window[2] << 2) ^
       present_window[3];
```

#### 字典保存历史候选

原字典为：

```cpp
uintDictV_t dict[LZ_DICT_SIZE];
#pragma HLS BIND_STORAGE variable=dict type=RAM_T2P impl=BRAM
```

每个哈希槽保存 `MATCH_LEVEL` 个历史候选。每个候选包含：

- 历史窗口的 `MATCH_LEN` 个字节；
- 该窗口在输入中的位置索引。

新窗口写入时，旧候选整体移位，当前窗口插入到最新位置。

#### 搜索最佳匹配

原代码依次遍历 `MATCH_LEVEL` 个候选，并逐字节比较当前窗口与历史窗口。遇到第一个不同字节后停止累计匹配长度。

候选还必须满足以下条件：

- 匹配长度不小于 `MIN_MATCH`；
- 历史位置必须早于当前位置；
- 偏移不能超过 LZ4 允许范围；
- 偏移不能小于 `MIN_OFFSET`；
- 对长度为 3 的短匹配执行额外的 4096 字节偏移限制。

原代码通过线性遍历保存最长匹配，最后输出一个 32 位中间值：

```text
[31:16] match_offset
[15: 8] match_length
[ 7: 0] 当前字面量字节
```

#### 原代码的字典初始化

每次调用开始时，原代码遍历整个 `LZ_DICT_SIZE` 字典，将全部条目写成无效值：

```cpp
for (int i = 0; i < LZ_DICT_SIZE; i++) {
    #pragma HLS PIPELINE II=1
    #pragma HLS UNROLL FACTOR=2
    dict[i] = resetValue;
}
```

字典有 4096 个槽，因此这段全量清空会带来明显的固定启动延迟。

### 2.4 `lzBestMatchFilter()`：过滤相邻匹配

初步匹配结果并不一定都适合直接编码。例如，相邻位置的匹配可能发生重叠，或者当前匹配会覆盖下一处更长的匹配。

`lzBestMatchFilter()` 根据当前位置和后续候选的匹配长度，过滤无效或收益较低的匹配，避免同一输入字节被多个序列重复使用，为后面的匹配扩展阶段提供一致的中间流。

### 2.5 `lzBooster()`：扩展固定窗口之外的匹配

`lzCompress()` 只在 `MATCH_LEN` 范围内快速判断初始匹配。`lzBooster()` 使用局部历史存储继续比较后续字节，在满足最大匹配长度、输入边界和偏移条件的前提下延长 Match。

这一阶段能把固定长度的初始匹配扩展为更长的 LZ4 匹配，从而提高或维持压缩比。

### 2.6 `lz4CompressPart1()`：拆分字面量与匹配描述

`lz4CompressPart1()` 读取前三级产生的 32 位中间值，并将数据分成两路：

- `lit_outStream`：需要原样输出的 Literal 字节；
- `lenOffset_Stream`：每个 LZ4 序列的字面量长度、匹配长度和偏移。

`lenOffset_Stream` 的 64 位格式为：

```text
[63:32] literal length
[31:16] match offset
[15: 0] match length（已经减去 LZ4 最小匹配长度 4）
```

### 2.7 `lz4CompressPart2()`：生成标准 LZ4 字节流

该函数使用状态机将两路中间流重新组合成 LZ4 格式。原版本保留六个状态：

- `WRITE_TOKEN`：输出 Token；
- `WRITE_LIT_LEN`：输出扩展字面量长度；
- `WRITE_LITERAL`：逐字节输出 Literal；
- `WRITE_OFFSET0`：输出 Offset 低字节；
- `WRITE_OFFSET1`：输出 Offset 高字节；
- `WRITE_MATCH_LEN`：输出扩展匹配长度。

主循环设置 `PIPELINE II=1`。当新的长度/偏移描述可读时，代码先保存到 `nextLenOffsetValue`，再把其中的字段缓存到局部变量，减少状态机内部重复的位域提取。

### 2.8 原代码的主要瓶颈

综合代码结构可以看出，原实现的主要瓶颈包括：

1. 每次调用都完整清空 4096 项字典，产生较大的固定启动开销；
2. `MATCH_LEVEL` 个候选按线性方式处理；
3. 每个候选内部的 `MATCH_LEN` 字节比较存在顺序累计关系；
4. 最佳匹配通过线性比较选择，组合路径随候选数量增加；
5. 原哈希函数较简单，哈希冲突可能增加无效候选；
6. LZ4 Token 在运行时分别拼接高、低 4 位；
7. Part1 与 Part2 之间的流若存储类型或深度不合适，会使 DATAFLOW 阶段互相阻塞。

根据实验阶段记录，初始测试延迟约为 **3390 cycles**。

---

## 3. 我做了怎样的优化

### 3.1 优化一：Token 查找表、预读取和局部缓存

第一阶段主要优化 `lz4CompressPart2()` 编码状态机。

### Token 查找表

LZ4 Token 的本质是：

```text
token = (min(literal_length, 15) << 4) |
         min(match_length, 15)
```

最终代码建立 `token_lookup[16][16]`，预存全部 256 种组合，并进行完全数组分区：

```cpp
static const ap_uint<8> token_lookup[16][16] = { /* 0x00～0xFF */ };
#pragma HLS ARRAY_PARTITION variable=token_lookup complete dim=0
```

运行时先得到 `lit_token` 和 `match_token`，再直接查表得到输出字节，避免在状态机关键路径上分别写入两个位域。

### 长度/偏移预读取

使用 `readOffsetFlag` 控制 `lenOffset_Stream` 的读取时机，在进入 `WRITE_TOKEN` 之前将下一组描述保存到 `nextLenOffsetValue`。这样状态机可以复用本地寄存器，而不是在多个分支中直接访问流。

### 局部变量缓存和预计算

64 位描述只提取一次：

```cpp
lit_len_tmp   = nextLenOffsetValue.range(63, 32);
match_len_tmp = nextLenOffsetValue.range(15, 0);
match_off_tmp = nextLenOffsetValue.range(31, 16);
```

同时提前计算 `match_offset_plus_one`，供两个 Offset 输出状态直接取低、高字节。

需要注意：预读取、局部缓存和 Offset 预计算已经存在于当前归档的“原版本”文件中；最终文件相对该归档版本可直接确认的新增重点是 **Token 查找表**。实验记录中的第一阶段是更早的完整演进过程。

这一阶段主要改善估计时钟周期：实验记录显示时钟周期由 **13.220 ns 降至 12.303 ns**；延迟由 3390 变为 3392 cycles，基本不变。这说明它主要缩短组合关键路径，而不是减少状态机需要输出的字节周期数。

### 3.2 优化二：消除全字典清空

最终 `lz_compress.hpp` 将原来的普通字典替换为静态历史数据和有效标志：

```cpp
static uintDictV_t historyData[LZ_DICT_SIZE];
static ap_uint<32> validityFlags[LZ_DICT_SIZE];
```

查字典时，只有有效标志匹配当前批次时才使用历史内容，否则把该槽视为空：

```cpp
uintDictV_t dictReadValue = 0;
if (validityFlags[hash] == processingBatch) {
    dictReadValue = historyData[hash];
}
```

这相当于使用“标记失效”代替逐项物理清零，删除了每次处理前遍历 4096 项字典的启动阶段。对于仅 1248 字节的当前样例，清空字典的固定开销占比很大，因此这是最终代码中非常重要的延迟优化来源。

`historyData` 和 `validityFlags` 均映射为双口 BRAM，并采用循环因子 8 的数组分区，以提高可并行访问能力。

### 3.3 优化三：乘法哈希与字典访问优化

原来的移位异或哈希改成多个乘法常量与异或组合：

```cpp
hash = (present_window[0] * 2654435761UL) ^
       (present_window[1] * 2246822519UL) ^
       (present_window[2] * 3266489917UL) ^
       (present_window[3] * 668265263UL);
hash &= LZ_DICT_SIZE - 1;
```

这样做的目标是改善哈希分布，降低不同窗口落入同一槽位的概率。最终综合使用了 3 个 DSP，主要与新增乘法运算有关。

同时增加：

```cpp
#pragma HLS dependence variable=historyData inter false
#pragma HLS dependence variable=validityFlags inter false
```

用于告诉 HLS 工具不同迭代间不存在需要保守等待的伪相关，从而帮助主循环维持 `II=1`。

哈希函数发生变化可能改变候选碰撞关系，因此“压缩比不降低”不能只由公式直接保证；当前项目通过样例压缩、解压对比和压缩比 **2.09396** 验证了本测试数据上没有退化。

### 3.4 优化四：并行评估所有匹配候选

原代码使用单一的 `match_length` 和 `match_offset`，逐个候选更新当前最佳结果。优化代码为每个候选建立独立数组：

```cpp
uint8_t  match_lengths[MATCH_LEVEL];
uint32_t match_offsets[MATCH_LEVEL];
bool     match_valids[MATCH_LEVEL];
```

候选循环使用完全展开，使 `MATCH_LEVEL` 个候选可以形成并行硬件比较通路。

每个候选内部先并行计算所有字节是否相等：

```cpp
bool byte_matches[MATCH_LEN];
for (int m = 0; m < MATCH_LEN; m++) {
    #pragma HLS UNROLL
    byte_matches[m] = present_window[m] == compareWith.range(...);
}
```

随后从字节相等结果得到连续前缀匹配长度。与原来的顺序循环相比，这会消耗更多比较器和寄存器，但能降低循环控制造成的串行延迟。

### 3.5 优化五：并行验证匹配条件

优化代码将长度、索引、最大偏移、最小偏移和短匹配限制分别计算：

```cpp
bool length_ok      = len >= MIN_MATCH;
bool index_ok       = currIdx > compareIdx;
bool offset_ok      = (currIdx - compareIdx) < LZ_MAX_OFFSET_LIMIT;
bool min_offset_ok  = (currIdx - compareIdx - 1) >= MIN_OFFSET;
bool special_case_ok = !(len == 3 && (currIdx - compareIdx - 1) > 4096);
```

最后统一组合为 `match_valids[level]`。这种写法便于 HLS 将相互独立的条件并行实现，减少长串条件控制对关键路径的影响。

### 3.6 优化六：锦标赛式最佳匹配选择

原代码依次比较全部候选，属于线性选择。优化代码把候选两两比较：

```text
MATCH_LEVEL 个候选
       │
       ▼
第一级：两两比较
       │
       ▼
第二级：继续两两比较
       │
       ▼
最终最佳长度和偏移
```

各级循环均使用 `UNROLL`。树形选择的逻辑深度接近 `O(log MATCH_LEVEL)`，比线性依赖链更适合硬件并行实现。

第二阶段实验记录显示，延迟由 **3392 cycles 降至 1599 cycles**，下降约 **52.9%**。这一阶段的显著收益来自匹配查找并行化，以及最终代码中可直接看到的全字典清空消除。

### 3.7 优化七：显式流水线和窗口并行更新

最终代码对 `lzCompress()` 函数和主循环明确设置 `PIPELINE II=1`，并使用 `INLINE off` 保留模块层次。

滑动窗口移动、窗口写入字典、候选字节比较及残余数据处理循环使用 `UNROLL` 或 `PIPELINE II=1`，尽量让每周期读入一个新字节并产生一项中间结果。

### 3.8 优化八：FIFO 深度与 BRAM 映射

`lz4CompressPart1()` 与 `lz4CompressPart2()` 之间有两条关键流：

```cpp
hls::stream<uint8_t, MAX_LIT_COUNT> lit_outStream;
hls::stream<ap_uint<64>, c_gmemBurstSize> lenOffset_Stream;
```

最终版本将两条流都显式映射为 BRAM FIFO：

```cpp
#pragma HLS BIND_STORAGE variable=lit_outStream type=FIFO impl=BRAM
#pragma HLS BIND_STORAGE variable=lenOffset_Stream type=FIFO impl=BRAM
```

Literal 数据量较大，长度/偏移流的生产和消费速率又会随状态机状态变化。更深的 BRAM FIFO 可以吸收两个阶段的速率差，使 Part1 继续分析输入时，Part2 同时编码之前的序列，降低 DATAFLOW 背压停顿。

归档原版本已经包含 `#pragma HLS dataflow`，因此最终阶段并非第一次引入 DATAFLOW；可以由代码直接确认的实质变化是：

- 流深度通过模板参数显式化；
- `lit_outStream` 明确绑定 BRAM；
- `lenOffset_Stream` 由 SRL FIFO 改为 BRAM FIFO；
- DATAFLOW 指令的位置和代码结构被进一步明确。

第三阶段实验记录显示，延迟由 **1599 cycles 降至 1343 cycles**，下降约 **16.0%**。

### 3.9 没有采纳的方案

根据实验记录，以下方案经评估后没有进入最终实现：

- **合并两个 Offset 输出状态**：LZ4 Offset 固定为两个字节，合并状态仍需两周期控制，反而增加额外标志和分支；
- **大容量 Literal 预读取缓存**：顺序流本身带宽已较高，新增缓存的管理成本和 BRAM 开销大于收益；
- **双缓冲激进预读取**：容易导致过度读取、流同步复杂化或数据流失衡；
- **过度重构条件逻辑**：可能增加多路选择器深度，使估计时钟周期变差；
- **过度激进的字典分区**：会增加 BRAM 碎片和布线压力；
- **继续拆分过多 DATAFLOW 阶段**：FIFO 深度和阶段节拍难以平衡，可能引入死锁风险。

---

## 4. 优化前后结构对比

| 对比项 | 原代码 | 优化代码 |
| --- | --- | --- |
| 字典初始化 | 每次遍历 4096 项并清零 | 历史数据加有效标志，按需判定 |
| 字典存储 | 单个双口 BRAM 字典 | `historyData`、`validityFlags` 双口 BRAM并分区 |
| 哈希函数 | 移位与异或 | 乘法哈希、异或并掩码 |
| 候选处理 | 逐个候选处理 | `MATCH_LEVEL` 候选展开并行 |
| 字节比较 | 候选内顺序累计 | 字节相等判断完全展开 |
| 约束判断 | 长条件串 | 独立计算后并行合并 |
| 最佳匹配选择 | 线性更新 | 多级锦标赛选择 |
| Token 生成 | 分别拼接高低 4 位 | 16×16 完全分区查找表 |
| 长度/偏移访问 | 流读取与状态处理耦合 | 预读取、局部缓存、Offset+1 预计算 |
| Part1/Part2 中间流 | Literal 自动映射；描述流使用 SRL | 两条流均明确使用 BRAM FIFO |
| 顶层接口 | `lz4CompressEngineRun` | 保持不变 |
| 样例压缩比 | 2.09396 | 2.09396 |
| 测试延迟 | 约 3390 cycles | 1343 cycles |

---

## 5. 性能提升计算

根据各阶段实验记录：

| 阶段 | 延迟 | 相对上一阶段变化 |
| --- | ---: | ---: |
| 初始版本 | 3390 cycles | 基线 |
| Token、预读取与局部缓存阶段 | 3392 cycles | 基本不变 |
| 匹配查找并行化阶段 | 1599 cycles | 下降 52.9% |
| FIFO/存储与最终数据流调整 | 1343 cycles | 下降 16.0% |

最终版本相对 3390 cycles 初始基线的总体降幅为：

```text
(3390 - 1343) / 3390 × 100% ≈ 60.4%
```

实验记录末尾曾写成“3391 降至 2366，下降 30.2%”，但该数字与同一记录中的三阶段表格、当前目录名以及最终协同仿真报告均不一致。以最终 `lz4CompressEngineRun_cosim.rpt` 为准，最终延迟应为 **1343 cycles**，相对约 3390 cycles 的正确总体降幅约为 **60.4%**。

---

## 6. 最终综合与验证结果

### 6.1 测试环境与时序

| 项目 | 最终结果 |
| --- | ---: |
| Vitis HLS | 2024.2 |
| 目标器件 | `xc7z020-clg484-1` |
| 目标时钟周期 | 13.720 ns |
| 估计时钟周期 | 12.341 ns |
| 估计最高频率 | 约 81.03 MHz |
| 协同仿真延迟 | 1343 cycles |

估计时钟周期小于目标周期，说明 HLS 综合估计满足当前时钟约束。

### 6.2 功能与压缩效果

- C 仿真完成且报告 `0 errors`；
- 样例压缩比为 **2.09396**；
- 实验记录说明解压结果与原始文件一致；
- Verilog C/RTL 协同仿真状态为 **Pass**；
- 协同仿真的最小、平均、最大延迟均为 **1343 cycles**。

因此，当前测试证明最终代码在 `sample.txt` 上保持了压缩效果和数据可逆性，同时显著降低了处理延迟。

### 6.3 HLS 综合资源

根据最终 `csynth.xml`：

| 资源 | 使用量 | 器件可用量 | 占比 |
| --- | ---: | ---: | ---: |
| BRAM_18K | 118 | 280 | 42.14% |
| FF | 3,521 | 106,400 | 3.31% |
| LUT | 8,699 | 53,200 | 16.35% |
| DSP | 3 | 220 | 1.36% |
| URAM | 0 | 0 | — |

资源增长符合“资源换延迟”的优化目标：

- 并行候选与字节比较增加 LUT、FF；
- 字典分区和深 FIFO 增加 BRAM；
- 乘法哈希引入少量 DSP；
- 所有资源仍处于目标器件容量范围内。

### 6.4 当前代码的适用边界

当前最终报告只验证了官方 `sample.txt`。因此：

- 压缩比 2.09396 和 1343 cycles 是当前样例结果，不应直接外推到所有输入；
- 乘法哈希改变了候选碰撞关系，建议增加随机数据、重复数据、不可压缩数据和 64 KB 边界数据回归；
- 第一版流式 `lzCompress()` 中 `processingBatch` 当前固定为 1。若同一硬件实例在不复位的情况下重复处理多个独立任务，应进一步验证或完善批次递增与回绕处理，防止旧字典项被误认为当前任务有效；
- 深 FIFO 提高吞吐稳定性，但也是最终 BRAM 使用达到 118 个的重要原因，后续可以在保持 1343 cycles 的前提下继续寻找最小安全深度。

---

## 7. 最终结论

第二问的优化没有改变 `lz4CompressEngineRun` 顶层接口，也没有改变 LZ4 的 Token、Literal、Offset 和 Match Length 格式。优化重点是将软件式的串行搜索重构为适合 FPGA 的并行数据通路：

1. 使用有效标志代替全字典清空，消除大额固定启动延迟；
2. 使用乘法哈希改善字典分布，并对字典进行 BRAM 映射和循环分区；
3. 展开候选和字节比较，并行验证匹配条件；
4. 使用锦标赛结构选择最佳匹配，缩短选择逻辑深度；
5. 使用 Token 查找表、预读取、局部缓存和 Offset 预计算优化编码状态机；
6. 使用 `PIPELINE II=1`、`UNROLL`、`DATAFLOW` 和依赖声明提高并行度；
7. 将关键中间 FIFO 映射到 BRAM，减少流水线阶段之间的背压。

最终在当前样例上保持 **2.09396** 的压缩比，C 仿真无错误，RTL 协同仿真通过；延迟由约 **3390 cycles 降至 1343 cycles，总体下降约 60.4%**，估计时钟周期达到 **12.341 ns**。
