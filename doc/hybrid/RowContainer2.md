# RowContainer Memory Layout 改造设计

## 动机与目标

当前 RowContainer 将 null bits 集中存放在 keys 与 accumulators 之间。
当 key 数量较多，或者 accumulator / dependent columns 占用空间较大时，
字段与对应 null bit 的距离可能很远，对 CPU cache 不友好。

新设计将 null bits 分散到字段附近：
- keys 和 payload（accumulators / dependent columns）分别按最多 8 个字段分组。
- 每组使用一个 byte 存放 null bits，优先复用该组可用的 alignment padding。
- 没有可用 padding 时，插入一个 null byte，使它到组内所有字段的 byte distance 之和最小。
- free flag  和可选 probed bit 位于 keys 的最后一个bitmap 的后面，如果byte够用，复用，不够增加一个byte。
- 把[next same-key row]放到 keys 的最后一个bitmap 的后面，避免多一次random access。


## 现有实现与约束

[RowContainer.cpp](../../bolt/exec/RowContainer.cpp) 中的现有布局大致如下：

```text
[keys][集中存放的 null bits / control flags]
[aligned accumulators][dependent columns][optional row size][optional next pointer]
[row alignment padding]
```

启用 normalized key 时，其存储位于 row 指针之前，不属于上述字段分组。

- keys 和 dependent columns 按字段宽度紧凑排列，没有逐字段的自然对齐 padding。
- accumulators 按各自的 alignment 要求对齐，最终行大小按行 alignment 向上取整。
- [RowColumn](../../bolt/exec/RowContainer.h) 已保存独立的字段 offset、null byte offset 和 null mask，可以表示分散的 null bits。
- 初始化、设置全 null 和部分序列化路径假设 null bytes 连续，需要随布局一起修改。
- 行回收时会在行首写入 free-list next pointer；free flag 必须避开这段覆盖区域。

本次设计保留字段顺序和现有对齐要求，不引入 keys / dependent columns 的额外自然对齐。

## 分组与 null bit 映射

keys 和 payload 独立分组，不跨两者的边界。payload 内保持 accumulators 在前、
dependent columns 在后的顺序，允许这两类字段落在同一组中。

每组最多包含 8 个逻辑字段，不重排字段，不拆分 accumulator 的内部存储。
每个 accumulator 整体视为一个字段。

组内第 `i` 个字段对应 bit `i`，其中 `0 <= i < 8`：

```text
nullByte(column) = group.nullByteOffset
nullMask(column) = 1 << i
nullBitOffset(column) = group.nullByteOffset * 8 + i
```

补充规则：

- 不足 8 个字段时，剩余 bits 保留，不分配给下一组的字段。
- non-nullable 字段仍占用组内字段编号，但使用 `nullMask = 0`。
- 建议全组 non-nullable 时省略 null byte；空组不分配 null byte。
- 保留现有语义：bit 为 1 表示 null；新行中 accumulator 初始为 null，keys 和 dependent columns 的初始 null bits 为 0。
- free / probed flags 使用独立元数据描述，不作为字段参与分组和距离计算。

按组取整、keys 与 payload 独立分组可能增加 null bytes 总数；
padding 复用可以抵消部分开销，但不保证新布局一定比旧布局小。

## 距离定义

采用 null byte 到字段**起始 byte**的绝对距离，各字段等权：

```text
D(b) = sum(abs(b - offset[i])), 0 <= i < groupFieldCount
```

其中：

- `b` 为 null byte 相对 row 的 byte offset。
- `offset[i]` 为插入 null byte 并完成 alignment 后，字段的最终起始 offset。
- VARCHAR 等变长字段使用行内描述符的位置，不计算行外数据的距离。
- accumulator 使用整个 accumulator 的起始位置，不对内部成员分别计分。

这里没有采用“到字段所占字节区间的最近距离”。两种定义对较大字段可能产生不同结果，
实现和测试应统一使用上述起始 byte 定义。

## 布局算法

布局在 RowContainer 构造时计算，运行时通过预先生成的 offset / mask 访问字段。
按物理顺序处理各组，使用该组实际起始 offset 进行计算。
以下算法求每组在给定起始位置下的最优位置，不承诺整行布局的全局最优。

### 1. 计算不含本组 null byte 的基准布局

使用原有字段顺序、宽度和 alignment，计算字段 offset 以及可复用的 padding 区间。

padding 必须由 RowContainer 布局管理：

- accumulator 内部的 padding 属于 accumulator，不能直接复用。
- 组间 alignment padding 必须有唯一归属，不能被相邻两组重复使用。
- 实现时建议将下一组首字段对齐之前的 padding 归属下一组，作为该组的 leading padding。
- 末尾 row alignment padding 仅在确定最终行尾后评估，不应在布局未完成时假定可用。
- 已分配给控制信息的 byte 不再是可用 padding。

### 2. 优先复用可用 padding

如果该组存在可用 padding，在这些区间内选择使 `D(b)` 最小的 byte。
距离相同则选择较小的 byte offset，保证布局确定性。

此时字段 offset 不变，也不增加空间。对于固定字段 offset，绝对距离和在中位数区间达到最小；
可以利用这一性质选择每个 padding 区间的候选点，无需逐 byte 扫描整个区间。

### 3. 没有可用 padding 时插入 null byte

对于 `m` 个字段，枚举组首、字段之间和组尾的 `m + 1` 个插入边界，最多 9 个：

1. 在该边界插入一个 null byte。
2. 重新计算后续字段的 offset 和 alignment。
3. 如果插入产生新的 alignment 间隙，允许 null byte 在保持这些字段 offset 不变的合法空闲区间内移动，并取距离最小的位置。
4. 使用最终字段 offset 计算 `D(b)`。
5. 选择距离最小的候选；距离相同时优先选择本组结束位置较小的候选，再选择较小的 null byte offset。

本组结束位置作为局部空间开销的比较依据，最终仍需重新计算后续组、行尾元数据和行 alignment。
一个新增 null byte 可能带来多个 byte 的对齐开销。

不能直接使用插入前字段 offset 的中位数决定位置：
中位数结论要求字段位置固定，而插入 byte 可能移动后续字段。

### 示例：8 个紧凑排列的 8-byte 字段

```text
插入前：
字段       F0  F1  F2  F3  F4  F5  F6  F7
offset      0   8  16  24  32  40  48  56

插入后：
布局       [F0][F1][F2][F3][N][F4][F5][F6][F7]
字段       F0  F1  F2  F3   N  F4  F5  F6  F7
offset      0   8  16  24  32  33  41  49  57
```

该示例中 null byte 位于 offset 32，组大小从 64 增至 65 bytes。
如果这些字段是要求 8-byte alignment 的 accumulators，`F4` 必须移动到 offset 40，
需要按重新对齐后的布局计算距离与空间开销。

## 元数据与初始化

保留 `RowColumn` 的访问方式，将新的字段 offset 和 null bit 物理位置写入现有描述。
额外维护按物理 byte 去重的 null byte 描述表，建议包含：

| 属性 | 用途 |
| --- | --- |
| byte offset | 定位行内 null byte |
| field null mask | 标识该 byte 中有效的字段 null bits |
| initial value | 初始化或复用行时恢复字段 null bits |

控制位另行描述。如果后续实现选择让控制位复用 null byte 的剩余 bits，
必须合并初始化值，并分别处理字段位和控制位的更新，不能互相覆盖。

`initializeRow()` 不能再从第一个 null byte 开始整体 `memcpy`，
`setAllNull()` 也不能再对这段内存整体 `memset`，否则可能覆盖穿插其中的字段。
应按描述表写入各个 null byte，并显式保留原有 free / probed 状态转换语义。
连续的 null bytes 可以在确认安全后合并写入。

## 代码改造范围

| 位置 | 改造内容 |
| --- | --- |
| `RowContainer` 构造函数 | 分组、padding 归属、候选布局计算，生成最终 offset / mask |
| `RowColumn` / `nullOffsets_` | 使用新的物理位置，消除 null bits 连续的隐含假设 |
| `initializeRow()` / `setAllNull()` | 按 null byte 描述表初始化或设置字段 null bits |
| `extractSerializedRows()` / `storeSerializedRow()` | 在分散的物理 null bits 与连续的逻辑 bitmap 之间转换 |
| free / probed flags | 独立记录位置，保证 free flag 不被行首 free-list pointer 覆盖 |
| `rowIdOffset_` | 从最终 dependent column offset 获取，不能沿用插入前的布局游标 |
| row size / next pointer / normalized key | 使用最终布局重新计算位置、行大小与对齐，保留原有生命周期语义 |
| aggregate / JIT 消费方 | 核查通过 offset / mask 访问的路径，排除对旧布局的硬编码假设 |

序列化建议保留连续的逻辑 bitmap，将内存布局与序列化格式解耦：
写出时按逻辑字段映射打包，读入时反向映射，不能直接复制分散的物理 null bytes。
需要明确保留既有格式中的 bit 编号、长度和控制位语义，不能由新的物理分组推导这些信息。

原始固定行拷贝路径也需要检查源和目标是否使用相同布局；
已有的 `RowFormatInfo` 等布局描述必须与实际行内存一致。

`BmRowLayout` 是另一套布局实现。如果后续将该设计扩展到 BmRowContainer，
还需修改其 null 初始化和 block schema 中的连续 bitmap 描述；本设计不默认同时改造该实现。

## 验证计划

### 正确性

- 覆盖 0、1、7、8、9、16、17 个字段，以及 keys / payload 独立分组的边界。
- 覆盖全 non-nullable、混合 nullable、末组不足 8 个字段、无 key 和无 payload。
- 覆盖混合字段宽度、不同 accumulator alignment、多个 padding 区间和插入引起的重新对齐。
- 检查字段、null bytes、控制信息互不重叠；各 accumulator 和行 stride 满足对齐约束。
- 用小规模穷举校验候选选择符合 padding 优先和距离最小规则，以及相同输入产生相同布局。
- 覆盖新行初始化、`setAllNull()`、erase / free-list reuse、probed flag 和 normalized key。
- 覆盖变长字段释放、聚合状态初始化与销毁、hybrid rowId 存取、序列化往返和原始行拷贝。
- 验证受影响的 Presto / Spark 模式；启用 JIT 时验证生成代码使用新的 offset / mask。

### 性能

在相同 schema、数据和 null 分布下，对比旧布局与新布局：

- `fixedRowSize`、null bytes 数量、padding 开销和总内存占用。
- 新行初始化、行复用、store / extract 和序列化吞吐。
- hash join、group by 和 sort 等实际访问路径的吞吐与 cache misses。
- 窄行、宽行、大 accumulator、不同 null 比例，以及不同工作集大小。

同时观察读取局部性收益和分散初始化带来的写入成本；
不得仅凭 byte distance 下降判断整体性能改善。
