# OpenHBX Base Die Flash Controller 概要设计

**文档版本**：V1.0
**文档位置**：`docs/HLD_05_BASE_DIE_FLASH_CONTROLLER.md`
**对应文档**：`docs/LLD_05_BASE_DIE_FLASH_CONTROLLER.md`
**设计状态**：🔵 `IMPLEMENTED`（`status: IMPLEMENTED`；尚无本轮合规验证证据）；外部参数🔴 `BLOCKED_SPEC`（`dependency_status: BLOCKED_SPEC`）
**适用基线**：OCP HBF v0.7.0；OpenHBX 首期 `OCP_HBF_0_7`

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 重建Base Die控制器；移出地址映射与TSV owner |

## 1. 模块概述

本模块把03 Host transaction和04 typed address转换为HBF控制动作，拥有DLU accumulator、read/batch scheduler、每Bank cache协调、ECC pipeline、Scratchpad、Admin/lifecycle queue、Controller token/credit与completion。它通过06 PAL访问互联和NAND。

不拥有：Host ordering/response（03）、R1--R5/BlockSequence/ZoneMap状态（04）、TSV lane/EAT/repair（06）、NAND Page/Block/payload及Bank EAT（07）。控制器旁路查询/预约04，只消费06返回的PAL completion，不得复制04、06或07的权威状态。

关键不变量：完整4 KiB前不Program；write non-posted且介质commit后才成功；重复sector报Overlap；pending上限和timeout可判定；read命中accumulator已收sector时forward，缺失sector报0xA；同Bank sense严格有序；每Bank至少两个4 KiB cache buffer；Accepted精确一次terminal；PAL Busy不泄漏credit。

## 2. 需求回顾与追踪

| 需求ID | 需求描述 | 优先级 | 版本 | 实现状态 |
|---|---|---|---|---|
| CTRL-001 | 64个sector聚合为4 KiB DLU并从首sector计时 | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| CTRL-002 | overlap、上限、timeout映射0x2/0x4/0x5 | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| CTRL-003 | 仅完整且04许可的DLU向PAL Program | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| CTRL-004 | media commit后才产生write terminal | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| CTRL-005 | 实现accumulator forwarding与0xA | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| CTRL-006 | 同Bank sense有序且Regular结束Batch | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| CTRL-007 | 每Bank至少两个4 KiB cache buffer | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| CTRL-008 | 完整映射Read/Write status、ErrorInfo及data-valid | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| CTRL-009 | Admin、Scratchpad、lifecycle/reset有界执行 | P1 | V1.0 | 🔵 `IMPLEMENTED` |
| CTRL-010 | 介质操作只经06 PAL且05不拥有TSV | P0 | V1.0 | 🔵 `IMPLEMENTED` |
| CTRL-011 | vendor参数必须有来源或synthetic标签 | P1 | V1.0 | 🔴 `BLOCKED_SPEC` |

| 需求ID | 来源/证据类别 | owner组件 | Test ID |
|---|---|---|---|
| CTRL-001 | OCP摘要第4节；规范事实 | `DluAccumulator` | CTRL-T01 |
| CTRL-002 | OCP表13；规范事实 | `DluAccumulator` | CTRL-T02 |
| CTRL-003 | OCP摘要第4、6节；规范事实 | `BaseDieFlashController` | CTRL-T03 |
| CTRL-004 | OCP non-posted write；规范事实 | `BaseDieFlashController` | CTRL-T04 |
| CTRL-005 | OCP mixed workload；规范事实 | `DluAccumulator` | CTRL-T05 |
| CTRL-006 | OCP摘要第4节；规范事实 | `FlashScheduler` | CTRL-T06 |
| CTRL-007 | OCP摘要第4节/BUCCAP.NCBB；规范事实 | `BankCacheManager` | CTRL-T07 |
| CTRL-008 | OCP表12；规范事实 | `EccPipeline` | CTRL-T08 |
| CTRL-009 | OCP表9及摘要第2、3、9节；规范事实 | `ControlPlane` | CTRL-T09 |
| CTRL-010 | ADR-CTRL-001；设计选择 | `FlashPalPort` | CTRL-T10 |
| CTRL-011 | OCP product-specific字段；外部依赖 | profile validator | CTRL-T11 |

## 3. 系统架构设计

```text
03 Host -> 05 BaseDieFlashController
             +-> DluAccumulator -> Scheduler
             +<-> 04 Address/BlockSequence（旁路查询/预约）
             +-> Read/Batch -> CacheManager -> ECC
             +-> Scratchpad / Admin / Lifecycle
             +-> FlashPalPort --------------------> 06 PAL -> 07 NAND
             <- typed completion -----------------+
          -> 03 Host response completion
```

| 组件 | 主要文件 | 职责 | 拥有状态 | 依赖 |
|---|---|---|---|---|
| `BaseDieFlashController` | `base_die_flash_controller.{h,cpp}` | admission/route/completion | queues/token table/credits | 03/04/06 |
| `DluAccumulator` | `dlu_accumulator.{h,cpp}` | sector聚合/timeout/forward | pending DLU/payload | kernel |
| `FlashScheduler` | `flash_scheduler.{h,cpp}` | Regular/Batch/Program/Admin仲裁 | per-bank queues/age | PAL capability |
| `BankCacheManager` | `bank_cache_manager.{h,cpp}` | buffer reservation与命中 | cache metadata/data refs | scheduler |
| `EccPipeline` | `ecc_pipeline.{h,cpp}` | encode/decode/status | bounded stages | RAS profile |
| `ControlPlane` | `control_plane.{h,cpp}` | Scratchpad/Admin/lifecycle | memory/admin queue/state | 03 register、04 admin |

### 3.1 BaseDieFlashController

**文件路径**：`include/openhbx/hbf/controller/base_die_flash_controller.h`、`src/hbf/controller/base_die_flash_controller.cpp`

**职责**：协调admission、路由、completion和terminal。  
**关键组件**：Facade、request record、credit、completion router。  
**输入输出**：03 transaction -> 06 PAL command -> 03 completion；旁路查询/预约04。  
**状态所有权**：05拥有queue、token和credit。  
**依赖边界**：依赖02及03/04/06公开端口；不拥有TSV/NAND状态。  
**对应需求**：CTRL-003/004/010。  
**实现状态**：🔵 `IMPLEMENTED`（`status: IMPLEMENTED`）。

### 3.2 DluAccumulator

**文件路径**：`include/openhbx/hbf/controller/dlu_accumulator.h`、`src/hbf/controller/dlu_accumulator.cpp`

**职责**：聚合sector、计时并提供read forwarding。  
**关键组件**：`PendingDlu`、64-bit mask、deadline。  
**输入输出**：64 B sector/token/cycle -> accepted/full/error/forward。  
**状态所有权**：05从首sector至terminal拥有4 KiB buffer。  
**依赖边界**：依赖02事件和payload store，不判断04顺序。  
**对应需求**：CTRL-001/002/005。  
**实现状态**：🔵 `IMPLEMENTED`（`status: IMPLEMENTED`）。

### 3.3 FlashScheduler

**文件路径**：`include/openhbx/hbf/controller/flash_scheduler.h`、`src/hbf/controller/flash_scheduler.cpp`

**职责**：执行per-Bank确定性仲裁和Batch边界处理。  
**关键组件**：work queue、age、stable priority。  
**输入输出**：FlashWork/resources -> selected work/stall。  
**状态所有权**：05拥有work queue。  
**依赖边界**：依赖cache credit和06 capability，不读取PAL EAT。  
**对应需求**：CTRL-006。  
**实现状态**：🔵 `IMPLEMENTED`（`status: IMPLEMENTED`）。

### 3.4 BankCacheManager

**文件路径**：`include/openhbx/hbf/controller/bank_cache_manager.h`、`src/hbf/controller/bank_cache_manager.cpp`

**职责**：管理每Bank至少两个cache buffer。  
**关键组件**：`CacheBuffer`、reserve/fill/lookup/release。  
**输入输出**：Bank/DLU/completion -> handle/hit/stall。  
**状态所有权**：05拥有cache metadata和缓存payload引用。  
**依赖边界**：依赖04 typed address和payload store；不拥有NAND Page。  
**对应需求**：CTRL-007。  
**实现状态**：🔵 `IMPLEMENTED`（`status: IMPLEMENTED`）。

### 3.5 EccPipeline

**文件路径**：`include/openhbx/hbf/controller/ecc_pipeline.h`、`src/hbf/controller/ecc_pipeline.cpp`

**职责**：执行bounded ECC encode/decode并形成typed结果。  
**关键组件**：ECC stages、`EccResult`。  
**输入输出**：payload/reliability result -> encoded payload或OK/CECC/UECC/retry/erased/capacity-unusable/die-blocked。  
**状态所有权**：05拥有stage credit。  
**依赖边界**：依赖02和来源化RAS profile，不拥有raw NAND状态。  
**对应需求**：CTRL-008/011。  
**实现状态**：🔵 `IMPLEMENTED`；外部参数🔴 `BLOCKED_SPEC`。

### 3.6 ControlPlane

**文件路径**：`include/openhbx/hbf/controller/control_plane.h`、`src/hbf/controller/control_plane.cpp`

**职责**：协调Scratchpad、Admin和lifecycle/reset。  
**关键组件**：volatile memory、Admin queue、reset coordinator。  
**输入输出**：03 intent -> 04旁路规则命令、06 PAL命令、typed completion。  
**状态所有权**：05拥有易失memory/admin状态。  
**依赖边界**：依赖03 register、04 Admin端口和06 PAL。  
**对应需求**：CTRL-009/011。  
**实现状态**：🔵 `IMPLEMENTED`；vendor行为🔴 `BLOCKED_SPEC`。

## 4. 高层详细设计

### 4.1 Write

每DLU保存64-bit mask、4 KiB payload、关联Host token和deadline。新sector原子检查overlap与pending credit；完整后冻结payload，向04申请顺序reservation，必要时先发auto-erase，再经ECC encode和scheduler向PAL Program。只有PAL回报media commit成功后，04 complete reservation并逐个终结Host command；失败进入replay并返回Program Fail。

### 4.2 Read与cache

读取先查accumulator：已收sector直接forward，pending但缺失返回0xA。否则按Regular/Batch进入scheduler；Regular关闭当前batch。cache manager至少配置双buffer，PAL read完成后映射OK、CECC、UECC/refresh、retry、Read Erased Page、Capacity Unusable和Die Temporarily Blocked，payload有效性随typed result传播。Die处于恢复/blocked状态时新请求立即返回0x9，不能在scheduler中无限等待。

### 4.3 Admin、Scratchpad和lifecycle

Scratchpad为可选易失64 B存储并拥有高优先级端口；Admin使用表9 typed opcode，Zone Remap/Reduced Capacity委托04，介质相关命令经PAL。reset由generation隔离旧事件，清理易失accumulator/cache/scratchpad（按reset domain），保留规范要求sticky状态。

## 5. 接口设计

| 接口 | 方向 | 输入/输出 | 所有权/backpressure | 时序 |
|---|---|---|---|---|
| `submit(DecodedHostTxn)` | 03 -> 05 | txn -> admission | Accepted才转移payload；Busy零副作用 | event phase |
| `map/reserve/complete` | 05 <-> 04 | address/reservation/result | 04拥有cursor/map | 同步reserve，异步complete |
| `issue(FlashPalCommand)` | 05 -> 06 | typed command/token/payload | PAL Accepted才转移command ref | 异步typed completion |
| `on_pal_completion` | 06 -> 05 | token/status/data-valid | generation校验，exactly once | completion phase |
| `complete(HostCompletion)` | 05 -> 03 | token/OCP status/payload | 03拥有response ordering | terminal |

## 6. 数据结构设计

| 结构 | owner | 生命周期 | 不变量 |
|---|---|---|---|
| `PendingDlu` | accumulator | first sector至terminal | mask与payload sector一致、deadline固定 |
| `ControllerRequest` | controller | Accepted至terminal | Host token唯一 |
| `CacheBuffer` | cache manager | reserve至release | bank固定、至少2/bank |
| `FlashPalCommand` | 05至06值对象 | issue至completion | address来自04、command typed |
| `ControllerCompletion` | 05至03值对象 | terminal route | status/data-valid一致 |

## 7. 关键流程与状态机

```text
write sector -> accumulate -> full -> reserve order -> [erase] -> ECC -> PAL Program
 -> completion success -> commit sequence -> Host responses
 -> failure -> replay gate -> Host error
```

| 状态 | 事件/Guard | 动作 | 下一状态 | 失败处理 |
|---|---|---|---|---|
| `Collecting` | sector unique/not full | 写sector/mask | `Collecting/Full` | overlap终结该命令 |
| `Collecting` | deadline | 终结关联命令/回收 | `Terminal` | status 0x5 |
| `Full` | 04 reservation | 冻结并排队 | `Ready` | order/replay status |
| `Ready` | PAL Accepted | 转移backend credit | `InFlight` | Busy保留队首 |
| `InFlight` | completion success | sequence commit/response | `Terminal` | stale仅回收 |
| `InFlight` | failure | 04 replay gate/错误响应 | `Terminal` | payload无效 |

## 8. 性能与资源设计

accumulator内存约`max_pending_dlu * (4096 + metadata)`；cache最小`banks * 2 * 4096`；queue、ECC stage和PAL credits均有界。并行度受Host Channel、Bank cache、scheduler和PAL资源共同限制；TSV带宽/EAT由06建模，05不得重复计算。timeout、ECC latency及策略必须由带来源profile提供；synthetic值进入resolved config/artifact。

资源预算按 OCP per-channel 子系统对齐（ADR-CHAN-009，2026-10-15）：DLU 聚合器 pending 与 ECC decode credits 按 channel 计费，scheduler 队列维持 per-Bank（Bank 唯一归属 channel）；单控制器实例布局不变（ADR-CHAN-008），但任一 channel 的预算耗尽或 PAL Busy 不影响其他 channel 的准入与排空（CHAN-I7，测试 CHAN-T13）。

## 9. 错误、恢复与可观测性

错误在owner处生成并于唯一映射点转为OCP表12/13 status。CECC保持data-valid，UECC/program fail无有效数据；retry携带stage；erased page、capacity unusable和die blocked保持独立typed结果。trace记录token/DLU/mask/deadline/reservation/PAL token/cache/ECC/status；counter满足Host accepted守恒、PAL issued守恒、pending DLU不超限。reset递增generation、取消future commit并精确回收Host/PAL/cache credit；duplicate/stale completion不二次提交。

## 10. 测试与验收

| Test ID | 层级 | production path与oracle | 证据 |
|---|---|---|---|
| CTRL-T01/T02 | E1 | 真实accumulator；mask、deadline、overlap、上限及零副作用 | E1 |
| CTRL-T03/T04 | E2/E3 | 03->05、05查询04后经06 PAL到07；不完整不issue、Media commit前无成功response | E3 |
| CTRL-T05/T06/T07 | E2 | accumulator/cache/scheduler；forward、0xA、batch终止、双buffer | E2 |
| CTRL-T08 | E2 | 完整表12/13状态映射；data-valid/ErrorInfo及0x9立即完成精确 | E2 |
| CTRL-T09/T10 | E2 | Admin/lifecycle/PAL公开端口；无TSV内部访问 | E2 |
| CTRL-T11 | E1 | 缺失来源参数启动拒绝或明确synthetic标签 | BLOCKED_SPEC/ E1 |

实验执行、证据保存和状态更新统一遵循
[`实验与测试执行规范.md`](实验与测试执行规范.md)。
