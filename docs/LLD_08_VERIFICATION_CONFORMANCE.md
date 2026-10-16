# OpenHBX HBF 验证与一致性详细设计

**文档版本**：V1.0
**文档位置**：`docs/LLD_08_VERIFICATION_CONFORMANCE.md`  
**对应HLD**：`docs/HLD_08_VERIFICATION_CONFORMANCE.md`  
**设计状态**：🟡 `PARTIAL`（`status: PARTIAL`）
**覆盖需求**：`VER-001`至`VER-008`

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 首期HBF验证工具、测试族与E4证据设计 |
| V1.1 | 2026-08-30 | 接入S9 vector、admission oracle、gap manifest和证据schema |

## 1. 模块概述与约束

本LLD只设计测试代码与manifest，不改变01至07生产状态。正式构建和测试默认在Conda环境中通过CMake/CTest执行，Docker作为固定工具链的可选复现路径；artifact只写`build/artifacts/<suite>`。fake生产组件最多E2，E3/E4必须链接真实`OpenHbxSystem`。

| 内容类别 | 仓库相对存储位置 | 存储规则 | 状态 |
|---|---|---|---|
| 本LLD/HLD | `docs/LLD_08_VERIFICATION_CONFORMANCE.md`、`docs/HLD_08_VERIFICATION_CONFORMANCE.md` | 版本控制文档 | ⚪ `PLANNED` |
| Verification support | `tests/verification/support/` | 测试公共工具 | ⚪ `PLANNED` |
| Test suites | `tests/verification/config/`、`tests/verification/conformance/`、`tests/verification/system/`、`tests/verification/performance/` | 测试源码 | ⚪ `PLANNED` |
| Manifests | `tests/verification/manifests/` | 只读输入资产 | ⚪ `PLANNED` |
| 正式artifact | `build/artifacts/<suite>/<experiment-id>/<run-id>/` | 仅gitignored build目录 | ⚪ `PLANNED` |

## 2. 需求到实现映射

| 需求ID | HLD章节 | 文件/符号 | Test ID | 状态 |
|---|---|---|---|---|
| VER-001 | 4 | `test_config_conformance.cpp` | VER-CFG-* | `PLANNED` |
| VER-002 | 4 | `tests/verification/conformance/test_ocp_vectors.cpp` | VER-OCP-* | 🔴 `BLOCKED_SPEC`（未提取字段） |
| VER-003 | 4 | `test_hbf_pipeline.cpp` | VER-SYS-* | `PLANNED` |
| VER-004 | 7 | `test_fault_reset.cpp` | VER-FAULT-* | `PLANNED` |
| VER-005 | 4 | `test_determinism.cpp` | VER-DET-001 | `PLANNED` |
| VER-006 | 8 | `tests/performance/test_flash_read_bandwidth.cpp` | VER-PERF-001 | `PARTIAL`（E4 model-resource候选） |
| VER-007 | 4/9 | `EvidenceIndex` | VER-E4-AUDIT | `PLANNED` |
| VER-008 | 4 | `tests/verification/manifests/spec_gap_manifest.yaml` | VER-SPEC-GAP | 🔴 `BLOCKED_SPEC` |

## 3. 核心类型与数据结构

| 字段 | 类型/单位 | 合法范围 | owner/生命周期 | 不变量 |
|---|---|---|---|---|
| `experiment_id` | stable string | 唯一 | manifest/run | artifact一致 |
| `evidence_level` | E0..E4 | 声明值 | manifest | 不超过production path |
| `cycle_budget` | `uint64_t` cycle | >0 | harness | 功能判据非wall-clock |
| `seed` | `uint64_t` | 固定 | manifest | 所有PRNG派生自此 |
| `event_digest` | stable hash | schema versioned | oracle | 忽略pointer/wall-clock |
| `spec_locator` | section/table/field/hash | E4必填 | vector | 可回查原文 |

`LogicalImage`按`channel,dlu,generation`维护committed payload；pending write不会提前进入期望图。`ConservationSample`保存accepted/terminal/outstanding、credit、token、payload和event数。

## 4. 头文件与公开边界

`tests/verification/support/system_harness.h`装配真实系统；`scoreboard.h`、`event_oracle.h`、`invariant_checker.h`和`artifact_writer.h`为测试工具；`spec_vector.h`读取只读golden数据。生产代码仅可暴露通用snapshot/event/fault接口，不得出现`#ifdef test`修改行为。

## 5. 函数与接口详细设计

### `SubmitResult SystemHarness::submit(Operation op)`

调用真实入口并记录before/after snapshot。Backpressured时op仍归driver，event/token/credit/stats delta必须为零；Accepted时注册scoreboard obligation。harness不循环到成功以免隐藏backpressure。

### `DrainResult SystemHarness::drain(uint64_t max_cycles)`

逐simulation cycle推进至所有owner idle或预算耗尽；后者返回非idle tree与event ring。宿主timeout只由CTest设置为hang guard。

### `CheckResult InvariantChecker::check(const SystemSnapshot&)`

在线检查`accepted=terminal+outstanding`、credit capacity、token/payload唯一owner和非负计数；final检查event queue、queues与contexts全空。首次失败冻结witness，不被后续错误覆盖。

### `Digest EventOracle::digest() const`

按schema规范化event字段和有意允许的无序集合，再计算稳定hash；不得包含地址随机化、pointer、日志时间或宿主调度顺序。

## 6. 内部逻辑、状态机与算法

测试run状态为`Created -> Validated -> Running -> Draining -> Checking -> ArtifactCommitted -> Passed/Failed`。任一步骤异常都进入`Failed`并提交bundle；manifest/schema/spec gap在构造系统前失败。

driver每cycle先提交arrival operation，再按02定义tick phase观察events。scoreboard只在成功terminal应用write；read在data-valid时逐byte或signature比较。fault rule以`target,stage,occurrence,cycle`匹配且记录hit count，未命中也是失败。

## 7. 逐文件设计

| 路径 | 动作 | owner | 职责 | Test/需求 | 状态 |
|---|---|---|---|---|---|
| `tests/verification/support/system_harness.h`、`tests/verification/support/system_harness.cpp` | 新增 | 08 | real system/run/drain | VER-003/004 | `PLANNED` |
| `tests/verification/support/scoreboard.h`、`tests/verification/support/scoreboard.cpp` | 新增 | 08 | payload logical oracle | VER-003 | `PLANNED` |
| `tests/verification/support/event_oracle.h`、`tests/verification/support/event_oracle.cpp` | 新增 | 08 | causality/digest | VER-002/005 | `PLANNED` |
| `tests/verification/support/invariant_checker.h`、`tests/verification/support/invariant_checker.cpp` | 新增 | 08 | conservation | VER-003/004 | `PLANNED` |
| `tests/verification/support/artifact_writer.h`、`tests/verification/support/artifact_writer.cpp` | 新增 | 08 | evidence/replay | VER-007 | `PLANNED` |
| `tests/verification/config/test_config_conformance.cpp` | 新增 | 08 | schema/profile negative matrix | VER-001 | `PLANNED` |
| `tests/conformance/test_ocp_transaction_vectors.cpp` | 新增 | 08 | 已提取transaction-level OCP vectors | VER-002/006 | `VERIFIED`（E3/E4候选字段范围） |
| `tests/conformance/test_host_admission_oracle.cpp` | 新增 | 08 | 同步多child及Busy零副作用E2 oracle | VER-002/004 | `VERIFIED`（E2） |
| `tests/verification/system/test_hbf_pipeline.cpp` | 新增 | 08 | full payload/order/drain | VER-003 | `PLANNED` |
| `tests/verification/system/test_fault_reset.cpp` | 新增 | 08 | fault/reset matrix | VER-004 | `PLANNED` |
| `tests/verification/system/test_determinism.cpp` | 新增 | 08 | triple-run digest | VER-005 | `PLANNED` |
| `tests/performance/test_flash_read_bandwidth.cpp` | 新增 | 08 | 单endpoint Flash read/Fabric ceiling | VER-006 | `VERIFIED`（局部oracle；总体仍PARTIAL） |
| `tests/performance/test_hbf_max_read_bandwidth.cpp` | 新增 | 08 | 16 Channel/4 AXI/256 Bank每Channel，共4096 Bank并发read | VER-006 | `VERIFIED`（模型最大吞吐；总体仍PARTIAL） |
| `tests/conformance/spec_gap_manifest.json` | 新增 | 08 | 未提取字段/解除条件 | VER-008 | `BLOCKED_SPEC` |
| `tests/conformance/evidence-manifest.schema.json` | 新增 | 08 | E3/E4 artifact字段契约 | VER-007 | `VERIFIED`（当前bundle） |
| `tests/conformance/CMakeLists.txt` | 新增 | 08 | labels/targets与静态audit | 全部 | `VERIFIED`（3/3） |

表中每个路径均为从仓库根解析的完整仓库相对路径；成对文件的实际`.h`与`.cpp`路径在下列小节明确列出。机器可读状态以反引号文本为准。

### System harness（`tests/verification/support/system_harness.h`、`tests/verification/support/system_harness.cpp`）

**职责与设计**：header声明create/submit/advance/drain/snapshot/finish；source仅通过公开factory装配真实01至07并捕获completion/event。**输入输出**：manifest/workload -> terminal与snapshot。**状态/所有权**：拥有test session和真实system，不拥有生产内部状态。**错误边界**：Backpressured不隐藏重试，超预算报告非idle tree。**依赖**：production public APIs。**需求/测试/状态**：VER-003/004，VER-SYS/FAULT，`PLANNED`。

### Scoreboard（`tests/verification/support/scoreboard.h`、`tests/verification/support/scoreboard.cpp`）

**职责与设计**：header声明expected operation/logical image；source用独立payload pattern在成功terminal才提交期望write。**输入输出**：submit/terminal -> byte或signature差异。**状态/所有权**：拥有测试期望图，不写生产payload。**错误边界**：data-invalid不得比较payload，TimingOnly不得声称byte oracle。**依赖**：public completion types。**需求/测试/状态**：VER-003，VER-SYS-001，`PLANNED`。

### Event与不变量oracle（`tests/verification/support/event_oracle.h`、`tests/verification/support/event_oracle.cpp`、`tests/verification/support/invariant_checker.h`、`tests/verification/support/invariant_checker.cpp`）

**职责与设计**：前者定义required edge、允许无序集合和stable digest；后者检查accepted/terminal/outstanding、credit/token/payload/event守恒。**输入输出**：public event/snapshot -> pass或首个witness。**状态/所有权**：仅保存有界观察记录。**错误边界**：unknown schema、重复token、负计数立即失败。**依赖**：versioned observation schema。**需求/测试/状态**：VER-002至005，VER-OCP/SYS/FAULT/DET，`PLANNED`。

### Artifact writer（`tests/verification/support/artifact_writer.h`、`tests/verification/support/artifact_writer.cpp`）

**职责与设计**：header声明begin/append/finish；source校验实验规范必填字段并以临时文件后原子rename提交bundle。**输入输出**：run metadata/events/results ->成功或失败artifact。**状态/所有权**：拥有当前artifact session。**错误边界**：路径越界、缺字段或写失败使测试失败。**依赖**：build artifact root和versioned JSON schema。**需求/测试/状态**：VER-007，VER-E4-AUDIT，`PLANNED`。

### 配置一致性测试（`tests/verification/config/test_config_conformance.cpp`）

**Fixture**：相同canonical HBF config经YAML/ConfigNode/builder进入真实resolver。**输入/oracle**：unknown字段、单位、capability和非法组合矩阵；合法resolved dump/hash相同，非法项在build前typed拒绝。**证据等级**：真实配置生产路径可达E3。**需求/状态**：VER-001，VER-CFG-*，`PLANNED`。

### OCP vector测试（`tests/verification/conformance/test_ocp_vectors.cpp`）

**Fixture**：真实HBF pipeline与携带section/table/field/hash的只读SpecVector。**输入/oracle**：64 B/DLU边界、ordering/status等已提取字段逐项比较；未提取字段不得生成expected。**证据等级**：完整production path和规范vector时E4。**需求/状态**：VER-002/008，VER-OCP-*；提取缺口保持`BLOCKED_SPEC`。

### 系统、故障与确定性测试（`test_hbf_pipeline.cpp`、`test_fault_reset.cpp`、`test_determinism.cpp`）

**Fixture**：真实01至07、SparsePayload、fixed config/seed/workload；fault测试仅用公开受控hook。**输入/oracle**：queue saturation、read/write、各stage fault/reset、三次独立运行；检查payload、typed status、completion-time commit、exactly-once、drain、守恒及digest完全一致。**证据等级**：E3。**需求/状态**：VER-003/004/005，VER-SYS/FAULT/DET，`PLANNED`。

### Resource ceiling测试（`tests/performance/test_flash_read_bandwidth.cpp`）

**Fixture A**：真实Host->Address->Controller->PAL/Fabric->NAND->Host单endpoint基线，full-duplex oracle按`4096/max(forward_command_cycles,return_data_cycles)`计算。**Fixture B**：16 Channel、4个虚拟AXI/Channel、256个owned Bank/Channel，共4096个并发Bank；AXI共享Channel resource，Forward/Return独立EAT，每条route携带64条Channel-local lane，repair manager维护16×64 active和每Channel本地spare。raw link使用100%能力，不预置OCP用户效率。**Oracle/结果**：16777216 B/8884 cycle，`1888.475462 GB/s <= 4096 GB/s raw ceiling`，raw利用率`46.1054%`，OCP用户目标比`61.4738%`；4096个payload逐byte为`0xa5`，failure为0且outstanding归零。测量起点启用2000-cycle周期日志，输出各窗口的completed read/write bytes和GB/s；日志仅用于观察，结构化stats仍是oracle。**证据等级**：`E4-model-resource`候选。

### Spec gap manifest（`tests/verification/manifests/spec_gap_manifest.yaml`）

**职责**：逐字段记录`SPEC_EXTRACT_REQUIRED`定位、受阻Test ID和解除条件。**输入输出**：spec audit -> gap report；不向生产配置注入默认值。**状态/所有权**：08维护的只读验证资产。**错误边界**：无locator的gap或无来源expected使audit失败。**需求/测试/状态**：VER-008，VER-SPEC-GAP，`BLOCKED_SPEC`。

### Verification CMake（`tests/verification/CMakeLists.txt`）

**职责与设计**：注册support target、真实production links、CTest labels/timeout和build内artifact root。**输入输出**：测试源/manifest -> CTest manifest。**状态**：无运行产品状态。**错误边界**：重复Test ID、fixture缺失、E3/E4链接fake target时configure失败。**依赖**：根CMake与Docker构建入口。**需求/测试/状态**：VER-001至008，configure audit，`PLANNED`。

每个测试文件使用生产target；只有component test可在manifest明确列出fake边界。

## 8. 配置、错误、统计与Debug

manifest严格实现实验规范字段：Experiment ID、Requirement、Objective、Evidence level、Production path、Preconditions、Config、Workload、Seed、Oracle、Budget、Artifacts和Replay。unknown字段拒绝。测试统计与产品stats分namespace，测试不得回写生产counter。失败分类为`ManifestError, SpecGap, ProductMismatch, InvariantFailure, Timeout, ArtifactFailure`。

E3/E4 artifact最小集合严格采用实验规范第8节；另存spec locator、first difference、fault hit ledger与resource ceiling公式。输出路径由CTest传入`build/artifacts/<suite>/<id>/<run-id>`。

## 9. 测试用例设计

| Test ID | Fixture/Precondition | Input/Fault | 精确Oracle | 证据 |
|---|---|---|---|---|
| VER-CFG-001 | resolver三入口 | canonical/unknown/unit/bad combo | dump相同或构造前typed reject | E3 |
| VER-OCP-HOST-001 | real HBF pipeline+extracted vector | 64 B、DLU边界、same ID | status、ordering与vector逐项同 | E4 |
| VER-SYS-001 | SparsePayload real 01..07 | write/read、queue saturation | byte image、exactly once、drain | E3 |
| VER-FAULT-001 | real pipeline/fault ports | ECC/raw/TSV/program/reset矩阵 | data-valid、commit、retire、守恒 | E3 |
| VER-DET-001 | 同binary/config/seed三次 | mixed workload+fault | digest/result/stats完全一致 | E3 |
| VER-PERF-001 | real pipeline；single-endpoint synthetic profile | 1 warmup + 15个4 KiB Flash read | 61440 B/8146 cycle；`7.542352 <= 7.699248 B/cycle` | E4 candidate |
| VER-PERF-002 | real pipeline；16 Channel、4 AXI/Channel、256 Bank/Channel | 4096个并行4 KiB Flash read | 16777216 B/8884 ns；`1888.475462 GB/s`，raw利用率46.1054% | E4 candidate |
| VER-E4-AUDIT | evidence index | 所有E4条目 | locator/hash/bundle/replay齐全 | E4 |
| VER-SPEC-GAP | gap manifest | 未提取字段 | 不存在无来源expected值 | audit |

每项明确cycle/request budget、成功/失败artifact和允许误差；性能误差只允许一个operation边界窗口并在manifest写明。执行统一遵循[`实验与测试执行规范.md`](实验与测试执行规范.md)。

## 10. 实施顺序、ADR与完成定义

实施顺序：manifest/schema -> harness/artifact -> scoreboard/invariants -> config/system -> fault/reset -> determinism -> OCP vectors -> ceilings/E4 audit。

| ADR ID | 决策 | 背景 | 备选 | 理由 | 后果 | 状态 |
|---|---|---|---|---|---|---|
| ADR-08-001 | E3/E4只走真实对象图 | fake无法证明系统行为 | 拼装fake pipeline | 防止假通过 | 测试成本较高 | `PLANNED` |
| ADR-08-002 | OCP vector要求原文定位/hash | 摘要可能不完整 | 常识补齐 | 保持客观性 | 部分项BLOCKED_SPEC | `PLANNED` |
| ADR-08-003 | hybrid bonding排除首期门槛 | 非OCP产品行为且缺vendor数据 | 纳入默认矩阵 | 避免错误承诺 | 后续独立研究验证 | `PLANNED` |

完成定义：所有工具和测试接入Conda/Docker CMake/CTest统一入口；VER-001至007按各自实际等级生成可重放artifact；只有具备规范定位和对应oracle的已提取字段才可形成E4字段证据。VER-008 gap关闭后方可声明完整OCP HBF一致性；未关闭项继续保持`BLOCKED_SPEC`或`EXTERNAL_DEPENDENCY`。

当前执行证据：2026-08-30，Docker Debug、Ramulator2 OFF，`ctest -L openhbx_s9 --output-on-failure`为3/3通过；artifact位于`build/artifacts/stages/s9/EXP-S9-CONFORMANCE/20260830T103513Z/`。vector测试经过真实01至07并提供E3/E4候选字段证据；Host admission oracle因Controller为受控fake只计E2；速度等级只核对规范算式，不是吞吐饱和实验。

### Flash 读取延迟与带宽实验规划（2026-09-16）

读取性能实验统一复用真实 `OpenHbxSystem`、`RequestBridge` 和 64 B DLU 写入准备流程，禁止用整页伪造 Host write fixture。现有 `openhbx_flash_read_bandwidth_test` 已同时采集每个 4 KiB read 的 latency 样本（min/P50/P95/max）及批量 bytes/cycle、GB/s，因此作为第一项实验：`EXP-PERF-HOST-FLASH-READ`（`VER-PERF-001`）。

后续两项实验应在该 fixture 上增量实现：

- `EXP-PERF-FLASH-READ-LATENCY-SWEEP`（`VER-PERF-003`，E4-model-resource candidate）：固定已写入页面和配置，扫描 outstanding/concurrency=1,2,4,8,16，报告每档 latency min/P50/P95/max、completion cycles 和 accepted/terminal 守恒；每档都要经过真实 DLU 写入和 reset。
- `EXP-PERF-FLASH-READ-BW-SWEEP`（`VER-PERF-004`，E4-model-resource candidate）：使用不同 bank 数量/请求批次扫描 1,2,4,8,...，报告有效 GB/s、bytes/cycle、warmup 后稳态窗口及 fabric/media theoretical ceiling；不得将 OCP/vendor wall-clock 结论从 synthetic `tCK` 推导出来。

两项实验必须由独立 CTest target 注册，输出到 `build/artifacts/performance/<experiment-id>/<run-id>/`，manifest 固定 config hash、seed、workload hash、cycle budget 和 replay 命令。若 fixture 未完成完整 page program、reset 或 drain，测试必须失败，不能记录性能数字。

### 读取性能结果解释（2026-09-16）

本轮 Conda/Debug/CTest 复现的单 endpoint 实验为 15 次 read、61440 B、67996 simulation cycles、`0.903583 B/cycle`、`0.903583 GB/s`；latency 为 min=4533、P50=13598、P95=13599、max=13599 cycles。latency 起点是 `RequestBridge::try_submit` 接受前记录的 `system.cycle()`，终点是 `SystemCompletion.completed_at`，包含 Host、Controller、PAL/Fabric、NAND read command/sense 和返回路径，不包含宿主 wall-clock。结果通过 `4096/max(forward_command_cycles,return_data_cycles)` ceiling 检查；synthetic `tCK=1 ns` 和 media timing 不能推出真实器件规格。
