# OpenHBF DRAM 扩展：第一轮事实核对

## 1. 目标

本文件记录把 Ramulator 2 的 DRAM 模型引入 OpenHBF 前的事实核对。目标是复用成熟代码和框架，而不是重新实现 JEDEC DRAM 时序；同时避免把 OpenHBF 的 HBF Host Channel 与 Ramulator 的 DRAM channel 误认为同一概念。

## 2. Ramulator 2 的实际分层

从 `thirdparty/ramulator2/src/ramulator/memory_system/impl/generic_dram_system.cpp`、`controller/controller_base.*`、`dram/*` 及其 README 可确认：

```text
IMemorySystem / GenericDRAMSystem
  ├── IChannelMapper
  └── IController[0..N-1]
        ├── scheduler
        ├── row policy
        ├── refresh manager
        ├── address mapper
        └── DRAMDevice
              └── Channel -> Rank -> BankGroup -> Bank -> Row/Column
```

`GenericDRAMSystem` 明确采用“一个 controller 对应一个 channel”。它负责 channel 选择、请求大小检查和逐周期 tick；controller 负责请求队列、调度、刷新、地址映射及 DRAM command；`DRAMDevice` 负责层级状态、命令前置条件和 timing constraint。DRAM 标准主要由 `python/ramulator/dram/*.py` 描述，再生成 C++ 规格代码。

## 3. OpenHBF 的实际 channel

OpenHBF 的 `ChannelId` 出现在 Host、Address、Controller 和 Interconnect 边界。`ChannelTopology` 维护 `banks_by_channel`，`HbfAddressMapper` 要求请求访问其 Host Channel 所拥有的 PhysicalBank；`ResolvedHbfConfig` 还校验 Bank 是否能平均分配给 Host Channel。

因此 OpenHBF Host Channel 的定义是：

- 主机可见的 ownership 和容量切片；
- 由 `Core Die / Die / Bank` 物理 NAND 拓扑组成；
- 约束 Host ordering、地址合法性、DLU 和 Flash controller 访问权限；
- 不等于一个独立控制器，也不天然拥有独立时钟或 DRAM command 状态。

## 4. 两个 channel 是否一致

结论：**抽象层级相近，但语义不一致，不能合并为一个 `ChannelId`。**

| 维度 | Ramulator channel | OpenHBF Host Channel |
|---|---|---|
| 核心含义 | 一个 controller 的 DRAM 并行时序域 | NAND Bank ownership/容量域 |
| 所有者 | GenericDRAMSystem/controller | Host、AddressTopology、HBF controller 协同 |
| 下级层次 | rank、bank group、bank、row/column | core die、die、bank、block、page |
| 请求入口 | DRAM transaction，通常不超过 tx bytes | Host packet/DLU/Flash operation |
| 地址选择 | channel mapper + DRAM addr mapper | HBF Host Channel + HBF address mapper |
| 时间模型 | controller clock、DRAM commands、JEDEC timing | OpenHBF EventKernel、PAL、NAND stage 和 HBF protocol |
| 复用策略 | 可共享同一 controller 配置的多个 channel | ownership 不能跨 channel 伪造或复制 |

二者可通过显式映射关联，例如 `DramEndpointId -> RamulatorChannelId`，但不能让 DRAM channel 覆盖 HBF channel 的 ownership 语义。

## 5. 可直接复用的 Ramulator 代码

优先复用以下完整子系统，不复制到 OpenHBF 核心目录：

1. `src/ramulator/dram/`：`DRAMSpec`、`DRAMDevice`、`DRAMNode` 和生成的标准规格。
2. `src/ramulator/controller/`：`ControllerBase`、`GenericDDRController`、LPDDR/HBM controller、scheduler、row policy、refresh 和 addr mapper。
3. `src/ramulator/memory_system/`：`IMemorySystem`、`GenericDRAMSystem`、channel mapper。
4. `python/ramulator/dram/` 及构建代码生成链：DRAM 标准的唯一参数来源。
5. Ramulator 的 controller、device timing、smoke 测试作为 vendor regression。

不直接复用为 OpenHBF 生产模块的部分：frontend、gem5 wrapper、Ramulator 的全局 Request 生命周期、Ramulator channel mapper 对 HBF Host Channel 的替代逻辑。

## 6. 第一轮风险结论

- 若把 HBF `ChannelId` 直接传给 Ramulator controller，会把容量 ownership 错误地当成 DRAM 时序域。
- 若让 Ramulator `GenericDRAMSystem::send()` 直接接收 HBF Host 请求，会绕过 Host ordering、HBF address mapping、Controller admission 和 PAL。
- 若复制生成的 DRAM C++ 文件而不保留 Python DSL 和生成流程，后续标准升级会产生不可审计的分叉。
- HBF 的 4 KiB page/DLU 与 DRAM transaction bytes 不同，必须在适配器处明确拆分或聚合关系。

## 7. 第一轮建议

建立独立 `dram` vendor adapter 边界：OpenHBF 继续拥有 Host、HBF Address、Flash Controller 和 EventKernel；Ramulator 只作为 DRAM backend，拥有 DRAM channel/controller/device 的内部状态。两边用 typed request、显式 endpoint 映射和一次 completion 回调连接。
