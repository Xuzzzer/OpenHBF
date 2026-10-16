# HBF Host Channel、Core Die 与 NAND Array 切片

## 1. 冻结结论

OpenHBF 将 Host Channel 解释为同一个物理 HBF NAND stack 上的互斥容量
切片，而不是物理拓扑的父节点。一个 Channel 不能访问其他 Channel 拥有的
NAND array；多个 Channel 也不会各自复制一套 Core Die、Die和Bank。

正确关系是：

```text
HBF Stack
├── Base Die / Host Channel[0..15]
└── Physical Core Die domain
    └── NAND Die[0..D-1]
        └── Bank/Plane[0..B-1]
            └── Block
                └── 4 KiB Page

Host Channel --ownership mapping--> Bank/Plane slice
```

因此物理容量为：

```text
NCDU x dies_per_core x banks_per_die x blocks_per_bank x pages_per_block
```

不能再乘 `channels`。Channel只划分该容量。

## 2. Ownership映射边界

OCP明确规定Channel访问独立容量池，并通过配置暴露Core Die、Die和Bank
映射信息；规范没有要求所有产品采用同一个Bank取模函数。因此正式
`OCP_HBF_0_7` profile必须从可定位配置字段或vendor profile解析ownership，
不能隐式使用固定公式。

用于单元测试和无vendor参数的synthetic profile可以采用均匀垂直切片：

```text
Channel 0  owns Die 0/Bank 0,  Die 1/Bank 0,  ... Die 15/Bank 0
Channel 1  owns Die 0/Bank 1,  Die 1/Bank 1,  ... Die 15/Bank 1
...
Channel 15 owns Die 0/Bank 15, Die 1/Bank 15, ... Die 15/Bank 15
```

synthetic规则为：

```text
owner_channel = physical_bank % channel_count
```

并要求 `banks_per_die` 可被 `channels` 整除。若每Die有32个Bank且有16个
Channel，则每个Channel在每颗Die上拥有两个Bank：Channel 0拥有Bank 0和16，
Channel 1拥有Bank 1和17，以此类推。

命令同时携带Channel owner和物理地址。只有满足下式才合法：

```text
command.channel == owner_channel(command.physical_bank)
```

所以Channel 0不能通过修改地址访问Channel 1的Bank 1。

## 3. NCDU

OCP `BUCCAP.NCDU` 表示每个UCIe Channel覆盖的Core Die数量，编码为
1、2、4、8或16。OpenHBF将它实现为Channel切片覆盖的物理Core Die域数。

NCDU=2时：

```text
Channel 0 owns Core 0/所有Die/Bank 0切片
              + Core 1/所有Die/Bank 0切片
Channel 1 owns Core 0/所有Die/Bank 1切片
              + Core 1/所有Die/Bank 1切片
```

NCDU=4同理，每个Channel的互斥Bank切片跨Core 0..3延伸。它不表示
`channels x NCDU`套独立物理阵列，也不允许Channel访问这些Core Die中的
其他Channel Bank切片。

字段 `Geometry::core_dies_per_channel` 暂时保留以兼容已有源码和metadata，
其值按上述NCDU语义解释，不参与Channel维度的物理容量复制。

## 4. 资源与并行性

OCP没有定义一个由所有Host Channel共享的单端口Die/CoreDie总线，因此默认
EAT不得凭空加入这种全局瓶颈。参考profile只使用：

```text
BankArray(core, die, bank)
ChannelMediaPath(channel, core)
```

`BankArray`约束物理array的Sense/Program/Verify/Erase。同一Bank严格串行，
不同Bank默认可并行。`ChannelMediaPath`约束该Channel到指定Core Die域的
DataIn/DataOut；不同Channel拥有不同path，因此不会被一个没有规范依据的
全局CoreDie path全部串行化。

`ChannelMediaPath(channel, core)`不是Host UCIe link，也不拥有TSV mapping、spare或repair FSM；
这些仍属于01/03。它只是05 synthetic NAND ingress port的EAT，不包含UCIe/ECC/TSV
payload transfer。如果vendor profile声明
共享Die/CoreDie bus，可以显式增加`VendorResource`；不得把这种产品选择冒充
OCP默认。若03 profile已对同一Core-side传输段计时，则必须关闭05的对应path，
避免同一字节重复计费。

> **实现状态注记（2026-09-20）**：本节冻结的 `BankArray(core,die,bank)` +
> `ChannelMediaPath(channel,core)` 资源模型已通过 channel 分片重构落地——
> 单体 `NandFlashDevice` 的全局 `die_eat_` 总线被每 Channel 私有的
> `ChannelMediaPath` EAT 取代。实现架构、类设计与需求追踪见
> [`HBF_CHANNEL_PARTITIONED_MODEL.md`](HBF_CHANNEL_PARTITIONED_MODEL.md)
> （CHAN-003/004）。

## 5. 状态所有权

- `MediaTopology`的PageKey只由CoreDie/Die/Bank/Block/Page构成；
- BBT以物理Block为唯一身份，不按Channel复制；
- Die温度、恢复和故障状态不按Channel复制；
- Reduced Capacity仍提供Channel-local bitmap，只列出该Channel拥有的Block；
- metadata中的Channel字段用于校验和恢复owner视图，不增加物理容量。

## 6. 规范与产品选择边界

OCP明确规定一个stack最多16个Host Channel、Channel容量隔离、NCDU能力和
跨Channel interleaving，但没有完整固定所有vendor的Bank/Plane ownership
函数。`bank % channel_count`是OpenHBX synthetic测试profile，不应被宣称为所有
HBF产品必须采用的物理布线。未来vendor profile可替换ownership mapping，
但必须继续满足：每个物理Bank恰有一个owner，Channel不能跨owner访问，物理
容量不因Host Channel数量而复制。
