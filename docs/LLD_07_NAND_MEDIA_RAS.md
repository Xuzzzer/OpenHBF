# OpenHBX NAND Media 与 RAS 详细设计

**文档版本**：V1.1
**文档位置**：`docs/LLD_07_NAND_MEDIA_RAS.md`  
**对应HLD**：`docs/HLD_07_NAND_MEDIA_RAS.md`  
**设计状态**：🟡 `PARTIAL`（`status: PARTIAL`）
**覆盖需求**：`MEDIA-001`至`MEDIA-008`、`CHAN-002`至`CHAN-007`、`CHAN-009`、`CHAN-012`

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 首期HBF NAND实现级设计 |
| V1.1 | 2026-09-20 | Channel 分片改造：`NandFlashDevice` 单体退役，拆分为 `DieDomain` + 每 Channel `NandChannelSlice` + `NandMediaRouter`；全局 die EAT 由 `ChannelMediaPath(channel,core)` 取代。架构与 ADR 见 [`HBF_CHANNEL_PARTITIONED_MODEL.md`](HBF_CHANNEL_PARTITIONED_MODEL.md) |

## 1. 模块概述与约束

本LLD仅设计HBF NAND插件。物理层次固定为`Core Die -> Die -> Bank -> Block -> 4 KiB Page`，NOR不在TASK1范围。PAL/TSV归06，ECC与Host可见错误策略归05，replay地址和Block workflow归04。07独占Page/Block/payload/BBT和NAND EAT。OCP未给出的`tR/tPROG/tERS`、cell mode和温度系数只能使用明确标记的synthetic/vendor profile。

| 内容类别 | 仓库相对存储位置 | 存储规则 | 状态 |
|---|---|---|---|
| 本LLD/HLD | `docs/LLD_07_NAND_MEDIA_RAS.md`、`docs/HLD_07_NAND_MEDIA_RAS.md` | 版本控制文档 | ⚪ `PLANNED` |
| Media公开头文件 | `include/openhbx/media/`、`include/openhbx/media/nand/` | 跨模块值类型与NAND facade | ⚪ `PLANNED` |
| NAND内部实现 | `src/media/nand/` | topology、state、payload、timing、EAT与command engine | ⚪ `PLANNED` |
| RAS内部实现 | `src/ras/` | BBT、raw reliability与Die environment | ⚪ `PLANNED` |
| Unit/Component测试 | `tests/unit/media/nand/`、`tests/unit/ras/`、`tests/component/media/` | 测试源码，不写artifact | ⚪ `PLANNED` |
| 正式artifact | `build/artifacts/<suite>/<experiment-id>/<run-id>/` | 仅gitignored build目录 | ⚪ `PLANNED` |

## 2. 需求到实现映射

| 需求ID | HLD章节 | 实现文件/符号 | 算法 | Test ID | 状态 |
|---|---|---|---|---|---|
| MEDIA-001 | 3/6 | `MediaTopology` | checked flatten/decode | VER-MEDIA-001 | `PLANNED` |
| MEDIA-002 | 4/7 | `CommandEngine`,`EatTable` | per-stage FSM | VER-MEDIA-002 | `PLANNED` |
| MEDIA-003 | 4 | `StateStore::commit`,`PageStore` | terminal transaction | VER-MEDIA-003 | `PLANNED` |
| MEDIA-004 | 7 | `CommandLegality` | state/sequence guards | VER-MEDIA-004 | `PLANNED` |
| MEDIA-005 | 4/9 | `BadBlockTable` | bitmap/version/reason | VER-MEDIA-005 | `PLANNED` |
| MEDIA-006 | 4/9 | `ReliabilityModel` | deterministic raw outcome | VER-MEDIA-006 | `PLANNED` |
| MEDIA-007 | 7 | `cancel_generation` | abort/stale guard | VER-MEDIA-007 | `PLANNED` |
| MEDIA-008 | 8 | `DieEnvironment` | sourced multiplier | VER-MEDIA-008 | 🔴 `BLOCKED_SPEC` |
| CHAN-003/004 | 分片架构文档§3/§5.4 | `NandChannelSlice` | per-channel bank slice + ChannelMediaPath EAT | CHAN-T03/04 | 🟢 `VERIFIED`（E2） |
| CHAN-005/009 | 分片架构文档§5.3 | `DieDomain` | 共享 blocked/BBT/capacity 视图 | CHAN-T05/09 | 🟢 `VERIFIED`（E2） |
| CHAN-006 | 分片架构文档§5.5 | `NandMediaRouter`+slice validate | channel 分发与 ownership 拒绝 | CHAN-T06 | 🟢 `VERIFIED`（E2） |
| CHAN-002 | 分片架构文档§5.2 | `FlashCommand.channel` | 全链路显式 channel | CHAN-T02 | 🔵 `IMPLEMENTED`（无专用 oracle） |

## 3. 核心类型与数据结构

| 字段 | 类型/单位 | 合法范围/默认值 | owner/生命周期 | 不变量 |
|---|---|---|---|---|
| `address` | `PhysicalAddress` | profile范围内 | context复制 | owner channel不得跨界 |
| `page_state` | `PageState` | Erased/Pending/Valid | StateStore持久 | terminal commit才改变 |
| `block_epoch` | `uint64_t` | 单调 | StateStore | erase成功递增 |
| `payload` | 4096 B/signature | mode决定 | PageStore | snapshot不可变 |
| `pec/read_count` | `uint64_t` | checked递增 | Reliability/State | 不回退 |
| `stage_end` | `Cycle` | >=start | CommandContext | checked无溢出 |
| `raw_outcome` | tagged enum | profile支持集合 | completion值 | data_valid显式 |

`StateDelta`暂存Program payload引用、Erase block和计数增量，只有`commit_terminal(token,delta)`可写持久状态。

## 4. 头文件与公开边界

公开头：`include/openhbx/media/types.h`定义command/completion（含`FlashCommand.channel`）；`include/openhbx/media/nand/nand_media_router.h`定义06可见的`pal::IFlashMediaPort`实现；`include/openhbx/media/nand/nand_channel_slice.h`与`include/openhbx/media/nand/die_domain.h`供02装配使用；`include/openhbx/media/media_snapshot.h`定义只读诊断。NAND内部头位于`src/media/nand/`，RAS内部头位于`src/ras/`。06只能依赖`types.h`与router头，不能包含上述内部路径。

## 5. 函数与接口详细设计

### `AdmissionResult NandChannelSlice::try_issue(FlashCommand command, Cycle now)`

依次验证地址可展平、**ownership**（`command.channel == 本 slice channel` 且 bank ∈ `owned_banks`，CHAN-006）、全局`CapacityUnusable`与BBT（经`DieDomain`，CHAN-009）、payload与Page/Block legality，再检查per-slice inflight credit（CHAN-010）、token唯一性与首stage EAT。Busy/Rejected时介质状态与事件队列零变化（`command`虽按值传入，但不产生任何可观测副作用）；Accepted后创建context、pending delta并安排stage event。router（`NandMediaRouter::try_issue_media`）在此之前只做channel越界检查与分发，不做ownership判定（ADR-CHAN-004）。

### `void CommandEngine::on_stage_event(StageEvent event)`

校验generation/token/stage sequence；调用environment/reliability决定当前stage结果；若继续则预约下一stage，若terminal则以单一transaction提交StateStore与PageStore，再产生completion。提交任一部分失败视为integrity error且不得半提交。

### `CommitResult StateStore::commit_terminal(Token, const StateDelta&)`

Program验证Page仍处于reservation对应epoch后原子写payload并置Valid；Erase验证Block epoch后清payload/Page、递增epoch与PEC。failure/abort调用`discard_delta`。

### `RawReadClass ReliabilityModel::evaluate_read(uint64_t page, uint64_t pec, double temperature_c)`

输入seed、稳定地址、PEC/read count、retention age、temperature band与fault rules；不使用调用顺序之外的不稳定熵。返回raw bit-error分类与retry/refresh提示，不执行ECC或Host status映射。

## 6. 内部逻辑、状态机与算法

| 当前状态 | 事件 | Guard | 动作 | 下一状态 | 失败处理 |
|---|---|---|---|---|---|
| `Received` | issue | valid+credit | reserve first stage | `StageInFlight` | Busy/Rejected |
| `StageInFlight` | stage event | gen/seq匹配 | evaluate/reserve next | `StageInFlight` | raw fail->`Completing` |
| `StageInFlight` | final verify | state epoch匹配 | atomic commit | `Completing` | no commit failure |
| `Completing` | emit | obligation存在 | completion/release | `Terminal` | duplicate integrity |
| 任意 | reset | scope命中 | discard/cancel/abort | `Terminal` | stale忽略 |

Read snapshot在sense成功后创建，若后续raw outcome为UECC则completion设`data_valid=false`并不外传payload。Program顺序guard消费04提供的expected page约束，但权威Page状态检查仍在07。EAT预约资源ID排序以确保确定性。

### 6.1 Flash命令合法性与提交语义

| 命令 | Admission检查 | 最低stage | terminal成功提交 | 失败行为 |
|---|---|---|---|---|
| Read | 地址在界、Block可访问、资源/credit可用 | command、sense、data snapshot | 不修改Page/Block；返回issue时刻可见数据的snapshot | erased、raw correctable、UECC、retry、temporary blocked使用typed outcome |
| Program | 4096 B payload、Block可用、Page已擦除、04顺序reservation匹配 | data-in、array program、verify | 原子写payload并将Page置Valid；随后完成04 reservation | Program Fail不写payload、不推进Page，进入replay协作 |
| Erase | Block在界、非factory bad、无冲突reservation | erase setup、array erase、verify | 原子清除Block全部Page/payload并递增erase epoch与PEC | Erase Fail保持原状态并返回typed failure |
| Reset/Abort | scope与generation合法 | cancel/abort | 不提交pending delta；释放本generation资源 | stale event仅记录，不二次完成 |

同Bank的array stage按照EAT串行；不同Bank只有在resource mask无交集时并行。Plane/Multi-Plane等可选命令必须通过独立capability启用，关闭时不能改变上述基本命令语义。

## 7. 逐文件设计

| 路径 | 动作 | owner | 职责 | 需求 | 状态 |
|---|---|---|---|---|---|
| `include/openhbx/media/types.h` | 新增 | 07 | public command/completion | MEDIA-002/006 | `PLANNED` |
| `include/openhbx/media/nand/nand_flash_device.h`、`src/media/nand/nand_flash_device.cpp` | 新增 | 07 | facade/assembly | 全部 | `PLANNED` |
| `src/media/nand/topology.h`、`src/media/nand/topology.cpp` | 新增 | 07 | geometry/index | MEDIA-001 | `PLANNED` |
| `src/media/nand/state_store.h`、`src/media/nand/state_store.cpp` | 新增 | 07 | Page/Block terminal commit | MEDIA-003/004 | `PLANNED` |
| `src/media/nand/page_store.h`、`src/media/nand/page_store.cpp` | 新增 | 07 | sparse/timing payload | MEDIA-003 | `PLANNED` |
| `src/media/nand/eat_table.h`、`src/media/nand/eat_table.cpp` | 新增 | 07 | Bank/Die EAT | MEDIA-002 | `PLANNED` |
| `src/media/nand/command_engine.h`、`src/media/nand/command_engine.cpp` | 新增 | 07 | stage FSM/event | MEDIA-002/007 | `PLANNED` |
| `src/media/nand/timing_profile.h`、`src/media/nand/timing_profile.cpp` | 新增 | 07 | sourced stage timing | MEDIA-002/008 | `PLANNED` |
| `src/ras/reliability.h`、`src/ras/reliability.cpp` | 新增 | 07 | deterministic raw faults | MEDIA-006/008 | `PLANNED` |
| `src/ras/bbt.h`、`src/ras/bbt.cpp` | 新增 | 07 | bad/retire bitmap | MEDIA-005 | `PLANNED` |
| `src/ras/die_environment.h`、`src/ras/die_environment.cpp` | 新增 | 07 | temperature/path observation | MEDIA-008 | `BLOCKED_SPEC` |
| `include/openhbx/media/nand/die_domain.h`、`src/media/nand/die_domain.cpp` | 新增 | 07 | Die级共享状态（blocked/BBT/未来温度） | CHAN-005/009 | 🟢 `VERIFIED`（E2） |
| `include/openhbx/media/nand/nand_channel_slice.h`、`src/media/nand/nand_channel_slice.cpp` | 新增 | 07 | 每Channel介质切片（stage FSM/Bank与ChannelMediaPath EAT/payload） | CHAN-003/004/006 | 🟢 `VERIFIED`（E2） |
| `include/openhbx/media/nand/nand_media_router.h` | 新增 | 07 | `IFlashMediaPort`实现，按`command.channel`分发（无状态，header-only，无对应 .cpp） | CHAN-002/006 | 🟢 `VERIFIED`（E2） |
| `include/openhbx/media/nand/nand_flash_device.h`、`src/media/nand/nand_flash_device.cpp` | 删除 | 07 | 单体facade退役，职责拆入上三组件 | — | `PLANNED` |
| `tests/unit/media/nand/test_*.cpp`、`tests/unit/ras/test_*.cpp` | 新增 | 08 | topology/state/EAT/RAS unit | MEDIA-* | `PLANNED` |
| `tests/component/media/test_nand_device.cpp` | 新增 | 08 | real media+EventKernel | MEDIA-002至007 | `PLANNED` |

表中路径均为从仓库根解析的完整仓库相对路径；以下逐文件小节给出实际文件路径。机器可读状态以反引号文本为准。

### 公共类型（`include/openhbx/media/types.h`）

**职责**：声明公开command、completion和snapshot ID，不暴露状态容器。

**关键组件**：`FlashCommand`、`MediaCompletion`、raw outcome和data-valid。

**输入与输出**：06命令 -> 07 typed completion。

**状态与所有权**：无运行态；Accepted后payload归07。

**依赖与边界**：依赖common types；校验family、地址形状及payload长度。

**对应需求**：MEDIA-002/006；测试`VER-MEDIA-002/006`。

**实现状态**：⚪ `PLANNED`。

### NAND facade（`include/openhbx/media/nand/nand_flash_device.h`、`src/media/nand/nand_flash_device.cpp`）

**职责**：导出Media facade并完成子组件装配和配置校验。

**关键组件**：`try_issue()`、`reset()`、`snapshot()`、`capabilities()`。

**输入与输出**：FlashCommand -> issue result/completion。

**状态与所有权**：持有子组件及公开inflight入口。

**依赖与边界**：依赖02 event port；错误family、无credit或未初始化不得进入热路径。

**对应需求**：MEDIA-001至008；测试`VER-MEDIA-002/007`。

**实现状态**：⚪ `PLANNED`。

> **退役注记（V1.1）**：本单体 facade 在 Channel 分片改造中退役：装配职责上移至 02 `OpenHbxSystem`（构建 `DieDomain` + N 个 `NandChannelSlice` + `NandMediaRouter`），介质引擎职责由 `NandChannelSlice` 承担，Die 级状态由 `DieDomain` 承担。全局 `die_eat_`、类内 `blocked_dies_`/`bbt_`、`planes_*` 死字段与 `struct Reservation` 死代码一并删除。

### Topology（`src/media/nand/topology.h`、`src/media/nand/topology.cpp`）

**职责**：实现immutable geometry和地址解析。

**关键组件**：checked flatten/decode、capacity和Channel ownership validator。

**输入与输出**：PhysicalAddress <-> resource index。

**状态与所有权**：构造后只读，不持payload。

**依赖与边界**：依赖resolved geometry；越界、ownership错误和乘法溢出拒绝。

**对应需求**：MEDIA-001；测试`VER-MEDIA-001`。

**实现状态**：⚪ `PLANNED`。

### State与payload（`src/media/nand/state_store.h`、`src/media/nand/state_store.cpp`、`src/media/nand/page_store.h`、`src/media/nand/page_store.cpp`）

**职责**：管理legality、pending delta与terminal transaction。

**关键组件**：`reserve()`、`commit_terminal()`、`discard_delta()`、`read_snapshot()`。

**输入与输出**：token/address/delta -> committed state或typed failure。

**状态与所有权**：唯一拥有Page/Block/payload；pending与持久态分离。

**依赖与边界**：依赖Topology与BBT；epoch mismatch、duplicate terminal或内存失败不得半提交。

**对应需求**：MEDIA-003/004；测试`VER-MEDIA-003/004`。

**实现状态**：⚪ `PLANNED`。

### Media EAT（`src/media/nand/eat_table.h`、`src/media/nand/eat_table.cpp`）

**职责**：预览并原子预约Bank/Die资源。

**关键组件**：resource preview、atomic reserve和token cancel。

**输入与输出**：resource set+now+duration -> start/end。

**状态与所有权**：唯一拥有NAND array EAT，不拥有06传输EAT。

**依赖与边界**：依赖Topology/TimingProfile；未知资源、溢出或部分预约全部回滚。

**对应需求**：MEDIA-002/007；测试`VER-MEDIA-002/007`。

**实现状态**：⚪ `PLANNED`。

### Command engine（`src/media/nand/command_engine.h`、`src/media/nand/command_engine.cpp`）

**职责**：推进Read/Program/Erase stage FSM。

**关键组件**：context、stage event、generation guard、completion port。

**输入与输出**：accepted command/event -> next event或completion。

**状态与所有权**：拥有contexts、stage和terminal obligation。

**依赖与边界**：依赖EAT、State/Page、Reliability和02；Busy/Rejected零副作用，stale/duplicate不提交。

**对应需求**：MEDIA-002/003/007；测试`VER-MEDIA-002/003/007`。

**实现状态**：⚪ `PLANNED`。

### Timing profile（`src/media/nand/timing_profile.h`、`src/media/nand/timing_profile.cpp`）

**职责**：加载并校验NAND stage时序。

**关键组件**：operation stage、resource mask、单位和source metadata。

**输入与输出**：profile+operation context -> immutable stage list。

**状态与所有权**：仅持只读参数。

**依赖与边界**：依赖00 config/common time；缺stage、零duration或无来源vendor值拒绝。

**对应需求**：MEDIA-002/008；测试`VER-MEDIA-002/008`。

**实现状态**：synthetic为⚪ `PLANNED`；vendor thermal为🔴 `BLOCKED_SPEC`。

### Reliability与BBT（`src/ras/reliability.h`、`src/ras/reliability.cpp`、`src/ras/bbt.h`、`src/ras/bbt.cpp`）

**职责**：产生raw outcome并维护BBT/retirement。

**关键组件**：read counter、retention/disturb、bad bitmap、reason和version。

**输入与输出**：operation context/fault -> raw result及health delta。

**状态与所有权**：07唯一拥有这些介质健康状态。

**依赖与边界**：依赖State/profile/fault timeline，不依赖05 ECC实现；非法stage或重复retire typed处理。

**对应需求**：MEDIA-005/006；测试`VER-MEDIA-005/006`。

**实现状态**：⚪ `PLANNED`。

### Die environment（`src/ras/die_environment.h`、`src/ras/die_environment.cpp`）

**职责**：维护Die环境观测并选择有来源的multiplier。

**关键组件**：temperature band、availability view和environment timeline。

**输入与输出**：environment event -> band/snapshot。

**状态与所有权**：拥有Die温度观察时间线，不拥有06 TSV repair。

**依赖与边界**：依赖02/profile；未知Die、非单调cycle或无来源系数拒绝。

**对应需求**：MEDIA-008；测试`VER-MEDIA-008`。

**实现状态**：🔴 `BLOCKED_SPEC`。MEDIA-008 解除阻断后，Die 温度时间线的归宿为下述 `DieDomain`（ADR-CHAN-001），本文件届时合并。

### DieDomain（`include/openhbx/media/nand/die_domain.h`、`src/media/nand/die_domain.cpp`）

**职责**：Die 级共享物理状态的唯一 owner：blocked_dies、全局 BBT（物理 Block 身份）、全局容量耗尽视图；未来吸收 Die 温度时间线。

**关键组件**：`die_blocked()/set_die_blocked()`、`bad_block_table()`、`capacity_unusable()`（`bbt.size() >= total_blocks`，CHAN-009）。**无 `reset()`**：BBT 与 blocked dies 跨系统 reset 保持不变，与旧单体设备行为一致（物理坏块身份/注入阻塞不是运行时命令状态，CHAN-005）。

**输入与输出**：slice 的只读查询；写操作（BBT 标记、die 阻塞注入）仅由 RAS/测试经 02 持有的唯一实例进行，系统 reset 不触碰本对象。

**状态与所有权**：跨全部 channel 共享；slice 经引用持有，只读查询，不得回写。

**依赖与边界**：依赖 `MediaTopology` 与 `ras::BadBlockTable`；自身不产生事件，不操作 EventQueue。

**对应需求**：CHAN-005/009；测试 CHAN-T05/09。

**实现状态**：🟢 `VERIFIED`（E2，2026-09-20，`ctest` 23/23）。`openhbx_channel_partition_test` 断言跨 channel 共享 BBT 视图与全局 `CapacityUnusable`；`openhbx_media_component_test` 断言 `set_die_blocked` 导致的 `DieTemporarilyBlocked` 终态。Die 温度时间线仍为 🔴 `BLOCKED_SPEC`（MEDIA-008）。

### NandChannelSlice（`include/openhbx/media/nand/nand_channel_slice.h`、`src/media/nand/nand_channel_slice.cpp`）

**职责**：单个 Host Channel 拥有的 Bank 切片的介质引擎：stage FSM、Bank array EAT、`ChannelMediaPath(channel, core_die)` EAT、自有 Page/Block/payload/erase 计数。

**关键组件**：`try_issue()`、`reset()`、`snapshot()`、`page_is_valid()`、`block_erase_count()`；`uses_channel_path()` 取代旧 `uses_die()`（`ReadCommand`/`ProgramData`/`EraseSetup` 预约 slice 私有 path EAT，CHAN-004）。

**输入与输出**：router 分发的 `FlashCommand`（`channel` 字段须等于本 slice）-> issue result/completion。

**状态与所有权**：`contexts_`/`pages_`/`erase_counts_`/`bank_eat_`/`channel_media_path_eat_` 仅含 `owned_banks` 条目；`owned_banks` 由 02 在装配期从 `ChannelTopology::banks_by_channel` 展开注入（ownership 唯一来源仍是 04，ADR-CHAN-004）。validate 首查 ownership 成员资格，违例 typed reject 零副作用（CHAN-006）。

**依赖与边界**：持有 `DieDomain&`（只读）、`EventQueue&`、独立 `HandlerId`（`kMediaSliceHandlerBase + channel`）与 slice 本地 generation；reset 调用 `EventQueue::discard_generation(handler, generation)` 作用域版本，不波及其他 slice（CHAN-007）。`max_inflight` 为每 slice 上限（CHAN-010）。

**对应需求**：CHAN-003/004/006/010/012；测试 CHAN-T03/04/06。

**实现状态**：🟢 `VERIFIED`（E2，2026-09-20，`ctest` 23/23）。`openhbx_channel_partition_test` 断言 ownership 拒绝零副作用、同 core die 双 channel 完成周期一致（证明全局 die 总线已删除）、reset 隔离与每 slice 守恒；`openhbx_media_flash_semantics_test` 在新装配下保持 stage/commit/backpressure 断言。未覆盖：CHAN-010 的「全局 = Σ slice」语义与 CHAN-012 的汇总一致性未断言。**统计口径**：`snapshot()` 中 `inflight`/`queued_events`/`committed_pages`/`stale_events` 为 slice 私有、可跨 channel 求和；`bad_blocks` 取自共享 `DieDomain` 全局 BBT，每 slice 读数相同，求和会把 stack 级计数放大 N 倍，须从单一 slice 或 `DieDomain` 直接读取。

### NandMediaRouter（`include/openhbx/media/nand/nand_media_router.h`，header-only）

**职责**：实现 `pal::IFlashMediaPort`，按 `command.channel` 分发到对应 slice；channel 越界返回 `Rejected(InvalidArgument)`。

**关键组件**：`try_issue_media()`；无动态运行态，持有按 channel 值索引的 slice 指针数组（非 owning）。

**输入与输出**：06 `FlashCommand` -> slice issue result。

**状态与所有权**：无自有介质状态；不复制 04 ownership 表。

**依赖与边界**：依赖 slice 公开接口；装配由 02 完成。

**对应需求**：CHAN-002/006；测试 CHAN-T02/06。

**实现状态**：🟢 `VERIFIED`（E2，2026-09-20，`ctest` 23/23）。`openhbx_channel_partition_test` 断言 channel 越界 `Rejected(InvalidArgument)` 与按 channel 正确分发。端到端 channel 保真（CHAN-002）目前仅由 S8 系统测试与 16 channel 性能测试隐式覆盖，无专用 oracle。

### 07测试文件（`tests/unit/media/nand/test_*.cpp`、`tests/unit/ras/test_*.cpp`、`tests/component/media/test_media_component.cpp`、`tests/component/media/test_channel_partition.cpp`）

**测试域**：unit按Topology、State/Page、EAT、Timing、Reliability/BBT拆fixture；component使用真实 `DieDomain`+`NandChannelSlice`（+`NandMediaRouter`）与 EventKernel，06边界以受控port driver替代。`test_media_component.cpp`/`test_flash_semantics.cpp` 以单 channel 装配（一个 slice 拥有全部 Bank）覆盖既有 MEDIA 语义；`test_channel_partition.cpp` 以双 channel 装配覆盖 CHAN 系列分片不变量。**输入/oracle**：非对称geometry、完整state矩阵、fixed seed faults、stage reset、错误归属命令；检查手算cycle、completion-time commit、payload、BBT、跨 slice 隔离及守恒。**证据等级**：unit E1，component E2；05->06->07真实路径的E3由08承担。**状态**：🟢 `VERIFIED`（E1/E2 范围，2026-09-20）。

每个source实现对应header契约；不得引入NVMe、传统L2P/GC、TSV映射或Host status。

## 8. 配置、错误、统计与Debug

配置包含geometry、`payload_mode`、inflight depth、各operation stage duration/resource mask、cell/page layout、factory bad manifest、seed、retry/refresh阈值、retention/disturb与temperature bands。非法单位、缺stage、零duration、容量溢出或vendor参数无来源在启动前拒绝。

错误分为config、admission、raw media和integrity四类；Host映射不在07。统计在accept、stage terminal、state commit和completion处分别递增，避免bytes重复。Debug snapshot含contexts、EAT、Page/Block摘要、BBT version、fault rule hits及generation。

### 8.1 `GAP-010`实现约束

当前实现只有外部调用`bad_block_table().mark()`后拒绝目标Block的路径，`ReliabilityModel::fails()`与BBT没有提交关系。后续必须区分可重试操作失败与确认坏块：只有达到profile规定的确认条件后，才在verify completion阶段提交`BadBlockReason::ProgramFailure`或`EraseFailure`并递增BBT version；重复故障不得重复递增。factory manifest应在首个Host admission前按flattened physical Block ID装载，越界、重复或坏块数量超过可用容量必须结构化拒绝配置。

BBT提交后通过窄typed事件通知04，事件不得暴露07内部容器。04完成视图切换前，新请求必须被有界阻塞或返回typed状态；无替代容量时发布`CapacityUnusable`。测试需证明失败前后Page/Block状态、BBT reason/version、mapping epoch、replay目标和terminal结果一致，且不存在旧坏块上的后续介质issue。

## 9. 测试用例设计

| Test ID | Fixture/输入 | Fault/Timing | Oracle | 证据 |
|---|---|---|---|---|
| VER-MEDIA-001 | tiny非对称geometry | 边界/overflow | flatten round-trip与容量 | E1 |
| VER-MEDIA-002 | real engine/EventKernel | 同/异Bank | stage cycle精确、资源EAT | E2 |
| VER-MEDIA-003 | SparsePayload | verify前后read | completion前旧值，成功后新值 | E2 |
| VER-MEDIA-004 | Page/Block状态矩阵 | duplicate/nonsequence/bad | typed reject且零副作用 | E2 |
| VER-MEDIA-005 | BBT manifest | runtime failure | version/reason/容量视图 | E3 |
| VER-MEDIA-006 | fixed seed fault rules | CECC/UECC/retry/notice | raw result与命中次数 | E3 |
| VER-MEDIA-007 | 各stage reset | same-cycle边界 | stale不提交且exactly once | E3 |
| VER-MEDIA-008 | sourced vendor profile | temperature bands | golden结果可重放 | E4 |

### 2026-09-16 实验设计与首个新增实验

新增 `openhbx_media_flash_semantics_test`（`VER-MEDIA-003`、`VER-MEDIA-004`，E1），位置为
`tests/unit/media/nand/test_flash_semantics.cpp`。该测试使用最小非对称 geometry 和固定
payload，覆盖：Program completion 的最早 cycle（提前 dispatch 不得完成）、Program 后
重复 Program 的拒绝及零副作用、Erase 仅清除目标 block、以及 erased-page Read 的
`ReadErasedPage`/`data_valid=false` 语义。测试通过 `tests/unit/media/nand/CMakeLists.txt`
注册并由 CTest label `openhbx_s3;openhbx_media` 执行；正式 artifact 应写入
`build/artifacts/stages/s3/EXP-S3-NAND-MEDIA/<UTC-run>/`，manifest 记录 Conda 环境、配置、
seed=`not-applicable`、固定 workload、cycle budget 和 replay 命令。该实验只证明 NAND
组件局部语义，不升级 E3/E4 或完整 Flash pipeline 状态。

正式执行与artifact遵循[`实验与测试执行规范.md`](实验与测试执行规范.md)。TimingOnly不能用于逐byte数据正确性证据。

## 10. 实施顺序、ADR与完成定义

顺序：types/topology -> state/page -> timing/EAT -> engine -> BBT/reliability -> environment -> 06接线 -> E3。

| ADR ID | 决策 | 背景 | 备选 | 理由 | 后果 | 状态 |
|---|---|---|---|---|---|---|
| ADR-07-001 | completion-time atomic commit | non-posted持久语义 | issue即写 | 避免未来状态可见 | 需要pending delta | `PLANNED` |
| ADR-07-002 | raw reliability与ECC分离 | ECC与Host status属于05策略 | 07直接Host status | owner明确、可换模型 | 多一层映射 | `PLANNED` |
| ADR-07-003 | synthetic/profile参数必须标来源 | OCP无NAND内部时序 | 隐式默认 | 客观可核查 | 配置更严格 | `PLANNED` |

完成定义：所有文件/CMake接入；MEDIA-001至007达到相应证据且真实pipeline drain；payload、state、token和event守恒；MEDIA-008仅在公开/vendor profile与E4 golden齐备后解除`BLOCKED_SPEC`。


> **源码同步（2026-09-15）**：当前仓库已落地对应 `include/openhbx/...` 公开接口及 `src/...` 生产实现；文档中的测试路径和验证状态仍以实际 CMake/CTest artifact 为准。仅有源码不构成 `VERIFIED` 证据。
