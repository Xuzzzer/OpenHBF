# OpenHBX 互联与 Flash PAL 详细设计

**文档版本**：V1.0
**文档位置**：`docs/LLD_06_INTERCONNECT_FLASH_PAL.md`
**对应HLD**：`docs/HLD_06_INTERCONNECT_FLASH_PAL.md`
**设计状态**：🟡 `PARTIAL`（`status: PARTIAL`）；hybrid bonding依赖🔴 `BLOCKED_SPEC`（`dependency_status: BLOCKED_SPEC`）
**覆盖需求**：`PAL-001`至`PAL-007`

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 首期HBF PAL、TSV与封装互联实现级设计 |

## 1. 模块概述与约束

本LLD实现HLD-06边界。PAL为项目内部Physical Abstraction Layer，不是OCP术语；Host link归03，地址映射归04，控制策略归05，NAND Page/Block归07。只支持首期HBF；DRAM与hybrid bonding正式profile不在覆盖范围，后者为`BLOCKED_SPEC`。

### 1.1 目录与存储位置

| 内容 | 仓库相对目录/完整路径 | 用途 | 状态 |
|---|---|---|---|
| 本LLD | `docs/LLD_06_INTERCONNECT_FLASH_PAL.md` | PAL、互联、EAT与repair实现契约 | ⚪ `PLANNED` |
| 对应HLD | `docs/HLD_06_INTERCONNECT_FLASH_PAL.md` | 06边界与需求基线 | ⚪ `PLANNED` |
| 公开头文件 | `include/openhbx/interconnect/` | PAL/Fabric/Media端口与值类型 | ⚪ `PLANNED` |
| 私有头文件 | `include/openhbx/interconnect/detail/` | `EatTable`与`PalContext` | ⚪ `PLANNED` |
| 生产实现 | `src/interconnect/` | Fabric、PAL、TSV repair和profile | ⚪ `PLANNED` |
| PAL公开头文件 | `include/openhbx/pal/` | Controller到Media的异步PAL端口 | ⚪ `PLANNED` |
| PAL生产实现 | `src/pal/` | PAL FSM、重试和completion correlation | ⚪ `PLANNED` |
| 单元测试 | `tests/unit/interconnect/test_fabric.cpp` | route/EAT/ceiling oracle | ⚪ `PLANNED` |
| 组件测试 | `tests/component/pal/test_flash_pal.cpp` | PAL FSM与公开media fake边界 | ⚪ `PLANNED` |
| 正式artifact | `build/artifacts/<suite>/<experiment-id>/<run-id>/` | route/EAT、repair与验证证据；目录须被`.gitignore`覆盖 | ⚪ `PLANNED` |

机器状态：`status: PLANNED`；hybrid bonding为`dependency_status: BLOCKED_SPEC`。

## 2. 需求到实现映射

| 需求ID | HLD章节 | 实现文件/符号 | 状态/算法 | Test ID | 当前状态 |
|---|---|---|---|---|---|
| PAL-001 | 4/5 | `FlashPal::try_issue` | validate/admit | `VER-PAL-001` | `PLANNED` |
| PAL-002 | 4/7 | `PalContext`、`on_event` | async FSM | `VER-PAL-002` | `PLANNED` |
| PAL-003 | 4/8 | `Fabric::reserve`、`EatTable` | atomic EAT | `VER-PAL-003` | `PLANNED` |
| PAL-004 | 4/9 | `TsvRepairManager` | epoch map | `VER-PAL-004` | `PLANNED` |
| PAL-005 | 7 | `reset`/generation guard | cancel/stale | `VER-PAL-005` | `PLANNED` |
| PAL-006 | 8 | `FabricProfile::validate` | ceiling/source | `VER-PAL-006` | `PLANNED` |
| PAL-007 | 8 | profile validator | external data gate | `VER-CFG-006` | `BLOCKED_SPEC` |

## 3. 核心类型与数据结构

| 字段 | 类型/单位 | 合法范围/默认值 | owner与生命周期 | 不变量 |
|---|---|---|---|---|
| `token` | `RequestToken` | 非零 | 05创建，terminal释放 | inflight唯一 |
| `generation` | `Generation` | 单调 | 02拥有 | event必须匹配当前值 |
| `payload` | `PayloadHandle`/byte | Program=4096 B | Accepted才转移 | terminal前不丢失 |
| `resource_eat` | `Cycle` | 非负 | `EatTable` | 单调、无溢出 |
| `repair_epoch` | `uint64_t` | 单调 | repair manager | reservation后冻结 |
| `stage` | `PalStage` | 枚举值 | `PalContext` | 只走合法转换 |

`IssueResult`为`Accepted{token}|Busy{reason}|Rejected{error}`。`PalStatus`至少含`Success, Corrected, Uncorrectable, ProgramFail, EraseFail, PathUnavailable, Aborted, InternalError`并携带`data_valid/retryable`。

## 4. 头文件与公开边界

`include/openhbx/interconnect/types.h`导出Transfer与route值类型；不得依赖07内部头。`fabric.h`导出`InterconnectFabric`；`include/openhbx/pal/flash_pal.h`导出05调用端口；`include/openhbx/pal/media_port.h`定义窄`IFlashMediaPort`；`tsv_repair.h`导出fault输入与只读snapshot。`detail/eat_table.h`和`src/pal/pal_context.h`为私有头，其他模块不得包含。

## 5. 函数与接口详细设计

### `IssueResult FlashPal::try_issue(FlashPhysicalRequest&& request, Cycle now)`

**调用方向**：05->06。前置条件为当前generation、04产生的canonical地址、合法operation及payload。校验失败Rejected；credit或route资源不足Busy；二者均不转移所有权、不新增event、不改EAT。Accepted时request与payload转入`PalContext`，原子预约forward transfer并安排事件。

### `TransferReservation Fabric::try_reserve(const Transfer&, Cycle now)`

展开route、排序去重resource ID，按`(direction, resource ID)`选择EAT并以`start=max(now,EAT...)`计算end；全部checked成功后一次更新对应方向的EAT。任何错误返回时table版本和值不变。复杂度`O(R log R)`，R为route资源数。Forward/Return使用独立EAT以表达full-duplex；inflight credit 按 channel 计费（CHAN-013，ADR-CHAN-009：原 cube 级共享池会使一个 channel 的压力挤占其他 channel 的在途预算）。Fabric `queue_depth` 同样按路由携带的每个资源独立计费，不再有 cube 级 `active` 总量检查。

route构造规则固定为：endpoint编号仍是`channel * axi_interfaces + axi_interface`，用于PAL请求相关和AXI可观测身份；resource编号只由`channel`决定。因此同一Channel的所有AXI endpoint共享该Channel bandwidth，不同Channel可并行。禁止按endpoint创建独享物理resource，否则会把虚拟AXI数量错误乘入物理带宽。

每条route的`logical_lanes`必须包含该Channel完整lane集合：`[channel * lanes_per_channel, (channel + 1) * lanes_per_channel)`。`TsvRepairManager(channel_count, active_lanes_per_channel, spare_lanes_per_channel)`为每个Channel建立独立物理stride和free-spare集合；`apply_fault`由logical owner选择本Channel spare。snapshot中16×64 active映射和16组spare必须可审计。

### `void FlashPal::on_event(const PalEvent&)`

仅由02 EventKernel调用。先校验generation/token/stage/sequence；stale只记数，duplicate变为integrity记录。forward完成时调用media port，media Busy则安排有界retry；media completion预约return transfer；return完成产生唯一上游completion。

### `RepairResult TsvRepairManager::apply_fault(const LinkFault&)`

先由故障physical lane反查logical lane及Channel owner，再只从该Channel的free-spare集合选择最低编号spare，更新mapping并递增epoch；无本地spare则标记该Channel route unavailable。已有reservation不回写route，其他Channel映射与spare集合不变。

## 6. 内部逻辑、状态机与算法

| 当前状态 | 事件/条件 | Guard | 动作/副作用 | 下一状态 | 失败处理 |
|---|---|---|---|---|---|
| `Received` | issue | credit+valid | 创建context/reserve | `ForwardInFlight` | Busy/Rejected零副作用 |
| `ForwardInFlight` | transfer done | generation匹配 | issue media | `MediaInFlight` | Busy->`MediaPending` |
| `MediaPending` | retry | budget未耗尽 | issue media | `MediaInFlight` | 耗尽->return failure |
| `MediaInFlight` | media completion | token匹配 | reserve return | `ReturnInFlight` | typed failure同路径返回 |
| `ReturnInFlight` | transfer done | sequence匹配 | release+callback | `Terminal` | duplicate忽略并告警 |
| 任意非terminal | reset | scope命中 | abort/release/complete | `Terminal` | stale event丢弃 |

同cycle排序使用02定义的`phase,sequence`：fault/reset先于新admission，已完成stage event按sequence处理。PAL不提前预约07的NAND EAT。

## 7. 逐文件设计

| 路径 | 动作 | owner | 主要职责 | 对应需求 | 状态 |
|---|---|---|---|---|---|
| `include/openhbx/interconnect/types.h` | 新增 | 06 | 跨模块值类型 | PAL-001/002 | `PLANNED` |
| `include/openhbx/interconnect/fabric.h`、`src/interconnect/fabric.cpp` | 新增 | 06 | route与atomic EAT | PAL-003/006 | `PLANNED` |
| `include/openhbx/pal/flash_pal.h`、`src/pal/flash_pal.cpp` | 新增 | 06 | PAL FSM与派发 | PAL-001/002/005 | `PLANNED` |
| `include/openhbx/pal/media_port.h` | 新增 | 06 | 07窄端口 | PAL-002 | `PLANNED` |
| `include/openhbx/interconnect/tsv_repair.h`、`src/interconnect/tsv_repair.cpp` | 新增 | 06 | TSV fault/spare/epoch | PAL-004 | `PLANNED` |
| `include/openhbx/interconnect/fabric_profile.h`、`src/interconnect/fabric_profile.cpp` | 新增 | 06 | 参数校验、来源、ceiling | PAL-006/007 | `PLANNED` |
| `tests/unit/interconnect/test_fabric.cpp` | 新增 | 08 | EAT/route oracle | PAL-003/006 | `PLANNED` |
| `tests/component/pal/test_flash_pal.cpp` | 新增 | 08 | PAL FSM/fake media边界 | PAL-001/002 | `PLANNED` |

### 跨模块类型（`include/openhbx/interconnect/types.h`）

**职责与设计**：只声明`FlashPhysicalRequest`、`Transfer`、`PalCompletion`、typed status和稳定ID；不包含EAT容器或NAND状态。**关键API/输入输出**：为05->06请求、06->07命令及06->05 completion提供value contract。**状态与所有权**：无运行态；payload仅在Accepted时转移。**错误边界**：构造校验operation、4096 B payload及generation。**依赖**：common token/time/payload，不依赖07内部头。**需求/测试/状态**：PAL-001/002，`VER-PAL-001/002`，`PLANNED`。

### Fabric与EAT（`include/openhbx/interconnect/fabric.h`、`src/interconnect/fabric.cpp`）

**职责与设计**：header导出`InterconnectFabric::try_reserve()`和只读snapshot；source实现route展开、resource排序、checked cycle计算和atomic EAT更新。**关键组件**：`TransferReservation`、`RouteTable`、`EatTable`。**输入输出**：Transfer+now -> Accepted/Busy/Rejected及end cycle。**状态与所有权**：唯一拥有transfer queue和package/vertical resource EAT。**错误边界**：unknown endpoint、零带宽、溢出或部分预约必须零副作用。**依赖**：immutable `FabricProfile`和02 event port。**需求/测试/状态**：PAL-003/006，`VER-PAL-003/006`，`PLANNED`。

### Flash PAL（`include/openhbx/pal/flash_pal.h`、`src/pal/flash_pal.cpp`）

**职责与设计**：header导出05调用的`try_issue/reset/snapshot`；source实现forward transfer、media issue、return transfer和terminal FSM。**关键组件/API**：`FlashPal`、`PalContext`、`on_event()`。**输入输出**：canonical physical request -> typed asynchronous completion。**状态与所有权**：拥有inflight、credit及completion obligation，不拥有Page/Block。**错误边界**：Busy/Rejected回滚；media Busy有界retry；duplicate/stale不二次完成。**依赖**：Fabric、`IFlashMediaPort`、02 EventKernel。**需求/测试/状态**：PAL-001/002/005，`VER-PAL-001/002/005`，`PLANNED`。

### Media端口（`include/openhbx/pal/media_port.h`）

**职责与设计**：声明06调用07的窄`try_issue_media()`和completion sink，不暴露07私有类型。**输入输出**：`FlashCommand`与`MediaCompletion`。**状态与所有权**：接口自身无状态；07 Accepted后拥有command。**错误边界**：family/opcode不匹配为Rejected，Busy不转移所有权。**依赖**：公开media types。**需求/测试/状态**：PAL-002，`VER-PAL-002`，`PLANNED`。

### TSV repair（`include/openhbx/interconnect/tsv_repair.h`、`src/interconnect/tsv_repair.cpp`）

**职责与设计**：header导出fault输入、route查询和snapshot；source以最低稳定spare ID更新映射并递增epoch。**关键组件**：`TsvRepairManager`、`RepairMap`。**输入输出**：LinkFault/route -> repaired、degraded或unavailable。**状态与所有权**：唯一拥有active/spare map及repair epoch。**错误边界**：未知lane拒绝；旧reservation不被改写。**依赖**：fabric topology和02同cycle phase。**需求/测试/状态**：PAL-004，`VER-PAL-004`，`PLANNED`。

### Fabric profile（`include/openhbx/interconnect/fabric_profile.h`、`src/interconnect/fabric_profile.cpp`）

**职责与设计**：header声明immutable profile/source metadata；source校验lane、效率、延迟、spare和理论ceiling。**输入输出**：resolved config -> profile或结构化配置错误。**状态与所有权**：构造后只读。**错误边界**：缺单位、零lane、溢出及无来源hybrid bonding拒绝。**依赖**：00配置schema。**需求/测试/状态**：PAL-006/007，`VER-PAL-006`与`VER-CFG-006`，基础TSV为`PLANNED`、hybrid为`BLOCKED_SPEC`。

### 06测试文件（`tests/unit/interconnect/test_fabric.cpp`、`tests/component/pal/test_flash_pal.cpp`）

**测试域**：前者使用tiny immutable profile和手算timeline验证真实Fabric/EAT；后者使用真实PAL/EventKernel与明确fake media port覆盖success、Busy、failure、reset。**输入与oracle**：同/异route、capacity边界、lane fault及stage事件；检查cycle、零副作用、exactly-once和drain。**证据等级**：Fabric unit为E1/E2，fake media的PAL component最高E2；全链路E3由08 system suite承担。**状态**：`PLANNED`。

生产文件不得保存NAND状态或实现ECC/replay。测试fake media使PAL测试最高为E2。

## 8. 配置、错误、统计与Debug

配置键包括`queue_depth`、`package.resource_groups`、`vertical.lanes`、`bits_per_lane_per_cycle`、`efficiency_ppm`、`arbitration_cycles`、`propagation_cycles`、`spares`、`profile_source`和`media_retry_budget`；均为启动时静态值，零lane、效率越界、缺单位或溢出直接拒绝。`TsvHybridBonding`还要求外部参数集和source URI/version，否则`BLOCKED_SPEC`。

错误到05只在`FlashPal`映射一次；底层detail保留在diagnostic。统计递增点为reservation成功、transfer terminal、repair commit和PAL terminal。Debug event包含稳定ID而非指针/wall-clock。

## 9. 测试用例设计

| Test ID | Fixture/production path | 输入/Fault | Oracle | 最高证据 |
|---|---|---|---|---|
| `VER-PAL-001` | real PAL+fake media | opcode/size/address矩阵 | Busy/Rejected零状态差 | E2 |
| `VER-PAL-002` | real PAL+EventKernel | media success/fail/Busy | exactly once与FSM顺序 | E2 |
| `VER-PAL-003` | real Fabric | 同/异resource Transfer | cycle与EAT手算一致 | E2 |
| `VER-PAL-004` | real PAL/Fabric/repair | 指定cycle lane fault | epoch、spare、typed status | E3 |
| `VER-PAL-005` | real全链路 | 每stage reset | old generation不提交、drain全空 | E3 |
| `VER-PAL-006` | real Fabric饱和负载 | lane缩放 | measured<=ceiling且趋势单调 | E4 |

每项冻结config、seed、cycle budget并保存首个分歧token、route和EAT窗口。执行遵循[`实验与测试执行规范.md`](实验与测试执行规范.md)。

## 10. 实施顺序、ADR与完成定义

实施顺序：值类型/profile校验 -> EAT/fabric -> repair -> PAL成功路径 -> failure/reset -> E3/E4接线。

| ADR ID | 决策 | 背景/约束 | 备选方案 | 选择理由 | 代价/后果 | 状态 |
|---|---|---|---|---|---|---|
| ADR-06-001 | PAL与互联同域、NAND状态留在07 | PAL协调传输但不应复制Media | PAL拥有Page | owner清晰 | 跨模块异步接口增加 | `PLANNED` |
| ADR-06-002 | 每stage按需预约 | reset/故障可能改变后续可用性 | issue时预约全链路 | 避免未来资源虚占 | event数量增加 | `PLANNED` |
| ADR-06-003 | hybrid bonding为外部profile | OCP未给物理参数 | synthetic默认 | 防止伪称合规 | 首期不比较该技术 | `BLOCKED_SPEC` |

完成定义：全部生产文件接入CMake；PAL-001至006通过规定证据；credit/token/EAT守恒；HBF真实pipeline reset/fault/drain通过；PAL-007只有外部参数与E4基线齐备后才能解除`BLOCKED_SPEC`。


> **源码同步（2026-09-15）**：当前仓库已落地对应 `include/openhbx/...` 公开接口及 `src/...` 生产实现；文档中的测试路径和验证状态仍以实际 CMake/CTest artifact 为准。仅有源码不构成 `VERIFIED` 证据。
