# OpenHBX NAND Media 与 RAS 概要设计

**文档版本**：V1.1
**文档位置**：`docs/HLD_07_NAND_MEDIA_RAS.md`  
**对应文档**：`docs/LLD_07_NAND_MEDIA_RAS.md`  
**设计状态**：🟡 `PARTIAL`（`status: PARTIAL`）
**适用基线**：OCP HBF v0.7.0；首期HBF NAND synthetic/vendor profile

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 合并NAND介质、原始可靠性与环境观测边界 |
| V1.1 | 2026-09-20 | Channel 分片：单体 facade 拆分为 `NandMediaRouter` + 每 Channel `NandChannelSlice` + 共享 `DieDomain`；全局 die bus 由 `ChannelMediaPath(channel,core)` EAT 取代。见 [`HBF_CHANNEL_PARTITIONED_MODEL.md`](HBF_CHANNEL_PARTITIONED_MODEL.md) |

## 1. 模块概述

07是`Core Die -> Die -> Bank -> Block -> 4 KiB Page`物理状态、NAND array命令、payload与原始可靠性的唯一owner。该五级层次是TASK1首期HBF唯一物理层次；NOR不在TASK1范围。它接收06 PAL的`FlashCommand`，通过唯一EventQueue异步执行Read/Program/Erase阶段，只在terminal成功点提交payload/Page/Block状态。

包含：geometry、legality、stage timing、Bank/Die array EAT、payload backend、BBT、PEC/read counter、raw error、retention/disturb和temperature observation。不包含Host协议、DLU聚合/ECC策略、Host replay/Zone Map、PAL route或TSV repair。OCP未规定的cell mode及时序值必须标记synthetic/vendor，不能作为OCP保证。

建模粒度与HFSSS介质模块保持同一级工程分解：NAND topology、timing profile、EAT、command engine、state/payload、reliability和BBT分别建模。Read、Program和Erase至少具有独立stage与资源mask；Program/Erase必须包含verify与失败路径；同Bank冲突串行、无共享资源的Bank允许并行。不得把整个NAND设备退化为`operation -> fixed latency -> success`。Plane、Multi-Plane、cell mode及Die interleaving的具体时序只有在产品profile声明对应资源和参数时启用，不作为无来源的OCP默认行为。

不变量：Accepted精确一次terminal；失败/reset不提交未来状态；Program成功才使新payload可见；Erase成功才清Block并递增PEC；Read返回不可变snapshot；BBT与retirement状态只有07可写；相同seed/config/timeline结果确定。

## 2. 需求回顾与追踪

| 需求ID | 需求描述 | 优先级 | 版本 | 实现状态 |
|---|---|---|---:|---|
| MEDIA-001 | 固定`Core Die -> Die -> Bank -> Block -> 4 KiB Page`且可逆解析 | P0 | V1.0 | ⚪ `PLANNED` |
| MEDIA-002 | Read/Program/Erase按stage异步执行并遵守资源EAT | P0 | V1.0 | ⚪ `PLANNED` |
| MEDIA-003 | payload/Page/Block仅terminal成功提交 | P0 | V1.0 | ⚪ `PLANNED` |
| MEDIA-004 | 非法重复Program、非顺序Page和bad block访问被typed拒绝 | P0 | V1.0 | ⚪ `PLANNED` |
| MEDIA-005 | BBT、factory/runtime bad与retirement可观测 | P0 | V1.0 | ⚪ `PLANNED` |
| MEDIA-006 | raw read outcome区分correctable/uncorrectable/retry/refresh notice | P0 | V1.0 | ⚪ `PLANNED` |
| MEDIA-007 | reset隔离stale stage且释放reservation | P0 | V1.0 | ⚪ `PLANNED` |
| MEDIA-008 | temperature影响只依据显式且有来源的profile | P1 | V1.0 | 🔴 `BLOCKED_SPEC` |

状态图标仅用于阅读；机器可读状态是反引号内的`PLANNED`或`BLOCKED_SPEC`，版本不构成实现或验证证据。

| 需求ID | 来源/类别 | owner组件 | 验收/Test ID | 证据目标 |
|---|---|---|---|---|
| MEDIA-001 | OCP Product Description摘要；规范事实 | `MediaTopology` | `VER-MEDIA-001` | E1 |
| MEDIA-002 | OpenHBX选择；HFSSS EAT参考 | `CommandEngine` | `VER-MEDIA-002` | E2 |
| MEDIA-003 | OpenHBX不变量 | `StateStore/PageStore` | `VER-MEDIA-003` | E2 |
| MEDIA-004 | OCP program/replay语义；04契约 | `CommandLegality` | `VER-MEDIA-004` | E2 |
| MEDIA-005 | OCP BBT/reduced-capacity摘要 | `BadBlockTable` | `VER-MEDIA-005` | E3 |
| MEDIA-006 | OCP Host status边界；ECC/status归05 | `ReliabilityModel` | `VER-MEDIA-006` | E3 |
| MEDIA-007 | OpenHBX reset不变量 | `CommandEngine` | `VER-MEDIA-007` | E3 |
| MEDIA-008 | OCP thermal观察；vendor外部参数 | `DieEnvironment` | `VER-MEDIA-008` | E4，解除规范阻断后 |

## 3. 系统架构设计

```text
06 FlashPal -> NandMediaRouter (IFlashMediaPort, 按 command.channel 分发)
  -> NandChannelSlice[channel]
      -> Topology/Legality -> stage FSM -> Bank/ChannelMediaPath EAT -> system EventQueue
                               | event
                               +-> Reliability
                               +-> terminal transaction
                                   自有 Page/Block/payload/erase 计数
      <- typed MediaCompletion
  共享: DieDomain (blocked_dies / 全局BBT / 未来温度)，slice 只读引用
```

**Channel 分片（V1.1）**：介质不再是一个持有全部 channel 状态的单体对象。`NandChannelSlice` 仅拥有 `ChannelTopology` 判定归本 channel 的 Bank 集合（Bank array EAT、Page/Block/payload）；`ChannelMediaPath(channel, core_die)` EAT 为 slice 私有，取代原全局 `die_eat_` 总线（拓扑冻结结论 §4）；Die 级共享状态（blocked、BBT、未来温度）集中于 `DieDomain`。命令合法当且仅当 `command.channel == owner_channel(physical_bank)`，由 slice 成员资格校验结构性保证（CHAN-006）。架构、ADR 与 CHAN 需求见 [`HBF_CHANNEL_PARTITIONED_MODEL.md`](HBF_CHANNEL_PARTITIONED_MODEL.md)。

| 组件 | 主要文件 | 职责 | 输入 | 输出 | 拥有状态 | 依赖 |
|---|---|---|---|---|---|---|
| `NandMediaRouter` | `nand_media_router.*` | `IFlashMediaPort` 实现，按 channel 分发 | FlashCommand | issue/completion | 无动态状态 | slice 公开接口 |
| `NandChannelSlice` | `nand_channel_slice.*` | 每 Channel 介质引擎（facade/capability） | command | issue/completion | 本 channel Bank 切片/inflight | 02/06端口、DieDomain |
| `DieDomain` | `die_domain.*` | Die 级共享状态 | slice 查询/02 reset | blocked/BBT/容量视图 | blocked_dies、全局BBT | topology |
| `MediaTopology` | `topology.*` | geometry/address resolve | physical address | resource refs | immutable geometry | config |
| `CommandEngine` | `command_engine.*` | stage FSM | accepted command/event | completion | contexts/obligation | EAT/state/reliability |
| `StateStore/PageStore` | `state.*`,`page_store.*` | 权威介质与数据 | terminal delta | snapshot | Page/Block/payload | topology |
| `Reliability/BBT/Environment` | 对应文件 | raw fault、健康与环境 | op/context/fault | raw outcome | PEC/read count/BBT/temp | deterministic seed |

### `NandFlashDevice`

**文件路径**：`include/openhbx/media/nand/nand_flash_device.h`、`src/media/nand/nand_flash_device.cpp`

**职责**：提供NAND Media统一facade并装配子组件。

**关键组件**：`try_issue()`、`reset()`、`snapshot()`、`capabilities()`。

**输入与输出**：06的`FlashCommand` -> issue result与typed completion。

**状态与所有权**：拥有装配和inflight入口，不拥有Host状态。

**依赖与边界**：依赖02 EventKernel与06 media port；不实现PAL、ECC或replay。

**对应需求**：MEDIA-001至008。

**实现状态**：⚪ `PLANNED`。

> **退役注记（V1.1）**：单体 facade 在 Channel 分片改造中由 `NandMediaRouter`（06 端口实现）、N 个 `NandChannelSlice`（介质引擎）与共享 `DieDomain`（Die 级状态）取代，装配职责上移至 02。逐文件设计见 LLD_07 第7章。

### `MediaTopology`

**文件路径**：`src/media/nand/topology.h`、`src/media/nand/topology.cpp`

**职责**：实现冻结的五级geometry、容量与地址解析。

**关键组件**：checked flatten/decode、`capacity()`和ownership validation。

**输入与输出**：physical address <-> stable resource refs。

**状态与所有权**：拥有构造后不可变的geometry。

**依赖与边界**：依赖resolved config；只实现`Core Die -> Die -> Bank -> Block -> 4 KiB Page`，不实现NOR、Chip/Plane或额外Host层次。

**对应需求**：MEDIA-001。

**实现状态**：⚪ `PLANNED`。

### `CommandEngine`与Media EAT

**文件路径**：`src/media/nand/command_engine.h`、`src/media/nand/command_engine.cpp`、`src/media/nand/eat_table.h`、`src/media/nand/eat_table.cpp`、`src/media/nand/timing_profile.h`、`src/media/nand/timing_profile.cpp`

**职责**：执行Read/Program/Erase stage并预约NAND资源。

**关键组件**：stage FSM、Bank/Die EAT、sourced timing profile。

**输入与输出**：accepted command/event -> next event或completion。

**状态与所有权**：拥有context、terminal obligation和NAND EAT。

**依赖与边界**：依赖02 EventKernel、Topology及状态组件；不拥有06 TSV EAT。

**对应需求**：MEDIA-002、MEDIA-007、MEDIA-008。

**实现状态**：MEDIA-002/007为⚪ `PLANNED`；MEDIA-008为🔴 `BLOCKED_SPEC`。

### `StateStore`与`PageStore`

**文件路径**：`src/media/nand/state_store.h`、`src/media/nand/state_store.cpp`、`src/media/nand/page_store.h`、`src/media/nand/page_store.cpp`

**职责**：维护Page/Block legality并执行terminal atomic commit。

**关键组件**：pending delta、Sparse/TimingOnly payload、immutable snapshot。

**输入与输出**：token/address/delta -> committed state或typed error。

**状态与所有权**：唯一拥有Page、Block和payload。

**依赖与边界**：依赖Topology与BBT只读查询；completion前不得提交未来状态。

**对应需求**：MEDIA-003、MEDIA-004。

**实现状态**：⚪ `PLANNED`。

### `ReliabilityModel`、`BadBlockTable`与`DieEnvironment`

**文件路径**：`src/ras/reliability.h`、`src/ras/reliability.cpp`、`src/ras/bbt.h`、`src/ras/bbt.cpp`、`src/ras/die_environment.h`、`src/ras/die_environment.cpp`

**职责**：建模raw reliability、坏块/retirement与环境观测。

**关键组件**：PEC/read counter、retention/disturb、BBT、temperature timeline。

**输入与输出**：operation/fault/environment -> raw result、health delta和snapshot。

**状态与所有权**：唯一拥有介质健康状态。

**依赖与边界**：依赖稳定seed和显式profile；ECC/Host status归05。

**对应需求**：MEDIA-005、MEDIA-006、MEDIA-008。

**实现状态**：MEDIA-005/006为⚪ `PLANNED`；MEDIA-008为🔴 `BLOCKED_SPEC`。

## 4. 高层详细设计

Read阶段为`command -> sense -> raw reliability -> snapshot -> complete`；Program为`data-in -> array-program -> verify -> commit`；Erase为`command -> array-erase -> verify -> commit`。各stage只预约自身所需Bank array/Die path资源，不预约06的TSV资源。stage event重新检查generation和Die/Block状态。

`SparsePayload`保存访问过的4096 B Page；`TimingOnly`只保存signature并声明不可执行逐byte oracle。两模式不得改变时序或调度。Program pending payload在成功verify前不可读；Erase失败不清数据、不增PEC。

可靠性输出为raw result，05的ECC与错误策略负责映射Host status。BBT包含factory bad、runtime bad、reason和version。read counter达到显式阈值只产生refresh notice；是否进入Host replay workflow由04与05协同决定，07不复制有效数据。

## 5. 接口设计

| 接口 | 方向 | 语义 | backpressure/错误 | 所有权/时序 |
|---|---|---|---|---|
| `try_issue(FlashCommand&&, Cycle)` | 06->07 | validate、reserve首stage | Accepted/Busy/Rejected | Accepted才转移payload，异步完成 |
| `on_stage_event(StageEvent)` | 02->07 | generation/sequence校验并推进 | stale丢弃、integrity记录 | EventKernel上下文 |
| `snapshot()` | 08/debug->07 | 只读稳定快照 | 不暴露mutable ref | 同cycle phase后观察 |
| `inject_media_fault(FaultRule)` | 08/RAS->07 | 注册确定性fault | 非法target/stage拒绝 | 规则按stable ID排序 |
| `reset(scope,generation)` | 02->07 | abort命中context | 精确一次Aborted | 回收pending payload/EAT token |

## 6. 数据结构设计

核心结构为`PhysicalAddress`、`PageState{Erased,PendingProgram,Valid}`、`BlockState{Good,Bad,Retired}`、`CommandContext`、`StageSpec`、`StateDelta`、`PayloadSnapshot`、`RawReadClass`和`MediaCompletion`。地址、token和completion跨模块定义唯一；内部resource引用不得外泄。

容量为`core_dies * dies * banks * blocks * pages * 4096 B`，所有乘法checked；Host Channel是Bank ownership切片，不再次乘容量。

## 7. 关键流程与状态机

```text
Received -> Validated -> StageReserved -> StageInFlight
       -> (next stage)* -> Verify -> TerminalCommit -> Completed
       -> raw/fatal failure ---------------------------> Failed
Any nonterminal --reset--> Aborted；stale event不提交
```

非法Page状态、Block bad/retired、资源冲突和capacity exhaustion分别产生Rejected或Busy。failure completion必须说明`data_valid`；duplicate terminal不重复commit或释放。

## 8. 性能与资源设计

每Bank array与必要Die path有独立EAT；同Bank sense/program/erase按profile资源mask串行，不同Bank在无共享资源时并行。时序值、temperature multiplier及可靠性参数必须带source metadata。SparsePayload内存约为`O(touched_pages * 4096)`，metadata为`O(total_blocks + configured page-state representation)`；大型geometry必须支持稀疏/分层存储。吞吐上限取Bank并行度、stage duration及06互联上限的最小值。

## 9. 错误、恢复与可观测性

typed outcome包含`Success, ReadErasedPage, RawCorrectable, RawUncorrectable, RetrySuggested, RefreshNotice, ProgramFail, EraseFail, CapacityUnusable, DieTemporarilyBlocked, BadBlock, Retired, Aborted, IntegrityError`。07不直接生成Host AXI status；05负责映射OCP表12/13。trace记录token/address/stage/start/end/resource/raw outcome/seed rule；统计包括read/program/erase、PEC、read count、bad/retired、raw error、payload bytes、stage occupancy和stale event。snapshot必须支持定位首个非idle owner。

### 9.1 已知缺口：BBT只检查、不驱动恢复

当前`BadBlockTable`支持手工`mark()`和admission前`is_bad()`检查，但factory bad manifest未在系统启动时装载，Program/Erase verify failure也只返回失败状态，不会自动写入BBT。07尚未向04发布带reason/version的retirement事件，因此坏块不会触发Zone/容量视图更新；后续请求仍映射到原Block并再次被拒绝。

后续07实现负责在启动时装载并校验factory bad manifest，在运行期确认不可恢复的Program/Erase failure后原子提交BBT reason/version，并通过typed端口请求04执行retirement。07不直接修改Host地址公式或搬移有效数据；映射epoch和Host replay分别由04、05拥有。该闭环完成并通过`VER-MEDIA-005`与`GAP-010`测试前，factory/runtime bad只可声明为“可注入并拒绝访问”。

## 10. 测试与验收

| Test ID | 层级 | 场景 | Oracle | 证据 |
|---|---|---|---|---|
| `VER-MEDIA-001` | E1 | geometry边界/round-trip | 容量、owner与索引精确 | E1 |
| `VER-MEDIA-002` | E1/E2 | stage与Bank并行 | timeline等于手算EAT | E2 |
| `VER-MEDIA-003` | E2 | program/erase success/fail | terminal前不可见、失败不变 | E2 |
| `VER-MEDIA-004` | E2 | legality矩阵 | typed status与零副作用 | E2 |
| `VER-MEDIA-005` | E2/E3 | factory/runtime bad/retire | BBT/version/容量一致 | E3 |
| `VER-MEDIA-006` | E2/E3 | raw fault/retry/notice | outcome/data-valid/计数精确 | E3 |
| `VER-MEDIA-007` | E3 | 每stage reset | stale不提交、drain全空 | E3 |
| `VER-MEDIA-008` | E4 | 有来源thermal profile | 参数与结果可重放 | E4 |

实验执行、证据保存和状态更新统一遵循[`实验与测试执行规范.md`](实验与测试执行规范.md)。


> **源码同步（2026-09-15）**：当前仓库已落地对应 `include/openhbx/...` 公开接口及 `src/...` 生产实现；文档中的测试路径和验证状态仍以实际 CMake/CTest artifact 为准。仅有源码不构成 `VERIFIED` 证据。
