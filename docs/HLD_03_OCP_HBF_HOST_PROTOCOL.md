# OpenHBX OCP HBF Host Protocol 概要设计

**文档版本**：V1.0
**文档位置**：`docs/HLD_03_OCP_HBF_HOST_PROTOCOL.md`
**对应文档完整路径**：`docs/LLD_03_OCP_HBF_HOST_PROTOCOL.md`
**客观状态**：🟡 `PARTIAL`；Host transaction组件已有实现及E1/E2测试，逐bit UCIe/AXI外部细节仍为🔴 `BLOCKED_SPEC`。
**适用基线**：OCP HBF v0.7.0；OpenHBX 首期 `OCP_HBF_0_7`

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 按 OpenHBX 首期仅 HBF 的边界重建 Host 协议设计 |

## 1. 模块概述

### 1.1 定位与职责

本模块位于 Ramulator Frontend adapter 与地址/控制器之间，回答“一个外部请求怎样成为符合 OCP HBF 的 Host transaction，并怎样有序返回”。它拥有每 Host Channel 的 UCIe/AXI 协议状态、packet 分类、寄存器侧访问入口、ordering scoreboard、response queue 与 link-ready admission gate。

```text
Ramulator/Trace -> 03 Host Protocol -> 04 Address View -> 05 Controller
                         ^                                  |
                         +---------- typed response --------+
```

包含：最多 16 个独立 Host Channel、x64 full-duplex UCIe Streaming 抽象、1/2/4 个虚拟 AXI Interface、64 B Flash transaction、burst/DLU 边界校验、同 ID 顺序、跨 ID 同地址 hazard 顺序、Admin/CSR/sideband 分类、response 编码和 reset gate。

不包含：A1/R1-R5 映射（04）、DLU 聚合/ECC/调度（05）、TSV/介质访问（06 PAL）、NAND Page/Block 状态（07 Media）、Ramulator callback owner（02 Integration）。

关键不变量：`Busy` 零副作用；Accepted transaction 精确一次 terminal response；相同 AXI ID 按接收序返回；不同 ID 仅在没有同一 64 B 地址 hazard 时可乱序；Channel 不跨 owner 范围；`READY && BUCSTS.RDY && BUCC.EN` 前不接收 IO；generation 不匹配的 completion 不提交。

### 1.2 事实边界

- **规范事实**：OCP 摘要第 2--9 节给出的 Channel、UCIe、packet type、status、register 和 reset 语义。
- **外部依赖**：UCIe v3.0 Format 6 逐 bit header/CRC、完整 credit/FDI/RDI，AMBA AXI valid/ready；关闭条件为获得规范并形成可定位摘录。
- **OpenHBX 设计选择**：在未关闭外部依赖前提供 transaction-level、表驱动且非 bit-accurate 的协议模型。

该transaction-level边界不等于固定延迟stub：实现仍必须区分四阶段link initialization、`READY`/`BUCSTS.RDY`/`BUCC.EN` gate、1/2/4虚拟AXI Interface、request/response packet type、same-ID与同地址hazard顺序、sideband opcode、reset domain及credit/backpressure。允许省略的是PHY训练波形、逐bit flit/CRC和AMBA信号逐拍切换；这些能力在外部规范未闭环前不得进入bit-accurate conformance结论。

## 2. 需求回顾与追踪

### 2.1 需求进度总览

| 需求ID | 需求描述 | 优先级 | 版本 | 实现状态 |
|---|---|---|---|---|
| HOST-001 | 1至16个独立Host Channel | P0 | V1.0 | ⚪ `PLANNED` |
| HOST-002 | 64 B transaction不跨4 KiB DLU | P0 | V1.0 | ⚪ `PLANNED` |
| HOST-003 | typed packet分类并拒绝reserved | P0 | V1.0 | ⚪ `PLANNED` |
| HOST-004 | AXI ID与同地址hazard保序 | P0 | V1.0 | ⚪ `PLANNED` |
| HOST-005 | non-posted write terminal响应 | P0 | V1.0 | ⚪ `PLANNED` |
| HOST-006 | status/data-valid/ErrorInfo编码 | P0 | V1.0 | ⚪ `PLANNED` |
| HOST-007 | register access/reset/owner校验 | P0 | V1.0 | ⚪ `PLANNED` |
| HOST-008 | ready与`BUCC.EN`联合准入 | P0 | V1.0 | ⚪ `PLANNED` |
| HOST-009 | sideband opcode表驱动校验 | P1 | V1.0 | ⚪ `PLANNED` |
| HOST-010 | Busy/Rejected零副作用 | P0 | V1.0 | ⚪ `PLANNED` |
| HOST-011 | Format 6/AXI逐bit conformance | P0 | V1.0 | 🔴 `BLOCKED_SPEC` |

### 2.2 证据、Owner与Test追踪

| 需求ID | 可测试描述 | 来源 | 类别 | 优先级 | owner组件 | 验收/Test ID | 状态 |
|---|---|---|---|---|---|---|---|
| HOST-001 | 每 Cube 配置 1--16 个独立 Channel，跨 Channel 请求拒绝且零副作用 | OCP 摘要第2节 | 规范事实 | P0 | `HbfHostProtocol` | HOST-T01 | ⚪ `PLANNED` |
| HOST-002 | Flash read/write 基本 transaction 为 64 B，burst 不跨 4 KiB DLU | OCP 摘要第4节 | 规范事实 | P0 | `HostValidator` | HOST-T02 | ⚪ `PLANNED` |
| HOST-003 | 支持 Flash、Scratchpad、CSR/Admin packet type，reserved type 拒绝 | OCP 表9及摘要第3节 | 规范事实 | P0 | `PacketCodec` | HOST-T03 | ⚪ `PLANNED` |
| HOST-004 | 同 AXI ID 保序；不同 ID 可乱序但同 64 B 地址 hazard 保序 | OCP 摘要第3、4节 | 规范事实 | P0 | `OrderingScoreboard` | HOST-T04 | ⚪ `PLANNED` |
| HOST-005 | write 为 non-posted，只有 Controller terminal 后生成一次 response | OCP 摘要第4节 | 规范事实 | P0 | `ResponseRouter` | HOST-T05 | ⚪ `PLANNED` |
| HOST-006 | Read/Write status 与 data-valid/ErrorInfo 按表12/13编码 | OCP 表12、13 | 规范事实 | P0 | `ResponseCodec` | HOST-T06 | ⚪ `PLANNED` |
| HOST-007 | Always-On/MMIO register 按 access、reset domain、owner校验 | OCP 表15、16及摘要第8节 | 规范事实 | P0 | `RegisterPort` | HOST-T07 | ⚪ `PLANNED` |
| HOST-008 | link/Channel ready 与 `BUCC.EN` 联合控制 admission | OCP reset/power及摘要第9节 | 规范事实 | P0 | `ChannelGate` | HOST-T08 | ⚪ `PLANNED` |
| HOST-009 | sideband opcode 使用表驱动 enum 并拒绝 reserved value | OCP 表14 | 规范事实 | P1 | `SidebandCodec` | HOST-T09 | ⚪ `PLANNED` |
| HOST-010 | Busy/Rejected 不分配 token、不转移 payload、不产生 future event | ADR-HOST-001 | 设计选择 | P0 | `HbfHostProtocol` | HOST-T10 | ⚪ `PLANNED` |
| HOST-011 | Format 6/AXI wire-level 编解码在外部规范提取前不得宣称 bit-accurate | UCIe v3.0/AMBA 外部规范 | 外部依赖 | P0 | `PacketCodec` | HOST-T11 | 🔴 `BLOCKED_SPEC` |

## 3. 系统架构设计

```text
FrontendAdapter
  -> ChannelGate -> PacketCodec -> HostValidator -> OrderingScoreboard
  -> HostRequestPort(04/05)
  <- ControllerCompletion -> ResponseCodec -> ResponseRouter -> callback adapter
                         RegisterPort/SidebandCodec
```

| 组件 | 主要文件 | 职责 | 输入 | 输出 | 拥有状态 | 依赖 |
|---|---|---|---|---|---|---|
| `HbfHostProtocol` | `host_protocol.{h,cpp}` | admission与总协调 | request/event | accepted/status | Channel队列、token索引 | 02 kernel、04/05 ports |
| `HostValidator` | `host_validator.{h,cpp}` | 粒度、边界、packet字段校验 | immutable request | typed validation | 无 | resolved profile |
| `OrderingScoreboard` | `ordering_scoreboard.{h,cpp}` | ID及地址hazard barrier | accepted/completion | releasable token | sequence/address refs | 无 |
| `RegisterPort` | `register_port.{h,cpp}` | CSR/Admin/register侧语义 | register txn | value/status | register image | lifecycle port |
| `PacketCodec` | `packet_codec.{h,cpp}` | packet/sideband/response typed转换 | fields/result | envelope/response | descriptor表 | OCP tables |

所有异步事件进入 02 唯一 EventQueue；本模块不创建第二时间源。payload 仅在 Accepted 后由 transaction token 引用，terminal response 后释放。

### 3.1 `HbfHostProtocol`

**文件**：`include/openhbx/hbf/host/host_protocol.h`、`src/hbf/host/host_protocol.cpp`。**职责/关键组件**：admission、ChannelContext、dispatch和completion协调。**输入输出**：HostIngress/event到AdmissionResult/response。**owner/依赖**：03 owner；依赖02 kernel及04/05 ports。**需求/状态**：HOST-001/005/008/010；⚪ `PLANNED`。

### 3.2 `HostValidator`

**文件**：`include/openhbx/hbf/host/host_validator.h`、`src/hbf/host/host_validator.cpp`。**职责/关键组件**：粒度、DLU边界和字段校验。**输入输出**：immutable ingress到typed validation。**owner/依赖**：03 owner；依赖resolved profile。**需求/状态**：HOST-002；⚪ `PLANNED`。

### 3.3 `OrderingScoreboard`

**文件**：`include/openhbx/hbf/host/ordering_scoreboard.h`、`src/hbf/host/ordering_scoreboard.cpp`。**职责/关键组件**：ID与64B地址双barrier。**输入输出**：accepted/completion到releasable token。**owner/依赖**：03 owner；依赖stable token。**需求/状态**：HOST-004；⚪ `PLANNED`。

### 3.4 `RegisterPort`

**文件**：`include/openhbx/hbf/host/register_port.h`、`src/hbf/host/register_port.cpp`。**职责/关键组件**：descriptor、access mask和reset domain。**输入输出**：RegisterTxn到value/status/reset intent。**owner/依赖**：03 owner；依赖lifecycle port。**需求/状态**：HOST-007/008；⚪ `PLANNED`。

### 3.5 `PacketCodec`

**文件**：`include/openhbx/hbf/host/packet_codec.h`、`src/hbf/host/packet_codec.cpp`。**职责/关键组件**：packet/sideband/response typed转换。**输入输出**：fields/result到envelope/response。**owner/依赖**：03 owner；依赖OCP tables及外部wire规范。**需求/状态**：HOST-003/006/009为⚪ `PLANNED`；HOST-011为🔴 `BLOCKED_SPEC`。

## 4. 高层详细设计

### 4.1 Admission与分类

入口先检查 generation、Channel gate、队列 credit，再进行 packet type、地址粒度和 burst 边界校验。校验成功后原子预留 ordering 与 queue credit；任一步失败全部回滚。Flash IO 转发至 04/05；Scratchpad、CSR/Admin 转发到 05 对应端口。

### 4.2 Ordering与response

每 `(channel, axi_interface, axi_id)` 分配单调 sequence；每个 64 B 地址维护尚未 terminal 的 hazard 队列。Controller completion 只标记 ready，只有 ID 前序及地址前序均释放后才编码 response。Read 的 payload-valid 由 typed completion 决定；CECC 可带有效数据，UECC 不得泄露无效 payload。

### 4.3 Link、register与reset

Channel 分别经历 `Reset -> Initializing -> Ready -> Disabled/Faulted`。wire-level training 延迟由 profile 提供，未建模字段保持外部依赖标签。Register descriptor 决定 RO/RW/W1C、reserved mask、per-Channel/per-AXI owner及 reset domain；`BUCR` magic 请求生命周期端口执行 reset，`TMON.CES` 按规范跨 HBF_RESET 保持。

## 5. 接口设计

| 接口 | 方向 | 输入/输出 | 所有权与backpressure | 时序/错误 |
|---|---|---|---|---|
| `submit(HostIngress)` | 02 -> 03 | request -> `AdmissionResult` | Accepted才转移payload；Busy/Rejected零副作用 | 当前event phase |
| `dispatch(HostTransaction)` | 03 -> 04/05 | immutable txn/token | Busy则原子回滚或留在已拥有queue | 异步completion |
| `complete(ControllerCompletion)` | 05 -> 03 | token/status/payload | generation校验；stale只回收 | response phase |
| `read/write_register` | 03内部/05 | offset/value -> status | descriptor约束，不暴露内部引用 | 同步或产生reset事件 |
| `set_link_state` | lifecycle -> 03 | Channel/state/generation | 仅owner可调用 | gate变化后影响新admission |

## 6. 数据结构设计

| 结构 | owner | 生命周期 | 核心不变量 |
|---|---|---|---|
| `HostIngress` | 02至03值对象 | 单次submit | address为byte、size有显式单位 |
| `HostTransaction` | 03 | Accepted至terminal | token唯一、Channel固定 |
| `OrderingKey` | scoreboard | Accepted至release | ID序列与64 B地址序列分别单调 |
| `HostResponse` | 03至02 | response发送至callback | status/data-valid一致 |
| `RegisterDescriptor` | 静态表 | 产品生命周期 | offset唯一、reserved mask不可写 |

## 7. 关键流程与状态机

```text
receive -> gate/validate -> reserve credit+ordering
  -> Accepted -> dispatch -> controller completion -> ordering release -> response
  -> Busy/Rejected -> rollback -> no future event
```

| 当前状态 | 事件/条件 | 动作 | 下一状态 | 失败处理 |
|---|---|---|---|---|
| `Received` | gate/validation通过 | 原子预留 | `Queued` | typed reject |
| `Queued` | downstream accepted | 转移token | `InFlight` | Busy保留队首 |
| `InFlight` | completion generation匹配 | 标记ready | `ResponseHeld/Ready` | stale回收不响应 |
| `ResponseHeld` | barriers清除 | 编码一次response | `Terminal` | 不允许重复 |
| 任意非terminal | reset | 依policy终结并递增generation | `Terminal` | sticky寄存器例外 |

## 8. 性能与资源设计

队列容量按 `channels * axi_interfaces * queue_depth`；scoreboard内存为 `O(outstanding)`，地址索引为 `O(unique_64B_addresses)`。UCIe transaction-level serialization 由 `line_rate/x64/full-duplex` profile推导，vendor/外部字段缺失时不宣称物理带宽合规。确定性排序键为 `(cycle, phase, channel, axi, sequence, event_sequence)`；不得使用宿主时间。

## 9. 错误、恢复与可观测性

typed错误包括 invalid address/user/packet、temporarily restricted、overlap/pending-write及Controller传回状态；唯一映射位于 `ResponseCodec`。trace至少记录 generation/token/channel/axi/id/address64/packet/state/status；counter满足 `accepted = terminal + outstanding`、`responses <= accepted`。snapshot包含gate、queue、scoreboard barrier和register摘要，不包含裸指针。reset释放credit，stale completion只能计数且不得生成第二response。

## 10. 测试与验收

| Test ID | 层级 | production path | 场景与oracle | 证据 |
|---|---|---|---|---|
| HOST-T01/T02/T03 | E1 | Validator/Codec | Channel、64 B、burst、reserved表逐项判定且拒绝零副作用 | E1 |
| HOST-T04/T05 | E2 | Host->真实scoreboard->controller port | 同ID及hazard顺序严格；write仅terminal后一次响应 | E2 |
| HOST-T06/T07/T09 | E1/E2 | Codec/RegisterPort | 表12--16向量、reserved/access/reset oracle | E2 |
| HOST-T08/T10 | E2 | gate/kernel/Host | reset、disabled、queue满时token/event/credit不变 | E2 |
| HOST-T11 | E4 | wire codec | 仅在外部规范vector具备后逐bit比对 | 🔴 `BLOCKED_SPEC` |

实验执行、证据保存和状态更新统一遵循
[`实验与测试执行规范.md`](实验与测试执行规范.md)。本文件创建不构成实现或验证完成。
