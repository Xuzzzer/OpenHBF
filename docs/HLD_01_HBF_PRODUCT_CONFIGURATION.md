# OpenHBX HBF 产品配置与装配概要设计

**文档版本**：V1.0
**文档位置**：`docs/HLD_01_HBF_PRODUCT_CONFIGURATION.md`
**对应文档完整路径**：`docs/LLD_01_HBF_PRODUCT_CONFIGURATION.md`
**客观状态**：🟡 `PARTIAL`；配置解析、profile、resolved config、能力闭包、registry和composer已有生产实现，完整系统装配证据仍待补齐。规范字段提取项为🔴 `BLOCKED_SPEC`
**适用基线**：OCP HBF v0.7.0；当前 `thirdparty/ramulator2`
**阶段范围**：仅 `OCP_HBF_0_7` profile

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 按OpenHBX长期框架重建；冻结本阶段仅装配OCP HBF产品 |

## 1. 模块概述

### 1.1 定位与职责

本模块是OpenHBX的唯一配置入口和对象图装配入口，回答“用户给出的配置能否描述一个合法HBF产品，以及运行时应创建哪些组件”。处理顺序固定为：

```text
Ramulator ConfigNode / YAML
  -> schema defaults
  -> OCP_HBF_0_7 profile
  -> declared user overrides
  -> derived values
  -> capability/cross-field validation
  -> immutable ResolvedHbfConfig
  -> ProductComposer
  -> one OpenHbxSystem object graph
```

本模块拥有schema、profile、配置诊断、resolved config和装配事务；各运行模块拥有自己的队列与状态。

### 1.2 包含与不包含

包含：严格字段解析、显式单位、HBF profile冻结、派生值、能力校验、工厂注册、依赖拓扑排序、对象图一次性装配及resolved config导出。

不包含：Host事务处理、地址映射算法、DLU聚合、NAND命令时序、TSV传输、事件推进、动态热插拔，以及HBM/LPDDR组件实现。本阶段若配置选择`HBM2`、`HBM3`或`LPDDR5`，必须在启动时明确拒绝。

### 1.3 模块关系与不变量

上游是Ramulator `ConfigNode`或CLI YAML loader；下游是02 `OpenHbxSystem`及HBF Host、Controller、Address、Interconnect、NAND Media、RAS组件的构造接口。

- 每次成功解析只产生一个不可变`ResolvedHbfConfig`。
- 未知字段、未知`impl`、缺单位、溢出或能力不匹配不得静默回退。
- 装配要么完整成功，要么不发布半成品对象图。
- `resolved-config.yaml`必须包含默认值、用户覆盖、派生值、来源和synthetic参数标记。
- profile固定的协议语义不能被用户override改写。
- 所有设计项状态为`PLANNED`；规范字段未提取项保持`BLOCKED_SPEC`。

## 2. 需求回顾与追踪

### 2.1 需求进度总览

状态图例：⚪ `PLANNED`表示只有设计；🔴 `BLOCKED_SPEC`表示缺少决定语义的规范证据。图标不参与机器判定。

| 需求ID | 需求描述 | 优先级 | 版本 | 实现状态 |
|---|---|---|---|---|
| CFG-001 | 只接受`OCP_HBF_0_7` profile | P0 | V1.0 | ⚪ `PLANNED` |
| CFG-002 | 非阶段产品profile启动拒绝 | P0 | V1.0 | ⚪ `PLANNED` |
| CFG-003 | 固定顺序生成不可变resolved config | P0 | V1.0 | ⚪ `PLANNED` |
| CFG-004 | 严格拒绝未知或非法配置字段 | P0 | V1.0 | ⚪ `PLANNED` |
| CFG-005 | checked arithmetic派生容量与地址 | P0 | V1.0 | ⚪ `PLANNED` |
| CFG-006 | 校验组件能力闭包 | P0 | V1.0 | ⚪ `PLANNED` |
| CFG-007 | 冻结64 B transaction与4 KiB DLU | P0 | V1.0 | 🔴 `BLOCKED_SPEC` |
| CFG-008 | 校验最多16个Host Channel及ownership | P0 | V1.0 | 🔴 `BLOCKED_SPEC` |
| CFG-009 | 使用`kind + impl`注册组件 | P1 | V1.0 | ⚪ `PLANNED` |
| CFG-010 | 原子发布完整对象图 | P0 | V1.0 | ⚪ `PLANNED` |
| CFG-011 | resolved config记录字段来源 | P1 | V1.0 | ⚪ `PLANNED` |
| CFG-012 | 等价入口生成相同配置hash | P1 | V1.0 | ⚪ `PLANNED` |

### 2.2 证据、Owner与Test追踪

| 需求ID | 可测试描述 | 来源 | 类别 | 优先级 | owner组件 | 验收/Test ID | 状态 |
|---|---|---|---|---|---|---|---|
| CFG-001 | 接受且只接受`product.profile: OCP_HBF_0_7` | OpenHBX阶段设计选择，`任务更正方案.md`第1节 | 设计选择 | P0 | ProfileCatalog | CFG-T01 | ⚪ `PLANNED` |
| CFG-002 | HBM/LPDDR profile启动失败并指出配置路径和未支持阶段 | `TASK1-整体设计方案.md`第1.2节；本阶段裁剪 | 设计选择 | P0 | SchemaValidator | CFG-T02 | ⚪ `PLANNED` |
| CFG-003 | 加载顺序固定且输出不可变resolved config | `TASK1-整体设计方案.md`第6.1节 | 设计选择 | P0 | ConfigResolver | CFG-T03 | ⚪ `PLANNED` |
| CFG-004 | 未知字段、未知impl、非法枚举和缺失单位启动失败 | `TASK1-整体设计方案.md`第10节 | 设计选择 | P0 | SchemaValidator | CFG-T04 | ⚪ `PLANNED` |
| CFG-005 | 派生容量和地址范围使用checked arithmetic | OpenHBX设计选择；迁移前源码`include/openhbf/common/checked_math.h` | 源码约束 | P0 | DerivedValueEngine | CFG-T05 | ⚪ `PLANNED` |
| CFG-006 | 校验Host/Controller/Mapper/Interconnect/Media能力闭包 | `TASK1-整体设计方案.md`第10节 | 设计选择 | P0 | CapabilityValidator | CFG-T06 | ⚪ `PLANNED` |
| CFG-007 | profile冻结64 B Host transaction与4 KiB DLU | OCP HBF v0.7.0 Product Description/Host Interface；待PDF逐项定位 | 规范事实 | P0 | HbfProfile | CFG-T07 | 🔴 `BLOCKED_SPEC` |
| CFG-008 | profile支持最多16个独立Host Channel并校验ownership geometry | `任务更正方案.md`第6.2节；PDF精确定位待补 | 规范事实 | P0 | HbfProfile | CFG-T08 | 🔴 `BLOCKED_SPEC` |
| CFG-009 | 通过`kind + impl`注册组件，禁止散落字符串分支 | Ramulator `base/base.h`: `Implementation::create_child*`; OpenHBX ADR-01-001 | 源码事实/设计选择 | P1 | ComponentRegistry | CFG-T09 | ⚪ `PLANNED` |
| CFG-010 | 对象图仅在全部组件验证成功后原子发布 | OpenHBX ADR-01-002 | 设计选择 | P0 | ProductComposer | CFG-T10 | ⚪ `PLANNED` |
| CFG-011 | resolved config标记OCP、vendor、synthetic与用户覆盖来源 | `TASK1-整体设计方案.md`第10节 | 设计选择 | P1 | ConfigResolver | CFG-T11 | ⚪ `PLANNED` |
| CFG-012 | 配置hash在等价YAML与ConfigNode入口间一致 | Ramulator `base/config_node.h`; OpenHBX确定性要求 | 源码事实/设计选择 | P1 | CanonicalSerializer | CFG-T12 | ⚪ `PLANNED` |

## 3. 系统架构设计

```text
Config source
   | parse (no runtime side effects)
   v
SchemaValidator -> ProfileCatalog -> OverrideMerger -> DerivedValueEngine
   |                                                     |
   +---------------- ValidationReport <-------------------+
                         |
                         v
                CapabilityValidator
                         |
                         v
              immutable ResolvedHbfConfig
                         |
                         v
        ComponentRegistry -> ProductComposer
                         |
                         v
 OpenHbxSystem {Host, Address, Controller, Fabric, Media, RAS}
```

| 组件 | 主要文件 | 职责 | 输入 | 输出 | 拥有状态 | 依赖 |
|---|---|---|---|---|---|---|
| SchemaValidator | `hbf_config_schema.*` | 字段、类型、单位和范围检查 | ConfigNode/YAML树 | typed draft/诊断 | schema版本 | ConfigNode |
| ProfileCatalog | `hbf_product_profile.*` | 提供冻结的HBF默认组合 | profile名 | ProfileDefinition | profile表 | OCP提取结果 |
| ConfigResolver | `resolved_hbf_config.*` | 合并、派生、规范化、hash | draft+profile | immutable config | 来源图 | checked math |
| CapabilityValidator | `capability_validator.*` | 检查组件能力闭包 | resolved config+descriptor | report | 无运行状态 | 各模块descriptor |
| ComponentRegistry | `component_registry.*` | `kind+impl`到builder映射 | component spec | builder | 注册表 | 工厂机制 |
| ProductComposer | `hbf_product_composer.*` | 原子创建并连接对象图 | resolved config | `OpenHbxSystem` | 装配事务 | 02和各模块公开factory |

配置阶段不使用EventQueue；成功装配后，02创建唯一EventQueue并成为时间owner。

### 3.1 `SchemaValidator`

**文件**：`include/openhbx/config/hbf_config_schema.h`、`src/config/hbf_config_schema.cpp`。**职责/关键组件**：`HbfConfigSchema`检查字段、类型、单位和范围。**输入输出**：Raw tree到typed draft/诊断。**owner/依赖**：Config owner；依赖common config和Ramulator converter。**需求/状态**：CFG-004/005；⚪ `PLANNED`。

### 3.2 `ProfileCatalog`

**文件**：`include/openhbx/config/hbf_product_profile.h`、`src/config/hbf_product_profile.cpp`。**职责/关键组件**：`ProfileDefinition`冻结`OCP_HBF_0_7`。**输入输出**：profile名到只读definition。**owner/依赖**：Config owner；依赖OCP提取表。**需求/状态**：CFG-001/002/007/008；⚪ `PLANNED`，规范字段为🔴 `BLOCKED_SPEC`。

### 3.3 `ConfigResolver`

**文件**：`include/openhbx/config/resolved_hbf_config.h`、`src/config/resolved_hbf_config.cpp`。**职责/关键组件**：`HbfConfigResolver`合并、派生、冻结和hash。**输入输出**：draft/profile/override到immutable config。**owner/依赖**：Config owner；依赖schema、profile、checked math。**需求/状态**：CFG-003/005/011/012；⚪ `PLANNED`。

### 3.4 `CapabilityValidator`

**文件**：`include/openhbx/config/capability.h`、`src/config/capability.cpp`。**职责/关键组件**：typed capability closure。**输入输出**：config/descriptors到ValidationReport。**owner/依赖**：Config owner；依赖模块descriptor。**需求/状态**：CFG-006；⚪ `PLANNED`。

### 3.5 `ComponentRegistry`

**文件**：`include/openhbx/config/component_registry.h`、`src/config/component_registry.cpp`。**职责/关键组件**：`kind+impl` builder注册与seal。**输入输出**：key/builder到只读builder。**owner/依赖**：Config owner；依赖公开component factory。**需求/状态**：CFG-009；⚪ `PLANNED`。

### 3.6 `ProductComposer`

**文件**：`include/openhbx/config/hbf_product_composer.h`、`src/config/hbf_product_composer.cpp`。**职责/关键组件**：事务式创建、连接和回滚。**输入输出**：resolved config到唯一`OpenHbxSystem`。**owner/依赖**：Config owner；依赖02及全部公开port。**需求/状态**：CFG-010；⚪ `PLANNED`。

## 4. 高层详细设计

### 4.1 Schema与profile

schema按`system`、`host`、`address`、`controller`、`interconnect`、`media`、`ras`、`observability`分区。`product.family`固定为`HBF`，`product.profile`固定为`OCP_HBF_0_7`。profile提供协议固定值和带来源的模型默认值；前者不可覆盖，后者只有字段声明`overridable=true`才允许覆盖。

OCP PDF尚未定位的Host字段不得以常识补齐；对应键标为`SPEC_EXTRACT_REQUIRED`并使依赖该字段的正式profile构建失败。允许测试profile使用synthetic值时，resolved config必须记录`source: synthetic_test_only`。

### 4.2 派生与跨字段校验

派生值包括总容量、每Channel容量、地址上界、每DLU sector数、拓扑节点数、时钟换算比例和链路有效位宽。所有乘加、向上取整和单位换算使用checked arithmetic。验证至少覆盖：Channel不超过profile上限、物理Bank只归属一个Channel、64 B transaction不跨4 KiB、几何乘积可表示、TSV endpoint覆盖全部Core Die、controller要求NAND Program/Erase能力。

### 4.3 能力闭包

每个组件提供`ComponentDescriptor{kind, impl, provides, requires}`。validator构造需求集合并报告最小缺失能力、提供者和配置路径；它不尝试自动替换组件。HBF最小闭包含`Host.HbfUcieAxi`、`Address.HbfR1R5`、`Controller.SequentialProgram`、`Media.Nand.ProgramErase`、`Interconnect.StackVertical`和`System.AsyncCompletion`。

### 4.4 原子装配

`ProductComposer`按`Media -> Interconnect -> Address -> Controller -> Host -> RAS -> OpenHbxSystem`的依赖方向创建临时`unique_ptr`，连接只通过公开port；全部`configure/connect/seal`成功后才返回system。任一步失败按逆序析构，禁止注册future event、调用callback或泄漏payload。

## 5. 接口设计

| 接口 | 调用方向 | 输入/输出 | 所有权与时序 | 错误/backpressure |
|---|---|---|---|---|
| `resolve(ConfigNode)` | Adapter/CLI -> Resolver | 原始树 -> `Result<ResolvedHbfConfig>` | 输入借用；成功返回值对象 | 聚合路径化配置错误；无部分配置 |
| `validate(ResolvedHbfConfig)` | Composer -> Validator | config -> `ValidationReport` | 同步只读 | 不满足能力为启动错误 |
| `register_builder(ComponentKey, Builder)` | 静态注册 -> Registry | key+builder | 初始化期；重复key失败 | 不允许后注册覆盖 |
| `build(ResolvedHbfConfig)` | Adapter/runner -> Composer | config -> `unique_ptr<OpenHbxSystem>` | 成功转移system；失败无对象图 | typed build error；无运行backpressure |
| `serialize_resolved()` | artifact writer -> Config | config -> canonical YAML/hash | 只读，不推进cycle | I/O错误不改变config |

配置错误属于构造期`Rejected`，不创建request token。动态请求的Busy/Rejected由02及运行模块处理。

## 6. 数据结构设计

| 结构 | owner | 生命周期 | 核心内容 | 不变量 |
|---|---|---|---|---|
| `RawConfigTree` | loader | resolve调用期 | 原始路径和值 | 保留源位置供诊断 |
| `ProfileDefinition` | catalog | 进程期只读 | defaults、locked fields、来源 | profile名唯一 |
| `ResolvedHbfConfig` | system共享只读 | system全生命周期 | typed模块配置、derived、hash | 构造后不可变 |
| `CapabilitySet` | descriptor/config | 验证期 | typed capability与参数 | 不以自由字符串比较热路径 |
| `ValidationIssue` | report | 构造期 | code/path/value/constraint/source | 排序确定且可序列化 |
| `ComponentDescriptor` | registry | 进程期只读 | kind、impl、requires/provides | `kind+impl`唯一 |

跨模块唯一权威定义计划位于`include/openhbx/config/`；旧`OpenHbfConfig`只作为迁移输入，不形成第二份canonical config。

## 7. 关键流程与状态机

配置状态机：

| 当前状态 | 事件/条件 | Guard | 动作 | 下一状态 | 失败处理 |
|---|---|---|---|---|---|
| Empty | load | 输入可解析 | 建Raw tree | Parsed | ParseFailed |
| Parsed | select profile | 名称恰为OCP_HBF_0_7 | 加载defaults/locks | Profiled | UnsupportedProfile |
| Profiled | merge | override已声明 | 合并并记来源 | Merged | Unknown/LockedField |
| Merged | derive | checked math成功 | 计算派生值 | Derived | Overflow/InvalidUnit |
| Derived | validate | schema+能力闭包成立 | 冻结并hash | Resolved | ValidationFailed |
| Resolved | build | registry完整 | 临时创建/连接 | Built | 回滚全部临时对象 |

reset、timeout和stale completion不适用于配置状态机，因为运行事件尚未创建。重复build允许创建彼此独立的system；同一resolved config对象不被修改。

## 8. 性能与资源设计

配置不是热路径。解析复杂度目标为`O(K + C + E)`，其中`K`是键数、`C`是组件数、`E`是能力边数；内存预算为原始树、resolved树和诊断总字段大小之和。schema限制最大嵌套深度、列表长度和字符串长度，具体上限在LLD配置表冻结。canonical hash只由规范化值和schema/profile版本决定，不包含路径、pointer、wall-clock或映射迭代偶然顺序。

## 9. 错误、恢复与可观测性

配置错误使用`ConfigErrorCode`：`UnknownKey`、`MissingField`、`TypeMismatch`、`UnitRequired`、`OutOfRange`、`LockedField`、`UnsupportedProfile`、`UnknownImplementation`、`CapabilityMismatch`、`ArithmeticOverflow`和`BlockedSpec`。诊断携带JSON path、原值、约束、来源和候选值。

可观测输出包含配置hash、profile/schema版本、字段来源、能力清单、装配组件清单和synthetic字段列表。不得输出secret、pointer地址或未排序容器。构造失败不需要reset；回滚后注册表仍可用于下一次构造。

## 10. 测试与验收

| Test ID | 需求 | 层级 | Production path | 场景 | Oracle | 证据 |
|---|---|---|---|---|---|---|
| CFG-T01/T02 | CFG-001/002 | E1 | parser+catalog | HBF与非阶段profile | HBF通过；其他路径化拒绝 | E1 |
| CFG-T03/T12 | CFG-003/012 | E2 | YAML/ConfigNode->resolver | 等价输入、键顺序变化 | canonical YAML/hash相同 | E2 |
| CFG-T04/T05 | CFG-004/005 | E1 | validator+derive | fuzz边界、单位、溢出 | 精确error code且无config | E1 |
| CFG-T06/T09 | CFG-006/009 | E2 | registry+capability validator | 缺能力/重复注册 | 报告最小缺口；不覆盖builder | E2 |
| CFG-T07/T08 | CFG-007/008 | E4 | 规范vector+resolver | 固定粒度/Channel边界 | 与PDF提取表逐项一致 | E4，当前BLOCKED_SPEC |
| CFG-T10 | CFG-010 | E2 | composer+真实component factories | 第N组件构造失败 | 无发布system、无event/token泄漏 | E2 |
| CFG-T11 | CFG-011 | E2 | full resolver+serializer | synthetic/vendor覆盖 | 每字段来源可核验 | E2 |

实验执行、证据保存和状态更新统一遵循
[`实验与测试执行规范.md`](实验与测试执行规范.md)。

验收要求：全部PLANNED文件落地；OCP锁定字段完成PDF定位；合法HBF对象图可构造；所有非法组合在cycle 0前拒绝；CFG-T01至T12达到表列证据级别。文档本身不构成实现或验证证据。
