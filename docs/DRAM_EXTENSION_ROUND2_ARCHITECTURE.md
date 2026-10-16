# OpenHBF DRAM 扩展：第二轮架构方案

## 1. 设计原则

本轮把第一轮的事实核对转成可实现架构：

- 保留 OpenHBF 的单一生产路径和状态所有权；
- 复用 Ramulator 的 DRAM 时序、调度和标准定义；
- 不把 HBF Host Channel 与 DRAM channel 合并；
- 让 DRAM 成为可选 backend，不影响现有 `OCP_HBF_0_7` NAND profile；
- 真实 DRAM 路径必须通过 CMake/CTest 和可重放 artifact 验证。

## 2. 推荐对象图

```text
Host request
  -> HBF HostProtocol
  -> HBF AddressMapper / ChannelTopology
  -> HBF Controller admission and ordering
  -> DramPalAdapter
       -> DramEndpointMap (HBF channel/physical target -> DRAM endpoint)
       -> RamulatorMemorySystem
            -> GenericDRAMSystem
                 -> Ramulator Controller[channel]
                      -> DRAMDevice / JEDEC timing
       <- typed DramCompletion
  -> HBF Controller terminal commit
  -> Host response
```

`DramPalAdapter` 是唯一允许 OpenHBF 依赖 Ramulator C++ ABI 的边界。OpenHBF 生产代码不直接 include `ControllerBase`、`DRAMNode` 或生成的 DRAM implementation。

## 3. 两种集成模式

### 3.1 DRAM backend 模式（首选）

保留现有 OpenHBF Host 和系统生命周期，在 06 PAL 位置增加 `ramulator_dram` backend。Flash Controller 将一个已完成地址翻译的 memory operation 转换成 `DramRequest`，adapter 将其转换成 Ramulator `Request`，并在 Ramulator tick/completion 后转换回 `DramCompletion`。

适用于：用 DRAM 研究缓存、控制器或 HBF 与 DRAM 混合系统，同时保持 OpenHBF 端到端 oracle。

### 3.2 独立 DRAM system 模式（过渡验证）

先用 OpenHBF 配置和 artifact 体系驱动一个独立 `GenericDRAMSystem`，验证 DDR4/DDR5/HBM 等标准的构建、请求、tick、completion 和 stats，再接入真实 HBF pipeline。

该模式最高只能证明 Ramulator backend 的 E1/E2 契约，不能证明 HBF 全链路。

## 4. 显式概念映射

新增以下类型，避免复用同名但语义不同的类型：

```cpp
struct DramEndpointId { uint32_t value; };
struct RamulatorChannelId { uint32_t value; };
struct DramAddress { std::vector<uint64_t> levels; };
struct DramRequest {
  Token token;
  DramEndpointId endpoint;
  uint64_t byte_address;
  uint32_t size_bytes;
  RequestKind kind;
};
```

`Hbf ChannelId` 只出现在 `DramEndpointMap` 的输入侧。映射策略必须在 resolved config 中显式记录：

```text
HBF Host Channel -> HBF physical/ownership target -> DRAM endpoint -> Ramulator channel
```

默认首版可采用一对一 endpoint 映射；多 HBF channel 共享一个 DRAM channel 时，必须显式标记共享资源和仲裁策略，不能通过 `% channel_count` 静默映射。

## 5. 时钟和事件边界

Ramulator controller 有自己的 `m_clk` 和 `tCK`，OpenHBF EventKernel 有自己的 simulation cycle。首版建议使用明确的 clock bridge：

```text
OpenHBF cycle -> accumulated DRAM ticks by clock ratio -> Ramulator tick()
```

adapter 负责保存 pending token，不允许 Ramulator callback 直接修改 HBF 持久状态。只有收到并校验 generation/token 的 `DramCompletion` 后，HBF Controller 才提交 terminal state。

如果未来允许不同 DRAM endpoint 使用不同频率，必须把每个 endpoint 的 ratio 和 phase 写入 resolved config，并定义同周期顺序；首版先要求所有 DRAM controllers 同频，符合 `GenericDRAMSystem` 当前约束。

## 6. 地址和粒度

HBF 地址翻译先完成 ownership、block/page 和权限检查；adapter 再把已确认的 byte address 转成 Ramulator 地址。不得让 Ramulator addr mapper 决定 HBF Bank ownership。

HBF 4 KiB read/write 与 Ramulator `tx_bytes` 的关系必须显式：

- `size_bytes <= tx_bytes` 的请求可拆成多个 DRAM transaction；
- 每个子请求继承父 token、generation 和 byte range；
- 只有所有子请求完成后才能产生一个 HBF terminal；
- 任一子请求失败时，父请求按 typed failure 终止，未完成子请求必须取消或进入明确 drain 状态；
- payload ownership 只在 HBF Controller 和 adapter 之间转移一次。

首版建议只开放 `DramAccessGranule == tx_bytes` 的配置，避免先实现复杂的 partial transaction 合并；4 KiB 上层操作由 bounded burst splitter 处理。

## 7. 配置扩展

在现有 `ResolvedHbfConfig` 外增加独立 `DramConfig`，不把 DRAM 字段塞进 HBF geometry：

```yaml
memory_backend:
  kind: ramulator2
  standard: DDR4
  controller_impl: GenericDDR
  channels: 2
  tx_bytes: 64
  clock_ratio: 1
  endpoint_mapping: explicit
  overrides: {}
```

配置解析顺序仍遵守现有 ConfigResolver：schema -> profile -> overrides -> derived values -> capability validation -> immutable resolved config。DRAM 标准、controller、channel 数量、tx bytes、tCK 和 override 来源必须进入 canonical hash。

## 8. 状态与故障边界

Ramulator 的 DRAM row/bank timing state 由 Ramulator 拥有；HBF 的 Host ordering、cache、NAND payload、generation 和 terminal ledger 由 OpenHBF 拥有。Reset 时 adapter 必须先停止 admission，再使旧 generation 的 Ramulator completion 失效，最后清理未完成映射；不能直接清空 Ramulator 私有队列。

Ramulator 的 stats 作为 backend observation 导出，不能直接作为 HBF accepted/terminal oracle。OpenHBF 自己维护跨模块守恒：`accepted = terminal + outstanding`。

## 9. 首版范围和排除项

首版只支持一个 Ramulator DRAM backend、同频 controller、DDR4/DDR5 中至少一个标准、显式 endpoint mapping 和 read/write 基础路径。暂不把 gem5 frontend、Ramulator Python frontend、HBM 专用语义或 DRAM refresh fault 注入直接暴露给 HBF Host。
