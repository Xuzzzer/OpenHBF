# OpenHBX HBF 验证与一致性概要设计

**文档版本**：V1.0
**文档位置**：`docs/HLD_08_VERIFICATION_CONFORMANCE.md`  
**对应文档**：`docs/LLD_08_VERIFICATION_CONFORMANCE.md`  
**设计状态**：🟡 `PARTIAL`（`status: PARTIAL`）
**适用基线**：OCP HBF v0.7.0；首期 `OCP_HBF_0_7` production pipeline

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 建立首期HBF配置、协议、全链路和E4一致性验证 |
| V1.1 | 2026-08-30 | 按S0-S8 artifact审计状态并接入S9 transaction-level验证资产 |

## 1. 模块概述

08负责证明首期HBF实现满足配置契约、OCP可定位规则与OpenHBX跨模块不变量。它拥有test harness、oracle、manifest和artifact schema，不拥有生产状态或产品行为。测试通过公开接口、只读snapshot及受控fault hook观察；fake测试最高E2，完整HBF合规结论必须来自真实production pipeline的E3/E4。

关键不变量：test oracle独立于被测实现；相同binary/config/seed/workload产生稳定digest；Accepted=Terminal+Outstanding；Busy/Rejected零副作用；completion-time commit；reset隔离旧generation；失败证据可重放。

## 2. 需求回顾与追踪

| 需求ID | 需求描述 | 优先级 | 版本 | 实现状态 |
|---|---|---|---:|---|
| VER-001 | schema/profile/非法跨字段配置在启动前判定 | P0 | V1.0 | 🟢 `VERIFIED`（E1/E2） |
| VER-002 | OCP Host 64 B/DLU边界、ordering/status按golden验证 | P0 | V1.0 | 🟡 `PARTIAL` |
| VER-003 | 真实01至07全链路payload与terminal因果正确 | P0 | V1.0 | 🟢 `VERIFIED`（E3 transaction-level） |
| VER-004 | fault/reset/stale/duplicate completion无泄漏或双完成 | P0 | V1.0 | 🟢 `VERIFIED`（S8已覆盖的reset/duplicate范围） |
| VER-005 | 重复运行observable digest一致 | P0 | V1.0 | 🟢 `VERIFIED`（E3） |
| VER-006 | Host、TSV、Bank和Media吞吐不越理论ceiling | P1 | V1.0 | 🟡 `PARTIAL`（单endpoint synthetic Fabric ceiling候选证据） |
| VER-007 | 每项E4均链接规范原文/vector和可重放artifact | P0 | V1.0 | 🟢 `VERIFIED`（仅当前已提取字段范围） |
| VER-008 | 未提取OCP字段不得由协议常识补齐 | P0 | V1.0 | 🔴 `BLOCKED_SPEC` |

状态图标仅用于阅读；机器可读状态是反引号中的`PLANNED`或`BLOCKED_SPEC`。

### 2.1 当前证据基线

| Stage | 最高证据 | 权威artifact | Docker/CTest结果 | 允许声明 |
|---|---:|---|---|---|
| S0 | E0 | `build/artifacts/stages/s0/EXP-S0-BUILD-CONTRACT/20260828T145000Z/` | `openhbx_build_contract` 1/1 | 构建骨架 |
| S1 | E1 | `build/artifacts/stages/s1/EXP-S1-EVENT-KERNEL/20260828T145833Z/` | `openhbx_s1` 1/1 | EventKernel局部契约 |
| S2 | E1/E2 | `build/artifacts/stages/s2/EXP-S2-HBF-CONFIG/20260828T150916Z/` | `openhbx_s2` 2/2 | 配置与事务式装配 |
| S3 | E1/E2 | `build/artifacts/stages/s3/EXP-S3-NAND-MEDIA/20260828T160000Z/` | `openhbx_s3` 3/3 | NAND/RAS组件；旧`153000Z`已废止 |
| S4 | E1/E2 | `build/artifacts/stages/s4/EXP-S4-HBF-ADDRESS/20260828T153231Z/` | `openhbx_s4` 2/2 | 地址/拓扑组件 |
| S5 | E1/E2 | `build/artifacts/stages/s5/EXP-S5-INTERCONNECT-PAL/20260830T074707Z/` | `openhbx_s5` 2/2 | PAL/Fabric组件 |
| S6 | E1/E2 | `build/artifacts/stages/s6/EXP-S6-BASE-DIE-CONTROLLER/20260830T081326Z/` | `openhbx_s6` 2/2 | Controller组件 |
| S7 | E1/E2 | `build/artifacts/stages/s7/EXP-S7-OCP-HBF-HOST/20260830T084719Z/` | `openhbx_s7` 2/2 | transaction-level Host组件 |
| S8 | E3 | `build/artifacts/stages/s8-current/EXP-S8-OPENHBX-SYSTEM/20260830T101705Z/` | Ramulator OFF/ON完整suite 20/20 | 真实01至07 pipeline、65/65守恒、三次digest |

S8旧路径`build/artifacts/stages/s8-legacy/EXP-S8-OPENHBX-SYSTEM/20260830T091149Z/`已由handoff明确废止。S9测试源码存在不构成E4证据；必须由Docker CMake/CTest成功和完整artifact bundle共同升级状态。

S9于2026-08-30通过Docker Debug构建及`ctest -L openhbx_s9 --output-on-failure`，3/3通过；权威artifact为`build/artifacts/stages/s9/EXP-S9-CONFORMANCE/20260830T103513Z/`。其中真实pipeline vector是E3及E4候选字段证据，受控Controller的Host oracle是E2，manifest audit是发布门禁。本结果不得表述为完整OCP conformance。

后续Docker性能门禁`openhbx_flash_read_bandwidth_test`已通过：真实production path在单Host Channel、单AXI Interface、单Bank synthetic profile下，1次warmup后完成15次4 KiB read，共61440 B、8146 simulation cycles，测得`7.542352 B/cycle`，不超过按Fabric forward/return资源公式得到的`7.699248 B/cycle` ceiling。OpenHBX复用既有Ramulator adapter默认值，将synthetic baseline冻结为`tCK=1000 ps`（1 ns/cycle、1 GHz），因此该次模型换算值为`7.542352 GB/s`。该结果只构成`E4-model-resource`候选证据；`tCK`是建模假设而非OCP/vendor保证值，且未覆盖多endpoint、Host/Bank/Media分别饱和或扩展趋势，`VER-006`继续保持`PARTIAL`。

当前`openhbx_hbf_max_read_bandwidth_test`使用完整synthetic资源形状：16 Host Channel、每Channel 4个共享物理带宽的虚拟AXI、16 Core Die、每Core Die 16 Die、每Die 16 Bank。4096个物理Bank均匀分配后每Channel拥有256个Bank，并保持每Channel独立的x64 lane/repair域。真实Host写链路准备4096个不同Bank页面，reset清除Controller cache后同时读取4096个4 KiB页面。2026-09-03 Docker结果为16777216 B/8884 ns，即`1888.475462 GB/s`（`1.888475 TB/s`）；相对4096 GB/s raw ceiling利用率`46.1054%`，相对OCP 3072 GB/s用户目标为`61.4738%`。测试还从测量起点每2000 simulation cycles自动报告区间完成字节和读写GB/s，用于观察流水线填充、稳态和排空；正式oracle仍使用结构化stats和完整批次守恒。

| 需求ID | 来源/类别 | owner组件 | 验收/Test ID | 证据目标 |
|---|---|---|---|---|
| VER-001 | TASK1第10节；设计选择 | Config suite | `VER-CFG-*` | E1/E3 |
| VER-002 | OCP Host Interface摘要；规范事实 | Protocol suite | `VER-OCP-*` | E4 |
| VER-003 | OpenHBX不变量 | System suite | `VER-SYS-*` | E3 |
| VER-004 | 01至07 HLD契约 | Fault suite | `VER-FAULT-*` | E3 |
| VER-005 | OpenHBX确定性要求 | Determinism suite | `VER-DET-001` | E3 |
| VER-006 | 实验规范E4 | Performance suite | `VER-PERF-*` | E4 |
| VER-007 | 实验规范artifact门禁 | Conformance manifest | `VER-E4-AUDIT` | E4 |
| VER-008 | `SPEC_EXTRACT_REQUIRED` | Spec audit | `VER-SPEC-GAP` | audit/解除阻断 |

## 3. 系统架构设计

```text
Test Manifest -> Driver -> Real OpenHbxSystem (01..07) -> Completion
      |                         | public event/snapshot
      +-> Scoreboard/EventOracle/InvariantChecker
      +-> ArtifactWriter -> replay bundle / E4 evidence
```

| 组件 | 主要文件 | 职责 | 输入 | 输出 | 拥有状态 | 依赖 |
|---|---|---|---|---|---|---|
| `SystemHarness` | `system_harness.*` | 真实对象图、推进/drain | manifest/workload | observations | test session | production APIs |
| `PayloadScoreboard` | `scoreboard.*` | 独立logical image | accepted/terminal | byte oracle | expected image | pattern |
| `EventOracle` | `event_oracle.*` | 因果/确定性 | public events | edge/digest | observation log | stable schema |
| `InvariantChecker` | `invariant_checker.*` | 守恒检查 | snapshots/stats | failure witness | samples | module snapshots |
| `ArtifactWriter` | `artifact_writer.*` | E3/E4证据 | metadata/results | replay bundle | output session | experiment spec |

### `SystemHarness`

**文件路径**：`tests/verification/support/system_harness.h`、`tests/verification/support/system_harness.cpp`

**职责**：通过公开factory装配并驱动真实01至07。

**关键组件**：`submit()`、`advance()`、`drain()`、`snapshot()`、`finish()`。

**输入与输出**：manifest/workload -> terminal、event和snapshot。

**状态与所有权**：拥有test session，不拥有生产状态。

**依赖与边界**：依赖production public APIs，不使用私有状态捷径。

**对应需求**：VER-003、VER-004。

**实现状态**：⚪ `PLANNED`。

### `PayloadScoreboard`

**文件路径**：`tests/verification/support/scoreboard.h`、`tests/verification/support/scoreboard.cpp`

**职责**：维护独立committed logical image。

**关键组件**：payload pattern、expected operation和difference reporter。

**输入与输出**：accepted/terminal -> byte或signature difference。

**状态与所有权**：拥有测试期望图，不写生产payload。

**依赖与边界**：只依赖公开completion；TimingOnly不提供逐byte结论。

**对应需求**：VER-003。

**实现状态**：⚪ `PLANNED`。

### `EventOracle`与`InvariantChecker`

**文件路径**：`tests/verification/support/event_oracle.h`、`tests/verification/support/event_oracle.cpp`、`tests/verification/support/invariant_checker.h`、`tests/verification/support/invariant_checker.cpp`

**职责**：检查因果、确定性和跨模块守恒。

**关键组件**：required edges、stable digest、Accepted等式、credit/token/payload/event checker。

**输入与输出**：public events/snapshots -> pass或首个witness。

**状态与所有权**：只拥有有界观察记录。

**依赖与边界**：依赖versioned observation schema，不依赖日志文本或pointer。

**对应需求**：VER-002至VER-005。

**实现状态**：⚪ `PLANNED`。

### `ArtifactWriter`

**文件路径**：`tests/verification/support/artifact_writer.h`、`tests/verification/support/artifact_writer.cpp`

**职责**：校验manifest并原子提交E3/E4 bundle。

**关键组件**：`begin()`、`append()`、`finish_success()`、`finish_failure()`。

**输入与输出**：metadata/events/results -> replayable artifact。

**状态与所有权**：拥有当前run output session。

**依赖与边界**：依赖实验规范和build artifact root；不得写入源码目录。

**对应需求**：VER-007。

**实现状态**：⚪ `PLANNED`。

### Conformance与系统测试组件

**文件路径**：`tests/verification/config/test_config_conformance.cpp`、`tests/verification/conformance/test_ocp_vectors.cpp`、`tests/verification/system/test_hbf_pipeline.cpp`、`tests/verification/system/test_fault_reset.cpp`、`tests/verification/system/test_determinism.cpp`、`tests/verification/performance/test_resource_ceilings.cpp`

**职责**：覆盖配置、OCP vector、真实全链路、fault/reset、determinism和resource ceiling。

**关键组件**：Config、Conformance、System、Fault、Determinism和Performance suites。

**输入与输出**：冻结manifest/workload/fault -> 自动oracle及artifact。

**状态与所有权**：08拥有测试资产，不拥有产品运行状态。

**依赖与边界**：依赖真实01至07；Media只接受冻结的五级HBF层次，NOR不在测试范围。

**对应需求**：VER-001至VER-007。

**实现状态**：VER-001/003至007为⚪ `PLANNED`；未提取的VER-002 vector为🔴 `BLOCKED_SPEC`。

## 4. 高层详细设计

配置suite校验canonical dump、unknown字段、单位、capability和HBF-only约束。协议suite使用从OCP原文冻结的vector；未完成提取的字段进入gap manifest而非猜测expected。系统suite提交read/write/admin序列，scoreboard只在成功terminal更新committed image。fault suite在稳定event phase注入ECC/raw media/TSV/reset错误。determinism suite独立运行至少三次比较规范化event digest。performance suite按资源公式构造单瓶颈负载。

E4 evidence必须记录规范定位、production path、resolved config hash、seed/workload hash、binary/revision、oracle与artifact。hybrid bonding不属于首期合规矩阵。

## 5. 接口设计

| 接口 | 方向 | 契约 | 错误/边界 | 所有权 |
|---|---|---|---|---|
| `Harness::submit` | test->system | 不代替driver隐藏重试 | Backpressured由runner保留 | Accepted才交系统 |
| `Harness::drain` | test->system | 有界cycle并报告非idle owner | 超预算失败而非假成功 | harness拥有session |
| `Oracle::record` | production tap->oracle | 只读稳定event | unknown schema拒绝 | oracle复制值 |
| `FaultPort::inject` | suite->生产hook | target/phase/cycle显式 | 非法组合加载失败 | 生产模块拥有实际fault状态 |
| `ArtifactWriter::finish` | harness->artifact | 成功失败均完整 | 缺必填字段测试失败 | 原子提交bundle |

## 6. 数据结构设计

`ExperimentManifest`包含ID、requirement、evidence level、production path、revision/image/config/workload/seed、oracle、budget和artifacts。`ExpectedOperation`、`ObservedEvent`、`ConservationSample`与`Difference`只用稳定ID/cycle/enum/bytes，不含pointer或wall-clock。`SpecVector`必须含OCP section/table/field定位和extract hash。

## 7. 关键流程与状态机

```text
Load manifest -> validate -> build real system -> run bounded workload
 -> online invariants -> drain -> final oracle -> write artifact -> pass/fail
config/spec gap -> fail before build or mark BLOCKED_SPEC
timeout -> capture nonidle owners + event window -> fail
```

reset/fault test覆盖事件前、同cycle及事件后；同cycle顺序来自02公开phase。duplicate/stale观察必须产生计数但不得改变terminal/payload。

## 8. 性能与资源设计

测试按simulation cycle预算，不以宿主秒数判功能；timeout仅hang guard。resource ceiling分别计算Host link、PAL/TSV、Bank array、NAND stage与controller throughput，系统上限为最小值。报告warmup/window、样本数、分位数和误差来源。harness内存为有界event ring加必要scoreboard；大负载使用signature抽查但不能冒充全payload验证。

当前实现的`tests/performance/test_flash_read_bandwidth.cpp`验证单endpoint基线；full-duplex ceiling使用`4096/max(forward_command_cycles, return_data_cycles)`，禁止再把两个独立方向的占用串行相加。`tests/performance/test_hbf_max_read_bandwidth.cpp`验证16 Channel、多AXI共享Channel resource、多Bank流水饱和。两者均不是vendor wall-clock或bit-accurate PHY证明；报告GB/s时必须同时展示`tCK`、line rate、效率和来源等级。

## 9. 错误、恢复与可观测性

失败bundle至少包含`failure.json,event-window.jsonl,outstanding.json,replay.sh`及实验规范最小artifact。首个分歧记录token、generation、expected/actual、cycle和owner。oracle自身异常与产品失败分开分类。测试不得直接改private state、读取未提交payload或依赖日志文本作为唯一oracle。

## 10. 测试与验收

| Test族 | production path | 核心oracle | 证据 |
|---|---|---|---|
| VER-CFG-* | ConfigResolver/ProductComposer | canonical parity与非法拒绝 | E1/E3 |
| VER-OCP-* | real Host/Controller/Mapper | golden vector、status/order | E4 |
| VER-SYS-* | real 01->07 | payload、exactly-once、drain | E3 |
| VER-FAULT-* | real pipeline+fault ports | typed result、零半提交、守恒 | E3 |
| VER-DET-001 | real pipeline三次 | digest逐项相同 | E3 |
| VER-PERF-* | real pipeline | observed<=resource ceiling | E4 |
| VER-E4-AUDIT | evidence index | 每条E4定位与bundle完整 | E4 |

实验执行、证据保存和状态更新统一遵循[`实验与测试执行规范.md`](实验与测试执行规范.md)。在`VER-008`缺口关闭前，只能对已提取字段逐项声明一致，不得声明完整OCP conformance。

读取 latency 的测量区间从 Host request 被 `RequestBridge` 接受时的模拟 cycle 开始，到 `SystemCompletion.completed_at` 结束，包含完整 production path。最近一次 Conda/CTest 复现为 15 次 read、61440 B、67996 cycles、P50=13598 cycles、P95=13599 cycles、0.903583 GB/s；该数字仅适用于 synthetic profile。
