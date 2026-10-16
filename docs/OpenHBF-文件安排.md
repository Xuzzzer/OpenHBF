# OpenHBX TASK1文件安排与所有权总索引

**版本**：V3.0

**TASK1范围**：`OCP_HBF_0_7`

## 1. 文档定位

本文是8份专项LLD的总索引。具体生产文件、测试文件、API、依赖和状态以对应LLD第7章“逐文件设计”为唯一权威来源。文件拆分变化时先修订专项LLD，再同步本文模块描述；不以总体文档冻结未经实现验证的文件数量。

OpenHBX是项目名。本轮只安排HBF产品实现，不创建HBM、LPDDR或DRAM状态机文件。

## 2. 权威文档

| 编号 | HLD | LLD | 状态 |
|---|---|---|---|
| 01 | `HLD_01_HBF_PRODUCT_CONFIGURATION.md` | `LLD_01_HBF_PRODUCT_CONFIGURATION.md` | `PLANNED` |
| 02 | `HLD_02_SYSTEM_INTEGRATION_EVENT_KERNEL.md` | `LLD_02_SYSTEM_INTEGRATION_EVENT_KERNEL.md` | `PLANNED` |
| 03 | `HLD_03_OCP_HBF_HOST_PROTOCOL.md` | `LLD_03_OCP_HBF_HOST_PROTOCOL.md` | `PLANNED`/`BLOCKED_SPEC` |
| 04 | `HLD_04_HBF_ADDRESS_TOPOLOGY.md` | `LLD_04_HBF_ADDRESS_TOPOLOGY.md` | `PLANNED`/`BLOCKED_SPEC` |
| 05 | `HLD_05_BASE_DIE_FLASH_CONTROLLER.md` | `LLD_05_BASE_DIE_FLASH_CONTROLLER.md` | `PLANNED`/`BLOCKED_SPEC` |
| 06 | `HLD_06_INTERCONNECT_FLASH_PAL.md` | `LLD_06_INTERCONNECT_FLASH_PAL.md` | `PLANNED`/`BLOCKED_SPEC` |
| 07 | `HLD_07_NAND_MEDIA_RAS.md` | `LLD_07_NAND_MEDIA_RAS.md` | `PLANNED` |
| 08 | `HLD_08_VERIFICATION_CONFORMANCE.md` | `LLD_08_VERIFICATION_CONFORMANCE.md` | `PLANNED`/`BLOCKED_SPEC` |

旧六模块HLD/LLD已被以上文档取代，不再作为文件计划或状态依据。

## 3. 唯一执行路径

```text
YAML / Ramulator / Trace / Synthetic
  -> 01 Profile and Composition
  -> 02 OpenHbxSystem / EventKernel
  -> 03 OCP HBF Host Protocol
  -> 04 Address and Topology
  -> 05 Base Die Controller
  -> 06 Interconnect and Flash PAL
  -> 07 NAND Media and RAS
  -> terminal completion
```

08通过公开接口、trace和artifact观察该路径。任何adapter、app或测试不得绕过`OpenHbxSystem::try_submit()`，不得建立私有EventQueue、固定延迟Media或第二套Controller/PAL。

## 4. 模块路径边界

### 4.1 01 配置与装配

建议根路径为：

```text
include/openhbx/config/
src/config/
configs/products/
tests/unit/config/
```

只注册`OCP_HBF_0_7`。01拥有schema、profile、resolved config、capability report和composer；不拥有运行时请求状态。

### 4.2 02 系统与事件内核

建议根路径为：

```text
include/openhbx/common/
include/openhbx/system/
src/common/
src/system/
src/integration/
apps/
tests/unit/system/
tests/integration/system/
```

02拥有cycle、EventQueue、token/generation、结构化event journal、RequestBridge和Ramulator callback lifetime；不实现HBF协议或Media行为。日志专项说明见[`OPENHBX_LOGGING_AND_OBSERVABILITY.md`](OPENHBX_LOGGING_AND_OBSERVABILITY.md)。

Channel 分片重构在 02 侧新增 `include/openhbx/system/handler_ids.h`（HandlerId 分段分配）与 `EventQueue::discard_generation(HandlerId, Generation)` 作用域丢弃，见 [`HBF_CHANNEL_PARTITIONED_MODEL.md`](HBF_CHANNEL_PARTITIONED_MODEL.md) §5.6/§5.7。

### 4.3 03 Host协议

建议根路径为：

```text
include/openhbx/hbf/host/
src/hbf/host/
tests/unit/hbf/host/
tests/component/hbf/host/
```

03拥有Host Channel、UCIe/AXI transaction-level状态、packet、ordering和response gate；不拥有地址公式或DLU。

### 4.4 04 地址与拓扑

建议根路径为：

```text
include/openhbx/hbf/address/
src/hbf/address/
tests/unit/hbf/address/
tests/component/hbf/address/
```

04拥有Channel ownership、A1/R1-R5、BlockSequence/replay view、ZoneMap和retirement view；不拥有payload或NAND EAT。

### 4.5 05 Base Die控制器

建议根路径为：

```text
include/openhbx/hbf/controller/
src/hbf/controller/
tests/unit/hbf/controller/
tests/component/hbf/controller/
```

05拥有DLU、scheduler、Bank cache、ECC、Scratchpad、Admin和lifecycle；经06 PAL访问介质，不保存TSV或NAND权威状态。

### 4.6 06 互联与PAL

建议根路径为：

```text
include/openhbx/interconnect/
include/openhbx/pal/
src/interconnect/
src/pal/
tests/unit/interconnect/
tests/component/pal/
```

06拥有package/TSV route、transfer EAT、spare/repair epoch和FlashPhysicalRequest生命周期；不拥有Page/Block/payload。

### 4.7 07 NAND与RAS

建议根路径为：

```text
include/openhbx/media/nand/
include/openhbx/ras/
src/media/nand/
src/ras/
tests/unit/media/nand/
tests/unit/ras/
tests/component/media/
```

07拥有NAND物理状态、payload、stage、Media EAT、BBT和raw reliability。ECC/Host status策略仍归05，TSV fault归06。

Channel 分片重构在 07 侧新增 `include/openhbx/media/nand/{die_domain,nand_channel_slice,nand_media_router}.h` 与 `src/media/nand/{die_domain,nand_channel_slice}.cpp`（`NandMediaRouter` 无状态，header-only）；单体 `NandFlashDevice` 退役。测试侧新增 `tests/component/media/test_channel_partition.cpp`（CHAN 系列分片不变量，E2）。逐文件权威设计见 LLD_07 第7章与 [`HBF_CHANNEL_PARTITIONED_MODEL.md`](HBF_CHANNEL_PARTITIONED_MODEL.md) §5.3-5.5。

### 4.8 08 验证

建议根路径为：

```text
include/openhbx/verification/
src/verification/
tests/system/
tests/conformance/
tests/fixtures/
```

08拥有reference model、vector、oracle、manifest和artifact schema，不拥有生产状态或生产fallback。

## 5. 唯一状态Owner

| 状态 | 唯一owner |
|---|---|
| schema、profile、resolved config、capability report | 01 |
| Ramulator Request副本、callback lifetime | 02 RequestBridge |
| 全局cycle、event、sequence、generation | 02 EventKernel |
| Host admission、AXI ordering、response gate | 03 |
| Channel ownership、R1-R5、sequence/replay/ZoneMap | 04 |
| pending DLU、Bank cache、ECC job、Admin/lifecycle | 05 |
| PAL context、route/TSV EAT、mapping/spare/repair | 06 |
| Page/Block/payload、Media EAT、BBT/raw reliability | 07 |
| reference result、oracle、artifact schema | 08 |

Owner之外的模块只持有不可变snapshot、稳定token、generation和窄port，不得包含其他模块的内部头文件以直接修改状态。

## 6. 依赖规则

生产依赖方向为`01 -> 02 -> 03 -> 04/05 -> 06 -> 07`。04与05通过公开reservation/completion契约协作；04不直接issue Media，05不复制mapping状态。02可以向各模块分发event/reset，但不读取其私有状态。

反向completion只能通过公开端口和EventKernel：07返回06，06返回05，05提交04的reservation结果并返回03，03完成response后由02执行callback。

## 7. 文件变更规则

- 新生产文件必须在一个LLD第7章中有且只有一个owner；
- 跨模块公共类型放在最小权威模块，不建立`misc`或重复types；
- header/source成对时LLD必须分别说明公开契约和实现职责；
- 测试文件必须记录production path、fixture、fake边界、oracle和证据等级；
- 文件存在只支持`PLANNED`设计落点，不能自动支持`IMPLEMENTED`或`VERIFIED`；
- 构建、测试和artifact位置遵循[`实验与测试执行规范.md`](实验与测试执行规范.md)。
