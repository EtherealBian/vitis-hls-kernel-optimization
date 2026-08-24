# 算法与文件索引

本文说明哪些文件实现了哪个算法功能，并区分参赛优化源码、AMD 上游支撑代码、测试文件和结果文件。

## HMAC-SHA256

| 文件 | 类型 | 功能 |
| --- | --- | --- |
| `security/L1/include/xf_security/sha224_256.hpp` | 参赛优化 | SHA-224/SHA-256 补位、消息调度、压缩函数、双核分发和有序合并 |
| `security/L1/include/xf_security/hmac.hpp` | AMD 上游 | HMAC 的 ipad/opad 构造、内外两次哈希数据流 |
| `security/L1/include/xf_security/types.hpp` | AMD 上游 | 密码算子公共数据类型和流结构 |
| `security/L1/include/xf_security/utils.hpp` | AMD 上游 | 位操作及流处理辅助函数 |
| `security/L1/tests/hmac/sha256/test.cpp` | AMD 测试 | `test_hmac_sha256` 顶层与 C 仿真测试入口 |
| `security/L1/tests/hmac/sha256/gld.dat` | 测试数据 | HMAC-SHA256 参考输入与摘要 |
| `security/L1/tests/hmac/sha256/run_hls.tcl` | 复现脚本 | 配置 Zynq-7020、14 ns 时钟并执行 HLS 流程 |
| `security/L1/tests/hmac/sha256/reports/` | 结果 | 最终 C 仿真、综合和协同仿真记录 |
| `baselines/security/sha224_256.hpp` | 基线 | 优化前 SHA-224/SHA-256 实现，用于差异对照 |

## LZ4 压缩

| 文件 | 类型 | 功能 |
| --- | --- | --- |
| `data_compression/L1/include/hw/lz_compress.hpp` | 参赛优化 | LZ77 哈希字典、滑动窗口、候选匹配和最佳匹配归约 |
| `data_compression/L1/include/hw/lz4_compress.hpp` | 参赛优化 | LZ4 序列编码、阶段间流、外存接口和 DATAFLOW 调度 |
| `data_compression/L1/include/hw/lz_optional.hpp` | AMD 上游 | 最佳匹配过滤和匹配扩展等 LZ 公共阶段 |
| `data_compression/L1/include/hw/compress_utils.hpp` | AMD 上游 | 压缩流的数据结构和公共工具 |
| `data_compression/L1/include/hw/mm2s.hpp` | AMD 上游 | AXI 存储器到流的数据搬运 |
| `data_compression/L1/include/hw/s2mm.hpp` | AMD 上游 | 流到 AXI 存储器的数据搬运 |
| `data_compression/L1/include/hw/stream_downsizer.hpp` | AMD 上游 | 宽总线数据拆分为字节流 |
| `data_compression/L1/tests/lz4_compress/lz4_compress_test.cpp` | AMD 测试 | `lz4CompressEngineRun` 顶层、软件解压和结果比较 |
| `data_compression/L1/tests/lz4_compress/sample.txt` | 测试数据 | 最终结果对应的压缩测试样例 |
| `data_compression/L1/tests/lz4_compress/run_hls.tcl` | 复现脚本 | 配置 13.72 ns 时钟并执行压缩核 HLS 流程 |
| `data_compression/L1/tests/lz4_compress/reports/` | 结果 | 最终 C 仿真、综合和协同仿真记录 |
| `baselines/data_compression/` | 基线 | 优化前的 `lz_compress.hpp` 和 `lz4_compress.hpp` |

## Cholesky 分解

| 文件 | 类型 | 功能 |
| --- | --- | --- |
| `solver/L1/include/hw/cholesky.hpp` | 参赛优化 | Cholesky 基础/低延迟架构、复数运算、倒数平方根和三角矩阵输出 |
| `solver/L1/include/hw/matrixDecomposition.hpp` | AMD 上游 | 矩阵分解公共接口和相关辅助结构 |
| `utils/L1/include/xf_utils_hw/` | AMD 上游 | HLS 流和矩阵计算公共硬件工具 |
| `solver/L1/tests/cholesky/kernel/kernel_cholesky_0.cpp` | AMD 测试 | `kernel_cholesky_0` 综合顶层 |
| `solver/L1/tests/cholesky/kernel/kernel_cholesky.hpp` | AMD 测试 | 测试核参数、类型和接口声明 |
| `solver/L1/tests/cholesky/host/test_cholesky.cpp` | AMD 测试 | 矩阵输入、参考结果比较和残差校验 |
| `solver/L1/tests/cholesky/datas/complex_fixed/` | 测试数据 | 复数定点矩阵及其参考分解结果 |
| `solver/L1/tests/cholesky/complex_fixed_arch0/run_hls.tcl` | 复现脚本 | 固定 3×3、上三角、架构 0 和 7.70 ns 时钟约束 |
| `solver/L1/tests/cholesky/complex_fixed_arch0/reports/` | 结果 | 最终 C 仿真、综合和协同仿真记录 |
| `baselines/solver/cholesky.hpp` | 基线 | 优化前 Cholesky 实现 |

## 自动生成 RTL

Vitis HLS 可由上述 C++ 在本地重新生成 Verilog/VHDL。仓库不提交 `.prj/`、综合缓存和自动生成 RTL，因为它们体积大、与工具版本强绑定且不应作为人工维护源。生成 RTL 的行为由三个 `run_hls.tcl` 和归档的 `csynth.xml` 共同记录。
