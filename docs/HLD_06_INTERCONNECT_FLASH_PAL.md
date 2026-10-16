# OpenHBX 互联与 Flash PAL 概要设计

**文档版本**：V1.0
**文档位置**：`docs/HLD_06_INTERCONNECT_FLASH_PAL.md`
**对应文档**：`docs/LLD_06_INTERCONNECT_FLASH_PAL.md`
**设计状态**：🟡 `PARTIAL`（`status: PARTIAL`）；hybrid bonding依赖🔴 `BLOCKED_SPEC`（`dependency_status: BLOCKED_SPEC`）
**适用基线**：OCP HBF v0.7.0；OpenHBX 首期 `OCP_HBF_0_7`

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 按首期仅完成HBF的范围建立互联与Flash PAL设计 |

## 1. 模块概述

本模块位于HBF控制器与NAND Media之间，回答“4 KiB物理命令如何经封装与垂直路径异步到达目标资源”。Physical Abstraction Layer（PAL）是OpenHBX内部架构名，不是OCP HBF术语；其设计借鉴SimpleSSD/HFSSS的命令分阶段和EAT思想，但转换为唯一EventQueue、整数cycle和typed completion。

本模块包含：package/vertical route、Transfer queue/credit、lane/TSV资源EAT、故障检测与spare repair、`FlashPhysicalRequest`派发和completion回传。不包含Host UCIe/AXI协议、地址映射、DLU聚合/ECC/replay、NAND Page/Block/payload或NAND array阶段。NAND状态唯一归07。

关键不变量：Accepted请求精确一次terminal；Busy/Rejected零副作用；只预约本模块资源；事件携带token与generation而非裸指针；reset后的stale事件不抵达07；repair epoch在每次route reservation时冻结；PAL completion不等于Host completion。

首期正式profile为带来源标签的TSV synthetic baseline。`TSV + hybrid bonding`仅允许外部/vendor研究profile，在pitch、lane、BER、latency、energy及thermal数据齐备前不是HBF合规门槛。

## 2. 需求回顾与追踪

| 需求ID | 需求描述 | 优先级 | 版本 | 实现状态 |
|---|---|---|---|---|
| PAL-001 | PAL只接受完整4 KiB Read/Program或Block Erase | P0 | V1.0 | ⚪ `PLANNED` |
| PAL-002 | Accepted经唯一EventQueue精确一次terminal | P0 | V1.0 | ⚪ `PLANNED` |
| PAL-003 | 共享route资源按EAT串行，独立资源并行 | P0 | V1.0 | ⚪ `PLANNED` |
| PAL-004 | TSV故障触发spare或typed degraded/unavailable | P0 | V1.0 | ⚪ `PLANNED` |
| PAL-005 | reset隔离旧generation并回收credit/token | P0 | V1.0 | ⚪ `PLANNED` |
| PAL-006 | profile携带来源且理论带宽可推导 | P1 | V1.0 | ⚪ `PLANNED` |
| PAL-007 | hybrid bonding缺vendor数据时禁止正式profile | P1 | V1.0 | 🔴 `BLOCKED_SPEC` |

| 需求ID | 来源/证据类别 | owner组件 | Test ID |
|---|---|---|---|
| PAL-001 | OCP摘要DLU=4 KiB；规范事实/设计选择 | `FlashPal` | `VER-PAL-001` |
| PAL-002 | OpenHBX全局不变量；设计选择 | `FlashPal` | `VER-PAL-002` |
| PAL-003 | OpenHBX设计选择，参考HFSSS EAT | `InterconnectFabric` | `VER-PAL-003` |
| PAL-004 | OCP摘要Base Die负责TSV repair；规范事实 | `TsvRepairManager` | `VER-PAL-004` |
| PAL-005 | OpenHBX全局不变量；设计选择 | `FlashPal` | `VER-PAL-005` |
| PAL-006 | ADR-HBX-004/005；设计选择 | `FabricProfile` | `VER-PAL-006` |
| PAL-007 | 外部vendor依赖 | 配置校验器 | `VER-CFG-006` |

## 3. 系统架构设计

```text
05 HBF Controller --FlashPhysicalRequest--> FlashPal
       ^                                      |
       | typed completion                     +-> PackageFabric
       |                                      +-> TsvVerticalLink/EAT
       |                                      +-> TsvRepairManager
       |                                              |
       +---------------- system EventQueue <----------+
                                                      |
                                         07 NandMediaRouter
                                          -> N x NandChannelSlice
```

| 组件 | 主要文件 | 职责 | 输入 | 输出 | 拥有状态 | 依赖 |
|---|---|---|---|---|---|---|
| `FlashPal` | `flash_pal.*` | admission、阶段编排、terminal义务 | physical request | issue result/completion | inflight、credit | EventKernel、fabric、media port |
| `InterconnectFabric` | `fabric.*` | route、传输预约及完成 | `Transfer` | transfer event | queue、resource EAT | immutable profile |
| `TsvRepairManager` | `tsv_repair.*` | fault、spare、epoch化route | fault/route query | repaired route | active/spare map、epoch | fabric topology |
| `FabricProfile` | `fabric_profile.*` | 参数、来源和能力 | resolved config | immutable profile | 无运行态 | 配置模块 |

全局时间与future event由02拥有；06只持EventKernel端口。07只在收到media issue后拥有NAND command lifecycle。

### 3.1 FlashPal

**文件路径**：`include/openhbx/pal/flash_pal.h`、`src/pal/flash_pal.cpp`

**职责**：协调forward transfer、media issue、return transfer和terminal。  
**关键组件**：`FlashPal`、`PalContext`、PAL FSM。  
**输入输出**：05结合04旁路结果形成的request -> 05 typed completion。  
**状态所有权**：06拥有inflight、PAL credit和terminal义务。  
**依赖边界**：依赖02、Fabric与07窄端口；不拥有04或07状态。  
**对应需求**：PAL-001/002/005。  
**实现状态**：⚪ `PLANNED`（`status: PLANNED`）。

> **Channel 分片注记（2026-09-20）**：`FlashPhysicalRequest` 新增强类型 `channel` 字段（05 从 `HbfAddress` 直接携带），`FlashPal::issue_media` 透传至 `FlashCommand.channel`，不再在 PAL→Media 边界丢弃 channel 维度（CHAN-002）。`IFlashMediaPort` 的实现者由单体设备改为 07 的 `NandMediaRouter`（ADR-CHAN-003），06 接口签名不变。见 [`HBF_CHANNEL_PARTITIONED_MODEL.md`](HBF_CHANNEL_PARTITIONED_MODEL.md) §5.2/§5.5。

### 3.2 InterconnectFabric

**文件路径**：`include/openhbx/interconnect/fabric.h`、`src/interconnect/fabric.cpp`

**职责**：执行route展开、transfer reservation和带宽ceiling计算。  
**关键组件**：route table、`EatTable`。  
**输入输出**：Transfer/now -> reservation/end cycle或typed failure。  
**状态所有权**：06唯一拥有resource EAT和transfer queue。  
**依赖边界**：依赖immutable profile与02 event port。  
**对应需求**：PAL-003/006。  
**实现状态**：⚪ `PLANNED`（`status: PLANNED`）。

### 3.3 TsvRepairManager

**文件路径**：`include/openhbx/interconnect/tsv_repair.h`、`src/interconnect/tsv_repair.cpp`

**职责**：处理TSV fault、spare选择与降级结果。  
**关键组件**：active/spare map、repair epoch。  
**输入输出**：fault/route query -> repaired route/status。  
**状态所有权**：06唯一拥有TSV映射和epoch。  
**依赖边界**：依赖fabric topology及02事件phase。  
**对应需求**：PAL-004。  
**实现状态**：⚪ `PLANNED`（`status: PLANNED`）。

### 3.4 FabricProfile

**文件路径**：`include/openhbx/interconnect/fabric_profile.h`、`src/interconnect/fabric_profile.cpp`

**职责**：校验互联参数来源、范围和理论ceiling。  
**关键组件**：lane/efficiency/latency/spare/source metadata。  
**输入输出**：resolved config -> immutable profile或结构化错误。  
**状态所有权**：06构造后只读。  
**依赖边界**：只依赖00 schema。  
**对应需求**：PAL-006/007。  
**实现状态**：PAL-006为⚪ `PLANNED`；PAL-007为🔴 `BLOCKED_SPEC`。

## 4. 高层详细设计

`FlashPal`执行`validate -> reserve PAL credit -> resolve route(epoch) -> reserve transfer -> Accepted`。命令传输完成事件再调用07；07 Busy时PAL按显式策略保留请求并调度确定性retry，不重新预约已经完成的前向传输。07 Accepted后，PAL等待media terminal，再预约反向data/status transfer，最后向05返回typed completion。任一步骤同步失败均在Accepted前原子回滚；Accepted后只能走terminal成功、terminal失败或reset abort。

`InterconnectFabric`把route展开为共享resource group，完成cycle为：

```text
start = max(now, max(resource_eat))
end = start + arbitration + ceil(bits/effective_bits_per_cycle) + propagation
```

一次reservation原子更新全部EAT。Read反向数据、Program前向数据和小型命令/status可使用不同traffic class，但不得隐式获得无限带宽。

Host endpoint与物理资源不得一一等同。endpoint仍以`channel * axi_interfaces + axi_interface`标识请求来源，但同一Host Channel内的1/2/4个AXI Interface是虚拟队列、ordering和地址窗口，所有AXI route必须引用同一个Channel resource；增加AXI数量不能增加该Channel的x64 lane带宽。不同Host Channel引用不同Channel resource，可独立并行。UCIe/PAL路径按full-duplex处理，Forward和Return分别维护EAT；同一方向共享Channel ceiling，两个方向不互相串行化。

`active_lanes`和`spare_lanes`均定义为每Channel数量。每条Channel route展开完整的`active_lanes`集合，全局logical lane身份为`channel * active_lanes + lane`；16个x64 Channel因此有1024条互不混淆的active lane。repair域按Channel隔离，只能使用故障lane所属Channel的spare，禁止跨Channel借用。

`TsvRepairManager`在故障事件后增加repair epoch。新请求使用新映射；旧reservation保持冻结route，若其lane在完成前失效则按profile策略terminal失败，不静默改道。无spare时暴露`PathUnavailable`或`Degraded`能力。

## 5. 接口设计

| 接口 | 方向 | 契约 | backpressure/错误 | 时序与所有权 |
|---|---|---|---|---|
| `try_issue(FlashPhysicalRequest&&, Cycle)` | 05->06 | 仅接收04已映射的canonical物理地址和合法operation | `Accepted/Busy/Rejected` | Accepted才转移request/payload；completion异步 |
| `try_transfer(Transfer&&, Cycle)` | PAL->fabric | 原子预约route资源 | Busy/invalid route/overflow | Accepted后由fabric持有至transfer event |
| `try_issue_media(FlashCommand&&)` | PAL->07 | 不修改07内部状态 | 透传Busy/Rejected | 07 Accepted后由07拥有command |
| `on_media_completion(MediaCompletion)` | 07->PAL | token、generation和operation必须匹配 | duplicate/stale记录integrity error | PAL预约返回transfer后才上报05 |
| `inject_link_fault(FaultEvent)` | 07 RAS/test hook->repair | cycle与目标确定 | 未知资源拒绝 | 同cycle按EventKernel phase排序 |

## 6. 数据结构设计

`FlashPhysicalRequest`包含`token, generation, operation, PhysicalAddress, PayloadHandle, issue_cycle`；payload仅Program携带4096字节，Read在completion返回，Erase无payload。`Transfer`包含`id, direction, bits, route, traffic_class, repair_epoch`。`PalCompletion`包含`token, status, data_valid, payload, media_detail`。互联值类型位于`include/openhbx/interconnect/types.h`，PAL公开端口位于`include/openhbx/pal/`；内部EAT与route容器不公开。

不变量：token唯一；Read成功/CECC可携带有效payload，UECC/path failure无有效payload；EAT单调且checked arithmetic；credit计数满足`capacity = free + reserved`。

## 7. 关键流程与状态机

```text
Received -> ForwardReserved -> ForwardInFlight -> MediaPending
 -> MediaInFlight -> ReturnReserved -> ReturnInFlight -> Terminal
                    \-> FailureReturn ------------------^
Any nonterminal --reset--> Aborted；旧event -> StaleDiscarded
```

若初始credit、route或transfer无法预约，返回Busy且无token/event/EAT变化。非法地址family、payload长度或opcode返回Rejected。Media Busy采用有界retry budget；耗尽后返回typed failure。duplicate completion不再次释放资源或调用上游。

## 8. 性能与资源设计

队列深度、每Channel lane/spare数、每lane bits/cycle、可选校准效率、arbitration/propagation cycle均来自resolved profile。最大吞吐基线必须使用100% raw link能力，不得预置OCP用户带宽比例；协议开销应由显式传输字段和调度自然产生。双工按独立resource group计算；测量不得使用宿主wall-clock。空间复杂度为`O(inflight + channels * lanes + resources + routes)`。

预算归属按 OCP per-channel 子系统对齐（ADR-CHAN-009，2026-10-15）：Fabric `queue_depth` 按路由携带的资源计费（生产装配中一条路由恰好携带一个 Channel 路径资源），PAL `max_inflight` 按 channel 计费（cube 总在途 = 该值 × `host_channels`）；任一 channel 预算耗尽可能返回 Busy，但不得消耗其他 channel 的同名预算（CHAN-I7，测试 CHAN-T13/T14）。

## 9. 错误、恢复与可观测性

公开错误包括`InvalidRequest`、`NoCredit`、`PathUnavailable`、`TransferOverflow`、`MediaRejected`、`MediaFailure`、`Aborted`和`IntegrityError`。trace至少记录token、generation、route、repair epoch、resource、start/end cycle、stage和status；snapshot包含queue、EAT、active/spare map及inflight owner。统计包括transfer bits/cycles、queue high-watermark、TSV利用率、repair/degraded次数、stale/duplicate事件和retry次数。

## 10. 测试与验收

| Test ID | 层级 | production path | 核心场景 | Oracle | 证据 |
|---|---|---|---|---|---|
| `VER-PAL-001` | E1/E2 | controller port->PAL | operation/payload边界 | 非法输入零副作用 | E2 |
| `VER-PAL-002` | E2 | PAL->EventKernel->media fake port | success/failure/Busy | Accepted恰好一次terminal | E2 |
| `VER-PAL-003` | E2 | real fabric | 同/异resource route | EAT串行与并行cycle精确 | E2 |
| `VER-PAL-004` | E2/E3 | real repair+fabric | lane fault/spare耗尽 | epoch、映射、降级符合profile | E3 |
| `VER-PAL-005` | E3 | real system pipeline | 各阶段reset | stale不提交、资源归零 | E3 |
| `VER-PAL-006` | E4 | real fabric | 饱和流量 | 吞吐不越理论上限 | E4 |

实验执行、证据保存和状态更新统一遵循[`实验与测试执行规范.md`](实验与测试执行规范.md)。文档与文件落地不构成实现或验证证据。


> **源码同步（2026-09-15）**：当前仓库已落地对应 `include/openhbx/...` 公开接口及 `src/...` 生产实现；文档中的测试路径和验证状态仍以实际 CMake/CTest artifact 为准。仅有源码不构成 `VERIFIED` 证据。
