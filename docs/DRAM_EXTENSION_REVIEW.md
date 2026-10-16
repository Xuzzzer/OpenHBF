# OpenHBF DRAM 扩展方案审查

## 1. 审查对象

本审查覆盖：

- `DRAM_EXTENSION_ROUND1_INVESTIGATION.md`
- `DRAM_EXTENSION_ROUND2_ARCHITECTURE.md`
- `DRAM_EXTENSION_ROUND3_CODE_PLAN.md`

## 2. 关键结论

方案选择“复用 Ramulator 设备和控制器 + OpenHBF typed adapter”，而不是复制代码后重写 DRAM。最重要的边界是：Ramulator channel 和 OpenHBF Host Channel 都是并行资源概念，但不具有同一所有权、地址和时序语义。显式 `DramEndpointId -> RamulatorChannelId` 映射可以连接两者，统一类型则会制造错误。

## 3. 方案优点

### 3.1 OpenHBF 现有框架的优势

- **明确的状态所有权**：Host ordering、HBF topology、Flash Controller、PAL、Media 和 EventKernel 已有边界，适合把 DRAM 限定为 backend。
- **统一的 typed 生命周期**：token、generation、terminal 和 drain 规则能包住 Ramulator callback，避免第三方完成事件直接污染持久状态。
- **配置可审计**：现有 resolved config 和 canonical hash 可以把 DRAM standard、controller、channel mapping、tx bytes 和 tCK 纳入同一份可重放配置。
- **验证分级清晰**：E1/E2 backend、E3 真实 OpenHBF pipeline、E4 resource ceiling 的边界已经定义，可避免把 Ramulator smoke test 当成 HBF conformance。
- **backend 可替换**：保留 NAND 路径，同时以 `IDramBackend` 增加 Ramulator，不必让 HBF Host 了解具体 DRAM 标准。

### 3.2 Ramulator 的优势

- 已有 DRAM 标准 DSL、生成链、层级设备状态、时序约束和多种 controller；
- 已有 scheduler、row policy、refresh、address mapper 和 device timing 测试；
- `GenericDRAMSystem` 的 controller/channel 结构适合封装为 backend 的内部实现。

## 4. 审查发现的限制与修正

1. **不能直接 copy 单个 `.cpp`**：DRAM 代码依赖生成器、Python DSL、公共 base、注册宏和构建规则。修正为保留完整 vendor subtree 或继续使用 submodule，并固定 revision。
2. **不能把 GenericDRAMSystem 当成 HBF System**：它只有 DRAM channel/controller 语义，不负责 HBF Host admission、NAND ownership、payload commit 或 HBF reset。修正为 adapter 组合而非继承替换。
3. **粒度存在硬冲突**：HBF 的 4 KiB DLU/page 与 Ramulator tx bytes 通常不同。修正为首版限制 granule，随后由 splitter/aggregator 管理父子 token。
4. **时钟不能隐式相等**：Ramulator 的 `m_clk/tCK` 和 OpenHBF cycle 不必一致。修正为显式 clock bridge，并在 config/hash/artifact 中记录 ratio 和 phase。
5. **共享 channel 需要建模**：多个 HBF endpoint 映射同一个 Ramulator channel 时会共享 controller、queue 和 timing state。修正为显式共享资源配置和测试，禁止默认 modulo。
6. **reset 不能清空第三方私有状态**：修正为 generation fencing、停止 admission、adapter drain/cancel contract，并在必要时重建 backend 实例。

## 5. 未决问题和推荐默认值

| 问题 | 首版默认值 | 放宽条件 |
|---|---|---|
| 支持的 DRAM 标准 | DDR4 或 DDR5 先选一个 | 该标准 smoke/device timing 通过后再扩展 |
| DRAM controller 频率 | 所有 controller 同频 | 具备 phase/ratio E3 证据后允许异频 |
| endpoint 映射 | 一对一显式映射 | 共享 channel 有独立 backpressure/ceiling 测试后允许 |
| HBF transaction 粒度 | `tx_bytes` 对齐 | splitter/aggregator 和 payload oracle 完成后放宽 |
| reset 处理 | generation fencing + backend drain | 有安全重建和 replay 证据后支持强制重置 |
| DRAM stats | observation only | 不直接替代 HBF conservation oracle |

## 6. 审查后的实施顺序

1. 先不改 OpenHBF 生产路径，完成 Ramulator 上游 revision、生成链和许可证审计。
2. 实现 `IDramBackend`、typed request/completion、endpoint map 和 clock bridge。
3. 用独立 backend CTest 验证真实 Ramulator 请求生命周期。
4. 通过 PAL registry 接入一个最小 HBF Controller 路径。
5. 增加粒度拆分、共享 channel 和 reset/stale 测试。
6. 最后再开放多标准、多 endpoint 和性能实验。

## 7. 最终决策

采用本方案作为 DRAM 扩展设计基线。首个编码任务应是 adapter 和配置契约，而不是把 Ramulator 内部类散复制到 OpenHBF。只有当 adapter 的 E1/E2 证据和上游构建闭包稳定后，才进入 OpenHBF 真实 E3 pipeline；在此之前不得把 DRAM 扩展标记为 `VERIFIED`。
