# OpenHBX HBF Channel 分片建模架构设计

**文档版本**：V1.1
**文档位置**：`docs/HBF_CHANNEL_PARTITIONED_MODEL.md`
**关联文档**：`docs/HBF_CHANNEL_CORE_DIE_TOPOLOGY.md`（物理拓扑冻结结论）、`docs/LLD_07_NAND_MEDIA_RAS.md`（07 逐文件设计权威来源）、`docs/TRACEABILITY_MATRIX.md`
**设计状态**：🔵 `IMPLEMENTED`（Phase 1-4 已落地；07 范围的分片不变量为 🟢 `VERIFIED`(E2)，逐项状态见 §4 表与 `TRACEABILITY_MATRIX.md` §9）
**适用基线**：OCP HBF v0.7.0；OpenHBX 首期 `OCP_HBF_0_7`

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-09-20 | 首版：介质层按 Host Channel 分片，channel 全链路显式化 |
| V1.1 | 2026-09-20 | 实施完成后回填：Phase 1-4 全部落地，`ctest` 23/23；§4/§6 状态列按实际证据更新；§5.3 `DieDomain` 骨架对齐实现（无 `reset()`，构造参数为 `total_blocks`）；§7 标注已实现测试与未覆盖项；性能基线按 ADR-CHAN-002 重估（1.888→1.892 TB/s） |
| V1.2 | 2026-10-15 | 前端 per-channel 对齐（ADR-CHAN-009）：Fabric `queue_depth` 按路由资源计费、PAL `max_inflight` / DLU `max_pending` / ECC credits 按 channel 计费、`drive_scheduler` PAL Busy 延迟重排消除跨 channel 队头阻塞；新增 CHAN-I7 与 CHAN-T13/T14；`OpenHbxSystem` 默认 ramulator 风格控制台日志 + `OPENHBX_LOG_LEVEL` |

## 1. 背景与动机

### 1.1 现状问题

当前实现把全部 Host Channel 的 NAND 状态构建在**单一** `NandFlashDevice` 对象内，依靠地址区间切分使各 Channel 请求互不命中同一 Bank。这在功能上可行，但在物理 faithful 程度上存在三个确定性偏差：

1. **全局 Die 总线瓶颈（无规范依据）**。`src/media/nand/nand_flash_device.cpp` 的 `uses_die()`（:91-93）让 `ReadCommand`/`ProgramData`/`EraseSetup` 三个 stage 预约 `die_eat_`——一张以 flat die id 为键的 **Die 级共享 EAT**。效果：同一 Die 上不同 Channel 的命令被串行化，16 Channel 的介质带宽被人为耦合。`HBF_CHANNEL_CORE_DIE_TOPOLOGY.md` §4 已冻结：期望资源为 `BankArray(core,die,bank)` + `ChannelMediaPath(channel,core)`，**不得引入无 vendor profile 依据的全局 die bus**。
2. **channel 在 PAL→Media 边界丢失**。`pal::FlashPhysicalRequest`（`include/openhbx/pal/flash_pal.h:17`）只携带 `endpoint = channel*axi + axi`，且仅在 `fabric_.route_for(endpoint)` 使用；`FlashPal::issue_media`（`src/pal/flash_pal.cpp:130-133`）丢弃 endpoint 后，`media::FlashCommand`（`include/openhbx/media/types.h`）**完全没有 channel 字段**。介质层无法区分、无法按 channel 统计、无法结构性地拒绝跨 channel 访问。
3. **单体对象阻碍按 channel 演进**。HandlerId 常量 `kMediaHandler{101}`（`src/system/open_hbx_system.cpp:50-52`）只允许一个介质实例；`EventQueue::discard_generation` 为队列范围操作，N 个实例各自的 generation 计数会互相碰撞；`CapacityUnusable` 判定需要全局 BBT 视图，简单复制 N 份设备会得到 N 份独立 BBT。

### 1.2 目标

按 Ramulator 的 channel 分离范式重构：**请求在入口完成唯一一次 flat→channel 解码，之后 channel 以强类型显式贯穿 02→03→04→05→06→07；介质层由一个共享 `DieDomain`（Die 级物理状态）加每 Channel 一个 `NandChannelSlice`（该 Channel 拥有的 Bank 切片）组成；跨 channel 访问在类型与 ownership 校验上结构性不可能。**

## 2. 物理依据（冻结，不再讨论）

依据 `HBF_CHANNEL_CORE_DIE_TOPOLOGY.md`：

- OCP HBF v0.7.0：最多 16 个 Host Channel，Channel 是**同一物理 NAND stack 的互斥容量切片**，不是物理父级；
- 容量 = NCDU × dies × banks × blocks × pages，**不乘 channel 数**；
- synthetic ownership：`owner_channel = physical_bank % channel_count`；命令合法当且仅当 `command.channel == owner_channel(physical_bank)`；
- 介质资源 = `BankArray(core,die,bank)` + `ChannelMediaPath(channel,core)`；
- BBT 以物理 Block 为单位、Die 温度/故障/恢复为 Die 级——**均不属单个 channel**。

由此得出分片边界：**Bank 及以下（Page/Block/payload/Bank EAT）按 channel 切分；Die 级状态（blocked、BBT、未来温度）保持共享。**

## 3. 目标架构

```text
Ramulator / Trace / Synthetic (flat byte address)
  -> 02 RequestBridge            【唯一 flat->(channel, axi, local) 解码，已存在】
  -> 03 HostIngress.channel      【已显式】
  -> 05 ControllerRequest        【HbfAddress 含 channel，已显式】
  -> 06 FlashPhysicalRequest     【+ channel 字段（本次新增）】
  -> 07 FlashCommand             【+ channel 字段（本次新增）】
       |
       v
  NandMediaRouter (IFlashMediaPort)      —— 07 新组件，按 command.channel 分发
       |-- channel 0 -> NandChannelSlice#0 --+
       |-- channel 1 -> NandChannelSlice#1 --+--> 共享 DieDomain&
       |-- ...                               |     (blocked_dies / BBT / 未来温度)
       `-- channel N-1 -> NandChannelSlice#N-1 -+

  OpenHbxSystem: 1 个实例、1 个 EventQueue、8 phase tick 不变。
```

每个 `NandChannelSlice` 只持有 `ChannelTopology` 判定归该 channel 的 Bank 集合（`banks_by_channel[c]`），其 `pages_`/`erase_counts_`/`bank_eat_`/`channel_media_path_eat_` 均只含自有 Bank 条目；`ChannelMediaPath` EAT 以 `core_die` 为键、为 slice 私有——不同 channel 不再共享任何介质 EAT。

## 4. 需求定义与追踪

| 需求ID | 需求描述 | 优先级 | owner 组件 | Test ID | 状态 |
|---|---|---|---|---|---|
| CHAN-001 | flat→(channel, axi, local) 解码在 02 RequestBridge 唯一发生；下游禁止从裸地址重推 channel（`debug_address_snapshot` 的第二处解码改为复用同一解码helper） | P0 | `RequestBridge` | CHAN-T01 | 🔵 `IMPLEMENTED` |
| CHAN-002 | channel 以强类型显式贯穿 Bridge/Host/Controller/PAL/Media 全链路：`FlashPhysicalRequest.channel`、`FlashCommand.channel` | P0 | 06/07 端口类型 | CHAN-T02 | 🔵 `IMPLEMENTED` |
| CHAN-003 | 介质按 channel 分片：`NandChannelSlice` 仅拥有 owner Bank 的 Page/Block/payload/Bank EAT；不同 slice 状态互不包含 | P0 | `NandChannelSlice` | CHAN-T03 | 🟢 `VERIFIED`（E2） |
| CHAN-004 | 全局 `die_eat_` 总线删除，代之以 slice 私有 `ChannelMediaPath(channel, core_die)` EAT；跨 channel 介质路径零共享 | P0 | `NandChannelSlice` | CHAN-T04 | 🟢 `VERIFIED`（E2） |
| CHAN-005 | Die 级共享物理状态集中于 `DieDomain`：blocked_dies、全局 BBT（物理 Block 身份）、未来温度时间线；无 reset 方法，状态跨系统 reset 保持（行为保持） | P0 | `DieDomain` | CHAN-T05 | 🟢 `VERIFIED`（E2） |
| CHAN-006 | ownership 硬不变量：`command.channel == owner_channel(physical_bank)`；slice 对非自有 Bank typed reject 且零副作用 | P0 | `NandMediaRouter`+`NandChannelSlice` | CHAN-T06 | 🟢 `VERIFIED`（E2） |
| CHAN-007 | `EventQueue` 新增按 `(HandlerId, Generation)` 作用域的 discard；slice 间 reset 互不影响 | P0 | `EventQueue` | CHAN-T07 | 🟢 `VERIFIED`（E2） |
| CHAN-008 | HandlerId 分段分配，N 个 slice 处理器 id 唯一且有预留区间 | P0 | `openhbx/system/handler_ids.h` | CHAN-T08 | 🔵 `IMPLEMENTED` |
| CHAN-009 | `CapacityUnusable` 判定基于 `DieDomain` 全局 BBT 视图（`bbt.size() >= total_blocks`），而非单 slice 视图 | P0 | `DieDomain` | CHAN-T09 | 🟢 `VERIFIED`（E2） |
| CHAN-010 | `media_max_inflight` 语义明确为**每 slice** 上限；全局 in-flight = Σ slice | P0 | 01 config 文档化 | CHAN-T10 | 🔵 `IMPLEMENTED` |
| CHAN-011 | 确定性：同 seed/config/timeline 结果确定；排序键沿用 `(due, phase, sequence)`，跨 slice 无共享可变状态（DieDomain 只读查询 + 02 协调写） | P0 | 全链路 | CHAN-T11 | 🟢 `VERIFIED`（07 范围 E2；系统级仍以 `VER-005` 为准） |
| CHAN-012 | 介质统计可按 channel 分解（每 slice 独立计数，snapshot 汇总） | P1 | `NandChannelSlice` | CHAN-T12 | 🔵 `IMPLEMENTED` |

状态口径（`设计文档编写规范.md` §状态定义）：🔵 表示生产代码完成但缺少专用 oracle；🟢 表示已有满足 E1-E4 的自动测试。证据、运行日期与未覆盖范围以 [`TRACEABILITY_MATRIX.md`](TRACEABILITY_MATRIX.md) §9 的证据记录为唯一权威来源。

## 5. 类型与接口变更（代码骨架）

### 5.1 channel 强类型上移 common（跨模块最小权威模块）

`ChannelId` 被 02/03/04/05/06/07 共同使用，按 `OpenHBF-文件安排.md` §7「跨模块公共类型放在最小权威模块」上移至 `openhbx/common/strong_types.h`：

```cpp
// include/openhbx/common/strong_types.h （新增）
struct ChannelIdTag;
using ChannelId = StrongId<ChannelIdTag>;
```

`include/openhbx/hbf/address/hbf_address_types.h` 中删除自有 `ChannelIdTag`/`ChannelId` 定义，改为别名以保持源码兼容（`AddressValue` 与 `StrongId` 公开接口逐函数一致，别名无行为差异）：

```cpp
// include/openhbx/hbf/address/hbf_address_types.h （修改）
#include "openhbx/common/strong_types.h"
namespace openhbx::hbf::address {
using ChannelId = openhbx::ChannelId;   // 原 AddressValue<ChannelIdTag> 删除
}
```

### 5.2 全链路 channel 字段

```cpp
// include/openhbx/media/types.h —— FlashCommand 新增 channel
struct FlashCommand {
  Token token;
  Generation generation;
  CommandKind kind{CommandKind::Read};
  ChannelId channel{};            // 新增：命令归属的 Host Channel
  PhysicalAddress address;
  PayloadHandle payload;
};

// include/openhbx/pal/flash_pal.h —— FlashPhysicalRequest 新增 channel
struct FlashPhysicalRequest {
  Token token;
  Generation generation;
  media::CommandKind kind{media::CommandKind::Read};
  ChannelId channel{};            // 新增：05 从 HbfAddress 直接携带
  media::PhysicalAddress address;
  std::uint64_t endpoint{0};      // 保留：仅 fabric route 使用
  PayloadHandle payload;
};
```

`FlashPal::issue_media` 构造 `FlashCommand` 时透传 `request.channel`，不再丢弃 channel 维度。`integration::BridgeRequest` **不**新增 channel 字段——它承载的是 Ramulator ABI 的 flat 地址，解码是其下游 `RequestBridge::try_submit` 的职责（CHAN-001）。

### 5.3 `DieDomain`（07 新增，Die 级共享状态唯一 owner）

```cpp
// include/openhbx/media/nand/die_domain.h
namespace openhbx::media::nand {

class DieDomain {
 public:
  explicit DieDomain(std::uint64_t total_blocks);

  // Die 级阻塞（温度恢复/故障），跨 channel 共享；stage 边界由 slice 查询
  bool die_blocked(std::uint64_t flat_die_id) const noexcept;
  void set_die_blocked(std::uint64_t flat_die_id, bool blocked);

  // 全局 BBT：物理 Block 身份唯一；CapacityUnusable 需要全局视图（CHAN-009）
  ras::BadBlockTable& bad_block_table() noexcept { return bbt_; }
  bool capacity_unusable() const noexcept;   // bbt_.size() >= total_blocks

  // 无 reset()：BBT 与 blocked dies 在系统 reset 后保持不变，与旧单体设备
  // 行为一致（物理坏块身份与注入的 die 阻塞不是运行时命令状态）（CHAN-005）。
  // 未来 MEDIA-008 解除阻断后，Die 温度时间线归入本类（吸收 LLD_07 规划的
  // DieEnvironment 角色），温度不属单个 channel。

 private:
  std::uint64_t total_blocks_{0};
  std::set<std::uint64_t> blocked_dies_;
  ras::BadBlockTable bbt_;
};

}  // namespace openhbx::media::nand
```

### 5.4 `NandChannelSlice`（07 新增，取代单体 `NandFlashDevice`）

```cpp
// include/openhbx/media/nand/nand_channel_slice.h
namespace openhbx::media::nand {

struct NandChannelSliceConfig {
  config::HbfGeometry geometry;
  ChannelId channel;                       // 本 slice 服务的 Host Channel
  std::set<std::uint64_t> owned_banks;     // flat bank id 集合，02 由
                                           // ChannelTopology::banks_by_channel 构建
  PayloadMode payload_mode{PayloadMode::Sparse};
  StageTiming timing;
  ras::ReliabilityProfile reliability;
  std::size_t max_inflight{64};            // CHAN-010：每 slice 上限
};

class NandChannelSlice {
 public:
  NandChannelSlice(NandChannelSliceConfig config, DieDomain& die_domain,
                   EventQueue& events, HandlerId handler, CompletionSink completion);
  AdmissionResult try_issue(FlashCommand command, Cycle now);
  void reset(Generation next_generation, Cycle now);   // 只 discard 本 handler 的事件
  MediaSnapshot snapshot() const;
  bool page_is_valid(const PhysicalAddress& address) const;
  std::uint64_t block_erase_count(const PhysicalAddress& address) const;

 private:
  enum class Stage : std::uint8_t { /* 与现状一致 */ };
  struct Context { /* 与现状一致 */ };
  struct PageRecord { PayloadHandle payload; std::uint64_t signature{0}; };

  bool uses_channel_path(Stage stage) const;  // 原 uses_die()：
                                              // ReadCommand/ProgramData/EraseSetup
  // validate(): 顺序 = ownership（CHAN-006）→ DieDomain blocked/BBT/capacity
  //             → Page/Block 状态机；任何失败 typed reject 且零副作用

  NandChannelSliceConfig config_;
  DieDomain& die_domain_;              // 共享，只读查询（CHAN-005）
  MediaTopology topology_;
  EventQueue& events_;
  HandlerId handler_;                  // kMediaSliceHandlerBase + channel
  CompletionSink completion_;
  Generation generation_{0};
  std::map<std::uint64_t, Context> contexts_;
  std::map<std::uint64_t, PageRecord> pages_;          // 仅 owned banks 的页
  std::map<std::uint64_t, std::uint64_t> erase_counts_;
  std::map<std::uint64_t, Cycle> bank_eat_;            // BankArray，仅 owned
  std::map<std::uint64_t, Cycle> channel_media_path_eat_; // ChannelMediaPath(channel,
                                                          // core_die)，按 core_die 键
  std::uint64_t committed_pages_{0}, stale_events_{0};
};

}  // namespace openhbx::media::nand
```

删除项：`NandFlashDevice`、`die_eat_`、`blocked_dies_`（移入 DieDomain）、`bbt_`（移入 DieDomain）、`planes_*` 死字段、`struct Reservation` 死代码、硬编码温度 25.0（`nand_flash_device.cpp:136`；温度待 MEDIA-008 后入 DieDomain）。

### 5.5 `NandMediaRouter`（07 新增，06 端口的实现）

```cpp
// include/openhbx/media/nand/nand_media_router.h
namespace openhbx::media::nand {

// 实现 pal::IFlashMediaPort；无自有动态状态，按 command.channel 分发。
// 不复制 04 的 ownership 表：slice 的 owned_banks 成员资格检查即
// ownership 硬校验（CHAN-006），唯一 ownership 来源仍是 04 ChannelTopology
// （由 02 在装配期展开为各 slice 的 owned_banks）。
class NandMediaRouter : public pal::IFlashMediaPort {
 public:
  explicit NandMediaRouter(std::vector<NandChannelSlice*> slices_by_channel);
  AdmissionResult try_issue_media(FlashCommand command, Cycle now) override;
  // 实现：channel.value() >= slices_.size() -> Rejected(InvalidArgument)
  //       否则转发 slices_[channel.value()]->try_issue(...)
 private:
  std::vector<NandChannelSlice*> slices_;   // 按 channel 值索引，非 owning
};

}  // namespace openhbx::media::nand
```

`integration::NandMediaPort`（现持有单一 `NandFlashDevice*`）改为持有 `NandMediaRouter&`，或直接删除该中间层由 `FlashPal` 持有 router——实现时取调用点更少者，以 LLD_06/07 更新为准。

### 5.6 HandlerId 分段（CHAN-008）

```cpp
// include/openhbx/system/handler_ids.h （新增，02 拥有）
namespace openhbx {
inline constexpr HandlerId kSystemHandlerId{1};
inline constexpr HandlerId kPalHandlerId{2};
// 介质 slice 段：[1024, 1024+16)，OCP 上限 16 channel
inline constexpr std::uint64_t kMediaSliceHandlerBase{1024};
inline constexpr HandlerId media_slice_handler(ChannelId c) {
  return HandlerId{kMediaSliceHandlerBase + c.value()};
}
}  // namespace openhbx
```

`open_hbx_system.cpp:50-52` 的 101/102/103 常量删除；`EventQueue::register_handler` 的重复拒绝逻辑天然保证段内唯一。

### 5.7 EventQueue 作用域 discard（CHAN-007）

```cpp
// include/openhbx/system/event_queue.h （新增方法）
// 仅丢弃同时匹配 handler 与 generation 的事件；slice reset 不波及其他 slice。
void discard_generation(HandlerId handler, Generation generation);
```

现有队列范围 `discard_generation(Generation)` 保留给系统级 reset（02 唯一调用方）。`NandChannelSlice::reset` 调用作用域版本；`DieDomain` 无 reset 方法，也不操作队列（它自己无事件，且 BBT/blocked dies 跨 reset 保持）。

## 6. 关键 ADR

| ADR ID | 决策 | 备选 | 理由 | 代价 | 状态 |
|---|---|---|---|---|---|
| ADR-CHAN-001 | Bank 及以下按 channel 分片，Die 级共享 | 整 Die 分片 / 全共享 | 拓扑冻结结论：Channel 是 Bank 容量切片；Die 温度/BBT 物理上属 Die | slice 需持有 DieDomain& | 🔵 `ACCEPTED`（已实现） |
| ADR-CHAN-002 | `ChannelMediaPath(channel,core)` EAT 取代全局 die bus | 保留 die_eat_ 并文档化 | die bus 无规范/vendor 依据，且使 16 channel 带宽互相耦合；拓扑文档 §4 已禁止 | 性能测试基线数值变化：16 channel 最大读实测 1.888→1.892 TB/s（8884→8869 cycles），已在 `README.md` §4 重估 | 🔵 `ACCEPTED`（已实现） |
| ADR-CHAN-003 | 单一 `NandMediaRouter` 实现 `IFlashMediaPort`，按 channel 分发 | 06 直接持有 N 个 port | `IFlashMediaPort` 接口不变，06 无感知；分发逻辑归属 07 | 多一层间接 | 🔵 `ACCEPTED`（已实现，header-only 无状态） |
| ADR-CHAN-004 | ownership 校验落在 slice 成员资格，router 不复制 04 表 | router 内建 owner 表 | 避免 07→04 依赖反转与第二份 ownership 来源 | 错误归因在 slice 而非 router | 🔵 `ACCEPTED`（已实现） |
| ADR-CHAN-005 | `media_max_inflight` 语义改为每 slice | 全局池 | 物理上 channel 前端独立；全局池会重新引入跨 channel 耦合 | resolved config 总量含义变化，已在 `LLD_01` §8.1 文档化（CHAN-010） | 🔵 `ACCEPTED`（已实现） |
| ADR-CHAN-006 | `ChannelId` 上移 `common/strong_types.h`，04 别名兼容 | 07 携带裸 u64 / 07 include 04 | 跨模块公共类型归最小权威模块；强类型不降级 | 04 头文件改动，全仓库别名替换 | 🔵 `ACCEPTED`（已实现） |
| ADR-CHAN-007 | EventQueue 增加 `(handler, generation)` 作用域 discard | slice 共享全局 generation | 各 slice generation 计数独立，作用域 discard 保证 reset 隔离且 O(本 handler 事件) | EventQueue 增加一个过滤维度 | 🔵 `ACCEPTED`（已实现） |
| ADR-CHAN-008 | 控制器本轮保持单实例（一个 Base Die 托管全部 channel 前端） | 每 channel 一个控制器实例 | 物理上 Base Die 是一个 die；`BaseDieFlashController` 构造已接受 bank 子集，未来拆分成本低 | 控制器内部 per-channel 公平性维持现状 | 🔵 `ACCEPTED`（本轮不拆） |
| ADR-CHAN-009 | 前端共享组件内部预算全部按 channel 计费（`InterconnectFabric.queue_depth` 按路由资源、`FlashPal.max_inflight` 按 channel、DLU 聚合器 `max_pending` 按 channel、ECC credits 按 channel）；`drive_scheduler` 遇 PAL Busy 改为延迟重排（deferred requeue） | 维持 cube 级全局池 / PAL Busy 时队头阻塞 | OCP HBF v0.7 §4.4：Base Die 内部子系统按 channel 组织，一个 channel 的压力不得消耗其他 channel 的传输/在途预算；全局池使 16 channel 互相耦合，队头阻塞会让 Busy channel 饿死其余 channel | 单实例组件内增加 `*_by_channel_` 映射；`ControllerConfig.queue_depth` 维持 per-Bank 语义（Bank 唯一归属 channel，天然隔离） | 🔵 `ACCEPTED`（已实现，2026-10-15） |

### ADR-CHAN-009 落地记录（2026-10-15）

对齐 OCP HBF v0.7「Figure 1 展示单个 channel 的 Base Die 主要模块；仅 Debug Controller 为 16 channel 共用」的子系统组织，将介质层之外的前端共享组件内部预算全部改为按 channel 计费，单实例布局不变（ADR-CHAN-008 依然成立）：

- `InterconnectFabric`：删除 cube 级 `active_.size() >= queue_depth` 检查；`queue_depth` 改为按路由携带的每个资源独立计费（生产装配中一条路由恰好携带一个 Channel 路径资源），`release`/`reset` 对称回收。
- `FlashPal`：`max_inflight` 改为每 channel 在途上限，cube 总在途 = `max_inflight` × `host_channels`，与 CHAN-010 的 `media_max_inflight` 每 slice 语义一致。
- `DluAccumulator`：`max_pending` 改为每 channel 待聚合 DLU 上限，`expire`/`release`/`reset` 对称维护 `pending_by_channel_`。
- `BaseDieFlashController`：ECC decode credits 按 `ChannelId` 计费；`drive_scheduler` 遇 PAL Busy 不再停止本次排空（队头阻塞），改为将该项延迟到本趟结束后重新入队，Busy channel 不再阻塞其他 channel 的已就绪工作。
- 隔离性测试：CHAN-T13（`tests/component/pal/test_flash_pal.cpp`，channel 0 在途打满后 channel 1 仍有完整独立预算）、CHAN-T14（`tests/unit/interconnect/test_fabric.cpp`，路由资源 A 打满不影响资源 B）。
- 既有全部单 channel 用例语义不变（单 channel 下 per-channel 与全局池等价）；`ctest` 全量回归通过（见 `build/artifacts/` 当次证据）。
- 日志：`OpenHbxSystem` 默认安装 ramulator 风格控制台 sink（`console_log`，`[module] [level] cycle=...` 写 `std::clog`），运行时 `set_log_level` 过滤（默认 Warn），trace runner 支持 `OPENHBX_LOG_LEVEL=error|warn|info|debug|trace|off`。设计约束仍遵循 `docs/OPENHBX_LOGGING_AND_OBSERVABILITY.md`（日志不作判定 oracle）。

## 7. 不变量与测试计划

新增/更新不变量：

1. **CHAN-I1（ownership 硬不变量）**：任何进入 slice 的命令满足 `bank ∈ slice.owned_banks`；违例 typed reject，介质状态零副作用（对应 CHAN-T06：对每对 (channel, bank) 组合注入错误归属命令，验证拒绝且 snapshot 不变）。
2. **CHAN-I2（介质路径隔离）**：两 channel 对各自 Bank 饱和加压，彼此的 `ChannelMediaPath`/Bank EAT 时间线互不影响（CHAN-T04：单 channel 与 16 channel 满载下，同 channel 的完成时间线一致——回归 `tests/performance/test_hbf_max_read_bandwidth.cpp`，带宽基线按 ADR-CHAN-002 重估）。
3. **CHAN-I3（reset 隔离）**：slice k reset 后，其他 slice 的 pending 事件与上下文不受影响；stale 事件不提交（CHAN-T07）。
4. **CHAN-I4（容量全局性）**：BBT 标记跨 channel 的物理 Block 后，全局容量视图一致；`CapacityUnusable` 在全局耗尽时对所有 channel 生效（CHAN-T09）。
5. **CHAN-I5（确定性）**：同 seed 双跑 snapshot 逐字节一致；跨 slice 事件交错不改变各 slice 内部结果（CHAN-T11）。
6. **CHAN-I6（守恒）**：`accepted = terminal + outstanding` 按 slice 成立，汇总后与系统级统计一致（CHAN-T12）。
7. **CHAN-I7（前端预算隔离）**：任一 channel 的 Fabric 路由资源 / PAL 在途 / DLU 待聚合 / ECC credit 耗尽时，其他 channel 的同名预算与准入结果不受影响；PAL Busy 不产生跨 channel 队头阻塞（CHAN-T13：PAL 在途隔离；CHAN-T14：Fabric 路由资源隔离；ADR-CHAN-009）。

测试落点：单元 `tests/unit/media/nand/`（slice/router/die_domain）、集成 `tests/component/media/`（router+slice+DieDomain）、系统回归 `tests/system/test_open_hbx_system.cpp`、性能 `tests/performance/test_hbf_max_read_bandwidth.cpp`（基线重估，ADR-CHAN-002）。`apps/openhbx_trace_runner.cpp`、`tests/conformance/test_ocp_transaction_vectors.cpp` 中直接构造单体设备/flat 地址的用例迁移到 router+slice 装配。

**实施结果（2026-09-20）**：

- 新增 `tests/component/media/test_channel_partition.cpp`（CTest 名 `openhbx_channel_partition_test`，labels 含 `CHAN-T06/T07/T09/T11/T12`）：以 1 个 `DieDomain` + 2 个 slice + router 的真实装配覆盖 CHAN-I1（ownership 拒绝零副作用，含 router 越界与 slice 级 channel 不匹配）、CHAN-I2 时序半（同 core die 双 channel 完成周期同为 25）、CHAN-I3（reset 隔离 + 新 generation 资源复用）、CHAN-I4（共享 BBT + 全局耗尽后两 channel 均拒绝）、CHAN-I5（双跑逐字段一致）、CHAN-I6（每 slice 守恒）。
- `tests/component/media/test_media_component.cpp` 与 `tests/unit/media/nand/test_flash_semantics.cpp` 已从单体 `NandFlashDevice` 迁移到单 channel 装配（一个 slice 拥有全部 Bank），既有 MEDIA 断言全部保留；`bad_block_table().mark` 与 `set_die_blocked` 改为经 `DieDomain` 调用。
- 性能回归：16 channel 最大读 4096/4096 完成、0 failure，1891.669 GB/s（8869 cycles），高于重构前 1888.475 GB/s（8884 cycles）；本配置瓶颈仍在 raw link（利用率 46.2%），故删除 die 总线带来的提升有限。
- 全量 `ctest` 23/23 通过。
- 仍未实现：CHAN-T01/T02/T03/T04（专用 oracle）、CHAN-T05/T08/T10 的专用用例、CHAN-T04 中「16 channel 满载与单 channel 时间线一致」的系统级对照、CHAN-T12 的「slice 汇总 == 系统统计」断言。`apps/openhbx_trace_runner.cpp` 与 `tests/conformance/test_ocp_transaction_vectors.cpp` 未直接构造单体设备（走 `OpenHbxSystem` 装配），故无需迁移。

## 8. 实施顺序

按依赖方向分四阶段，每阶段可独立构建、测试、提交（四个阶段均已完成，2026-09-20）：

- ✅ **Phase 1（channel 显式化，行为不变）**：`ChannelId` 上移 common（5.1）→ `FlashPhysicalRequest.channel`/`FlashCommand.channel` 字段与透传（5.2）→ `debug_address_snapshot` 复用唯一解码（CHAN-001）。介质仍单体，字段仅透传不消费。**落地记录**：`channel` 字段刻意插在 `kind` 与 `address` 之间，使遗漏该字段的聚合初始化在编译期报错而非静默取默认值；据此修正了 4 个测试文件共 20 处构造点。`OpenHbxSystem::decode_flat_address` 为唯一解码入口，`RequestBridge` 与 `debug_address_snapshot` 共用。
- ✅ **Phase 2（介质分片，核心）**：`EventQueue` 作用域 discard（5.7）→ `handler_ids.h`（5.6）→ `DieDomain`（5.3）→ `NandChannelSlice`（5.4，由 `NandFlashDevice` 改造）→ `NandMediaRouter` 与系统装配（5.5，`open_hbx_system.cpp` 按 channel 构建 N slice）→ 测试迁移与 CHAN-T06/07/09/11/12。**落地记录**：`nand_flash_device.{h,cpp}` 已删除；`integration::NandMediaPort` 改为持有 `pal::IFlashMediaPort*` 的迟绑定转发器，消除装配顺序约束；slice 的 `owned_banks` 由 02 从 `ChannelTopology::banks_by_channel` 展开注入（保持 04 为 ownership 唯一来源）；`debug_address_snapshot` 经 `ChannelTopology::owner_of` 路由到对应 slice。作用域 discard 的安全性已核对：PAL 自身 reset 先做队列级清理，再触发介质 reset，故 slice 级作用域 discard 不会遗留 PAL/Host 陈旧事件。
- ✅ **Phase 3（配置语义）**：`media_max_inflight` 每 slice 语义文档化（CHAN-010）——已写入 `LLD_01` §8.1 配置键表（全局上限 = `media_max_inflight` × `host_channels`）。
- ✅ **Phase 4（观测与收口）**：按 channel 统计（CHAN-012）——新增 `OpenHbxSystem::media_snapshot(ChannelId)`；性能基线按 ADR-CHAN-002 重估并回写 `README.md` §4；`TRACEABILITY_MATRIX.md` §9 与 `LLD_07` 状态列按实际证据更新。

## 9. 对既有文档的影响清单

| 文档 | 更新内容 |
|---|---|
| `HBF_CHANNEL_CORE_DIE_TOPOLOGY.md` §4 | 标注「期望资源已按本文实现」，链接本文 |
| `当前模拟模型问题与改造说明.md` | §4 `IntegrationEngine` 固定延迟第二路径已不存在于代码，标注 stale；新增第 7 问题指向本文 |
| `HLD_02/LLD_02` | HandlerId 分段、介质装配（N slice + DieDomain + router）、作用域 discard |
| `HLD_04` | `ChannelTopology` 作为 slice `owned_banks` 的唯一装配来源；`ChannelId` 上移说明 |
| `HLD_06/LLD_06` | `FlashPhysicalRequest.channel`、`IFlashMediaPort` 实现者变更为 router |
| `HLD_07/LLD_07` | `NandFlashDevice` 退役；`NandChannelSlice`/`DieDomain`/`NandMediaRouter` 逐文件设计（LLD_07 第 7 章为权威） |
| `OpenHBF-文件安排.md` | 注册新文件（§4.2 handler_ids.h；§4.7 die_domain/nand_channel_slice/nand_media_router） |
| `TRACEABILITY_MATRIX.md` | 登记 CHAN-001..012 与 CHAN-T01..T12 |
