# OpenHBX 系统集成与确定性事件内核底层设计

**文档版本**：V1.0
**文档位置**：`docs/LLD_02_SYSTEM_INTEGRATION_EVENT_KERNEL.md`
**对应HLD完整路径**：`docs/HLD_02_SYSTEM_INTEGRATION_EVENT_KERNEL.md`
**客观状态**：🟡 `PARTIAL`；system kernel生产文件已落地，integration adapter及系统级测试仍在完善。
**适用基线**：OCP HBF v0.7.0；当前 `thirdparty/ramulator2`
**覆盖需求**：`RAM-001`至`RAM-008`、`SYS-001`至`SYS-008`

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 定义Ramulator桥接、唯一system、事件、generation、completion和drain实现 |

**目录与存储位置**

| 项目 | 仓库相对位置 | 内容 |
|---|---|---|
| 本LLD | `docs/LLD_02_SYSTEM_INTEGRATION_EVENT_KERNEL.md` | 实现级权威设计 |
| 对应HLD | `docs/HLD_02_SYSTEM_INTEGRATION_EVENT_KERNEL.md` | 边界与需求权威来源 |
| System头/实现 | `include/openhbx/system/`、`src/system/` | cycle、event、completion、lifecycle |
| Adapter头/实现 | `include/openhbx/integration/`、`src/integration/` | Ramulator ABI、Request与Stats bridge |
| 计划测试 | `tests/unit/system/`、`tests/component/integration/`、`tests/system/` | E1--E3 oracle |
| 正式artifact | `build/artifacts/<suite>/<experiment-id>/<run-id>/` | event digest、snapshot与验证证据；目录须被`.gitignore`覆盖 |

## 1. 模块概述与约束

本LLD把02 HLD下沉为可编码文件和接口。当前Ramulator ABI以`thirdparty/ramulator2/src/ramulator/memory_system/i_memory_system.h`、`base/request.h`、`base/base.h`和`memory_system/impl/generic_dram_system.cpp`为源码事实。Adapter目标名计划为`OpenHBX`，可在迁移期接受`OpenHBF`别名但canonical resolved config只写`OpenHBX`。

本阶段只装配HBF生产路径；HBM/LPDDR请求不进入运行时。OpenHBX core使用C++17、整数cycle和单线程确定性事件语义；未来并行执行不得改变observable ordering。

## 2. 需求到实现映射

| 需求ID | HLD章节 | 实现文件/符号 | 状态/算法 | Test ID | 当前状态 |
|---|---|---|---|---|---|
| RAM-001/002 | 1,4.1 | `OpenHbxMemorySystem` | Ramulator implementation | RAM-T01/T02 | PLANNED |
| RAM-003/004 | 4.1 | `RamulatorRequestBridge::try_accept` | two-phase reservation | RAM-T03/T04 | PLANNED |
| RAM-005/006 | 4.1/4.3 | `deliver`/converter | depart-before-callback | RAM-T05/T06 | PLANNED |
| RAM-007 | 4.1 | `RamulatorConfigConverter` -> 01 | canonical conversion | RAM-T07 | PLANNED |
| RAM-008 | 4.5 | `RamulatorStatsBridge` | owner-tagged export | RAM-T08 | PLANNED |
| SYS-001/007 | 4.2 | `OpenHbxSystem` | sole facade/admission | SYS-T01/T07 | PLANNED |
| SYS-002 | 4.2 | `EventQueue` | heap ordered tuple | SYS-T02 | PLANNED |
| SYS-003 | 4.3 | `CompletionRegistry` | exactly-once state machine | SYS-T03 | PLANNED |
| SYS-004 | 4.4 | `SystemLifecycle::reset` | generation fence | SYS-T04 | PLANNED |
| SYS-005 | 4.4 | `OpenHbxSystem::drain` | quiescence loop | SYS-T05 | PLANNED |
| SYS-006 | 4.2 | `ClockDomainRegistry` | integer accumulator | SYS-T06 | PLANNED |
| SYS-008 | 4.5 | `SnapshotAggregator` | deterministic value snapshot | SYS-T08 | PLANNED |

## 3. 核心类型与数据结构

```cpp
using Cycle = std::uint64_t;
using Generation = std::uint64_t;
enum class SubmitCode { Accepted, Busy, Rejected };
enum class EventPhase : std::uint8_t {
  Reset, MediaCommit, ControllerCompletion, Interconnect,
  CreditReturn, ControllerSchedule, HostSchedule, FinalDelivery
};
enum class ObligationState { Accepted, Issued, Delivering };

struct EventKey { Cycle due; EventPhase phase; std::uint64_t sequence; };
struct Event { EventKey key; HandlerId handler; Generation generation; EventPayload payload; };
```

| 字段 | 类型/单位 | 合法范围/默认 | owner与生命周期 | 不变量 |
|---|---|---|---|---|
| `cycle_` | `Cycle` | 0..max | OpenHbxSystem | 不倒退 |
| `generation_` | `Generation` | >=1 | lifecycle | reset单调增加 |
| `sequence_` | uint64 | 0..max | EventQueue | 每次accepted schedule唯一 |
| `RequestToken` | strong uint64 | 非0 | CompletionRegistry | 活跃期唯一 |
| `BridgeEntry.callback` | callable | 可为空则no-op | bridge至delivery | 最多调用一次 |
| `BridgeEntry.request_view` | owned completion fields | ABI合法 | bridge | 不依赖临时转换对象 |
| `ClockDomain.accumulator` | integer | `< denominator` | clock registry | 无浮点漂移 |

`EventPayload`是受限variant或stable handle，不允许`void*`。跨模块对象引用通过构造期port持有，future event只保存handler ID。

## 4. 头文件与公开边界

| 头文件 | 可见性 | 导出内容 | 允许依赖 | 禁止内容 |
|---|---|---|---|---|
| `include/openhbx/system/open_hbx_system.h` | public | submit/tick/reset/drain/snapshot | public ports/config | Ramulator Request |
| `include/openhbx/system/host_request.h` | public | request/completion值类型 | common types | callback ABI |
| `include/openhbx/system/event_queue.h` | module-public | Event/EventQueue/handler registry | common types | module私有state pointer |
| `include/openhbx/system/completion_registry.h` | internal | obligation API | host types | Ramulator header |
| `include/openhbx/system/system_lifecycle.h` | internal | mode/reset/drain helpers | system ports | 介质私有状态 |
| `include/openhbx/integration/ramulator_memory_system.h` | adapter | `IMemorySystem`实现 | Ramulator+system facade | NAND/controller实现头 |
| `include/openhbx/integration/ramulator_request_bridge.h` | adapter | converter/bridge | Ramulator Request+host types | event scheduling策略 |
| `include/openhbx/integration/ramulator_stats_bridge.h` | adapter | stats export | Ramulator Stats+snapshot | counter ownership转移 |

## 5. 函数与接口详细设计

### 5.1 `bool OpenHbxMemorySystem::send(Ramulator::Request& req)`

**定义**：`src/integration/ramulator_memory_system.cpp`；对应RAM-002至005。调用方向Frontend -> Adapter。参数`req`借用；仅Accepted建立completion关联。前置：adapter完成init/setup且system Running。流程为validate、构造candidate、联合预留bridge/system credit、commit。Busy返回false且req所有字段不变；非法size/type按冻结策略抛配置/参数错误或返回typed rejection，不能假接受。异步completion设置`depart`后调用`req.callback(req)`一次。

### 5.2 `SubmitResult OpenHbxSystem::try_submit(HostRequest&& req)`

调用方向Adapter/DirectRunner -> System。Accepted转移request/payload并分配token；Busy/Rejected由调用者保留且system无副作用。函数不推进cycle，不同步完成。Rejected只用于非法生命周期或永不可能合法的输入；临时容量不足必须Busy。

### 5.3 `ScheduleResult EventQueue::schedule(EventSpec&& spec, Cycle now, EventPhase current)`

Accepted时生成sequence并取得payload；要求`due>now`，或`due==now && phase>current`。过去事件、关闭phase、未知handler或sequence溢出Rejected且payload仍归调用者。派发检查generation后调用注册handler；stale只释放payload并计数。

### 5.4 `FinishResult CompletionRegistry::finish_once(Completion&& completion)`

查找token并验证generation/state。成功先把entry转为Delivering，再交FinalDelivery事件；duplicate/unknown/stale返回不同integrity code。terminal payload只在成功时转移。callback返回或抛出后都erase entry并归还credit。

### 5.5 `DrainResult OpenHbxSystem::drain(Cycle max_cycles)`

将mode置Draining，循环检查全局idle；非idle则`tick()`，最多推进`max_cycles`。成功返回最终cycle和snapshot digest；超限返回`DrainTimeout`及非idle owners，不改变为Drained。宿主timeout不进入判定。

### 5.6 `void OpenHbxSystem::reset(const ResetRequest&)`

在下一个Reset phase执行：停止准入、checked increment generation、终止旧obligation、通知模块reset、清理/标记旧event，完成后回Running。reset不等同stats reset。generation溢出为不可恢复integrity error。

### 5.7 `void OpenHbxMemorySystem::tick()`及查询API

`tick()`只调用system一次；`get_clock_ratio()`返回01 resolved config整数值；`get_tCK()`返回HBF system domain配置值或明确unsupported值；`get_tx_bytes()`固定返回resolved Host transaction bytes（本profile为64）。查询不推进cycle。

## 6. 内部逻辑、状态机与算法

### 6.1 两阶段准入

```text
candidate = convert(req)                 // no mutation
if !bridge.can_reserve(): Busy
probe = system.probe(candidate)
if probe Busy/Rejected: return unchanged
bridge_reservation = reserve()
system_reservation = reserve(probe)
token = commit both reservations         // noexcept boundary
return Accepted
```

若两个commit无法设计为noexcept，使用RAII reservation，任何异常逆序rollback；测试逐注入点比较完整snapshot digest。

### 6.2 tick算法和phase门禁

```text
for phase in EventPhase order:
  queue.dispatch_due(cycle, phase, generation)
  phase_owner.tick_if_due(phase, cycle)
assert no event remains for closed phase at current cycle
cycle = checked_add(cycle, 1)
```

同一handler在当前phase产生的同phase事件必须调度到未来cycle，防止无界递归。同cycle新建后续phase事件按sequence进入本cycle后续派发。

### 6.3 obligation状态机

| 当前状态 | 事件/条件 | Guard | 动作/副作用 | 下一状态 | 失败处理 |
|---|---|---|---|---|---|
| Absent | commit admission | capacities reserved | 插入entry、accepted++ | Accepted | rollback |
| Accepted | mark_issued | token/gen匹配 | issued++ | Issued | integrity report |
| Accepted/Issued | finish | terminal/gen匹配 | terminal数据入entry | Delivering | duplicate/stale report |
| Delivering | FinalDelivery | current generation或reset abort | depart、callback、release | Absent | callback异常记录后release |
| Active | reset | old generation | 构造Aborted terminal | Delivering | 无第二terminal |

### 6.4 ClockDomain算法

每system tick执行`accumulator += numerator`，当`accumulator >= denominator`时对组件tick并减denominator；参数初始化时约分且checked。一个system tick可能需要多次component tick时，显式循环并设置每cycle最大值防止错误配置无限循环。

## 7. 逐文件设计

### 7.1 文件总表

| 路径 | 动作 | owner | 主要职责 | API/类型 | 依赖 | 需求 | 测试 | 状态 |
|---|---|---|---|---|---|---|---|---|
| `include/openhbx/hbf/host/host_types.h`、`include/openhbx/integration/request_bridge.h`、`src/integration/request_bridge.cpp` | 已实现 | Integration/Host | canonical request/completion bridge | value types | common | RAM-004/006 | RAM-T04/T06 | PARTIAL |
| `include/openhbx/system/event_queue.h`、`src/system/event_queue.cpp` | 新增/迁移 | System | deterministic events | `EventQueue` | common event | SYS-001/002/004、CHAN-007 | SYS-T01/T02/T04、CHAN-T07 | PLANNED |
| `include/openhbx/system/handler_ids.h` | 新增 | System | HandlerId 分段分配（system=1、pal=2、media slice 段 [1024,1040)） | `kSystemHandlerId`/`kPalHandlerId`/`media_slice_handler()` | strong types | CHAN-008 | CHAN-T08 | 🔵 `IMPLEMENTED` |
| `include/openhbx/system/observability.h`、`src/system/observability.cpp` | 新增 | System | structured event journal与确定性renderer | `ObservedEvent`/`EventJournal` | strong types | SYS-002/008 | SYS-T02/T08 | PARTIAL |
| `include/openhbx/system/completion_registry.h`、`src/system/completion_registry.cpp` | 新增/迁移 | System | exactly once | `CompletionRegistry` | host request | SYS-003/007 | SYS-T03/T07 | PLANNED |
| `include/openhbx/system/system_lifecycle.h`、`src/system/system_lifecycle.cpp` | 新增 | System | reset/drain/mode | lifecycle | module ports | SYS-004/005 | SYS-T04/T05 | PLANNED |
| `include/openhbx/system/clock_domain.h`、`src/system/clock_domain.cpp` | 新增 | System | integer ratios | registry | checked math | SYS-006 | SYS-T06 | PLANNED |
| `include/openhbx/system/open_hbx_system.h`、`src/system/open_hbx_system.cpp` | 新增/迁移 | System | unique facade/object owner | `OpenHbxSystem` | all public ports | SYS-001/007/008 | SYS-T01/T07/T08 | PLANNED |
| `include/openhbx/integration/ramulator_memory_system.h`、`src/integration/ramulator_memory_system.cpp` | 新增/迁移 | Integration | Factory/IMemorySystem | adapter API | Ramulator,01 | RAM-001/002/007 | RAM-T01/T02/T07 | PLANNED |
| `include/openhbx/integration/ramulator_request_bridge.h`、`src/integration/ramulator_request_bridge.cpp` | 新增/迁移 | Integration | Request/callback bridge | bridge | Ramulator,system | RAM-003..006 | RAM-T03..06 | PLANNED |
| `include/openhbx/integration/ramulator_stats_bridge.h`、`src/integration/ramulator_stats_bridge.cpp` | 新增 | Integration | Stats export | stats bridge | Ramulator Stats | RAM-008 | RAM-T08 | PLANNED |
| `tests/component/integration/test_ramulator_adapter.cpp` | 新增 | Verification | real ABI/factory/bridge | fixture | Ramulator+production adapter | RAM-* | RAM-T01..08 | PLANNED |
| `tests/unit/system/test_event_kernel.cpp` | 新增 | Verification | event/clock/registry | table/fuzz | production system core | SYS-001/002/003/006 | SYS-T01/02/03/06 | PLANNED |
| `tests/system/test_hbf_lifecycle.cpp` | 新增 | Verification | production reset/drain | HBF config | full object graph | SYS-003/004/005/007/008 | SYS-T03/04/05/07/08 | PLANNED |

### 7.2 生产文件逐项解析

#### Host request types（`include/openhbx/hbf/host/host_types.h`）

**职责与owner**：System模块唯一拥有canonical request/completion、payload handle和validation；不含Ramulator callback。**API**：value constructors、`validate_host_request()`。**输入输出**：adapter字段输入，typed request/completion输出。**状态与所有权**：Accepted后payload归system，delivery后释放。**错误与边界**：非法size/op/payload返回typed validation error且不转移所有权。**依赖**：common strong types。**需求/测试/状态**：RAM-004/006，RAM-T04/T06，`PLANNED`。

#### `EventQueue`（`include/openhbx/system/event_queue.h`、`src/system/event_queue.cpp`）

**职责与owner**：System模块唯一拥有future-event heap、sequence、handler registry和phase cursor。**API**：`schedule()`、`dispatch_due()`、`snapshot()`。**输入输出**：EventSpec输入，accepted/rejected及handler value event输出。**状态与所有权**：Accepted取得payload至派发/回收。**错误与边界**：过去事件、关闭phase、未知handler、sequence溢出拒绝；stale只回收。**依赖**：common event/checked math。**需求/测试/状态**：SYS-001/002/004，SYS-T01/T02/T04，`PLANNED`。

#### `CompletionRegistry`（`include/openhbx/system/completion_registry.h`、`src/system/completion_registry.cpp`）

**职责与owner**：System模块唯一拥有token obligation表和exactly-once terminal；不解释HBF status。**API**：`register_request()`、`mark_issued()`、`finish_once()`、`abort_generation()`。**输入输出**：token/sink/completion输入，FinalDelivery obligation输出。**状态与所有权**：Accepted至callback结束持有entry。**错误与边界**：duplicate、unknown、stale token分别诊断，绝不产生第二callback。**依赖**：HostRequest/EventQueue。**需求/测试/状态**：SYS-003/007，SYS-T03/T07，`PLANNED`。

#### `SystemLifecycle`（`include/openhbx/system/system_lifecycle.h`、`src/system/system_lifecycle.cpp`）

**职责与owner**：System模块协调Running/Resetting/Draining/Drained及所有`ILifecyclePort`；不直接清空模块私有容器。**API**：`request_reset()`、`begin_drain()`、`collect_idle_reasons()`。**输入输出**：lifecycle命令输入，mode/generation/诊断输出。**状态与所有权**：拥有mode与generation，模块状态仍归各port。**错误与边界**：reset/drain冲突、generation溢出和超限typed失败。**依赖**：公开lifecycle ports。**需求/测试/状态**：SYS-004/005，SYS-T04/T05，`PLANNED`。

#### `ClockDomainRegistry`（`include/openhbx/system/clock_domain.h`、`src/system/clock_domain.cpp`）

**职责与owner**：System模块唯一维护整数clock ratio和phase accumulator；不使用浮点推进。**API**：`add_domain()`、`ticks_due()`。**输入输出**：约分后的ratio与system tick输入，component tick count输出。**状态与所有权**：每domain accumulator由registry持有。**错误与边界**：零分母、溢出和单cycle异常tick数构造/运行失败。**依赖**：checked math。**需求/测试/状态**：SYS-006，SYS-T06，`PLANNED`。

#### `OpenHbxSystem`（`include/openhbx/system/open_hbx_system.h`、`src/system/open_hbx_system.cpp`）

**职责与owner**：System模块唯一拥有组件`unique_ptr`、cycle、EventQueue、completion registry和generation，并按phase驱动；不实现Host/Controller/Media算法。**API**：`try_submit()`、`tick()`、`reset()`、`drain()`、`snapshot()`。**输入输出**：HostRequest/lifecycle命令输入，submit/terminal/snapshot输出。**状态与所有权**：成功构造后拥有完整对象图。**错误与边界**：Busy/Rejected零event/token副作用，integrity failure保留snapshot。**依赖**：01 config及所有公开port。**需求/测试/状态**：SYS-001/007/008，SYS-T01/T07/T08，`PLANNED`。

#### `OpenHbxMemorySystem`（`include/openhbx/integration/ramulator_memory_system.h`、`src/integration/ramulator_memory_system.cpp`）

**职责与owner**：Integration模块唯一实现Ramulator `IMemorySystem`/`Implementation`、Factory注册及生命周期转发；不承载设备算法。**API**：`init/setup/send/tick/finalize/get_clock_ratio/get_tCK/get_tx_bytes`。**输入输出**：ConfigNode/Request输入，bool/callback/stats输出。**状态与所有权**：拥有一个`OpenHbxSystem`和adapter counters。**错误与边界**：setup不递归；构造失败不发布实现；finalize drain失败明确报告。**依赖**：Ramulator ABI、01 composer、system facade。**需求/测试/状态**：RAM-001/002/007，RAM-T01/T02/T07，`PLANNED`。

#### `RamulatorRequestBridge`（`include/openhbx/integration/ramulator_request_bridge.h`、`src/integration/ramulator_request_bridge.cpp`）

**职责与owner**：Integration模块唯一转换Request字段并关联原callback；不映射物理地址或添加延迟。**API**：`make_candidate()`、`try_accept()`、`deliver()`。**输入输出**：借用Request，输出HostRequest及terminal回填。**状态与所有权**：联合Accepted后拥有BridgeEntry至callback结束。**错误与边界**：false路径Request/bridge/system均不变；invalid type/size无entry；callback异常后仍释放。**依赖**：当前`base/request.h`、HostRequest、CompletionRegistry。**需求/测试/状态**：RAM-003..006，RAM-T03..06，`PLANNED`。

#### `RamulatorStatsBridge`（`include/openhbx/integration/ramulator_stats_bridge.h`、`src/integration/ramulator_stats_bridge.cpp`）

**职责与owner**：Integration模块只拥有adapter边界counter并把system权威统计导出Ramulator树；不重复拥有设备吞吐。**API**：`register_stats()`、`update_from()`、`reset_epoch()`。**输入输出**：SystemStats只读输入，Ramulator Stats view输出。**状态与所有权**：保存adapter counters与epoch基线。**错误与边界**：update/reset不推进cycle、不reset设备；字段冲突构造失败。**依赖**：Ramulator Stats和system snapshot。**需求/测试/状态**：RAM-008，RAM-T08，`PLANNED`。

### 7.3 测试文件解析

`test_ramulator_adapter.cpp`通过真实Factory和当前Ramulator Request类型测试ABI、false retry、字段保存、depart/callback和Stats，fake只允许替代completion sink，最高E2。

`test_event_kernel.cpp`表驱动phase/generation/overflow/ratio并随机打乱插入顺序；oracle是event digest、状态和守恒，E1。

`test_hbf_lifecycle.cpp`必须使用01 composer创建真实Host到NAND对象图，注入reset、队列满和drain超限；oracle通过公开snapshot/completion观察，目标E3。

## 8. 配置、错误、统计与Debug

| 配置键 | 类型/单位 | 默认/范围 | 非法行为 | 动态性 |
|---|---|---|---|---|
| `memory_system.impl` | enum | `OpenHBX` | Factory错误 | 静态 |
| `system.clock_ratio` | integer | 1，>=1 | 构造失败 | 静态 |
| `system.max_events` | count | profile值，>0 | 构造失败/满时Busy | 静态 |
| `system.max_outstanding` | count | profile值，>0 | 满时Busy | 静态 |
| `system.drain_cycle_budget` | cycle | 显式正数 | 0拒绝 | 每次调用可覆盖 |
| `system.tck_ns` | ns | profile/vendor值 | 缺来源时标synthetic或BLOCKED | 静态 |

错误：Request永久非法映射`RejectedRequest`；暂时容量不足映射false/Busy；HBF执行失败以terminal status返回；integrity错误不伪装Host状态。read数据的`data_valid`由completion携带。

统计递增点：attempt在`send`入口；accepted只在联合commit；issued只在下游issue；terminal只在`deliver_once`成功；callback只在实际调用；outstanding为accepted-terminal派生。`OpenHbxSystemStats`新增以下Host边界字段：

| 字段 | 类型 | 唯一递增点 | 口径 |
|---|---|---|---|
| `read_accepted_bytes`/`write_accepted_bytes` | `uint64_t` | FlashIo联合准入成功 | 按operation加请求size；非FlashIo不计 |
| `read_completed_bytes`/`write_completed_bytes` | `uint64_t` | FlashIo terminal分类完成 | Write成功加请求size；Read仅成功/corrected、data-valid且完整payload时加请求size |
| `read_completed_requests`/`write_completed_requests` | `uint64_t` | FlashIo terminal分类完成 | 满足相应成功规则的请求数 |
| `failed_requests` | `uint64_t` | FlashIo terminal分类完成 | abort、UECC、失败status、无效或不完整Read payload |
| `latency_samples`/`latency_sum_cycles` | `uint64_t` | FlashIo terminal分类完成 | 样本数与checked累计`terminal_cycle - accepted_cycle` |
| `latency_min_cycles`/`latency_max_cycles` | `uint64_t` | FlashIo terminal分类完成 | 首个样本初始化；无样本时为0 |
| `first_completion_cycle`/`last_completion_cycle` | `uint64_t` | FlashIo terminal | 全生命周期最早/最晚cycle；无样本时为0 |

System在accepted token记录中保存`accepted_cycle`、request size、packet type和operation，terminal分类后一次性更新上述字段；duplicate/stale terminal不得二次更新。corrected Read包括可纠正且对Host返回成功数据的状态，但必须同时满足`data_valid=true`和payload size等于请求size。只有`FlashIo`参与这些Host吞吐字段；Admin、CSR和Scratchpad完成仍进入通用terminal守恒，但不进入byte、成功/失败请求或latency测量口径。

统计跨reset累计，reset不得清零字段或丢失已经terminal的样本。所谓stats reset只是在consumer侧保存新的baseline snapshot，不修改System内部累计值或lifecycle表。一次测量使用`end - baseline`计算bytes、请求数和latency；测量窗口的首/末completion cycle由baseline之后实际出现的terminal确定，不能直接复用全生命周期`first_completion_cycle`。PAL、Controller和NAND只更新各自owner统计，禁止把传输byte、cache byte或介质byte再次累加进上述System Host byte字段。

Debug event字段固定为cycle/phase/sequence/handler/token/generation/type/result；snapshot按token和handler ID排序。失败artifact额外保存first divergence token和前后event window。

## 9. 测试用例设计

| Test ID | Objective/输入 | Production path/fake边界 | Oracle | Budget/证据 |
|---|---|---|---|---|
| RAM-T01 | Factory构造OpenHBX | real Ramulator Factory->01->adapter | 单system且impl可查询 | E2 |
| RAM-T02 | 全虚函数查询/tick | real adapter/system | tx=64、ratio一致、tick+1 | E2 |
| RAM-T03 | bridge/system容量满 | real admission；下游idle fixture | false前后Request/system digest相同 | E2 |
| RAM-T04 | accepted后调用栈对象边界 | real bridge/registry | metadata/payload完成正确 | E2 |
| RAM-T05 | success/error/duplicate completion | real delivery | depart先设、callback一次 | E2 |
| RAM-T06 | Read/Write ID与非法type | compile static_assert+runtime converter | 0/1不变；非法无污染 | E0/E2 |
| RAM-T07 | ConfigNode/YAML等价 | real converter/resolver | resolved hash相同 | E2 |
| RAM-T08 | accepted/backpressure/stats reset | real adapter Stats | counter守恒且设备不reset | E2 |
| SYS-T01/T02 | 多owner同cycle事件 | real queue/system ports | 固定phase/sequence digest | E1/E2 |
| SYS-T03 | terminal重复/乱序 | real registry | accepted=terminal+outstanding | E1/E3 |
| SYS-T04 | reset同cycleMediaCommit | full HBF production path | reset优先、旧commit不可见 | E3 |
| SYS-T05 | drain成功和cycle超限 | full path | 全owner idle或完整诊断 | E3 |
| SYS-T06 | ratio 1:1、1:3、3:2及边界 | real clock registry | tick计数公式精确 | E1 |
| SYS-T07 | Busy/Rejected故障注入 | full admission | 0 token/event/payload差异 | E3 |
| SYS-T08 | 活跃请求snapshot | real system | 两次读取不推进且稳定 | E2/E3 |

实验执行、证据保存和状态更新统一遵循
[`实验与测试执行规范.md`](实验与测试执行规范.md)。E3 artifact必须含resolved config、completion summary、stats、event digest、final snapshot；失败另存outstanding和event window。

## 10. 实施顺序、ADR与完成定义

实施顺序：冻结Ramulator ABI静态检查；迁移common强类型/EventQueue；实现completion和clock；实现唯一system；实现Request/Stats bridge与Factory；接01 composer；最后运行component和真实HBF system测试。

| ADR ID | 决策 | 背景/约束 | 备选方案 | 选择理由 | 代价/后果 | 状态 |
|---|---|---|---|---|---|---|
| ADR-02-001 | 一个system、一个EventQueue | 跨模块确定性 | 每模块独立queue | reset/drain/守恒可证明 | queue是共享关键路径 | PLANNED |
| ADR-02-002 | future event只保存handler ID/value | reset后对象寿命 | 保存裸pointer | generation隔离且可序列化 | 需handler registry | PLANNED |
| ADR-02-003 | 两阶段联合准入 | Ramulator false要求零副作用 | 先入bridge再尝试system | 精确回滚 | API设计更严格 | PLANNED |
| ADR-02-004 | system统计权威、adapter只记边界 | 防重复吞吐 | 各层自由累计总量 | owner清晰可守恒 | 查询需聚合 |
| ADR-02-005 | reset phase先于commit | 同cycle竞态 | sequence决定 | stale状态不可提交 | reset语义需全模块配合 | PLANNED |

完成定义：所有生产/测试文件和Factory/CMake链接落地；01能构造唯一HBF对象图；Ramulator真实Frontend能反压重试并收到一次callback；EventQueue、generation、reset和drain守恒成立；RAM-T01至08达到E2，SYS目标项按表达到E1/E2/E3；无固定延迟旁路、第二事件队列或未关闭`BLOCKED_SPEC`进入正式profile。当前状态为`PLANNED`。


> **源码同步（2026-09-15）**：当前仓库已落地对应 `include/openhbx/...` 公开接口及 `src/...` 生产实现；文档中的测试路径和验证状态仍以实际 CMake/CTest artifact 为准。仅有源码不构成 `VERIFIED` 证据。
