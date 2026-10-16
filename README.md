# OpenHBX

OpenHBX 是面向 OCP High Bandwidth Flash（HBF）的确定性模拟器。当前仓库实现 `OCP_HBF_0_7` 产品路径。

设计说明见 [`docs/README.md`](docs/README.md)，正式实验规则见 [`docs/实验与测试执行规范.md`](docs/实验与测试执行规范.md)。

## 1. 使用 Conda 在宿主机构建和执行

所有命令均从仓库根目录执行。项目要求 CMake 3.16 或更高版本，并使用 C++17 编译器。可以按下面的示例创建独立 Conda 环境（推荐 GCC 11）：

```bash
conda create -n openhbf -c conda-forge \
  cmake=3.27 ninja=1.11 make=4.4 \
  gcc_linux-64=11 gxx_linux-64=11 python=3.11
conda activate openhbf
```

也可以使用已有环境，只要其中工具版本满足要求。激活后，可用下列命令检查：

```bash
cmake --version
c++ --version
make --version
python --version
```

使用 Conda 构建时，构建目录为 `build/tests/`：

```bash
cmake --fresh -S . -B build/tests \
  -DOPENHBF_BUILD_TESTS=ON \
  -DOPENHBF_WITH_RAMULATOR2=OFF \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build/tests -j2
```

`--fresh` 只重建 `build/tests` 的 CMake cache，不会删除 `build/artifacts/` 中的实验证据。

运行全部测试：

```bash
ctest --test-dir build/tests --output-on-failure
```

运行最大 HBF 读带宽实验（实现文件为 `tests/performance/test_hbf_max_read_bandwidth.cpp`）：

```bash
ctest --test-dir build/tests \
  -R '^openhbx_hbf_max_read_bandwidth_test$' \
  --verbose
```

该测试覆盖16个Host Channel，每Channel拥有256个可并发Bank，共向4096个Bank各发出一个4 KiB读请求。运行时每2000个simulation cycles自动输出一次区间读写字节数和GB/s，无需从最终结果手工换算瞬时带宽。

完成一次构建后，也可以在仓库根目录直接使用快速命令：

```bash
make bandwidth
```

`make bandwidth` 只运行已经构建好的 `openhbx_hbf_max_read_bandwidth_test`，不会自动重新编译；首次运行或源码有变化时，先在仓库根目录执行 `CONDA_ENV="$CONDA_PREFIX" make build`。

构建产物统一放在 `build/tests/`，不会写入测试源码目录 `tests/`。`build-host/` 不再作为常规构建目录。

## 2. 使用 Docker 构建和执行

需要固定容器工具链时，首次构建开发镜像：

```bash
docker compose up -d --build dev
```

以后只需启动已有容器：

```bash
docker compose up -d dev
```

### 2.1 直接使用 Docker Compose

直接使用 Docker Compose 执行：

#### 2.1.1 配置和编译

```bash
docker compose exec -T dev cmake --fresh -S . -B build/tests \
  -DOPENHBF_BUILD_TESTS=ON \
  -DOPENHBF_WITH_RAMULATOR2=OFF \
  -DCMAKE_BUILD_TYPE=Debug

docker compose exec -T dev cmake --build build/tests -j2
```

#### 2.1.2 运行全部 HBF 读带宽实验

```bash
docker compose exec -T dev ctest \
  --test-dir build/tests \
  -R '^openhbx_hbf_max_read_bandwidth_test$' \
  --verbose
```

`--verbose`用于显示实验成功时的吞吐、延迟和检查结果；不加该参数时，CTest通常只显示通过或失败。

#### 2.1.3 运行全部测试

```bash
docker compose exec -T dev ctest \
  --test-dir build/tests \
  --output-on-failure
```

### 2.2 进入 Docker 后使用 CMake 和 Make

这种方法适合需要连续编译、运行和调试的开发过程。

#### 2.2.1 进入容器

```bash
docker compose exec dev bash
```

#### 2.2.2 配置和编译

```bash
mkdir -p build
cd build

cmake --fresh .. -B tests \
  -DOPENHBF_BUILD_TESTS=ON \
  -DOPENHBF_WITH_RAMULATOR2=OFF \
  -DCMAKE_BUILD_TYPE=Debug

cd tests
make -j2
```

`cmake .. -B tests`表示：

- `..`：源码位于当前 `build/` 的上一级；
- `-B tests`：生成物写入 `build/tests/`；
- `make -j2`：使用两个并行任务编译。

#### 2.2.3 运行全部 HBF 读带宽实验

此时位于 `build/tests/`：

```bash
ctest -R '^openhbx_hbf_max_read_bandwidth_test$' --verbose
```

也可以直接运行实验程序：

```bash
./tests/performance/openhbx_hbf_max_read_bandwidth_test
```

正式验证推荐使用 CTest；直接运行二进制主要用于调试。

## 4. 如何理解读带宽实验结果

当前 `openhbx_hbf_max_read_bandwidth_test` 的核心输出如下：

```text
experiment:
  id:                         EXP-PERF-HBF-MAX-READ
  evidence:                   E4-model-resource candidate

measurement:
  requests:                   4096 reads
  transferred:                16.000 MiB
  window:                     8869 cycles (8869.000 ns)
  throughput:                 1891.669 GB/s (1.892 TB/s)
  raw_link_ceiling:           4096.000 GB/s (4.096 TB/s)
  raw_link_utilization:       46.183 %
  ocp_user_target:            3072.000 GB/s (3.072 TB/s)
  ocp_target_achievement:     61.578 %

checks:
  completed_requests:         PASS (4096/4096)
  completed_bytes:            PASS (16777216 B)
  payload_integrity:          PASS
  outstanding_after_drain:    PASS (0)
  failures:                   PASS (0)
  observed_le_raw_ceiling:    PASS

result: PASS
```

结果说明：当前synthetic拓扑为16 Core Die、每Core Die 16 Die、每Die 16 Bank。16个Host Channel均匀拥有全部4096个物理Bank，即每Channel 256 Bank。实验向每个Bank并发发出一个4 KiB读请求，共完成4096个请求并传输16 MiB；2026-09-20测量窗口为8869个模拟周期，得到1.892 TB/s。该结果低于模型的4.096 TB/s raw link上限，达到3.072 TB/s OCP用户带宽参考值的61.578%。

该数字相比重构前的1.888 TB/s（8884 cycles，2026-09-03）略有提升，原因是Channel分片重构删除了无规范依据的全局die总线：介质路径改为每slice私有的`ChannelMediaPath(channel, core_die)` EAT，不同Host Channel的读命令不再在Die级被串行化（ADR-CHAN-002，见 [`docs/HBF_CHANNEL_PARTITIONED_MODEL.md`](docs/HBF_CHANNEL_PARTITIONED_MODEL.md)）。本配置下提升幅度有限，因为瓶颈仍在raw link侧（利用率46%），而非介质路径。

测量期间还会自动输出周期日志，例如：

```text
[OpenHBX][bandwidth] cycles=105357-107357 interval_cycles=2000 read_bytes=6819840 read_GBps=3409.920 ...
[OpenHBX][bandwidth] cycles=107357-109357 interval_cycles=2000 read_bytes=6901760 read_GBps=3450.880 ...
```

周期日志表示相邻2000-cycle窗口内实际完成的字节和带宽；最终`measurement`表示从开始注入到全部完成的批次平均带宽，两者口径不同。

这里的1.892 TB/s是当前配置和有限批次下的 **transaction-level模拟结果**。模型将OCP目标作为报告参考，尚未覆盖所有真实协议和芯片开销，因此不能把它解释为硅片保证值。

CTest还会把机器可读结果保存到 `build/artifacts/performance/EXP-PERF-HBF-MAX-READ/ctest/bandwidth.json`。

## 5. 使用短命令运行测试

根目录 `Makefile` 默认使用当前激活 Conda 环境中的 CMake 和 CTest；也可以通过 `CONDA_ENV=/path/to/env` 指定环境路径。

### 5.1 查看所有测试

```bash
make list-tests
```

当前会列出21项测试及其精确名称。

### 5.2 运行一个测试

```bash
make test-one TEST=openhbx_hbf_max_read_bandwidth_test
```

最大读带宽实验也提供了更短的等价命令：

```bash
make bandwidth
```

该命令不重新配置或编译，直接运行已有的`openhbx_hbf_max_read_bandwidth_test`，输出聚合吞吐、理论上限利用率和延迟分位数。首次运行或修改源码后先执行一次`make build`。

将 `TEST` 替换为 `make list-tests` 显示的任意名称即可。单项测试默认使用verbose输出，因此能看到成功实验的具体结果。

### 5.3 常用测试组

```bash
make test                  # 全部测试
make test-system           # 系统测试
make test-conformance      # OCP和证据一致性测试
make test-performance      # 所有性能测试
```

主要单项测试：

| 测试目的 | 测试名称 |
|---|---|
| EventQueue、completion和日志 | `openhbx_system_event_kernel_test` |
| 完整系统生命周期 | `openhbx_s8_system_test` |
| OCP transaction vectors | `openhbx_s9_ocp_transaction_vectors` |
| 单endpoint Flash读带宽 | `openhbx_flash_read_bandwidth_test` |
| 全部HBF读带宽 | `openhbx_hbf_max_read_bandwidth_test` |

## 6. 运行 Trace 模拟器

从仓库根目录执行：

```bash
make run
```

等价的直接命令是在 `build/tests/` 中执行：

```bash
./openhbx_trace_runner \
  ../../configs/products/ocp_hbf_0_7.yaml \
  ../../tests/traces/write_read.trace
```

一次完整运行的摘要示例：

```text
config_hash=openhbx-fnv1a64-v1:74b02c1c73517cc2 accepted=65 completed=65 cycles=99
```

`accepted=completed=65`说明所有已接受请求都产生了终态，没有遗留请求；`cycles=99`是本次工作负载使用的模拟周期数；`config_hash`与最大带宽实验一致，说明两者读取了同一个产品配置。

完成的请求逐行打印到 stdout：`token completed_cycle command_status payload_bytes payload_fnv1a64`，最后一列是返回载荷的 FNV-1a64 哈希，可用于写读回环的数据正确性校验。支持的环境变量：`OPENHBX_RESOLVED_CONFIG_OUT`（导出 resolved config）、`OPENHBX_RUN_SUMMARY_OUT`（导出运行摘要 JSON）、`OPENHBX_LOG_LEVEL=error|warn|info|debug|trace|off`（控制控制台日志级别，默认 `warn`）。

## 7. 日志与输出目录

OpenHBX使用结构化`ObservedEvent`。文本和JSONL日志用于诊断，不作为正确性判据。详细说明见 [`docs/OPENHBX_LOGGING_AND_OBSERVABILITY.md`](docs/OPENHBX_LOGGING_AND_OBSERVABILITY.md)。

默认控制台日志为 ramulator 风格单行格式（写 `std::clog`）：

```text
[open_hbx_system] [debug] cycle=79085 phase=final_delivery op=respond stage=staged result=success token=1
```

`OpenHbxSystem` 构造时默认安装该 sink（默认级别 `Warn`，即只打印 Warn/Error）；`set_log_level` 运行时调整，`set_log_sink({})` 关闭。插桩点：compose/reset（Info）、准入 Busy/Rejected（Debug/Warn）、响应 staged（Debug）、EventQueue schedule/dispatch（Trace）、callback 异常（Error）。

| 内容 | 目录 |
|---|---|
| CMake构建文件 | `build/tests/` |
| 正式实验结果 | `build/artifacts/<suite>/<experiment-id>/<run-id>/` |
| 测试源码 | `tests/` |

不要在仓库根目录、`src/`、`include/`、`tests/`、`docs/`或`configs/`中生成可执行文件、目标文件、日志或实验数据。
