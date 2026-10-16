# OpenHBX HBF Address and Topology 概要设计

**文档版本**：V1.0
**文档位置**：`docs/HLD_04_HBF_ADDRESS_TOPOLOGY.md`
**对应文档**：`docs/LLD_04_HBF_ADDRESS_TOPOLOGY.md`
**设计状态**：🔵 `IMPLEMENTED`（`status: IMPLEMENTED`；尚无本轮合规验证证据）
**适用基线**：OCP HBF v0.7.0；OpenHBX 首期 `OCP_HBF_0_7`

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 从旧 FTL 中提取静态地址、拓扑及 replay address view |

## 1. 模块概述

本模块回答“Host Channel local address 指向哪里”。它拥有 HBF geometry、Channel ownership、A1/R1--R5 checked arithmetic、ZoneMap/retirement容量视图、BlockSequence与 replay address view；输出不可变 typed physical address和访问判定。

它不拥有 DLU accumulator、scheduler、ECC、cache（05），TSV（06），Page payload/programmed/erase/Bank busy（07）。`BlockSequence`只记录规范顺序与 replay gate，不直接发 Program/Erase，也不修改介质状态。正式 HBF profile禁止传统任意页 L2P/P2L、GC、WL relocation、Trim和active relocation。

关键不变量：A1单位固定为64 B；R4=每Page的64 B单元（4 KiB时64）；checked arithmetic无溢出；每Bank仅属于一个Host Channel；Zone Remap只交换地址视图、不搬数据；replay序列按L2和R5生成；静态映射不读取动态介质状态。

## 2. 需求回顾与追踪

| 需求ID | 需求描述 | 优先级 | 版本 | 实现状态 |
|---|---|---|---|---|
| ADDR-001 | A1按64 B单位解析且DLU sector为0--63 | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| ADDR-002 | 实现L1/B1/P1/Bank_Num/L2及R1--R5公式 | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| ADDR-003 | Channel只能访问owner Bank容量切片 | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| ADDR-004 | geometry与反向映射使用checked arithmetic | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| ADDR-005 | 同block page顺序reserve/complete，page0产生auto-erase意图 | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| ADDR-006 | failure进入replay gate并生成`L2+n*R5` | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| ADDR-007 | Zone Remap只改变view并要求Host重写 | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| ADDR-008 | retirement/reduced capacity使地址不可用 | P1 | V1.0 | 🔵 `IMPLEMENTED` |
| ADDR-009 | 正式profile拒绝L2P、GC和active relocation | P0 | V1.0 | 🔵 `IMPLEMENTED` |

| 需求ID | 来源/证据类别 | owner组件 | Test ID |
|---|---|---|---|
| ADDR-001 | OCP 5.7、摘要第6节；规范事实 | `HbfAddressMapper` | ADDR-T01 |
| ADDR-002 | OCP 5.7、摘要第6节；规范事实 | `HbfAddressMapper` | ADDR-T02 |
| ADDR-003 | OCP Product Description；`docs/HBF_CHANNEL_CORE_DIE_TOPOLOGY.md` | `ChannelTopology` | ADDR-T03 |
| ADDR-004 | ADR-ADDR-001；设计选择 | `HbfGeometry` | ADDR-T04 |
| ADDR-005 | OCP摘要第4、6节；规范事实 | `BlockSequence` | ADDR-T05 |
| ADDR-006 | OCP 5.7；规范事实 | `BlockSequence` | ADDR-T06 |
| ADDR-007 | OCP Admin opcode 0x08；规范事实 | `ZoneMap` | ADDR-T07 |
| ADDR-008 | OCP摘要第9节；规范事实 | `ZoneMap` | ADDR-T08 |
| ADDR-009 | `docs/TASK1-整体设计方案.md`第1.2、10节；设计约束 | `HbfGeometry` | ADDR-T09 |

## 3. 系统架构设计

```text
03 Host(local byte address)
 -> Range/Unit Validator -> ChannelTopology -> HbfAddressMapper
 -> Zone/Capacity View -> BlockSequence/Replay View -> 05 Controller
```

| 组件 | 主要文件 | 职责 | 输入 | 输出 | 拥有状态 | 依赖 |
|---|---|---|---|---|---|---|
| `HbfGeometry` | `hbf_geometry.{h,cpp}` | 参数与容量推导 | resolved config | immutable geometry | R1--R5 | 00 config |
| `ChannelTopology` | `channel_topology.{h,cpp}` | owner关系 | channel/bank | allow/deny | owner表 | geometry |
| `HbfAddressMapper` | `hbf_address_mapper.{h,cpp}` | 正/反向规则映射 | local address | typed address | 无 | geometry/topology |
| `BlockSequence` | `block_sequence.{h,cpp}` | 顺序和replay gate | reserve/completion | permit/view | per-block cursor | mapper |
| `ZoneMap` | `zone_map.{h,cpp}` | zone交换/retirement视图 | logical zone | physical view | mapping/bitmap | admin |

所有对象由单一 EventKernel 驱动。mapper为纯函数；`BlockSequence`和`ZoneMap`的写操作仅由05在事件上下文调用。

### 3.1 HbfGeometry

**文件路径**：`include/openhbx/hbf/address/hbf_geometry.h`、`src/hbf/address/hbf_geometry.cpp`

**职责**：推导R1--R5、层次和checked容量，拒绝GC/L2P等非法profile。  
**关键组件**：`HbfGeometry`、checked arithmetic。  
**输入输出**：resolved config -> immutable geometry或结构化启动错误。  
**状态所有权**：04在产品生命周期唯一拥有，构造后只读。  
**依赖边界**：只依赖00配置，不读取请求或介质状态。  
**对应需求**：ADDR-002/004/009。  
**实现状态**：🔵 `IMPLEMENTED`（`status: IMPLEMENTED`）。

### 3.2 ChannelTopology

**文件路径**：`include/openhbx/hbf/address/channel_topology.h`、`src/hbf/address/channel_topology.cpp`

**职责**：描述Channel对Bank容量切片的唯一ownership，不复制物理树。  
**关键组件**：`owner_of()`、`can_access()`和owner表。  
**输入输出**：Channel/Bank -> owner判定。  
**状态所有权**：04拥有immutable owner表。  
**依赖边界**：依赖Geometry/profile，不依赖Controller或Media。  
**对应需求**：ADDR-003。  
**实现状态**：🔵 `IMPLEMENTED`（`status: IMPLEMENTED`）。

> **Channel 分片注记（2026-09-20）**：`ChannelTopology::banks_by_channel` 是 02 装配期构建各 `NandChannelSlice::owned_banks` 的唯一来源；07 不复制 owner 表，slice 的成员资格校验即 ownership 硬校验（ADR-CHAN-004）。`ChannelId` 强类型定义上移至 `openhbx/common/strong_types.h`，本模块以别名保持源码兼容（ADR-CHAN-006）。

### 3.3 HbfAddressMapper

**文件路径**：`include/openhbx/hbf/address/hbf_address_mapper.h`、`src/hbf/address/hbf_address_mapper.cpp`

**职责**：执行A1/R1--R5正反向映射和Zone只读view。  
**关键组件**：纯`map()`、`reverse()`及checked公式。  
**输入输出**：Channel local byte address <-> typed `HbfAddress`。  
**状态所有权**：无per-request状态。  
**依赖边界**：依赖Geometry、Topology和Zone snapshot；不访问05/06/07动态状态。  
**对应需求**：ADDR-001/002/003/007。  
**实现状态**：🔵 `IMPLEMENTED`（`status: IMPLEMENTED`）。

### 3.4 BlockSequence

**文件路径**：`include/openhbx/hbf/address/block_sequence.h`、`src/hbf/address/block_sequence.cpp`

**职责**：维护顺序Program、replay gate和replay地址view，不发介质命令。  
**关键组件**：`ProgramReservation`、cursor、`L2+n*R5` iterator。  
**输入输出**：05的reserve/complete/cancel -> permit或typed error。  
**状态所有权**：04唯一拥有per-block规则状态。  
**依赖边界**：依赖Mapper和02 token；05仅通过旁路端口查询/预约。  
**对应需求**：ADDR-005/006。  
**实现状态**：🔵 `IMPLEMENTED`（`status: IMPLEMENTED`）。

### 3.5 ZoneMap

**文件路径**：`include/openhbx/hbf/address/zone_map.h`、`src/hbf/address/zone_map.cpp`

**职责**：维护zone mapping、retirement bitmap和epoch；remap仅交换view。  
**关键组件**：`ZoneMapEntry`、active bitmap、epoch。  
**输入输出**：05 Admin命令/logical zone -> physical view/result。  
**状态所有权**：04唯一拥有mapping view。  
**依赖边界**：依赖BlockSequence冲突查询；不得调用06或搬移07 payload。  
**对应需求**：ADDR-007/008。  
**实现状态**：🔵 `IMPLEMENTED`（`status: IMPLEMENTED`）。

## 4. 高层详细设计

### 4.1 Geometry与规则映射

变量名必须带单位：`local_addr_bytes`、`a1_units64`、`dlu_index`。按摘要公式计算 L1/B1/P1/Bank_Num/L2，并映射到 CoreDie/Die/Bank/Block/Page/Sector。所有乘加先检查范围；不整除或超容量返回typed invalid address。

### 4.2 Channel ownership与拓扑

Host Channel访问互斥的Core Die/NAND容量池，不复制同一物理资源。正式profile必须从OCP配置字段或vendor profile解析Channel到Core Die/Die/Bank的ownership；OCP没有要求统一使用Bank取模。`owner_channel = bank % channel_count`只允许作为明确标记的synthetic测试profile。反向映射必须恢复相同local address和Channel。

### 4.3 顺序、replay及Zone视图

Controller在Program前调用reserve；正常只允许expected page，page0返回`AutoEraseRequired`意图。介质成功后才complete并推进cursor；failure不推进而切换`ReplayRequired`。replay view生成从page0至失败页的Host地址序列。Zone Remap原子交换映射条目并提升epoch，不生成数据搬移事件。

## 5. 接口设计

| 接口 | 方向 | 契约 | 所有权/backpressure | 错误 |
|---|---|---|---|---|
| `map(ChannelId, LocalByteAddress)` | 03/05 -> 04 | 返回typed `HbfAddress` | 值对象，无副作用 | range/owner/overflow |
| `reverse(HbfAddress)` | 05 -> 04 | 返回Channel local address | 纯函数 | non-canonical |
| `reserve_program(BlockKey, Page)` | 05 -> 04 | 原子创建reservation | Busy不改变cursor | order/replay blocked |
| `complete_program(Reservation, Result)` | 05 -> 04 | 成功推进，失败进入replay | token精确一次 | stale/duplicate |
| `apply_zone_remap(AdminOp)` | 05 -> 04 | 原子交换view并增epoch | 无payload搬移 | invalid/retired/busy |
| `replay_addresses(BlockKey)` | 05 -> 04 | 生成L2+nR5视图 | snapshot iterator | state mismatch |

## 6. 数据结构设计

| 结构 | owner/生命周期 | 核心字段 | 不变量 |
|---|---|---|---|
| `HbfGeometry` | 04/产品期 | R1--R5、各层数量 | 非零、乘积无溢出 |
| `HbfAddress` | 值对象 | channel/core_die/die/bank/block/page/sector | 每层在界 |
| `BlockSequenceState` | 04/每block | expected_page/mode/failure_page/epoch | 单owner单调提交 |
| `ProgramReservation` | 04至05 | block/page/epoch/token | 只complete一次 |
| `ZoneMapEntry` | 04/产品期 | logical/physical/retired/epoch | physical唯一或明确spare |

## 7. 关键流程与状态机

```text
map -> capacity/owner -> zone view -> physical address
program reserve -> [page0: auto-erase intent] -> PAL completion
  -> success: advance expected page
  -> failure: ReplayRequired -> host replay view -> resume
```

| 当前状态 | 事件/Guard | 动作 | 下一状态 | 失败处理 |
|---|---|---|---|---|
| `Empty` | reserve page0 | 返回erase意图并锁定 | `Reserved` | Busy零副作用 |
| `Sequential` | page==expected | 建reservation | `Reserved` | order violation |
| `Reserved` | media success | expected++ | `Sequential/Full` | stale拒绝 |
| `Reserved` | media failure | 记录failure page | `ReplayRequired` | 不推进 |
| `ReplayRequired` | replay按序成功 | 推进replay cursor | `ReplayRequired/Sequential` | 再失败保持gate |

reset使旧epoch reservation失效；动态介质数据不随ZoneMap改变。

## 8. 性能与资源设计

映射为O(1)整数运算；owner检查O(1)；每block sequence状态内存为`reachable_blocks * sizeof(state)`，允许稀疏表但访问结果必须确定；ZoneMap为`O(zones)`。replay地址迭代O(R3)。所有排序使用稳定block key和token，不使用unordered容器迭代顺序作为observable。容量上限由checked乘积推导，不给无来源vendor常数。

## 9. 错误、恢复与可观测性

错误包括invalid/range/overflow/channel-owner/order/replay-required/capacity-unusable/stale-reservation。04返回typed原因，由03或05映射OCP status。trace记录输入单位、公式中间值、zone epoch、block cursor和reservation token；snapshot不含payload。守恒式：`reservations_created = completed + cancelled + outstanding`；Zone remap前后可用zone数守恒（retirement除外）。

### 9.1 已知缺口：坏块与地址视图尚未联动

当前生产装配未向`HbfAddressMapper`传入`ZoneMap`，且mapper不查询07的`BadBlockTable`。因此factory bad或运行期标记的坏块仍会由固定R1--R5公式映射到原物理Block，只在NAND admission阶段返回`BadBlock`；Program/Erase failure也不会自动更新地址视图。`BlockSequence`的Replay状态、04的ZoneMap和07的BBT目前是彼此分离的机制，不能宣称已实现坏块规避或重映射。

后续实现必须保持R1--R5公式不变，不引入传统SSD任意L2P、GC或透明active-data relocation。factory bad manifest应在开放Host admission前形成可审计的Zone/容量视图，使逻辑地址只落到可用物理Block；运行期坏块应在verify failure确认后，由07提交BBT reason/version，04原子retire旧视图并提升mapping epoch，再由既有Host replay流程向新视图重写。无可用替代容量时返回`CapacityUnusable`。关闭条件见`GAP-010`。

## 10. 测试与验收

| Test ID | 层级 | 场景与oracle | 证据 |
|---|---|---|---|
| ADDR-T01/T02 | E1 | OCP公式golden、边界及单位；中间值逐项相等 | E1 |
| ADDR-T03/T04 | E1 | 全geometry owner/round-trip；溢出配置启动拒绝 | E1 |
| ADDR-T05/T06 | E2 | 真实sequence+controller port；成功才推进，replay地址严格L2+nR5 | E2 |
| ADDR-T07/T08 | E2 | remap无media copy event；retired地址不可用 | E2 |
| ADDR-T09 | E1 | 非法GC/L2P配置结构化拒绝 | E1 |

实验执行、证据保存和状态更新统一遵循
[`实验与测试执行规范.md`](实验与测试执行规范.md)。
