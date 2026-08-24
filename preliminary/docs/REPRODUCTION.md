# 复现指南

## 1. 环境

- AMD Vitis HLS 2024.2
- 支持器件：`xc7z020-clg484-1`
- Windows 或 Linux；命令行中能够直接调用 `vitis_hls`
- 建议保留至少 10 GB 临时空间，HLS 工程和 RTL 导出不会提交到 Git

归档报告由 Vitis HLS 2024.2 生成。其他版本可能改变 pragma 解释、调度结果和资源估计，因此即使功能验证通过，周期与资源也不一定逐项一致。

## 2. 运行 HMAC-SHA256

```bash
cd security/L1/tests/hmac/sha256
vitis_hls -f run_hls.tcl
```

脚本综合顶层 `test_hmac_sha256`，目标时钟 14.00 ns。C 仿真应验证两组输入且无错误；协同仿真归档结果为 Pass，延迟为 433 cycles。

## 3. 运行 LZ4 压缩

```bash
cd data_compression/L1/tests/lz4_compress
vitis_hls -f run_hls.tcl
```

脚本将 `sample.txt` 压缩到临时的 `sample.txt.encoded`，再通过软件解压比较原文。归档样例压缩比为 2.09396，协同仿真为 Pass，延迟为 1,343 cycles。生成的 `.encoded` 文件已被 `.gitignore` 忽略。

## 4. 运行 Cholesky 分解

```bash
cd solver/L1/tests/cholesky/complex_fixed_arch0
vitis_hls -f run_hls.tcl
```

编译宏固定为：

- `MATRIX_DIM=3`
- `MATRIX_LOWER_TRIANGULAR=0`，输出上三角矩阵
- `SEL_ARCH=0`
- 复数定点测试数据位于 `datas/complex_fixed/`

C 仿真通过条件包括测试程序的误差/残差判断。归档协同仿真为 Pass，单次事务延迟为 175 cycles，八组有效测试的总执行时间为 1,407 cycles；C 综合估计的单次延迟为 179 cycles。由于实现使用近似倒数平方根，移植到其他位宽或矩阵范围时应重新评估数值误差。

## 5. 控制执行阶段

三个脚本开头均提供以下开关：

```tcl
set CSIM 1
set CSYNTH 1
set COSIM 1
set VIVADO_SYN 1
set VIVADO_IMPL 0
```

将不需要的阶段改为 `0` 可缩短运行时间。例如只做 C 仿真时保留 `CSIM=1`，其余设为 `0`。默认不执行布局布线；若开启 `VIVADO_IMPL`，耗时和资源需求会显著增加。

## 6. 结果位置

新运行结果位于各测试目录生成的 `.prj/` 中。仓库内 `reports/` 保存参赛归档结果：

- `csim.log`：C 仿真日志
- `csynth.xml`：顶层时钟、延迟和资源估计
- `cosim.rpt`：C/RTL 协同仿真结果

比较结果时必须同时核对 Vitis 版本、器件、目标时钟、顶层函数、编译宏和测试输入。
