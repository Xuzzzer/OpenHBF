# OpenHBF DRAM 扩展：第三轮代码迁移与实施计划

## 1. 迁移策略

不从零重写 DRAM。采用“vendor subtree + OpenHBF adapter”的结构：

```text
thirdparty/ramulator2/                 # 上游镜像/子模块，保留原布局
src/dram/ramulator2_adapter/           # OpenHBF ABI 防腐层
include/openhbx/dram/                  # 稳定 typed API
src/config/                            # DramConfig/能力闭包
src/system/                            # clock bridge 和生命周期接入
tests/unit/dram/                       # adapter E1
tests/component/dram/                  # 真实 Ramulator E2
tests/system/                          # HBF + DRAM E3
```

不把 Ramulator 的生成 C++ 文件复制到 `src/`；若最终必须 vendor，复制整个可生成闭包（Python DSL、generator 输入、生成规则、C++输出和许可证），并在 manifest 固定上游 revision。

## 2. 代码复用清单

| 目标能力 | 复用路径 | 处理方式 |
|---|---|---|
| DRAM 标准参数 | `python/ramulator/dram/*.py` | 作为上游输入保留，构建时生成 |
| DRAM device/state | `src/ramulator/dram/` | 通过 adapter 间接链接 |
| 通用 controller | `src/ramulator/controller/impl/` | 通过 `IMemorySystem` 创建 |
| scheduler/row/refresh | `src/ramulator/controller/*` | 原样复用并注册 |
| channel mapper | `src/ramulator/memory_system/channel_mapper/` | 仅用于 DRAM channel，不能代替 HBF mapper |
| GenericDRAM system | `src/ramulator/memory_system/impl/` | 封装为 `RamulatorDramBackend` |
| frontend/gem5 wrapper | `frontend/`、`resources/gem5_wrappers/` | 首版不接入 |

## 3. 建议新增接口

### 3.1 `include/openhbx/dram/dram_types.h`

定义 `DramEndpointId`、`RamulatorChannelId`、`DramRequest`、`DramCompletion`、`DramStatus`、`DramStatsSnapshot`。类型只包含稳定值，不暴露 Ramulator `Request`、裸指针和内部地址节点。

### 3.2 `include/openhbx/dram/dram_backend.h`

定义：

```cpp
class IDramBackend {
 public:
  virtual AdmissionResult submit(DramRequest request) = 0;
  virtual void tick(uint64_t openhbf_cycle) = 0;
  virtual void reset(uint64_t generation) = 0;
  virtual DramSnapshot snapshot() const = 0;
  virtual ~IDramBackend() = default;
};
```

`submit` 只表示 backend 接受或拒绝排队，不表示 DRAM 完成。完成通过 typed sink/callback 回到 HBF Controller。

### 3.3 `src/dram/ramulator2_adapter/`

建议文件：

- `ramulator_dram_backend.{h,cpp}`：创建 `GenericDRAMSystem`、提交 request、推进 tick、收集 callback；
- `dram_endpoint_map.{h,cpp}`：HBF endpoint 到 Ramulator channel 的显式映射；
- `dram_clock_bridge.{h,cpp}`：OpenHBF cycle 到 Ramulator tick 的 ratio/phase；
- `dram_request_splitter.{h,cpp}`：4 KiB 上层请求到 tx bytes 子请求的拆分和聚合；
- `ramulator_config_bridge.{h,cpp}`：Resolved DramConfig 到 ConfigNode/上游配置的转换；
- `ramulator_completion_sink.{h,cpp}`：generation/token 校验和 exactly-once completion。

## 4. 分阶段实现

### S0：基线和许可证

冻结当前 Ramulator submodule revision、构建器版本、许可证和生成命令。记录 `git status`，不覆盖现有用户改动。

### S1：独立 backend smoke

只构建 `RamulatorDramBackend`，运行一个 DDR4/DDR5 controller、一个 channel、固定 read/write workload。验证 submit、tick、callback、stats 和 reset；证据 E1/E2。

### S2：映射和粒度

加入 `DramEndpointMap`、clock bridge 和 request splitter。覆盖一对一、多 endpoint 共享 channel、超过 tx bytes、边界地址、拒绝零副作用；证据 E1/E2。

### S3：OpenHBF controller/PAL 接入

在 PAL backend registry 中增加 `ramulator2`，保留现有 NAND backend。只允许 HBF Controller 通过 `IDramBackend` 访问 DRAM，验证 token、generation、payload 和 completion-time commit。

### S4：真实 HBF + DRAM 系统

将配置、Host admission、HBF address、Controller、DRAM backend、EventKernel 连接起来。运行小规模 read/write、backpressure、reset/stale completion 和 drain；目标 E3。

### S5：性能和回归

分别报告 HBF Host、DRAM channel、bank、controller queue 和 backend 的资源上限。复用 Ramulator 的 device timing/controller scheduling 测试，并新增 OpenHBF CTest labels 和 artifact manifest；性能证据目标 E4-model-resource。

## 5. CMake 集成建议

- 新增 `OPENHBF_WITH_RAMULATOR2` 的明确 backend 选项，默认值保持现有行为不变。
- Ramulator target 使用其所需的语言标准和生成步骤隔离在 thirdparty/adapter target；OpenHBF core 继续 C++17、`-Wall -Wextra -Wpedantic -Werror`。
- `src/dram/ramulator2_adapter` 只链接公开或稳定的 Ramulator interface target。
- 测试使用 `build/tests-dram/` 或规范允许的隔离构建目录；artifact 统一在 `build/artifacts/dram/...`。
- 禁止把生成文件写入 `src/`、`include/`、`tests/` 或 `docs/`。

## 6. 测试矩阵

| Test ID | 内容 | 证据 |
|---|---|---|
| DRAM-UNIT-001 | endpoint/channel 显式映射 | E1 |
| DRAM-UNIT-002 | tx bytes splitter/aggregator | E1 |
| DRAM-COMP-001 | 真实 Ramulator DDR 请求生命周期 | E2 |
| DRAM-COMP-002 | clock ratio/tick/completion | E2 |
| DRAM-SYS-001 | OpenHBF Host -> DRAM -> terminal | E3 |
| DRAM-SYS-002 | reset/stale/duplicate completion | E3 |
| DRAM-PERF-001 | channel/bank/controller ceiling | E4-model-resource |
| DRAM-AUDIT-001 | 上游 revision、生成链和许可证审计 | audit |

所有实验都必须固定 config、seed、workload、binary hash、环境、cycle budget 和 replay command，并保存规范要求的 artifact 集合。
