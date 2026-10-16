# OpenHBX OCP HBF Host Protocol 详细设计

**文档版本**：V1.0
**文档位置**：`docs/LLD_03_OCP_HBF_HOST_PROTOCOL.md`
**对应HLD完整路径**：`docs/HLD_03_OCP_HBF_HOST_PROTOCOL.md`
**客观状态**：🟡 `PARTIAL`；transaction-level Host组件已实现并覆盖unit/component测试；wire-level conformance为🔴 `BLOCKED_SPEC`。
**适用基线**：OCP HBF v0.7.0；OpenHBX 首期 `OCP_HBF_0_7`

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 首版可实现级Host协议设计 |

**目录与存储位置**

| 项目 | 仓库相对位置 | 内容 |
|---|---|---|
| 本LLD | `docs/LLD_03_OCP_HBF_HOST_PROTOCOL.md` | 实现级权威设计 |
| 对应HLD | `docs/HLD_03_OCP_HBF_HOST_PROTOCOL.md` | Host协议边界与需求 |
| 计划公开头文件 | `include/openhbx/hbf/host/` | Host值类型与公开port |
| 计划实现 | `src/hbf/host/` | admission、ordering、codec、register实现 |
| 计划测试 | `tests/unit/hbf/host/`、`tests/component/hbf/host/` | HOST-T01至HOST-T11 |
| 正式artifact | `build/artifacts/<suite>/<experiment-id>/<run-id>/` | Host trace、completion与验证证据；目录须被`.gitignore`覆盖 |

## 1. 模块概述与约束

本LLD覆盖`HOST-001`至`HOST-011`。上游为02 Ramulator/Frontend adapter与EventKernel，下游为04地址端口、05Controller端口和生命周期端口。03只拥有Host协议状态；不实现地址公式、DLU、ECC、TSV或介质状态。所有函数在单一确定性event context执行；Accepted后payload所有权才转移。UCIe v3.0 Format 6和AMBA AXI逐bit行为在外部规范可定位前不实现为conformance路径。

## 2. 需求到实现映射

| 需求ID | HLD章节 | 实现文件/符号 | 状态/算法 | Test ID | 当前状态 |
|---|---|---|---|---|---|
| HOST-001/008 | 4.1/4.3 | `host_protocol.*:ChannelContext/submit` | Channel gate | HOST-T01/T08 | PLANNED |
| HOST-002 | 4.1 | `host_validator.*:validate_flash` | checked range | HOST-T02 | PLANNED |
| HOST-003/009 | 4.1/4.3 | `packet_codec.*` | descriptor table | HOST-T03/T09 | PLANNED |
| HOST-004 | 4.2 | `ordering_scoreboard.*` | 双barrier | HOST-T04 | PLANNED |
| HOST-005/006 | 4.2 | `host_protocol.*:on_completion`、`packet_codec.*` | delayed response | HOST-T05/T06 | PLANNED |
| HOST-007 | 4.3 | `register_port.*` | descriptor/access/reset | HOST-T07 | PLANNED |
| HOST-010 | 4.1 | `host_protocol.*:AdmissionTxn` | reserve/rollback | HOST-T10 | PLANNED |
| HOST-011 | 1 | `packet_codec.*:WireCodec` | 禁止启用 | HOST-T11 | 🔴 `BLOCKED_SPEC` |

## 3. 核心类型与数据结构

| 字段 | 类型/单位 | 合法范围/默认值 | owner与生命周期 | 不变量 |
|---|---|---|---|---|
| `HostIngress::address_bytes` | `uint64_t/byte` | profile range | 02->03值对象 | 不隐式转64B单位 |
| `size_bytes` | `uint32_t/byte` | Flash为64或合法burst | 同上 | 不跨4KiB |
| `channel` | strong `ChannelId` | `[0,n)` | transaction期固定 | owner不可变 |
| `axi_id` | strong `AxiId` | profile width | scoreboard期 | 同ID sequence单调 |
| `generation` | `Generation` | kernel值 | token期 | completion必须相等 |
| `TxnRecord::state` | `TxnState` | Received..Terminal | 03 | 单向合法转换 |
| `address64` | `uint64_t/64B` | `address_bytes/64` | scoreboard | hazard key规范化 |
| `response.data_valid` | `bool` | false | response期 | 与status匹配 |

`PacketType={FlashIo,ScratchpadIo,CsrAdmin,Reserved}`；`AdmissionResult={Accepted(token),Busy(reason),Rejected(error)}`；`TxnState={Received,Queued,InFlight,ResponseHeld,ResponseReady,Terminal,Cancelled}`。`RegisterDescriptor`字段为offset、width、access、reset_domain、reset_value、reserved_mask、owner_scope、read/write hook；表在构造后不可变。

容量：`TxnRecord <= sum(channel.queue_depth)`；每Accepted token恰有一条record；`id_queues`和`address_queues`只保存token，不保存payload副本。

## 4. 头文件与公开边界

公开：`include/openhbx/hbf/host/host_protocol.h`导出`IHbfHostProtocol`、submit/completion/lifecycle API；`host_types.h`导出跨03/04/05的值类型；`register_port.h`仅导出寄存器事务接口。内部：`host_validator.h`、`ordering_scoreboard.h`、`packet_codec.h`不得被下游include，防止绕过owner。公开头只能依赖02公共token/event类型，不依赖06/07实现头。

## 5. 函数与接口详细设计

### 5.1 `AdmissionResult submit(HostIngress request)`

**定义位置**：`include/openhbx/hbf/host/host_protocol.h`；**需求**：HOST-001/002/008/010；**方向**：02->03。

前置：event context有效、request payload由caller拥有。依次执行gate、静态校验、credit探测，再构造`AdmissionTxn`预留queue和两个barrier。Accepted返回token并转移payload；Busy/Rejected回滚全部预留，token/event/payload owner不变。不得同步完成。测试HOST-T02/T08/T10检查snapshot digest不变。

### 5.2 `void on_controller_completion(ControllerCompletion result)`

**定义位置**：同上；**需求**：HOST-004/005/006；**方向**：05->03。

要求token存在且generation匹配。首次completion写入typed result并标记ready；scoreboard只在ID队首和address-hazard队首同时满足时释放。stale只递增counter并回收明确移交的result资源；duplicate记录invariant violation，不响应。response事件在固定response phase入队。

### 5.3 `RegisterResult access(RegisterTxn txn)`

**定义位置**：`register_port.h`；**需求**：HOST-007。descriptor查找失败、reserved写或owner错误返回typed error且image不变。`BUCR` magic不直接清状态，而向lifecycle port排一个reset intent；W1C只清允许位；`TMON.CES`遵守reset domain。

### 5.4 `void set_channel_state(ChannelId, LinkState, Generation)`

只允许lifecycle owner调用；新generation生效后更新gate。进入非ready态不静默丢弃in-flight，由reset policy产生terminal结果。调用顺序固定在admission phase前。

## 6. 内部逻辑、状态机与算法

原子admission伪码：`probe gate -> validate -> probe credits -> reserve queue -> reserve id barrier -> reserve address barrier -> publish token`；异常按逆序rollback。同cycle事件键为`phase/channel/axi/arrival_sequence`。

| 当前状态 | 事件/条件 | Guard | 动作/副作用 | 下一状态 | 失败处理 |
|---|---|---|---|---|---|
| Received | submit | 全检查通过 | 发布token/转移payload | Queued | rollback |
| Queued | dispatch | 下游Accepted | 标记issue | InFlight | Busy留队首 |
| InFlight | completion | generation相同 | 保存result | ResponseHeld/Ready | stale回收 |
| ResponseHeld | barrier release | 两队列均队首 | schedule response | ResponseReady | 无 |
| ResponseReady | response sent | 未terminal | 释放barrier/credit | Terminal | duplicate fault |
| 非terminal | reset | policy适用 | cancel并排terminal | Cancelled/Terminal | 旧事件隔离 |

barrier释放必须先从两个索引删除，再释放record，避免后继观察悬空token。64B hazard按实际burst展开地址集合并按升序预留，避免次序不确定。

## 7. 逐文件设计

### 7.1 文件总表

| 路径 | 动作 | owner | 主要职责/API | 依赖 | 需求/测试 | 状态 |
|---|---|---|---|---|---|---|
| `include/openhbx/hbf/host/host_types.h` | 新增 | 03 | typed ingress/response/status | 02 types | 全部 | PLANNED |
| `include/openhbx/hbf/host/host_protocol.h` | 新增 | 03 | 公开Host端口 | types | HOST-001..010 | PLANNED |
| `src/hbf/host/host_protocol.cpp` | 新增 | 03 | admission/dispatch/completion | 04/05 ports | HOST-T05/T10 | PLANNED |
| `include/openhbx/hbf/host/host_validator.h` | 新增 | 03 | 内部validator | config | HOST-002 | PLANNED |
| `src/hbf/host/host_validator.cpp` | 新增 | 03 | checked校验 | 无 | HOST-T02 | PLANNED |
| `include/openhbx/hbf/host/ordering_scoreboard.h` | 新增 | 03 | 双barrier类型 | token | HOST-004 | PLANNED |
| `src/hbf/host/ordering_scoreboard.cpp` | 新增 | 03 | reserve/release | 无 | HOST-T04 | PLANNED |
| `include/openhbx/hbf/host/packet_codec.h` | 新增 | 03 | descriptor API | OCP enums | HOST-003/006/009 | PLANNED |
| `src/hbf/host/packet_codec.cpp` | 新增 | 03 | table驱动转换 | 无 | HOST-T03/06/09 | PLANNED |
| `include/openhbx/hbf/host/register_port.h` | 新增 | 03 | CSR公开端口 | lifecycle | HOST-007 | PLANNED |
| `src/hbf/host/register_port.cpp` | 新增 | 03 | descriptor/reset hooks | config | HOST-T07 | PLANNED |
| `tests/unit/hbf/test_host_protocol.cpp` | 新增 | test | unit/component fixture | production files | HOST-T01..10 | PLANNED |
| `tests/conformance/hbf/test_host_wire_vectors.cpp` | 新增 | test | 外部vector | wire spec | HOST-T11 | 🔴 `BLOCKED_SPEC` |

### 7.2 生产文件解析

#### Host公共类型（`include/openhbx/hbf/host/host_types.h`）

**职责与设计**：单独header定义`HostIngress`、`HostTransaction`、`HostResponse`、packet/status enum和强类型ID；没有source，不包含算法或可变全局对象。**API及输入输出**：向02/04/05导出值类型，序列化边界保留byte、64B和generation单位。**owner/lifecycle**：类型由03定义，实例按submit至terminal传递，Accepted前仍由caller拥有payload。**错误与依赖**：只依赖02公共token/payload类型；非法组合由Validator/Codec处理，禁止依赖04--07实现头。**追踪**：HOST-001..010、HOST-T01..10；`PLANNED`。

#### Host协议Facade（`include/openhbx/hbf/host/host_protocol.h`、`src/hbf/host/host_protocol.cpp`）

**职责**：唯一协调admission、record、dispatch、completion和terminal；不解释地址公式或介质状态。**头文件/源文件**：header公开`submit/on_controller_completion/set_channel_state`窄端口；source实现原子reserve/rollback、generation和response排程。**API及输入输出**：输入`HostIngress`/`ControllerCompletion`，输出04/05 transaction及`HostResponse`。**owner/lifecycle**：持有Channel queue、record、credit和token索引，terminal后释放。**错误与依赖**：容量满Busy；stale不提交；依赖02 kernel及04/05公开port，禁止反向依赖。**追踪**：HOST-001/005/008/010、HOST-T05/T08/T10；`PLANNED`。

#### Host校验器（`include/openhbx/hbf/host/host_validator.h`、`src/hbf/host/host_validator.cpp`）

**职责**：校验Channel、packet字段、64B粒度和4KiB边界；不预留credit。**头文件/源文件**：header声明纯`validate` API，source执行checked range和burst展开。**输入输出**：immutable ingress/config -> typed validation。**owner/lifecycle**：无运行时状态。**错误与依赖**：Misaligned/OutOfRange/InvalidUserField，失败零副作用；只依赖resolved Host profile和公共types。**追踪**：HOST-001/002/003、HOST-T01/T02/T03；`PLANNED`。

#### Ordering Scoreboard（`include/openhbx/hbf/host/ordering_scoreboard.h`、`src/hbf/host/ordering_scoreboard.cpp`）

**职责**：维护same-ID与同64B地址双barrier；不编码response。**头文件/源文件**：header声明`reserve/mark_ready/release/cancel`，source维护稳定token deque并按升序展开burst地址。**输入输出**：token、ordering key和completion-ready -> releasable token集合。**owner/lifecycle**：03唯一拥有ID/address索引，Accepted建立、terminal/reset删除。**错误与依赖**：duplicate/stale为invariant fault；只依赖token和Host types，不调用Controller/Media。**追踪**：HOST-004、HOST-T04/T10；`PLANNED`。

#### Packet Codec（`include/openhbx/hbf/host/packet_codec.h`、`src/hbf/host/packet_codec.cpp`）

**职责**：表驱动转换OCP packet、Admin/sideband opcode和response status；不臆造UCIe wire bit。**头文件/源文件**：header导出typed encode/decode与descriptor查询，source实现表9/12/13/14和reserved校验。**输入输出**：packet fields或Controller result -> typed envelope/response。**owner/lifecycle**：descriptor为构造后不可变静态数据，无transaction状态。**错误与依赖**：reserved拒绝，status/data-valid不一致视为内部错误；依赖OCP enum和Host types。**追踪**：HOST-003/006/009/011、HOST-T03/T06/T09/T11；transaction路径`PLANNED`，wire路径`BLOCKED_SPEC`。

#### Register Port（`include/openhbx/hbf/host/register_port.h`、`src/hbf/host/register_port.cpp`）

**职责**：实现Always-On/MMIO descriptor、access attribute和reset hook；不拥有link/lifecycle状态。**头文件/源文件**：header公开`access/reset_domain`，source维护register image并产生reset intent。**输入输出**：offset/value/owner -> value/status或lifecycle intent。**owner/lifecycle**：03拥有image，按descriptor reset domain更新，sticky位例外。**错误与依赖**：unknown/reserved/RO写零副作用；依赖profile和lifecycle公开port。**追踪**：HOST-007/008、HOST-T07/T08；`PLANNED`。

### 7.3 测试文件解析

#### Host协议测试（`tests/unit/hbf/test_host_protocol.cpp`）

对象为真实Facade、Validator、Scoreboard、Codec和RegisterPort；fixture固定profile/generation，fake仅替代04/05公开端口。输入覆盖边界、乱序、Busy、reset/stale；oracle为response总序、status/data-valid、snapshot digest及token/event/credit守恒，最高E2。对应HOST-T01..10，`PLANNED`。

#### Wire向量测试（`tests/conformance/hbf/test_host_wire_vectors.cpp`）

对象为未来bit-accurate Codec；fixture必须引用正式UCIe/AMBA golden vector和版本，不能使用自生成向量作为规范oracle。逐bit header/CRC和reserved行为完全相等才通过，目标E4；缺少外部规范时skip不算通过。对应HOST-T11，`BLOCKED_SPEC`。

## 8. 配置、错误、统计与Debug

| 配置键 | 类型/单位 | 规则 | 非法行为 |
|---|---|---|---|
| `hbf.host.channels` | u8 | 1--16 | 启动拒绝 |
| `axi_interfaces` | enum | 1/2/4 | 启动拒绝 |
| `queue_depth` | u32 | >0且总量无溢出 | 启动拒绝 |
| `ucie.line_rate_gtps` | source-tagged | speed grade一致 | 缺来源拒绝 |
| `wire_model` | enum | transaction/bit_accurate | 后者BLOCKED_SPEC |

内部错误只由ResponseCodec映射；data-valid随typed result。统计：accepted/busy/rejected/terminal/stale/duplicate、per-status、ordering stalls、queue HWM。snapshot记录queue/token/barrier/register/generation；trace不含pointer/wall-clock。守恒为`accepted=terminal+outstanding`。

## 9. 测试用例设计

| ID | Requirement | Evidence | Production path/Fake | 输入与fault | Oracle |
|---|---|---|---|---|---|
| HOST-T02 | HOST-002 | E1 | Validator | DLU边界±1、burst0..63 | 精确accept/error，状态无变 |
| HOST-T04 | HOST-004 | E2 | Host+真实scoreboard；fake05 | 交错ID/同地址completion | response总序匹配规则 |
| HOST-T07 | HOST-007 | E2 | RegisterPort+lifecycle fake | RO/W1C/reserved/reset | image/reset intent精确 |
| HOST-T10 | HOST-010 | E2 | Host+kernel+fake downstream | queue满、reset、stale | token/event/credit守恒 |
| HOST-T11 | HOST-011 | E4 | 完整wire codec | 外部golden vectors | every bit/CRC匹配；当前阻断 |

统一fixture记录resolved config、generation和event digest；预算最多10万event。实验执行遵循[`实验与测试执行规范.md`](实验与测试执行规范.md)。

## 10. 实施顺序、ADR与完成定义

顺序：types/descriptor -> validator/scoreboard -> Host facade -> register/lifecycle -> component/system tests -> 外部wire conformance。

| ADR ID | 决策 | 背景/备选 | 理由 | 代价 | 状态 |
|---|---|---|---|---|---|
| ADR-HOST-001 | 原子reserve/rollback | 可分步可见 | 保证Busy零副作用 | admission对象增加 | PLANNED |
| ADR-HOST-002 | ID与地址双scoreboard | 只做AXI ID | 满足mixed hazard | 索引内存增加 | PLANNED |
| ADR-HOST-003 | wire模型默认transaction-level | 猜测bit布局 | 不伪造规范 | bit conformance阻断 | PLANNED |

完成定义：表中生产文件和API落地；HOST-001..010至少对应E1/E2通过，真实系统路径达到E3后才可更新相应状态；守恒/reset/stale oracle通过；HOST-011只有外部规范vector E4通过后解除`BLOCKED_SPEC`；不得以文档创建或fake pipeline宣称OCP完整协议完成。
