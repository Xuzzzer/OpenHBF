# OpenHBX Base Die Flash Controller 详细设计

**文档版本**：V1.0
**文档位置**：`docs/LLD_05_BASE_DIE_FLASH_CONTROLLER.md`
**对应HLD**：`docs/HLD_05_BASE_DIE_FLASH_CONTROLLER.md`
**设计状态**：🔵 `IMPLEMENTED`（`status: IMPLEMENTED`；尚无本轮合规验证证据）；外部参数🔴 `BLOCKED_SPEC`（`dependency_status: BLOCKED_SPEC`）
**适用基线**：OCP HBF v0.7.0；OpenHBX 首期 `OCP_HBF_0_7`

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 首版Controller实现级设计，PAL为唯一介质端口 |

## 1. 模块概述与约束

覆盖`CTRL-001`至`CTRL-011`。上游03，规则协作者04，下游06 PAL。05拥有请求、DLU、scheduler、cache、ECC、Scratchpad/Admin/lifecycle状态；不拥有Host response ordering、地址公式/sequence cursor、TSV或NAND状态。所有异步提交遵循`admit->reserve->issue->completion->commit->response`，介质成功前不提交Host write成功。

### 1.1 目录与存储位置

| 内容 | 仓库相对目录/完整路径 | 用途 | 状态 |
|---|---|---|---|
| 本LLD | `docs/LLD_05_BASE_DIE_FLASH_CONTROLLER.md` | Controller实现与验收契约 | 🔵 `IMPLEMENTED` |
| 对应HLD | `docs/HLD_05_BASE_DIE_FLASH_CONTROLLER.md` | Controller边界与需求基线 | 🔵 `IMPLEMENTED` |
| 公开/内部头 | `include/openhbx/hbf/controller/` | Facade、PAL端口与子组件类型 | 🔵 `IMPLEMENTED` |
| 生产实现 | `src/hbf/controller/` | accumulator、scheduler、cache、ECC和control plane | 🔵 `IMPLEMENTED` |
| 组件测试 | `tests/unit/hbf/test_flash_controller_components.cpp` | 05子组件自动oracle | 🔵 `IMPLEMENTED` |
| 集成测试 | `tests/integration/hbf/test_base_die_pal_path.cpp` | `03 -> 05（查询/预约04） -> 06 -> 07` | 🔵 `IMPLEMENTED` |
| 正式artifact | `build/artifacts/<suite>/<experiment-id>/<run-id>/` | Controller trace、snapshot与验证证据；目录须被`.gitignore`覆盖 | 🔵 `IMPLEMENTED` |

机器状态：`status: IMPLEMENTED`；测试需按实验规范重新执行；`dependency_status: BLOCKED_SPEC`仅用于未来源化vendor参数。

## 2. 需求到实现映射

| 需求ID | HLD章节 | 文件/符号 | 算法/状态 | Test ID | 状态 |
|---|---|---|---|---|---|
| CTRL-001/002 | 4.1 | `dlu_accumulator.*` | mask/deadline/bounded map | CTRL-T01/02 | IMPLEMENTED |
| CTRL-003/004 | 4.1 | `base_die_flash_controller.*` | reservation/PAL terminal | CTRL-T03/04 | IMPLEMENTED |
| CTRL-005 | 4.2 | `dlu_accumulator.*:probe_read` | forward/pending missing | CTRL-T05 | IMPLEMENTED |
| CTRL-006 | 4.2 | `flash_scheduler.*` | per-bank sense/batch | CTRL-T06 | IMPLEMENTED |
| CTRL-007 | 4.2 | `bank_cache_manager.*` | >=2 buffer credit | CTRL-T07 | IMPLEMENTED |
| CTRL-008 | 4.2 | `ecc_pipeline.*` | typed data-valid/retry | CTRL-T08 | IMPLEMENTED |
| CTRL-009 | 4.3 | `control_plane.*` | typed admin/lifecycle | CTRL-T09 | IMPLEMENTED |
| CTRL-010 | 1/5 | `flash_pal_port.h` | only backend | CTRL-T10 | IMPLEMENTED |
| CTRL-011 | 8 | config validator | source tag | CTRL-T11 | BLOCKED_SPEC |

## 3. 核心类型与数据结构

| 字段 | 类型/单位 | 范围/默认 | owner/生命周期 | 不变量 |
|---|---|---|---|---|
| `PendingDlu::address` | `DluAddress` | mapped范围 | accumulator/first至terminal | key唯一 |
| `received_mask` | u64/sector bits | 0..all ones | 同上 | bit与payload有效sector一致 |
| `payload` | 4096B handle | fixed | Accepted后05 | 不复制到queue |
| `host_tokens[64]` | token refs | 每sector最多1 | 同上 | overlap不覆盖 |
| `deadline_cycle` | u64/cycle | first+timeout | 同上 | 首sector后不延长 |
| `ControllerRecord::state` | enum | Admitted..Terminal | controller | 合法转换 |
| `CacheBuffer::state` | enum | Free/Reserved/Filling/Valid | cache | 单Bank归属 |
| `PalToken` | strong token | unique/generation | issue至completion | exactly once |
| `EccResult` | status+data_valid+retry | typed | pipeline至controller | CECC valid、UECC规则化 |

`AccumulatorResult={Accepted,Complete(DluHandle),Overlap,Limit,Timeout}`；`ReadProbe={Forward(payload),PendingMissing,Miss}`；`FlashPalCommand`为`Read/Program/Erase/Reset` tagged variant且携带04 typed address/reservation token。

容量为`pending<=max_pending_dlu`、`cache_buffers_per_bank>=2`、`controller_records<=queue credits`、`pal_outstanding<=pal credits`。

per-channel 语义（CHAN-013，ADR-CHAN-009，2026-10-15）：`max_pending_dlu` 与 ECC decode credits 均为**每 channel** 预算（实现为 `pending_by_channel_` / `ecc_inflight_by_channel_` 映射），一个 channel 的写聚合/读突发不得耗尽其他 channel 的同名预算；`queue_depth` 维持 per-Bank 语义（Bank 唯一归属 channel，天然隔离）。PAL Busy 时调度器不再停止本趟排空（队头阻塞），改为把该 work 延迟到本趟结束后重新入队（deferred requeue），Busy channel 不阻塞其他 channel 的就绪工作。

## 4. 头文件与公开边界

公开`base_die_flash_controller.h`仅暴露03 submit和06 completion入口；`controller_types.h`定义跨03/04/06值对象；`flash_pal_port.h`是唯一介质端口，禁止include PAL实现或TSV类型。`dlu_accumulator.h`、`flash_scheduler.h`、`bank_cache_manager.h`、`ecc_pipeline.h`、`control_plane.h`均为05内部或受限协作者头。

## 5. 函数与接口详细设计

### 5.1 `ControllerAdmission submit(DecodedHostTxn txn)`

**需求**：CTRL-001..005/009；**方向**：03->05。先按packet type路由，探测record和子组件credit，再原子Accepted。Busy不转移payload；Rejected返回可映射status且无future event。Write sector调用accumulator；Read调用probe后决定forward/0xA/scheduler；Admin调用control plane。

### 5.2 `AccumulateResult add_sector(DluKey, SectorIndex, PayloadSlice, HostToken, Cycle)`

检查sector<64、key有效。existing bit返回Overlap且不覆盖；新DLU在上限返回Limit；首sector创建deadline event并固定deadline。写入后若mask全1返回冻结的DluHandle。timeout event先generation/instance-id校验，终结所有已收Host token并回收payload。

### 5.3 `ScheduleResult enqueue(FlashWork work)`

per-bank有界队列；比较`priority, batch boundary, age, stable sequence`。同Bank一次只issue一个sense；Regular Read使batch generation结束。只有cache、PAL和必要ECC credit全部可用才issue，失败按逆序回滚。

### 5.4 `PalAdmission issue(FlashPalCommand command)`

**方向**：05->06；**需求**：CTRL-003/004/010。Accepted才转移command/payload ref和PAL credit；Busy保持work队首且不更新04 reservation；Rejected经统一completion path处理。05不读取TSV EAT。

### 5.5 `void on_pal_completion(FlashPalCompletion result)`

校验PalToken/generation。Read先ECC decode再填cache/Host completion；Program成功时调用04 complete成功，随后发Host success；Program failure调用04 failure并返回0x7。completion只消费一次；stale回收PAL/cache credit而不提交新状态。

### 5.6 `ControlResult submit_admin(AdminCommand)`

表9 opcode typed dispatch。Zone Remap/Reduced Capacity调用04；Secure Erase/BIST/feature若vendor行为未配置返回unsupported/blocked，不猜测；Register由03 port处理。Scratchpad按64B和owner范围同步入队，power/reset清除规则来自profile/reset domain。

## 6. 内部逻辑、状态机与算法

Write算法：`probe credits -> accumulator add -> if full freeze -> map/reserve04 -> optional erase work -> ECC encode -> scheduler -> PAL issue -> completion -> complete04 -> Host terminal`。04 reservation在PAL Busy时保持但必须有bounded timeout/cancel策略；若issue前取消则显式cancel reservation。

| 当前状态 | 事件/条件 | Guard | 动作 | 下一状态 | 失败处理 |
|---|---|---|---|---|---|
| Admitted | sector accepted | unique/credit | mask/payload更新 | Accumulating/Full | overlap/limit terminal |
| Accumulating | deadline | instance匹配 | terminal已收tokens | Terminal | stale忽略 |
| Full | 04 reserve | 顺序许可 | 冻结payload/work | Ready | status映射 |
| Ready | all credits+PAL accept | stable队首 | 转移PAL ref | InFlight | Busy回滚临时credit |
| InFlight | PAL read success | generation同 | ECC/cache | EccPending | typed failure |
| InFlight | PAL program success | generation同 | 04 commit | Responding | failure->replay |
| EccPending | ECC result | data-valid规则 | Host completion | Responding | retry/reissue有界 |
| Responding | 03 accepts completion | once | 释放record | Terminal | 03 Busy保留response |

reset顺序：关闭admission -> generation++ -> cancel未issue -> 标记inflight旧generation -> 清易失子组件 -> 为已Accepted产生policy规定terminal -> drain旧资源。事件同cycle phase固定为`timeout < media completion < Host response`或由上位02冻结，测试必须覆盖边界；最终选项记录在resolved config/ADR，不能依赖插入偶然性。

## 7. 逐文件设计

### 7.1 文件总表

| 路径 | 动作 | owner | 职责/API | 依赖 | 需求/Test | 状态 |
|---|---|---|---|---|---|---|
| `include/openhbx/hbf/controller/controller_types.h` | 新增 | 05 | typed work/result | 03/04 common | 全部 | IMPLEMENTED |
| `include/openhbx/hbf/controller/base_die_flash_controller.h` | 新增 | 05 | 公开facade | public ports | CTRL-003/004 | IMPLEMENTED |
| `src/hbf/controller/base_die_flash_controller.cpp` | 新增 | 05 | route/state/completion | 子组件 | CTRL-T03/04 | IMPLEMENTED |
| `include/openhbx/hbf/controller/flash_pal_port.h` | 新增 | 05/06契约 | PAL typed port | common token | CTRL-010 | IMPLEMENTED |
| `include/openhbx/hbf/controller/dlu_accumulator.h` | 新增 | 05 | accumulator API | event kernel | CTRL-001/002/005 | IMPLEMENTED |
| `src/hbf/controller/dlu_accumulator.cpp` | 新增 | 05 | mask/timeout/forward | payload store | CTRL-T01/02/05 | IMPLEMENTED |
| `include/openhbx/hbf/controller/flash_scheduler.h` | 新增 | 05 | scheduler API | PAL capability | CTRL-006 | IMPLEMENTED |
| `src/hbf/controller/flash_scheduler.cpp` | 新增 | 05 | deterministic arbitration | cache/PAL | CTRL-T06 | IMPLEMENTED |
| `include/openhbx/hbf/controller/bank_cache_manager.h` | 新增 | 05 | buffer credit API | address types | CTRL-007 | IMPLEMENTED |
| `src/hbf/controller/bank_cache_manager.cpp` | 新增 | 05 | cache state/data refs | payload | CTRL-T07 | IMPLEMENTED |
| `include/openhbx/hbf/controller/ecc_pipeline.h` | 新增 | 05 | encode/decode port | RAS config | CTRL-008 | IMPLEMENTED |
| `src/hbf/controller/ecc_pipeline.cpp` | 新增 | 05 | stage/status | kernel | CTRL-T08 | IMPLEMENTED |
| `include/openhbx/hbf/controller/control_plane.h` | 新增 | 05 | Admin/Scratchpad/lifecycle | 03/04 ports | CTRL-009 | IMPLEMENTED |
| `src/hbf/controller/control_plane.cpp` | 新增 | 05 | opcode/reset dispatch | profile | CTRL-T09 | IMPLEMENTED |
| `tests/unit/hbf/test_flash_controller_components.cpp` | 新增 | test | accumulator/cache/ECC/scheduler | production05 | CTRL-T01/02/05..08 | IMPLEMENTED |
| `tests/integration/hbf/test_base_die_pal_path.cpp` | 新增 | test | 03/04/05/06 contract | production path | CTRL-T03/04/09/10 | IMPLEMENTED |

### 7.2 生产文件解析

#### Controller公共类型（`include/openhbx/hbf/controller/controller_types.h`）

**职责与设计**：单独header定义work、DLU、cache、ECC、PAL与completion值类型；无source和可变状态。**API及输入输出**：为03/04/05/06提供tagged command/result和显式单位。**owner/lifecycle**：05定义，实例按admission至terminal流转。**错误与依赖**：禁止暴露TSV/Media内部类型；只依赖公共token、03/04 typed类型。**追踪**：CTRL-001..010、CTRL-T01..10；`PLANNED`。

#### Base Die Controller Facade（`include/openhbx/hbf/controller/base_die_flash_controller.h`、`src/hbf/controller/base_die_flash_controller.cpp`）

**职责**：request/credit/token/terminal唯一协调者；不拥有04规则状态或06/07状态。**头文件/源文件**：header公开submit/PAL completion/lifecycle端口，source实现路由、原子回滚和terminal。**输入输出**：03 transaction与06 completion -> 06 PAL command和03 completion；旁路查询/预约04。**owner/lifecycle**：持有record、queue和credit至terminal。**错误与依赖**：Busy零副作用、stale不提交；依赖03/04/06公开port，禁止PAL实现反向include。**追踪**：CTRL-003/004/010、CTRL-T03/T04/T10；`PLANNED`。

#### Flash PAL契约（`include/openhbx/hbf/controller/flash_pal_port.h`）

**职责与设计**：单独header定义05->06唯一typed backend端口`issue/on_completion/capabilities`；无source，不含TSV路由算法。**输入输出**：`FlashPalCommand` -> Accepted/Busy/Rejected及异步completion。**owner/lifecycle**：Accepted才向06转移command/payload引用，Busy/Rejected仍由05拥有。**错误与依赖**：generation/token/data-valid为契约字段；只依赖公共types，05不得include 06私有头。**追踪**：CTRL-004/010、CTRL-T04/T10；`PLANNED`。

#### DLU Accumulator（`include/openhbx/hbf/controller/dlu_accumulator.h`、`src/hbf/controller/dlu_accumulator.cpp`）

**职责**：唯一负责DLU mask、4KiB payload、Host token、deadline和read probe；不判断program顺序。**头文件/源文件**：header声明add/probe/cancel/timeout，source实现bounded map、instance generation和slice forwarding。**输入输出**：sector/token/cycle -> accepted/full/overlap/limit及forward结果。**owner/lifecycle**：首sector创建PendingDlu，terminal/reset回收。**错误与依赖**：overlap/limit/timeout typed且不覆盖数据；依赖02 EventKernel与payload store。**追踪**：CTRL-001/002/005、CTRL-T01/T02/T05；`PLANNED`。

#### Flash Scheduler（`include/openhbx/hbf/controller/flash_scheduler.h`、`src/hbf/controller/flash_scheduler.cpp`）

**职责**：per-bank work、Regular/Batch边界、stable priority与issue credit；不读取TSV/PAL EAT内部。**头文件/源文件**：header声明enqueue/select/complete，source按priority/batch/age/sequence确定性仲裁。**输入输出**：FlashWork/resource availability -> selected work/stall。**owner/lifecycle**：queue item从enqueue至PAL accepted/cancel归scheduler。**错误与依赖**：满时Busy，资源预约失败逆序回滚；依赖Cache/PAL capability接口。**追踪**：CTRL-006、CTRL-T06；`PLANNED`。

#### Bank Cache Manager（`include/openhbx/hbf/controller/bank_cache_manager.h`、`src/hbf/controller/bank_cache_manager.cpp`）

**职责**：每Bank至少双buffer的reserve/fill/lookup/release；不拥有NAND Page payload。**头文件/源文件**：header公开credit与handle API，source维护Free/Reserved/Filling/Valid状态和data refs。**输入输出**：Bank/DLU/token/completion -> cache handle/hit/stall。**owner/lifecycle**：05拥有cache metadata及缓存payload引用，evict/reset释放。**错误与依赖**：无free buffer返回Busy，stale fill不发布Valid；依赖04地址类型和payload store。**追踪**：CTRL-007、CTRL-T07；`PLANNED`。

#### ECC Pipeline（`include/openhbx/hbf/controller/ecc_pipeline.h`、`src/hbf/controller/ecc_pipeline.cpp`）

**职责**：bounded encode/decode stage及OK/CECC/UECC/retry typed结果；不拥有NAND raw状态。**头文件/源文件**：header声明submit/cancel/completion，source按profile latency安排02事件。**输入输出**：payload/raw reliability result -> encoded payload或`EccResult`。**owner/lifecycle**：stage credit从Accepted至completion归05 ECC组件。**错误与依赖**：CECC data-valid，UECC/retry严格按结果；依赖source-tagged RAS profile和kernel。**追踪**：CTRL-008/011、CTRL-T08/T11；`PLANNED`/参数`BLOCKED_SPEC`。

#### Control Plane（`include/openhbx/hbf/controller/control_plane.h`、`src/hbf/controller/control_plane.cpp`）

**职责**：Scratchpad、typed Admin和lifecycle/reset queue；不实现03寄存器image或04 ZoneMap内部算法。**头文件/源文件**：header声明submit_admin/scratchpad/reset，source按opcode委托04/06并执行易失状态清理。**输入输出**：03 Admin/Scratchpad/lifecycle intent -> 04命令、06 PAL命令、typed completion。**owner/lifecycle**：拥有scratchpad和admin queue，按reset domain终结。**错误与依赖**：unsupported vendor opcode保持BLOCKED/typed error；依赖03 register port、04 admin port、06 PAL port。**追踪**：CTRL-009/011、CTRL-T09/T11；`PLANNED`/`BLOCKED_SPEC`。

### 7.3 测试文件解析

#### Controller组件测试（`tests/unit/hbf/test_flash_controller_components.cpp`）

对象为真实Accumulator/Scheduler/Cache/ECC，fixture使用真实deterministic test clock，fake仅替代04/PAL公开端口。输入覆盖64 sector排列、deadline同cycle、queue/cache满和ECC矩阵；oracle为mask、顺序、data-valid及credit守恒，最高E2。对应CTRL-T01/T02/T05..08，`PLANNED`。

#### Base Die-PAL集成测试（`tests/integration/hbf/test_base_die_pal_path.cpp`）

对象路径为真实`03 -> 05（查询/预约04） -> 06 -> 07`；04是05的旁路规则协作者，不是通往06的数据转发节点。受控07 fixture时最高E2，真实生产07时目标E3。输入覆盖incomplete/full、PAL Busy/failure、Admin、reset/stale；oracle为介质issue/commit时点、Host terminal once及token/credit守恒。对应CTRL-T03/T04/T09/T10，`PLANNED`。

## 8. 配置、错误、统计与Debug

| 配置键 | 类型/单位 | 默认/规则 | 非法行为 |
|---|---|---|---|
| `max_pending_dlu` | u32/count | product profile，>0；per-channel（CHAN-013） | 启动拒绝 |
| `accumulation_timeout` | cycle+source | 必须>0且有来源/synthetic | 缺标签拒绝 |
| `cache_buffers_per_bank` | u32 | >=2 | 启动拒绝 |
| `queue_depth.*` | u32 | >0/乘积checked；per-Bank | 启动拒绝 |
| `ecc.impl/latency` | enum/cycle+source | capability匹配；credits per-channel（CHAN-013） | 启动拒绝 |
| `read_retry_limit` | u32 | source-tagged | 缺来源BLOCKED |

错误唯一映射：accumulator overlap/limit/timeout -> write 0x2/0x4/0x5；04 order/program replay -> 0x6/0x7；read pending missing -> 0xA；raw/ECC结果映射read 0x4/0x5/0x6；erased page -> read 0x7；capacity unusable -> read/write 0x8；die recovery/blocked -> read/write 0x9。0x9必须立即terminal，不能作为无界Busy留在queue。统计包含accepted/terminal/outstanding、sector/complete/timeout、PAL issued/completed、cache hit/stall、ECC result、batch、admin；bytes区分Host/cache/media避免重复吞吐。Debug snapshot按token/DLU/Bank排序，trace含generation、phase、mask、deadline、reservation、PAL token、data-valid。

## 9. 测试用例设计

| ID | Requirement | Evidence | Production path/Fake | Input/Fault | Oracle |
|---|---|---|---|---|---|
| CTRL-T01/02 | CTRL-001/002 | E1 | accumulator | 64乱序sector、duplicate、limit、timeout | mask/状态码/deadline/回收精确 |
| CTRL-T03/04 | CTRL-003/004 | E3目标 | 03->05（查询/预约04）->06->07 | incomplete/full、PAL Busy/fail | issue仅full；commit前无success；once |
| CTRL-T05 | CTRL-005 | E2 | accumulator+controller | pending received/missing | payload forward或0xA，无media read |
| CTRL-T06/07 | CTRL-006/007 | E2 | scheduler/cache+PAL fake | batch/regular、双buffer满 | sense序、batch边界、credit守恒 |
| CTRL-T08 | CTRL-008 | E2 | ECC+controller | OK/CECC/UECC/retry/erased/capacity-unusable/die-blocked | 表12/13 status、data-valid、ErrorInfo及0x9立即完成匹配 |
| CTRL-T09/10 | CTRL-009/010 | E2/E3 | control plane/PAL公开路径 | reset/Admin/stale/TSV fault result | 无跨owner访问、terminal守恒 |
| CTRL-T11 | CTRL-011 | E1 | config resolver | 缺source标签 | 启动拒绝或artifact标synthetic |

每项固定config/seed，预算最多100万cycle/10万request；失败artifact增加首个token分歧、mask和credit snapshot。执行遵循[`实验与测试执行规范.md`](实验与测试执行规范.md)。

## 10. 实施顺序、ADR与完成定义

顺序：types/PAL契约 -> accumulator -> 04 reservation集成 -> scheduler/cache -> ECC -> facade/completion -> Admin/lifecycle -> E3系统验证。

| ADR ID | 决策 | 背景/备选 | 理由 | 代价 | 状态 |
|---|---|---|---|---|---|
| ADR-CTRL-001 | 06 PAL为唯一backend | Controller内置TSV/media | 状态owner清晰且便于OpenHBX扩展 | 跨端口事件增加 | IMPLEMENTED |
| ADR-CTRL-002 | DLU payload单份+sector mask | 64份独立buffer | 限制内存并支持forward | 需slice所有权 |
| ADR-CTRL-003 | Program成功后才Host terminal | issue后提前response | OCP non-posted语义 | 延迟和outstanding增加 | IMPLEMENTED |
| ADR-CTRL-004 | deterministic多级priority | 宿主线程并发 | 可重放 | 并行由模型而非线程表达 | IMPLEMENTED |

完成定义：文件/API全部落地；CTRL-001..011均有实现落点或明确BLOCKED_SPEC；Busy/reset/stale、DLU、cache、ECC、Admin及credit守恒E1/E2通过；真实03->07 production pipeline的CTRL-T03/T04达到E3后才可声明HBF控制路径已验证；TSV状态在05生产文件中不存在；实验artifact和状态更新符合统一规范。
