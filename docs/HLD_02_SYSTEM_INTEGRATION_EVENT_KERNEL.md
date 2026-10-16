# OpenHBX 系统集成与确定性事件内核概要设计

**文档版本**：V1.0
**文档位置**：`docs/HLD_02_SYSTEM_INTEGRATION_EVENT_KERNEL.md`
**对应文档完整路径**：`docs/LLD_02_SYSTEM_INTEGRATION_EVENT_KERNEL.md`
**客观状态**：🟡 `PARTIAL`；EventQueue、clock、completion、lifecycle、observability和OpenHbxSystem已有源码及单元测试，Ramulator adapter与端到端证据仍未闭环。
**适用基线**：OCP HBF v0.7.0；当前 `thirdparty/ramulator2`
**阶段范围**：仅`OCP_HBF_0_7`生产对象图

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 合并Ramulator适配、唯一system、时间、事件、完成和drain责任域 |

## 1. 模块概述

### 1.1 定位与职责

本模块是HBF设备与Ramulator 2之间的唯一系统边界。它实现`IMemorySystem` adapter，将`Ramulator::Request`桥接为OpenHBX值对象，并由唯一`OpenHbxSystem`拥有cycle、EventQueue、generation、completion registry、drain和系统统计聚合。

```text
Ramulator IFrontEnd
  -> IMemorySystem::send(Request&)
  -> OpenHbxRamulatorAdapter
  -> RequestBridge
  -> OpenHbxSystem::try_submit
  -> Host -> Address -> Controller -> Interconnect -> NAND Media
  -> EventQueue completion
  -> CompletionRegistry::finish_once
  -> RequestBridge sets depart -> original callback exactly once
```

### 1.2 边界

包含：Factory/ConfigNode接入、`IMemorySystem` ABI、Request字段和callback生命周期桥接、唯一设备facade、整数cycle、确定性事件排序、generation fence、精确一次terminal、reset、drain和stats bridge。

不包含：OCP AXI/UCIe协议细节、地址映射、DLU聚合、调度、TSV和NAND状态机；这些由后续HBF模块拥有。Adapter不得实现固定延迟捷径、第二EventQueue或介质状态。

### 1.3 关键事实与不变量

Ramulator源码事实：`IMemorySystem`要求`send(Request&)`、`tick()`、`get_clock_ratio()`、`get_tx_bytes()`，并提供`get_tCK()`；`connect_frontend()`先setup实现再setup子组件；`GenericDRAMSystem::send`只在下游接受后统计。`Request`精确字段以当前`ramulator/base/request.h`为准。

- `send(false)`前后Ramulator Request、bridge、token、payload、event和system状态均不变。
- Accepted请求满足`accepted = terminal + outstanding`，callback精确一次。
- completion先设置`depart`再调用原callback。
- 系统只有一个整数simulation cycle和一个EventQueue。
- 同cycle按`cycle, phase, sequence`稳定排序。
- future event只持有稳定ID/value/generation，不持有跨模块裸pointer。
- reset后旧generation事件不得提交新状态。
- drain完成当且仅当所有模块、event、bridge、token、credit和payload owner为空。

## 2. 需求回顾与追踪

### 2.1 需求进度总览

| 需求ID | 需求描述 | 优先级 | 版本 | 实现状态 |
|---|---|---|---|---|
| RAM-001 | 由Ramulator Factory构造memory system | P0 | V1.0 | ⚪ `PLANNED` |
| RAM-002 | 实现完整`IMemorySystem` ABI | P0 | V1.0 | ⚪ `PLANNED` |
| RAM-003 | `send(false)`零副作用且可重试 | P0 | V1.0 | ⚪ `PLANNED` |
| RAM-004 | Accepted后安全保存Request生命周期 | P0 | V1.0 | ⚪ `PLANNED` |
| RAM-005 | depart先于一次callback | P0 | V1.0 | ⚪ `PLANNED` |
| RAM-006 | 保持Read/Write type ABI | P0 | V1.0 | ⚪ `PLANNED` |
| RAM-007 | ConfigNode与resolved config一致 | P0 | V1.0 | ⚪ `PLANNED` |
| RAM-008 | Stats树导出且不重复计数 | P1 | V1.0 | ⚪ `PLANNED` |
| SYS-001 | 唯一system拥有cycle/event/generation | P0 | V1.0 | ⚪ `PLANNED` |
| SYS-002 | 同cycle事件确定性排序 | P0 | V1.0 | ⚪ `PLANNED` |
| SYS-003 | Accepted精确一次terminal | P0 | V1.0 | ⚪ `PLANNED` |
| SYS-004 | reset generation隔离旧事件 | P0 | V1.0 | ⚪ `PLANNED` |
| SYS-005 | drain使用cycle budget和typed诊断 | P0 | V1.0 | ⚪ `PLANNED` |
| SYS-006 | 整数clock-domain换算 | P0 | V1.0 | ⚪ `PLANNED` |
| SYS-007 | Busy/Rejected无future obligation | P0 | V1.0 | ⚪ `PLANNED` |
| SYS-008 | snapshot只读且定位非idle owner | P1 | V1.0 | ⚪ `PLANNED` |

### 2.2 证据、Owner与Test追踪

| 需求ID | 可测试描述 | 来源 | 类别 | 优先级 | owner组件 | 验收/Test ID | 状态 |
|---|---|---|---|---|---|---|---|
| RAM-001 | 注册为Ramulator `memory_system`实现并可由Factory构造 | `base/base.h`, `factory.h`, `i_memory_system.h` | 源码事实 | P0 | RamulatorAdapter | RAM-T01 | ⚪ `PLANNED` |
| RAM-002 | 精确实现`send/tick/get_clock_ratio/get_tCK/get_tx_bytes` | `memory_system/i_memory_system.h` | 源码事实 | P0 | RamulatorAdapter | RAM-T02 | ⚪ `PLANNED` |
| RAM-003 | `send(false)`零副作用且Request可重试 | `GenericDRAMSystem::send`; 任务更正第6.1节 | 源码事实/设计要求 | P0 | RequestBridge/System | RAM-T03 | ⚪ `PLANNED` |
| RAM-004 | Accepted后保存Request所需值和callback，不依赖调用栈对象存活 | Ramulator `base/request.h`; OpenHBX ownership选择 | 源码事实/设计选择 | P0 | RequestBridge | RAM-T04 | ⚪ `PLANNED` |
| RAM-005 | 完成时设置`depart`后调用原callback一次 | 任务更正第6.1节 | 设计要求 | P0 | RequestBridge | RAM-T05 | ⚪ `PLANNED` |
| RAM-006 | `Read=0`、`Write=1` ABI不变，扩展命令不污染Ramulator枚举 | 任务更正第6.1节 | 集成要求 | P0 | RequestConverter | RAM-T06 | ⚪ `PLANNED` |
| RAM-007 | ConfigNode与01 resolved config语义一致 | `ConfigNode`; HLD_01 CFG-003/012 | 源码事实 | P0 | AdapterConfigBridge | RAM-T07 | ⚪ `PLANNED` |
| RAM-008 | Stats经Ramulator Stats树导出且不重复计数 | `Implementation::collect_stats`; OpenHBX ADR-02-004 | 源码事实/设计选择 | P1 | StatsBridge | RAM-T08 | ⚪ `PLANNED` |
| SYS-001 | 唯一`OpenHbxSystem`拥有cycle/EventQueue/generation | `TASK1-整体设计方案.md`第7节 | 设计选择 | P0 | OpenHbxSystem | SYS-T01 | ⚪ `PLANNED` |
| SYS-002 | 同cycle事件按固定phase和sequence确定排序 | 总体设计第7节；现有`common/event_queue.*` | 设计选择/源码参考 | P0 | EventQueue | SYS-T02 | ⚪ `PLANNED` |
| SYS-003 | Accepted请求精确一次terminal并满足守恒 | 实验规范第7节 | 设计要求 | P0 | CompletionRegistry | SYS-T03 | ⚪ `PLANNED` |
| SYS-004 | reset提升generation，旧事件只回收不提交 | 总体设计第7.4节；实验规范第7节 | 设计要求 | P0 | ResetCoordinator | SYS-T04 | ⚪ `PLANNED` |
| SYS-005 | drain给出成功或typed超限诊断，不用wall-clock判设备完成 | 实验规范第6/7节 | 设计要求 | P0 | DrainCoordinator | SYS-T05 | ⚪ `PLANNED` |
| SYS-006 | clock-domain换算使用整数、checked arithmetic且无时间倒退 | 总体设计第11节 | 设计选择 | P0 | ClockDomainRegistry | SYS-T06 | ⚪ `PLANNED` |
| SYS-007 | Busy/Rejected不创建future event或completion obligation | 设计文档规范第10.1节 | 设计要求 | P0 | OpenHbxSystem | SYS-T07 | ⚪ `PLANNED` |
| SYS-008 | snapshot可定位非idle owner且读取不推进cycle | 实验规范第8节 | 验证要求 | P1 | SnapshotAggregator | SYS-T08 | ⚪ `PLANNED` |

## 3. 系统架构设计

```text
Ramulator lifecycle domain                 OpenHBX deterministic domain
+------------------------+                 +---------------------------+
| ConfigNode / Factory   |--construct----->| OpenHbxSystem             |
| Request& / callback    |--RequestBridge->| admission + token         |
| tick schedule          |--one call------>| cycle + EventQueue        |
| Stats tree             |<--StatsBridge---| counters/snapshot         |
+------------------------+                 | generation/reset/drain    |
                                           +-------------+-------------+
                                                         |
                   +----------+----------+---------+------+------+
                   | HBF Host | Address  | Control | Fabric | NAND |
                   +----------+----------+---------+--------+------+
```

| 组件 | 主要文件 | 职责 | 输入 | 输出 | 拥有状态 | 依赖 |
|---|---|---|---|---|---|---|
| RamulatorAdapter | `ramulator_memory_system.*` | ABI、Factory、tick/stats转发 | Request/ConfigNode | bool/callback/stats | adapter counters | Ramulator、01 |
| RequestBridge | `ramulator_request_bridge.*` | 字段副本与callback关联 | Request& | HostRequest/terminal | bridge entries | completion registry |
| OpenHbxSystem | `open_hbx_system.*` | 唯一facade和phase驱动 | canonical request | submit/completion | module owners | all ports |
| EventQueue | `common/event_queue.*` | future event排序/派发 | Event | due events | heap+sequence | handler registry |
| CompletionRegistry | `completion_registry.*` | token/generation/terminal | accepted/terminal | callback dispatch | lifecycle table | bridge |
| Reset/Drain | `system_lifecycle.*` | generation与quiescence | reset/drain | result/snapshot | lifecycle mode | all modules |
| StatsBridge | `system_stats.*` | 聚合与Ramulator导出 | module counters | stats tree | counters/views | Ramulator Stats |

同步边界是`send()`准入；Accepted后的全部设备工作异步发生于`tick()`驱动的唯一EventQueue。

### 3.1 `RamulatorAdapter`

**文件**：`include/openhbx/integration/ramulator_memory_system.h`、`src/integration/ramulator_memory_system.cpp`。**职责/关键组件**：`OpenHbxMemorySystem`实现Factory、ABI和tick转发。**输入输出**：ConfigNode/Request到bool/callback/stats。**owner/依赖**：Integration owner；依赖Ramulator、01与system facade。**需求/状态**：RAM-001/002/007；⚪ `PLANNED`。

### 3.2 `RequestBridge`

**文件**：`include/openhbx/integration/ramulator_request_bridge.h`、`src/integration/ramulator_request_bridge.cpp`。**职责/关键组件**：两阶段准入和callback关联。**输入输出**：Request到HostRequest/terminal回填。**owner/依赖**：Integration owner；依赖HostRequest和CompletionRegistry。**需求/状态**：RAM-003--006；⚪ `PLANNED`。

### 3.3 `OpenHbxSystem`

**文件**：`include/openhbx/system/open_hbx_system.h`、`src/system/open_hbx_system.cpp`。**职责/关键组件**：唯一facade、对象owner和phase驱动。**输入输出**：canonical request到submit/completion/snapshot。**owner/依赖**：System owner；依赖全部公开port。**需求/状态**：SYS-001/007/008；⚪ `PLANNED`。

> **Channel 分片装配注记（2026-09-20）**：介质装配由单体 `NandFlashDevice` 改为：1 个 `DieDomain` + 每 Channel 一个 `NandChannelSlice`（`owned_banks` 由 04 `ChannelTopology::banks_by_channel` 展开注入）+ 1 个 `NandMediaRouter`（作为 06 的 `IFlashMediaPort`）。HandlerId 分段定义于新头文件 `include/openhbx/system/handler_ids.h`（`kSystemHandlerId=1`、`kPalHandlerId=2`、介质 slice 段 `[1024, 1040)`），取代 `open_hbx_system.cpp` 内 101/102/103 常量（CHAN-008）。系统 reset 逐个调用各 slice 的 `reset()`（各自作用域 discard）；`DieDomain` 无 reset 方法，其 BBT 与 blocked dies 跨 reset 保持，与旧单体设备行为一致（CHAN-005）。详见 [`HBF_CHANNEL_PARTITIONED_MODEL.md`](HBF_CHANNEL_PARTITIONED_MODEL.md) §5.6。

### 3.4 `EventQueue`

**文件**：`include/openhbx/system/event_queue.h`、`src/system/event_queue.cpp`。**职责/关键组件**：heap、handler registry、phase/sequence排序。**输入输出**：EventSpec到due event。**owner/依赖**：System owner；依赖common event/checked math。**需求/状态**：SYS-002/004；⚪ `PLANNED`。

> **Channel 分片注记（2026-09-20）**：新增 `discard_generation(HandlerId, Generation)` 作用域丢弃：仅移除同时匹配 handler 与 generation 的事件，使单个介质 slice 的 reset 不波及其他 slice（CHAN-007）；队列范围 `discard_generation(Generation)` 保留给系统级 reset，02 为其唯一调用方。

### 3.5 `CompletionRegistry`

**文件**：`include/openhbx/system/completion_registry.h`、`src/system/completion_registry.cpp`。**职责/关键组件**：token obligation和`finish_once`。**输入输出**：accepted/terminal到一次delivery。**owner/依赖**：System owner；依赖HostRequest/EventQueue。**需求/状态**：SYS-003/007；⚪ `PLANNED`。

### 3.6 `SystemLifecycle`

**文件**：`include/openhbx/system/system_lifecycle.h`、`src/system/system_lifecycle.cpp`。**职责/关键组件**：generation、reset、drain和idle reason。**输入输出**：lifecycle命令到mode/诊断。**owner/依赖**：System owner；依赖各模块lifecycle port。**需求/状态**：SYS-004/005；⚪ `PLANNED`。

### 3.7 `StatsBridge`

**文件**：`include/openhbx/integration/ramulator_stats_bridge.h`、`src/integration/ramulator_stats_bridge.cpp`。**职责/关键组件**：边界counter和Stats树导出。**输入输出**：SystemStats到Ramulator Stats。**owner/依赖**：Integration owner；依赖Ramulator Stats。**需求/状态**：RAM-008；⚪ `PLANNED`。

## 4. 高层详细设计

### 4.1 Ramulator适配与Request bridge

Adapter构造时调用01 resolver/composer；setup不递归调用子组件setup。`send()`先完成无副作用转换和预检，再请求system reservation；只有system返回Accepted后，bridge entry与completion obligation同时提交。Ramulator调用者保留原`Request`对象直到callback是现有Frontend常见契约；bridge另存完成所需字段并用受控handle关联原对象，测试必须覆盖栈临时对象边界，若当前ABI无法保证原对象寿命则采用拥有副本并在callback前回填的明确策略。

### 4.2 唯一system与tick phase

一次adapter `tick()`精确推进一个OpenHBX system cycle。每cycle先派发当前phase事件，再调用允许产生后续phase事件的组件调度；禁止向已关闭phase安排同cycle事件。固定phase至少包括Reset、MediaCommit、Address/Controller completion、Interconnect、CreditReturn、ControllerSchedule、HostSchedule和FinalDelivery；最终枚举由LLD冻结。

### 4.3 completion生命周期

Accepted时分配稳定token并注册`Accepted`；命令真正交给下游后变为`Issued`；typed terminal经`finish_once`转换为Host/Ramulator completion。duplicate、unknown或错误generation是integrity error，不产生第二callback。terminal时先提交结果和`depart`，调用callback，最后释放bridge和payload owner；callback抛异常也必须完成资源释放并记录错误。

### 4.4 reset与drain

reset关闭新准入、提升generation、使所有旧obligation以typed aborted terminal结束或按规范定义回收，然后逐模块reset。旧event到期只递增stale计数并释放其值资源。drain关闭新准入并推进模拟cycle，直到全局quiescence；cycle budget耗尽返回每个非idle owner、最早事件、token和credit差额。

### 4.5 Stats桥接

System拥有accepted、terminal、outstanding、latency和Host有效byte权威计数；模块统计保留owner前缀。`OpenHbxSystemStats`将Host流量拆为`read_accepted_bytes`、`write_accepted_bytes`、`read_completed_requests`、`read_completed_bytes`、`write_completed_requests`、`write_completed_bytes`和`failed_requests`，并记录latency样本、总和、min/max及首末completion cycle。只有`FlashIo`进入这些流量与latency口径；Admin、CSR和Scratchpad不计入产品数据吞吐。

读写accepted字段在FlashIo请求被System接受时按请求字节数增加；对应completed字段只在terminal成功时增加。Write成功以成功status为准；Read只有成功或corrected结果、`data_valid=true`且返回完整请求payload时才计成功和completed bytes，截断、无效数据、UECC、abort及其他失败均只增加`failed_requests`。`latency_sum_cycles`累计`terminal_cycle - accepted_cycle`，completion window由最早和最晚FlashIo terminal cycle界定。

这些字段跨reset单调累计，reset只改变generation和设备状态，不清零统计。性能测量在窗口开始保存baseline snapshot，结束时使用当前值减baseline值；completion window也以该次delta中首/末terminal为准，不把进程启动以来的累计值直接当测量窗口。Adapter只统计attempt、backpressured、conversion error和callback bridge事件。PAL、Controller、NAND可保留各自owner前缀的内部流量/操作计数，但不得再次计入Host accepted/completed bytes；Host byte、PAL传输byte和NAND介质byte分栏展示，不相加为产品有效吞吐。

## 5. 接口设计

| 接口 | 方向 | 输入/输出 | 所有权/时序 | Busy/错误 |
|---|---|---|---|---|
| `bool send(Request&)` | Frontend -> Adapter | Ramulator request -> bool | Accepted后bridge承担completion；同步准入 | false零副作用；非法ABI字段构造/参数错误 |
| `SubmitResult try_submit(HostRequest&&)` | Adapter -> System | value request -> Accepted/Busy/Rejected | 仅Accepted转移payload | Busy/Rejected无token/event |
| `void tick()` | Ramulator -> Adapter/System | 无 -> cycle+1 | 同步驱动异步事件 | integrity error fail-fast并留snapshot |
| `ScheduleResult schedule(Event&&)` | module -> EventQueue | future event | Accepted转移event payload | 过去/关闭phase/溢出Rejected |
| `FinishResult finish_once(Completion&&)` | module -> Registry | typed terminal | terminal消费obligation | duplicate/stale typed integrity结果 |
| `DrainResult drain(Cycle budget)` | runner/finalize -> System | budget -> drained/timeout | 停止准入并推进cycle | 超限不伪造成功 |
| `void reset(ResetRequest)` | control -> System | scope/kind | generation fence | reset中submit Busy |
| `SystemSnapshot snapshot() const` | debug -> System | 无 -> value snapshot | 只读不推进时间 | 序列化失败不改状态 |

## 6. 数据结构设计

| 结构 | owner/生命周期 | 核心字段 | 容量与不变量 |
|---|---|---|---|
| `HostRequest` | Accepted后system | token/op/address/size/payload/generation | 不含Ramulator裸引用 |
| `BridgeEntry` | RequestBridge至callback返回 | token、Request completion view、callback、generation | token唯一 |
| `Event` | EventQueue至派发 | due cycle、phase、sequence、handler ID、generation、value payload | due>=now |
| `CompletionEntry` | Registry Accepted至terminal | state、generation、sink、payload owner | 恰好一次terminal |
| `ClockDomain` | system全期只读 | ratio numerator/denominator、phase accumulator | checked整数换算 |
| `SystemSnapshot` | caller值对象 | cycle、generation、mode、queue/token/credit摘要 | 确定序列化 |

公共`Cycle/Generation/Token/Event`的唯一权威定义是`include/openhbx/common/`（迁移前对应`include/openhbf/common/`），其他模块不得重定义。

## 7. 关键流程与状态机

### 7.1 submit与完成

```text
send
 -> validate/convert without mutation
 -> probe/reserve system + bridge capacity
 -> Busy: rollback all reservations, return false
 -> Rejected: typed configuration/protocol path, no future event
 -> Accepted: commit token + bridge + payload
 -> issue -> scheduled events -> completion-time commit
 -> finish_once -> set depart -> callback -> release owners
```

| 当前状态 | 事件/条件 | Guard | 动作 | 下一状态 | 失败处理 |
|---|---|---|---|---|---|
| Absent | accept | capacities available | token+bridge注册 | Accepted | rollback/false |
| Accepted | downstream issue | generation current | mark issued | Issued | 保持并重试/typed terminal |
| Accepted/Issued | completion | token/gen匹配 | commit terminal | Delivering | duplicate integrity error |
| Delivering | callback return/throw | exactly once | 释放entry/payload | Terminal | 记录callback异常后释放 |
| Any active | reset | old generation | typed abort/回收 | Terminal | stale future events不提交 |

### 7.2 system lifecycle

`Running -> Resetting -> Running`或`Running -> Draining -> Drained`。Draining/Resetting期间新submit返回Busy且零副作用；Drained后submit为Rejected生命周期错误。reset与同cyclecompletion按Reset phase优先，避免旧completion先提交。

## 8. 性能与资源设计

EventQueue采用按`(cycle, phase, sequence)`排序的最小堆，schedule/pop为`O(log E)`；handler查找为稳定ID的`O(1)`表。completion和bridge容量由config明确限制，满时`send(false)`。内存预算：`E*sizeof(Event)+O*sizeof(CompletionEntry)+B*sizeof(BridgeEntry)+sum(payload)`。

clock换算不使用浮点累积误差；ratio通过整数phase accumulator决定组件tick。`get_tCK()`只报告profile定义的物理值，不作为EventQueue排序单位。宿主wall-clock只作测试hang guard。

## 9. 错误、恢复与可观测性

公开错误分为构造错误、准入Busy、协议Rejected、terminal HBF status和integrity error。只有terminal status可携带read payload；data-valid由typed completion显式给出。unknown/duplicate token、过去事件、sequence溢出、credit守恒破坏触发fail-fast snapshot，不映射为普通Host成功/失败。

核心counter：`ram.attempts/accepted/backpressured/callbacks`、`system.accepted/terminal/outstanding`、`system.read/write_accepted_bytes`、`system.read/write_completed_requests`、`system.read/write_completed_bytes`、`system.failed_requests`、`system.latency_samples/sum/min/max`、`system.first/last_completion_cycle`、`event.scheduled/dispatched/stale/max_depth`、`reset.count`、`drain.cycles/timeouts`。守恒式为：

```text
attempts = accepted + backpressured + rejected
accepted = terminal + outstanding
scheduled = dispatched + queued + cancelled_or_stale
bridge_entries = outstanding deliveries
```

snapshot包含cycle、generation、mode、最早事件、各phase深度、活跃token状态和各模块idle reason；不含pointer、wall-clock或unordered迭代顺序。

## 10. 测试与验收

| Test ID | 需求 | 层级 | Production path | 场景 | Oracle | 证据 |
|---|---|---|---|---|---|---|
| RAM-T01/T02 | RAM-001/002 | component | Factory->adapter | 构造/ABI/clock/tx bytes | 当前Ramulator真实接口可调用 | E2 |
| RAM-T03/T04 | RAM-003/004 | component | Frontend->adapter->system admission | 满队列、对象寿命 | false零差异；accepted可完成 | E2 |
| RAM-T05/T06 | RAM-005/006 | component | real bridge/registry | read/write、重复terminal | depart先设、callback一次、ID稳定 | E2 |
| RAM-T07/T08 | RAM-007/008 | component | ConfigNode->01->system->Stats | 配置/统计导出 | hash一致且计数守恒 | E2 |
| SYS-T01/T02 | SYS-001/002 | unit/component | real EventQueue/system | 同cycle乱序插入 | digest稳定、phase正确 | E1/E2 |
| SYS-T03/T07 | SYS-003/007 | system | production HBF pipeline | Busy/Rejected/terminal | 守恒且零副作用 | E3 |
| SYS-T04 | SYS-004 | system fault | production pipeline | reset与completion同cycle | stale不提交、一次abort | E3 |
| SYS-T05 | SYS-005 | system | production pipeline | drain成功/超限 | 全idle或typed snapshot | E3 |
| SYS-T06 | SYS-006 | unit/component | clock registry+system | 多ratio长运行 | 精确tick数、无倒退/溢出 | E1/E2 |
| SYS-T08 | SYS-008 | component | live system snapshot | 非idle/读取两次 | cycle不变、序列化相同 | E2 |

实验执行、证据保存和状态更新统一遵循
[`实验与测试执行规范.md`](实验与测试执行规范.md)。

验收必须使用真实`OpenHbxSystem`对象图形成E3证据；仅adapter object编译或fake pipeline不能证明HBF端到端完成。当前所有项保持`PLANNED`。


> **源码同步（2026-09-15）**：当前仓库已落地对应 `include/openhbx/...` 公开接口及 `src/...` 生产实现；文档中的测试路径和验证状态仍以实际 CMake/CTest artifact 为准。仅有源码不构成 `VERIFIED` 证据。
