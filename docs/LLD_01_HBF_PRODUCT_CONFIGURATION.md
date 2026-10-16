# OpenHBX HBF 产品配置与装配底层设计

**文档版本**：V1.0
**文档位置**：`docs/LLD_01_HBF_PRODUCT_CONFIGURATION.md`
**对应HLD完整路径**：`docs/HLD_01_HBF_PRODUCT_CONFIGURATION.md`
**客观状态**：🟡 `PARTIAL`；解析、profile、resolved config、registry和composer已实现，完整系统装配证据仍待补齐。规范字段提取项为🔴 `BLOCKED_SPEC`
**适用基线**：OCP HBF v0.7.0；当前 `thirdparty/ramulator2`
**覆盖需求**：`CFG-001`至`CFG-012`

## 修订历史

| 版本 | 日期 | 修订说明 |
|---|---|---|
| V1.0 | 2026-08-28 | 首次给出HBF-only配置、能力验证与对象图逐文件设计 |

**目录与存储位置**

| 项目 | 仓库相对位置 | 内容 |
|---|---|---|
| 本LLD | `docs/LLD_01_HBF_PRODUCT_CONFIGURATION.md` | 实现级权威设计 |
| 对应HLD | `docs/HLD_01_HBF_PRODUCT_CONFIGURATION.md` | 边界与需求权威来源 |
| 计划公开头文件 | `include/openhbx/config/` | 配置值、profile、capability、composer API |
| 计划实现 | `src/config/` | 解析、校验、注册与装配实现 |
| 计划测试 | `tests/unit/config/`、`tests/component/config/` | E1/E2 oracle |
| 正式artifact | `build/artifacts/<suite>/<experiment-id>/<run-id>/` | resolved config、manifest与验证证据；目录须被`.gitignore`覆盖 |

## 1. 模块概述与约束

本LLD实现`HLD_01_HBF_PRODUCT_CONFIGURATION.md`定义的配置边界。输入是Ramulator `ConfigNode`或YAML解析树，输出是唯一不可变`ResolvedHbfConfig`和完整`OpenHbxSystem`。本阶段不注册HBM/LPDDR builder；请求运行状态归02及后续模块。

规范字段精确定位未完成的键使用`BLOCKED_SPEC`，不得用实现默认值冒充OCP要求。OpenHBX core保持C++17；Ramulator类型仅出现在adapter/converter边界。

## 2. 需求到实现映射

| 需求ID | HLD章节 | 实现文件/符号 | 状态/算法 | Test ID | 当前状态 |
|---|---|---|---|---|---|
| CFG-001/002 | 2,4.1 | `HbfProductProfileCatalog::find` | allowlist | CFG-T01/T02 | PLANNED |
| CFG-003 | 1,4.1 | `HbfConfigResolver::resolve` | 五阶段流水 | CFG-T03 | PLANNED |
| CFG-004 | 4.1 | `HbfConfigSchema::validate` | strict schema | CFG-T04 | PLANNED |
| CFG-005 | 4.2 | `derive_hbf_values` | checked arithmetic | CFG-T05 | PLANNED |
| CFG-006 | 4.3 | `CapabilityValidator::validate` | requirement closure | CFG-T06 | PLANNED |
| CFG-007/008 | 4.1/4.2 | `make_ocp_hbf_0_7_profile` | locked fields | CFG-T07/T08 | 🔴 `BLOCKED_SPEC` |
| CFG-009 | 4.3 | `ComponentRegistry` | typed key registry | CFG-T09 | PLANNED |
| CFG-010 | 4.4 | `HbfProductComposer::build` | transactional builder | CFG-T10 | PLANNED |
| CFG-011/012 | 4.1 | `CanonicalConfigWriter` | sorted serialization/hash | CFG-T11/T12 | PLANNED |

## 3. 核心类型与数据结构

```cpp
enum class ProductProfile { OcpHbf0_7 };
enum class ComponentKind { Host, Address, Controller, Interconnect, Media, Ras };
enum class ConfigOrigin { SchemaDefault, ProductProfile, UserOverride, Derived, SyntheticTest };

struct SourcedValue { ConfigValue value; ConfigOrigin origin; SourceLocation source; };
struct ComponentKey { ComponentKind kind; std::string impl; };
struct Capability { CapabilityId id; std::optional<std::int64_t> parameter; };
struct ValidationIssue { ConfigErrorCode code; ConfigPath path; std::string constraint; SourceLocation source; };
```

| 字段/类型 | 类型/单位 | 合法范围/默认值 | owner与生命周期 | 不变量 |
|---|---|---|---|---|
| `product.profile` | enum | `OCP_HBF_0_7` | resolved config/system期 | 唯一受支持值 |
| `host.transaction_bytes` | byte | 64，locked | profile | 不可override |
| `controller.dlu_bytes` | byte | 4096，locked | profile | 可整除transaction |
| `topology.host_channels` | count | 1..16，默认待规范确认 | user/profile | 每Bank唯一owner |
| `system.clock_ratio` | integer | >=1 | system config | Ramulator调度整数比 |
| `media.geometry.*` | count | >0 | media config | 乘积不溢出 |
| `ResolvedHbfConfig::hash` | digest | SHA-256或项目统一算法 | config全生命周期 | canonical字节相同则相同 |

`ResolvedHbfConfig`只暴露const访问器；其子配置值类型不持有`ConfigNode`引用。诊断中的源位置只用于报告，不进入canonical hash。

## 4. 头文件与公开边界

| 头文件 | 可见性 | 导出内容 | 允许依赖 | 禁止内容 |
|---|---|---|---|---|
| `include/openhbx/config/hbf_config_schema.h` | internal | schema node与validator | common result/config | 运行组件类型 |
| `include/openhbx/config/hbf_product_profile.h` | public | profile ID/definition/catalog | value types | 可变全局profile |
| `include/openhbx/config/resolved_hbf_config.h` | public | module config views/hash | common types | Ramulator header |
| `include/openhbx/config/capability.h` | public | capability ID/set/descriptor | STL value types | 自由字符串逻辑 |
| `include/openhbx/config/component_registry.h` | internal | registry/builder契约 | component forward declarations | 模块私有头 |
| `include/openhbx/config/hbf_product_composer.h` | public | `build()` | resolved config/system fwd | 组件内部状态 |
| `include/openhbx/config/ramulator_config_converter.h` | adapter-only | ConfigNode转换 | Ramulator ConfigNode | 产品运行逻辑 |

旧`include/openhbf/integration/open_hbf_config.h`迁移期只转发或转换，不得继续拥有另一套默认值。

## 5. 函数与接口详细设计

### 5.1 `Result<ResolvedHbfConfig> HbfConfigResolver::resolve(const RawConfigTree&)`

**方向**：CLI/adapter -> Resolver。输入借用，返回值拥有全部规范化数据。前置条件仅为树可遍历。成功执行profile选择、merge、derive、schema及能力前置校验；失败返回按`path, code`稳定排序的issues，不缓存部分结果。对应CFG-T03/T04/T05。

### 5.2 `ValidationReport CapabilityValidator::validate(const ResolvedHbfConfig&, span<const ComponentDescriptor>) const`

同步只读。对每个requirement寻找同参数语义的provider，并校验唯一owner约束。报告全部可独立诊断问题；不修改config、不替换impl。对应CFG-T06。

### 5.3 `Result<void> ComponentRegistry::add(ComponentKey, ComponentBuilder)`

只允许进程初始化阶段调用。key及builder所有权转入registry；重复key返回`DuplicateRegistration`且旧builder保持不变。seal后注册失败。对应CFG-T09。

### 5.4 `Result<std::unique_ptr<OpenHbxSystem>> HbfProductComposer::build(const ResolvedHbfConfig&)`

按依赖顺序从registry创建组件。每个builder只取得所属config view；成功后`connect()`，最终由`OpenHbxSystem::create()`接收所有`unique_ptr`。失败销毁局部对象，不创建事件和completion。对应CFG-T10。

### 5.5 `CanonicalArtifact CanonicalConfigWriter::write(const ResolvedHbfConfig&)`

只读同步接口，按schema固定键序和明确单位序列化；输出YAML字节、hash和来源manifest。I/O由调用者完成，因此writer失败不会产生半写文件。对应CFG-T11/T12。

## 6. 内部逻辑、状态机与算法

### 6.1 resolve算法

```text
parse raw tree
if profile != OCP_HBF_0_7: UnsupportedProfile
copy schema defaults; overlay profile
for each user key in sorted path order:
  require declared && overridable && typed
derive with checked arithmetic
validate field and cross-field constraints
canonicalize; compute hash; freeze
```

错误聚合不改变后续字段检查的输入；依赖失败的派生字段跳过并产生一条`DependencyInvalid`，避免级联噪声。

### 6.2 装配事务状态机

| 当前状态 | 事件/条件 | Guard | 动作/副作用 | 下一状态 | 失败处理 |
|---|---|---|---|---|---|
| Created | validate | report empty | seal registry view | Validated | return report |
| Validated | create component | builder exists | 保存临时unique_ptr | Creating | 析构已有对象 |
| Creating | next | previous成功 | 继续拓扑序创建 | Creating/Connecting | 析构已有对象 |
| Connecting | connect port | capability匹配 | 建非 owning port | Connecting/Sealing | 断开并析构 |
| Sealing | create system | 所有port完整 | 转移全部owner | Published | 析构局部对象 |
| Published | return | always | 返回唯一system | Terminal | 不适用 |

没有运行时Busy；`build()`只有成功或typed rejection。builder不得调用`EventQueue::schedule`，该规则由fake event sink在CFG-T10检查。

## 7. 逐文件设计

### 7.1 文件总表

| 路径 | 动作 | owner | 主要职责 | 主要类型/API | 依赖 | 需求 | 测试 | 状态 |
|---|---|---|---|---|---|---|---|---|
| `include/openhbx/config/hbf_config_schema.h`、`src/config/hbf_config_schema.cpp` | 新增 | Config | strict schema | `HbfConfigSchema` | common config | CFG-004/005 | CFG-T04/T05 | PLANNED |
| `include/openhbx/config/hbf_product_profile.h`、`src/config/hbf_product_profile.cpp` | 新增 | Config | HBF profile | `ProfileDefinition` | schema | CFG-001/002/007/008 | CFG-T01/T02/T07/T08 | PLANNED/BLOCKED_SPEC |
| `include/openhbx/config/resolved_hbf_config.h`、`src/config/resolved_hbf_config.cpp` | 新增 | Config | merge/derive/freeze | `ResolvedHbfConfig` | checked math | CFG-003/005/011/012 | CFG-T03/T05/T11/T12 | PLANNED |
| `include/openhbx/config/capability.h`、`src/config/capability.cpp` | 新增 | Config | typed closure | `CapabilityValidator` | descriptors | CFG-006 | CFG-T06 | PLANNED |
| `include/openhbx/config/component_registry.h`、`src/config/component_registry.cpp` | 新增 | Config | builder registry | `ComponentRegistry` | factories | CFG-009 | CFG-T09 | PLANNED |
| `include/openhbx/config/hbf_product_composer.h`、`src/config/hbf_product_composer.cpp` | 新增 | Config | 原子装配 | `HbfProductComposer` | all public ports | CFG-010 | CFG-T10 | PLANNED |
| `include/openhbx/config/ramulator_config_converter.h`、`src/config/ramulator_config_converter.cpp` | 新增 | Adapter | ConfigNode边界 | `to_raw_tree` | Ramulator | CFG-003/012 | CFG-T03/T12 | PLANNED |
| `tests/unit/config/test_hbf_config.cpp` | 新增 | Verification | schema/profile/derive | fixtures | production resolver | CFG-001..008 | CFG-T01..08 | PLANNED |
| `tests/component/config/test_hbf_composer.cpp` | 新增 | Verification | registry/object graph | fail builders | production composer | CFG-006/009/010 | CFG-T06/T09/T10 | PLANNED |
| `tests/component/config/test_config_parity.cpp` | 新增 | Verification | YAML/ConfigNode parity | equivalent trees | converters | CFG-011/012 | CFG-T11/T12 | PLANNED |

### 7.2 生产文件逐项解析

#### `HbfConfigSchema`（`include/openhbx/config/hbf_config_schema.h`、`src/config/hbf_config_schema.cpp`）

**职责与owner**：Config模块唯一维护字段树、类型、显式单位、范围、锁定属性及容器规模；不保存profile值。**API**：`validate_tree()`、`field_at()`。**输入输出**：Raw tree输入，输出typed draft或稳定排序的`ValidationIssue`。**状态与所有权**：schema进程期只读，draft由调用者取得。**错误与边界**：unknown key、类型、单位、深度和列表超限均路径化拒绝。**依赖**：common config/result；禁止依赖运行组件。**需求/测试/状态**：CFG-004/005，CFG-T04/T05，`PLANNED`。

#### `HbfProductProfileCatalog`（`include/openhbx/config/hbf_product_profile.h`、`src/config/hbf_product_profile.cpp`）

**职责与owner**：Config模块唯一提供`OCP_HBF_0_7` defaults、locked fields和来源；不解析用户输入。**API**：`find()`、`make_ocp_hbf_0_7_profile()`。**输入输出**：profile ID输入，输出只读definition。**状态与所有权**：catalog拥有不可变profile表。**错误与边界**：HBM/LPDDR只形成`UnsupportedProfile`；未提取字段形成`BlockedSpecField`。**依赖**：schema和OCP提取表。**需求/测试/状态**：CFG-001/002/007/008，CFG-T01/T02/T07/T08，`PLANNED/BLOCKED_SPEC`。

#### `ResolvedHbfConfig`（`include/openhbx/config/resolved_hbf_config.h`、`src/config/resolved_hbf_config.cpp`）

**职责与owner**：Config模块唯一拥有merge、派生、冻结、canonical序列化和hash；不暴露setter。**API**：`HbfConfigResolver::resolve()`、typed config views、`serialize_canonical()`。**输入输出**：draft/profile/override输入，输出immutable config及来源manifest。**状态与所有权**：返回值拥有全部子配置并随system存活。**错误与边界**：locked override、非法单位、依赖无效和算术溢出不产生部分config。**依赖**：schema、profile、checked math。**需求/测试/状态**：CFG-003/005/011/012，CFG-T03/T05/T11/T12，`PLANNED`。

#### `CapabilityValidator`（`include/openhbx/config/capability.h`、`src/config/capability.cpp`）

**职责与owner**：Config模块唯一定义封闭`CapabilityId`、descriptor和需求闭包；不创建组件。**API**：`validate()`、`satisfies()`。**输入输出**：resolved config与descriptor集合输入，输出`ValidationReport`。**状态与所有权**：同步无运行状态，report归调用者。**错误与边界**：缺provider、参数不匹配和多owner均指出consumer、provider及路径。**依赖**：typed descriptor/value types。**需求/测试/状态**：CFG-006，CFG-T06，`PLANNED`。

#### `ComponentRegistry`（`include/openhbx/config/component_registry.h`、`src/config/component_registry.cpp`）

**职责与owner**：Config模块唯一管理`ComponentKey -> ComponentBuilder`注册和seal；不决定产品组合。**API**：`add()`、`find()`、`seal()`。**输入输出**：typed key/builder输入，输出builder只读引用或错误。**状态与所有权**：registry取得builder所有权，seal后只读。**错误与边界**：重复key、未知key和seal后注册不得覆盖现状。**依赖**：component公开forward declaration。**需求/测试/状态**：CFG-009，CFG-T09，`PLANNED`。

#### `HbfProductComposer`（`include/openhbx/config/hbf_product_composer.h`、`src/config/hbf_product_composer.cpp`）

**职责与owner**：Config模块执行HBF拓扑序创建、连接、seal和事务回滚；不访问Bank/Page/queue私有状态。**API**：`build()`。**输入输出**：immutable config输入，输出唯一`OpenHbxSystem`。**状态与所有权**：构造期临时持有全部`unique_ptr`，成功整体转移，失败逆序释放。**错误与边界**：builder/port/connect失败不发布system、不创建event/token。**依赖**：registry及各模块公开port。**需求/测试/状态**：CFG-010，CFG-T10，`PLANNED`。

#### `RamulatorConfigConverter`（`include/openhbx/config/ramulator_config_converter.h`、`src/config/ramulator_config_converter.cpp`）

**职责与owner**：Adapter边界唯一把当前`ConfigNode` map/sequence/scalar复制为Raw tree；不补默认值或调用Factory。**API**：`to_raw_tree()`。**输入输出**：借用ConfigNode，返回拥有值和路径的Raw tree。**状态与所有权**：无缓存，结果归调用者。**错误与边界**：不支持的node形态和深度超限路径化拒绝。**依赖**：Ramulator ConfigNode与config value types。**需求/测试/状态**：CFG-003/012，CFG-T03/T12，`PLANNED`。

### 7.3 测试文件解析

`test_hbf_config.cpp`使用表驱动字段边界、溢出和locked override；oracle是精确code/path及无resolved值，最高E1。`test_hbf_composer.cpp`仅在component factory边界使用故障builder，检查析构计数、零event/token和无published system，最高E2。`test_config_parity.cpp`经过两类真实converter和resolver，检查canonical bytes/hash，最高E2。

## 8. 配置、错误、统计与Debug

### 8.1 配置键

| 键 | 类型/单位 | 默认/范围 | 验证 | 动态性 |
|---|---|---|---|---|
| `product.family` | enum | `HBF` locked | 其他拒绝 | 静态 |
| `product.profile` | enum | `OCP_HBF_0_7` | 其他拒绝 | 静态 |
| `host.transaction_bytes` | byte | 64 locked | 不可覆盖 | 静态 |
| `controller.dlu_bytes` | byte | 4096 locked | transaction整除 | 静态 |
| `topology.host_channels` | count | profile值，1..16 | ownership可构造 | 静态 |
| `system.clock_ratio` | ratio | 1，>=1 | 可表示 | 静态 |
| `media.geometry.*` | count | profile/synthetic | >0、乘积安全 | 静态 |
| `model.media_max_inflight` | count | 256 | >0 | 静态 |
| `interconnect.impl` | enum | `HbfTsvBaseline` | 能力闭包 | 静态 |

`model.media_max_inflight` 的语义为**每 channel slice** 的 in-flight 上限（CHAN-010，ADR-CHAN-005）：Channel 分片后介质前端物理独立，全局介质 in-flight 上限 = `media_max_inflight` × `topology.host_channels`，不再是跨 channel 共享池。详见 [`HBF_CHANNEL_PARTITIONED_MODEL.md`](HBF_CHANNEL_PARTITIONED_MODEL.md) §4/§6。

同样的 per-channel 语义适用于其余前端预算键（CHAN-013，ADR-CHAN-009，2026-10-15）：`model.pal_max_inflight` 为**每 channel** 的 PAL 在途上限（cube 总在途 = 该值 × `topology.host_channels`）；`model.controller_pending_dlu` 为**每 channel** 的 DLU 聚合器待聚合上限；`model.controller_ecc_credits` 为**每 channel** 的 ECC decode credit 数；`model.controller_queue_depth` 维持 **per-Bank** 语义（Bank 唯一归属 channel，天然隔离）；Fabric `queue_depth` 按路由携带的资源计费（生产装配中一条路由恰好携带一个 Channel 路径资源），不再是 cube 级全局池。

错误映射遵循HLD第9章。配置统计不进入运行吞吐计数；manifest记录`config.validation_issue_count`、字段来源计数和组件清单。Debug dump按path排序，隐藏环境secret，不含pointer或wall-clock。

## 9. 测试用例设计

| Test ID | Objective/输入 | Production path/fake边界 | Oracle | Budget/证据 |
|---|---|---|---|---|
| CFG-T01 | 最小合法HBF配置 | real catalog+resolver | profile/config冻结 | 100键，E1 |
| CFG-T02 | HBM2/HBM3/LPDDR5 | real resolver | UnsupportedProfile及精确path | 3 case，E1 |
| CFG-T03 | defaults/profile/override优先级 | real resolver | 每字段值和origin | 500键，E1/E2 |
| CFG-T04 | unknown/type/unit/range/lock | real schema | code/path稳定 | table+fuzz seed，E1 |
| CFG-T05 | 容量边界和溢出 | real derive | checked result或ArithmeticOverflow | 1000 case，E1 |
| CFG-T06 | 缺Media/TSV能力 | real validator；descriptor fixture | 最小缺失集合 | E1/E2 |
| CFG-T07/T08 | 64B/4KiB/Channel规范vector | real profile | 与PDF冻结表一致 | E4，BLOCKED_SPEC |
| CFG-T09 | 重复/未知builder | real registry | 不覆盖旧builder | E1 |
| CFG-T10 | 每个构造/连接点注错 | real composer；fault builder边界 | owner析构、0 event、无system | E2 |
| CFG-T11/T12 | 来源与入口等价 | real converters/resolver/writer | YAML/hash完全相等 | E2 |

实验执行、证据保存和状态更新统一遵循
[`实验与测试执行规范.md`](实验与测试执行规范.md)。正式E2测试默认由Conda/CMake/CTest执行，也可使用Docker复现；artifact额外保存canonical config、validation report和实际执行环境身份。

## 10. 实施顺序、ADR与完成定义

实施顺序：冻结PDF字段表；实现value/schema；实现profile/resolver；实现capability/registry；实现composer；接入ConfigNode；补齐单元、组件和规范测试。

| ADR ID | 决策 | 背景/约束 | 备选方案 | 选择理由 | 代价/后果 | 状态 |
|---|---|---|---|---|---|---|
| ADR-01-001 | `kind+impl` typed registry | 长期OpenHBX扩展 | 散落if/else | 可验证且隔离产品 | 初期类型较多 | PLANNED |
| ADR-01-002 | transactional composition | 多组件构造可能失败 | 边建边发布 | 失败零运行副作用 | 需builder阶段约束 | PLANNED |
| ADR-01-003 | 本阶段只注册HBF | 任务目标是协议完整HBF | 同时做HBM/LPDDR | 降低未验证语义 | 其他profile明确拒绝 | PLANNED |
| ADR-01-004 | canonical config不可变 | 重放与确定性 | 运行时修改几何 | hash可作为证据 | 动态策略需事件化另设接口 | PLANNED |

完成定义：表中生产文件、CMake接入和测试文件落地；`OCP_HBF_0_7`所有locked字段有PDF定位；非法profile和非法组合在构造期拒绝；composer只创建一个合法HBF对象图；CFG-T01至T12按要求形成证据；没有`BLOCKED_SPEC`残留在声称支持的路径。当前仅完成设计，状态保持`PLANNED/BLOCKED_SPEC`。
