# OpenHBX HBF Address and Topology 详细设计

**文档版本**：V1.0
**文档位置**：`docs/LLD_04_HBF_ADDRESS_TOPOLOGY.md`
**对应HLD**：`docs/HLD_04_HBF_ADDRESS_TOPOLOGY.md`
**设计状态**：🔵 `IMPLEMENTED`（`status: IMPLEMENTED`；尚无本轮合规验证证据）
**适用基线**：OCP HBF v0.7.0；OpenHBX 首期 `OCP_HBF_0_7`

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 首版地址、拓扑和replay view实现设计 |

## 1. 模块概述与约束

覆盖`ADDR-001`至`ADDR-009`。03提供Channel local byte address；05调用mapping/reservation/admin；07仅消费typed physical address。04拥有geometry、owner、sequence和mapping view，不拥有payload、Bank busy、Page programmed、scheduler或TSV。A1永远是64B单位，禁止以`>>6`重复转换已经是A1的值。

### 1.1 目录与存储位置

| 内容 | 仓库相对目录/完整路径 | 用途 | 状态 |
|---|---|---|---|
| 本LLD | `docs/LLD_04_HBF_ADDRESS_TOPOLOGY.md` | 实现、算法和测试契约 | 🔵 `IMPLEMENTED` |
| 对应HLD | `docs/HLD_04_HBF_ADDRESS_TOPOLOGY.md` | 模块边界与需求基线 | 🔵 `IMPLEMENTED` |
| 公开头文件 | `include/openhbx/hbf/address/` | 04公开typed地址与端口 | 🔵 `IMPLEMENTED` |
| 生产实现 | `src/hbf/address/` | geometry、mapper、sequence和zone实现 | 🔵 `IMPLEMENTED` |
| 单元测试 | `tests/unit/hbf/test_address_topology.cpp` | 公式、边界、owner与状态机oracle | 🔵 `IMPLEMENTED` |
| 集成测试 | `tests/integration/hbf/test_sequence_controller.cpp` | 04旁路规则端口与05协作 | 🔵 `IMPLEMENTED` |
| 正式artifact | `build/artifacts/<suite>/<experiment-id>/<run-id>/` | 映射、replay与验证证据；目录须被`.gitignore`覆盖 | 🔵 `IMPLEMENTED` |

机器状态：`status: IMPLEMENTED`；测试需按实验规范重新执行。04是05查询/预约的旁路规则协作者，不是`05 -> 06`介质数据路径上的转发节点。

## 2. 需求到实现映射

| 需求ID | HLD章节 | 文件/符号 | 算法/状态 | Test ID | 状态 |
|---|---|---|---|---|---|
| ADDR-001/002/004 | 4.1 | `hbf_geometry.*`、`hbf_address_mapper.*` | checked R1--R5 | ADDR-T01/02/04 | IMPLEMENTED |
| ADDR-003 | 4.2 | `channel_topology.*` | owner table/formula | ADDR-T03 | IMPLEMENTED |
| ADDR-005/006 | 4.3 | `block_sequence.*` | reserve/commit/replay | ADDR-T05/06 | IMPLEMENTED |
| ADDR-007/008 | 4.3 | `zone_map.*` | epoch mapping/capacity | ADDR-T07/08 | IMPLEMENTED |
| ADDR-009 | 1 | `hbf_geometry.*:validate_profile` | forbidden keys | ADDR-T09 | IMPLEMENTED |

## 3. 核心类型与数据结构

| 字段 | 类型/单位 | 合法范围/默认 | owner/生命周期 | 不变量 |
|---|---|---|---|---|
| `LocalByteAddress::value` | u64/byte | capacity内 | caller值对象 | 64B aligned for transaction |
| `A1Units64::value` | u64/64B | derived | mapper栈值 | `bytes/64`恰一次 |
| `HbfGeometry::r1..r5` | u64/count | >0/profile | 04产品期 | R4=64 for 4KiB |
| `HbfAddress::*` | strong index | 各层范围 | 值对象 | channel owner合法 |
| `BlockState::expected_page` | u32/page | 0..R3 | 04 block期 | 成功completion才推进 |
| `mode` | enum | Sequential/Reserved/Replay/Full/Retired | 同上 | 转换受表约束 |
| `epoch` | u64 | 单调 | block/zone | reservation必须匹配 |
| `ZoneMapEntry` | ids+flags | bijective active view | 04产品期 | remap不复制payload |

`MapResult`为`Mapped(HbfAddress,AddressViewEpoch)`或typed error；`ReserveResult={Reserved(ProgramReservation),AutoEraseRequired(...),Busy,Rejected}`。`ProgramReservation`不可复制或带唯一token语义，字段含block/page/epoch/replay flag。

## 4. 头文件与公开边界

`hbf_address_types.h`是跨03/04/05/07唯一typed地址定义；`hbf_address_mapper.h`公开正反映射；`block_sequence.h`和`zone_map.h`只向05 Admin/Controller公开命令式端口。`hbf_geometry.h`由00构造后只读。内部checked arithmetic helper不得成为跨模块旁路。07禁止include block/zone内部头。

## 5. 函数与接口详细设计

### 5.1 `MapResult map(ChannelId channel, LocalByteAddress address) const`

**需求**：ADDR-001..004；**方向**：03/05->04。前置为64B aligned。先checked转换A1，再应用zone view和R1--R5公式，验证每层范围与owner。纯函数，无backpressure；错误不改变任何状态。复杂度O(1)。ADDR-T01..04做golden和round-trip。

### 5.2 `ReverseResult reverse(const HbfAddress&) const`

仅接受canonical、active、owner一致地址；使用checked逆公式返回Channel local byte address。`reverse(map(x))=x`适用于可访问范围。retired或歧义view返回typed error。

### 5.3 `ReserveResult reserve_program(BlockKey block, PageIndex page, Token token)`

**需求**：ADDR-005/006；**方向**：05->04。Guard为无outstanding reservation、epoch匹配、未retired。page0在Empty态返回携带reservation的auto-erase意图；其余page必须等于cursor，replay必须等于replay cursor。Busy/Rejected零副作用。

### 5.4 `CompleteResult complete_program(ProgramReservation r, MediaResult result)`

验证token/epoch/状态。成功才推进cursor；failure切换Replay并记录failure page；取消恢复未提交状态。duplicate/stale拒绝且不改变cursor。05负责OCP status映射。

### 5.5 `RemapResult apply_zone_remap(ZoneRemap command)`

在单event中验证两个zone active、无冲突reservation后交换physical view并提升epoch；不调用PAL/Media，不产生copy event。Busy表示存在reservation且零副作用。retirement从active bitmap移除并提升epoch。

## 6. 内部逻辑、状态机与算法

公式严格实现：`L1=A1/R4; B1=L1/(R1*R2*R3); P1=L1%(R1*R2*R3); Bank_Num=P1%(R1*R2); L2=B1*(R1*R2*R3)+Bank_Num`。每次乘加使用`checked_mul/add`，除数在构造期验证非零。

| 当前状态 | 事件/条件 | Guard | 动作 | 下一状态 | 失败 |
|---|---|---|---|---|---|
| Empty | reserve page0 | no outstanding | 创建reservation/erase intent | Reserved | Busy/reject |
| Sequential | reserve expected | page==cursor | 创建reservation | Reserved | order violation |
| Reserved | success | token+epoch match | cursor++ | Sequential/Full | stale |
| Reserved | failure | match | failure_page/cursor=0 | Replay | stale |
| Replay | reserve replay cursor | page<=failure_page | reservation | Reserved | replay required |
| 任意 | retire/reset epoch | policy allows | epoch++/取消 | Retired/初始 | old token stale |

确定性：稀疏block表的observable迭代先按`BlockKey`排序；zone remap涉及多个entry时按zone id锁序（单线程逻辑锁）并原子发布新epoch。

## 7. 逐文件设计

### 7.1 文件总表

| 路径 | 动作 | owner | 职责/API | 依赖 | 需求/Test | 状态 |
|---|---|---|---|---|---|---|
| `include/openhbx/hbf/address/hbf_address_types.h` | 新增 | 04 | strong units/address/result | common types | ADDR-001 | IMPLEMENTED |
| `include/openhbx/hbf/address/hbf_geometry.h` | 新增 | 04 | immutable geometry | config | ADDR-002/004/009 | IMPLEMENTED |
| `src/hbf/address/hbf_geometry.cpp` | 新增 | 04 | derive/validate | checked math | ADDR-T04/T09 | IMPLEMENTED |
| `include/openhbx/hbf/address/channel_topology.h` | 新增 | 04 | ownership API | geometry | ADDR-003 | IMPLEMENTED |
| `src/hbf/address/channel_topology.cpp` | 新增 | 04 | owner computation | config | ADDR-T03 | IMPLEMENTED |
| `include/openhbx/hbf/address/hbf_address_mapper.h` | 新增 | 04 | map/reverse API | types | ADDR-001/002 | IMPLEMENTED |
| `src/hbf/address/hbf_address_mapper.cpp` | 新增 | 04 | formulas/view | topology | ADDR-T01/02 | IMPLEMENTED |
| `include/openhbx/hbf/address/block_sequence.h` | 新增 | 04 | reservation API | token | ADDR-005/006 | IMPLEMENTED |
| `src/hbf/address/block_sequence.cpp` | 新增 | 04 | state/replay iterator | mapper | ADDR-T05/06 | IMPLEMENTED |
| `include/openhbx/hbf/address/zone_map.h` | 新增 | 04 | remap/retire API | types | ADDR-007/008 | IMPLEMENTED |
| `src/hbf/address/zone_map.cpp` | 新增 | 04 | atomic epoch view | block sequence | ADDR-T07/08 | IMPLEMENTED |
| `tests/unit/hbf/test_address_topology.cpp` | 新增 | test | golden/round-trip/negative | production04 | ADDR-T01..09 | IMPLEMENTED |
| `tests/integration/hbf/test_sequence_controller.cpp` | 新增 | test | 04/05 contract | production04/05 | ADDR-T05..08 | IMPLEMENTED |

### 7.2 生产文件解析

#### 地址公共类型（`include/openhbx/hbf/address/hbf_address_types.h`）

**职责与设计**：单独header定义byte/64B/DLU强类型、各层index、`HbfAddress`及typed result；无source、无算法状态。**API及输入输出**：为03/05/07提供值对象和显式转换入口。**owner/lifecycle**：定义owner为04，实例由caller按请求期持有。**错误与依赖**：构造器拒绝隐式单位混用；只依赖公共整数/token类型，禁止依赖Media。**追踪**：ADDR-001/002、ADDR-T01/T02；`PLANNED`。

#### Geometry（`include/openhbx/hbf/address/hbf_geometry.h`、`src/hbf/address/hbf_geometry.cpp`）

**职责**：推导R1--R5、层次数量和checked容量并拒绝传统FTL配置；不映射单次请求。**头文件/源文件**：header导出immutable getter/`create` result，source实现nonzero、checked mul/add及profile来源校验。**输入输出**：resolved config -> immutable geometry或启动错误。**owner/lifecycle**：04产品生命周期唯一实例，构造后只读。**错误与依赖**：overflow、zero divisor、forbidden key结构化拒绝；依赖00 config。**追踪**：ADDR-002/004/009、ADDR-T02/T04/T09；`PLANNED`。

#### Channel Topology（`include/openhbx/hbf/address/channel_topology.h`、`src/hbf/address/channel_topology.cpp`）

**职责**：计算/验证Host Channel对Bank容量切片的唯一ownership；不复制物理树。**头文件/源文件**：header声明`owner_of/can_access`，source实现profile冻结公式或表。**输入输出**：Channel/Bank -> owner判定。**owner/lifecycle**：ownership表由04产品期持有且immutable。**错误与依赖**：越界和跨owner返回typed reject；依赖Geometry和profile，不依赖Controller。**追踪**：ADDR-003、ADDR-T03；`PLANNED`。

#### Address Mapper（`include/openhbx/hbf/address/hbf_address_mapper.h`、`src/hbf/address/hbf_address_mapper.cpp`）

**职责**：纯`map/reverse`及A1/R1--R5公式；不访问Block动态状态或Media。**头文件/源文件**：header公开正反向API，source实现checked公式、owner校验和只读zone view查询。**输入输出**：Channel local byte address/HbfAddress -> mapped值或typed error。**owner/lifecycle**：mapper无per-request状态，引用产品期immutable Geometry/Topology/Zone snapshot。**错误与依赖**：错误携带公式阶段、单位和epoch；依赖上述公开接口。**追踪**：ADDR-001/002/003/007、ADDR-T01/T02/T03/T07；`PLANNED`。

#### Block Sequence（`include/openhbx/hbf/address/block_sequence.h`、`src/hbf/address/block_sequence.cpp`）

**职责**：唯一拥有program cursor、reservation和replay address view；不issue Program/Erase。**头文件/源文件**：header声明reserve/complete/cancel/replay iterator，source实现epoch状态机和`L2+n*R5`。**输入输出**：Block/Page/token/media result -> reservation/permit/replay view。**owner/lifecycle**：per-block状态由04从首次访问至retire/reset持有。**错误与依赖**：Busy/order/replay/stale均typed且失败不推进；依赖Mapper和02 token。**追踪**：ADDR-005/006、ADDR-T05/T06；`PLANNED`。

#### Zone Map（`include/openhbx/hbf/address/zone_map.h`、`src/hbf/address/zone_map.cpp`）

**职责**：唯一拥有zone映射、retirement bitmap和epoch；不搬payload、不产生PAL事件。**头文件/源文件**：header公开lookup/remap/retire snapshot，source原子验证并交换entry。**输入输出**：logical zone/Admin command -> physical view/remap result。**owner/lifecycle**：04产品期持有，remap/retire时epoch单调。**错误与依赖**：invalid/retired/outstanding reservation时拒绝或Busy且view不变；依赖BlockSequence查询接口。**追踪**：ADDR-007/008、ADDR-T07/T08；`PLANNED`。

### 7.3 测试文件解析

#### 地址拓扑单元测试（`tests/unit/hbf/test_address_topology.cpp`）

对象为真实Geometry/Topology/Mapper/BlockSequence/ZoneMap；fixture使用小型与非对称geometry，输入含golden、边界、overflow和禁止配置，seed不适用。oracle为公式中间值、round-trip、owner唯一、epoch/cursor和零副作用，证据E1。对应ADDR-T01..09，`PLANNED`。

#### Sequence-Controller集成测试（`tests/integration/hbf/test_sequence_controller.cpp`）

对象为真实04与05公开reservation契约，fake仅替代PAL completion；输入覆盖成功、失败、重放、remap冲突和reset/stale。oracle为成功才推进、`L2+n*R5`、无copy事件、reservation守恒，最高E2。对应ADDR-T05..08，`PLANNED`。

## 8. 配置、错误、统计与Debug

配置包含`r1..r5`、各层count、channels、owner_policy、zones/spares；均为静态immutable、非零、checked capacity。`gc/l2p/trim/active_relocation=true`启动拒绝。错误typed为Misaligned/OutOfRange/Overflow/WrongOwner/OrderViolation/ReplayRequired/Retired/Busy/Stale；04不直接编码Host status。

统计：maps/map_errors/owner_rejects/reservations/completions/replay/remaps/retired；构造后mapping热路径不修改stats之外的状态。Debug trace含A1/L1/B1/P1/BankNum/L2和epoch；snapshot含geometry hash、sorted block state、zone map，不含介质payload。守恒见HLD第9章。

### 8.1 `GAP-010`实现约束

当前`OpenHbxSystem`以空`zones`参数构造mapper，`map()`因此采用`logical block == physical block`；04也没有接收BBT变化的端口。后续应增加启动期factory-bad视图构建和运行期typed retirement事务，但不得让mapper直接读取07的可变内部表。跨模块事务至少携带physical Block、bad reason、BBT version、旧/新mapping epoch及outstanding状态，并按以下顺序执行：冻结受影响Block的新admission、等待或终止旧reservation、提交BBT、更新Zone/容量视图、启动Host replay。任一步失败不得发布半更新映射。

验收必须覆盖：factory bad从首次map起不可达；运行期Program/Erase failure后旧physical Block不再被issue；replay落到新视图且成功前Host不得收到成功；无替代容量返回`CapacityUnusable`；正反向映射、BBT version、mapping epoch和accepted/terminal计数守恒。

## 9. 测试用例设计

| ID | Requirement | Evidence | Fixture/Input | Oracle |
|---|---|---|---|---|
| ADDR-T02 | ADDR-002 | E1 | OCP公式向量、非对称R值 | 每个中间值和最终地址匹配 |
| ADDR-T03 | ADDR-003 | E1 | 全Channel/Bank组合 | owner唯一、跨界零副作用 |
| ADDR-T04 | ADDR-004 | E1 | UINT64边界配置 | 启动结构化拒绝，无wrap |
| ADDR-T05/06 | ADDR-005/006 | E2 | 04+05，成功/失败/乱序 | 成功才推进；L2+nR5精确 |
| ADDR-T07 | ADDR-007 | E2 | remap并监听PAL port | epoch/view交换，PAL事件为0 |
| ADDR-T09 | ADDR-009 | E1 | 每个禁止配置 | 每项启动拒绝且路径明确 |

预算每geometry最多全容量小模型枚举或10万case；artifact增加formula divergence record。执行遵循[`实验与测试执行规范.md`](实验与测试执行规范.md)。

## 10. 实施顺序、ADR与完成定义

顺序：strong units/checked math -> geometry/topology -> mapper/round-trip -> sequence -> zone/capacity -> 05集成。

| ADR ID | 决策 | 备选 | 理由 | 代价 | 状态 |
|---|---|---|---|---|---|
| ADR-ADDR-001 | 单位强类型+checked arithmetic | 裸u64 | 防止A1重复位移和overflow | 转换代码增加 | IMPLEMENTED |
| ADR-ADDR-002 | Channel为Bank容量切片 | 每Channel复制物理树 | 符合产品拓扑 | owner校验必需 | IMPLEMENTED |
| ADR-ADDR-003 | sequence保留在地址域 | 全放Controller | replay地址视图单一owner | 04含少量动态规则状态 | IMPLEMENTED |
| ADR-ADDR-004 | Zone Remap只交换view | media copy | 符合OCP Host重写 | Host需显式重放 | IMPLEMENTED |

完成定义：全部文件/API落地；ADDR-001..009有自动oracle；map/reverse、sequence/replay、zone无copy及reset/stale达到E1/E2；真实Host->Media系统E3前不得宣称端到端验证；所有状态维持PLANNED直至符合实验规范的证据更新。
