# RowContainer 字段与 accumulator 的地址对齐

`RowContainer` 只根据 accumulator 声明的要求做字段级对齐；普通 key、
dependent/payload 列和行内元数据，没有逐字段做自然对齐。
整行起始地址和行大小也会配合 accumulator 的要求对齐。

这里需要区分“当前布局保证对齐”和“C++ 类型本身的对齐要求”。

## 行内各部分如何处理

布局计算见 [RowContainer.cpp](../../bolt/exec/RowContainer.cpp) 的构造函数。

| 行内部分 | 当前布局的对齐规则 |
| --- | --- |
| Key 列 | 按 `typeKindSize()` 连续排列，不按各列 `alignof(T)` 补齐 |
| null bits、probed flag、free flag | 按 bit 存放，合并为若干字节 |
| Accumulator | 每个 accumulator 开始前，按它自己的 `alignment()` 补齐 |
| Dependent/payload 列，包括 hybrid `rowId` | 按类型宽度连续排列，不单独补齐 |
| 可选的 `uint32_t rowSize` | 直接接在列后，不单独做 4 字节对齐 |
| 可选的 `next` 指针 | 直接接在后面，不单独做指针对齐 |
| 行尾 padding | 将 `fixedRowSize_` 补齐到整行 `alignment_` |
| Normalized-key 前缀 | 前缀占用空间补齐到整行 `alignment_`，保证加上前缀后行首仍然对齐 |

代码中的：

```cpp
offset = std::max<int32_t>(offset, sizeof(void*));
```

只是确保 key 区至少能容纳 free-list 指针，不是把 offset 向上对齐到 8 的倍数。

## 哪些 accumulator 明确要求对齐

默认 [Aggregate::accumulatorAlignmentSize()](../../bolt/exec/Aggregate.h)
返回 `1`，即不要求额外 padding。当前生产聚合实现中，明确返回更强对齐要求的有：

| Accumulator / 聚合实现 | 声明的对齐要求 |
| --- | --- |
| [MinMaxAggregate<int128_t>](../../bolt/functions/prestosql/aggregates/MinMaxAggregates.cpp) | `sizeof(int128_t)`，16 字节 |
| [Presto FirstLastAggregateBase](../../bolt/functions/prestosql/aggregates/FirstLastAggregate.cpp) 中的 `int128_t` 特化 | `sizeof(int128_t)`，16 字节 |
| [Spark FirstLastAggregateBase](../../bolt/functions/sparksql/aggregates/FirstLastAggregate.cpp) 中的 `int128_t` 特化 | `sizeof(int128_t)`，16 字节 |
| [DecimalAggregate](../../bolt/functions/lib/aggregates/DecimalAggregate.h) | `alignof(LongDecimalWithOverflowState)`，通常为 16 字节 |
| [Spark DecimalSumAggregate](../../bolt/functions/sparksql/aggregates/DecimalSumAggregate.h) | `alignof(DecimalSum)`，通常为 16 字节 |
| [CentralMomentsAggregatesBase](../../bolt/functions/lib/aggregates/CentralMomentsAggregatesBase.h) | `alignof(CentralMomentsAccumulator)`，通常为 8 字节 |
| [EntropyAggregate](../../bolt/functions/prestosql/aggregates/EntropyAggregates.cpp) | `alignof(EntropyAccumulator)`，通常为 8 字节 |
| [ApproxDistinctAggregate](../../bolt/functions/prestosql/aggregates/ApproxDistinctAggregate.cpp) | `alignof(HllAccumulator)` |
| [MapSumAggregate](../../bolt/functions/prestosql/aggregates/MapSumAggregate.cpp) | `alignof(MapSumAccumulator)` |

这里的 `alignof(...)` 以目标平台编译结果为准。

是否需要对齐，与 accumulator 是否变长没有直接关系：变长状态在行内的固定部分
也可以要求对齐，行外分配的内存则由相应 allocator 管理。

## 如何保证 accumulator 的实际地址对齐

[RowContainer.cpp](../../bolt/exec/RowContainer.cpp) 中的构造函数和 `newRow()`
共同保证行首地址、accumulator 偏移以及行步长满足要求。下面是等价的伪代码：

```text
// 所有 alignment 必须是 2 的幂，因此取最大值即可。
rowAlignment = max(1, 所有 accumulator 的 alignment)

// 每个 accumulator 分别补齐自己的偏移，再累加其固定部分大小。
for accumulator in accumulators:
    offset = roundUp(offset, accumulator.alignment())
    accumulatorOffset = offset
    offset += accumulator.fixedWidthSize()

// 加上 dependent 列和可选元数据后，对齐行大小。
fixedRowSize = roundUp(offsetAfterAllFields, rowAlignment)

// 仅在启用 normalized key 时预留前缀。
normalizedKeySize = hasNormalizedKeys
    ? roundUp(sizeof(normalized_key_t), rowAlignment)
    : 0

// 分配的基址满足 rowAlignment。
row = allocateFixed(fixedRowSize + normalizedKeySize, rowAlignment)
    + normalizedKeySize
```

因此同时满足：

```text
rowAddress % rowAlignment == 0
(rowAddress + accumulatorOffset) % accumulatorAlignment == 0
```

[Aggregate::value<T>()](../../bolt/exec/Aggregate.h) 还会用 `BOLT_DCHECK`
检查第二个条件。仅对齐行首是不够的，因为 key 和 null bits 会让 accumulator
的起始偏移落在任意字节位置。

## int128_t 普通列与 accumulator 的区别

`int128_t` 作为普通 key/payload 列时，布局不额外做 16 字节对齐，
其标准存取路径在 [RowContainer.h](../../bolt/exec/RowContainer.h) 中有特化：

- 读取：`valueAt<int128_t>(const char*, int32_t)` 调用 `HugeInt::deserialize`。
- 写入：`storeWithNulls<TypeKind::HUGEINT>` 和 `storeNoNulls<TypeKind::HUGEINT>`
  调用 `HugeInt::serialize`。
- [HugeInt.h](../../bolt/type/HugeInt.h) 中的这两个函数都使用 `memcpy`，
  允许源或目标字节地址未对齐。

而 accumulator 通常通过 `reinterpret_cast<T*>(group + offset_)`
直接访问状态对象，所以相关 `int128_t` 聚合显式要求 16 字节对齐。

普通字段“不做布局补齐”也不意味着 C++ 类型没有对齐要求：当前通用
`RowContainer::valueAt<T>()` 仍直接解引用类型指针。
设计新的行布局或访问接口时，应以“明确支持未对齐的读写方式”或
“满足类型对齐要求”为依据，不能把当前紧凑布局推广为任意类型都能安全地未对齐访问。


## accumulators
###  DecimalSum
可以把 NULL 语义完全收进 DecimalSum 自己的状态里，让它不再依赖 RowContainer 的 null bit；但当前实现不能直接忽略这个 bit。 关键在于：当前 overflow 不
  是完整的“结果已失效”标志。

  目前存在三种不同的状态：

   状态                                                      当前如何表达
  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   没有非 NULL 输入                                          isEmpty == true
  ────────────────────────────────────────────────────────  ──────────────────────────────────────────────────
   本地累加结果超出可表示范围或目标 decimal 精度             根据 sum + overflow，由 computeFinalValue() 判断
  ────────────────────────────────────────────────────────  ──────────────────────────────────────────────────
   收到了已经溢出的 partial，即 (sum=NULL, isEmpty=false)    设置 RowContainer 的 null bit

  第三种是不能直接丢掉 null bit 的原因。bolt/functions/sparksql/aggregates/DecimalSumAggregate.h:274 遇到溢出的 partial 时，只执行：

  setNull(groups[i]);

  它没有把这个信息写入 DecimalSum::overflow 或 isEmpty。例如，group 已经累计到 10，然后收到一个溢出的 partial，此时 accumulator 内部仍可能是：

  sum = 10
  overflow = 0
  isEmpty = false

  只有外部 null bit 记录了这次失效。忽略这个 bit，就会把当前状态误读为有效的 10。

  另外，overflow 是有符号的算术溢出计数，不是不可恢复的错误标志。bolt/functions/sparksql/aggregates/DecimalSumAggregate.h:73 允许某些 overflow == ±1 的
  情况结合 sum 修正回来；而目标 decimal 精度溢出也可能发生在 overflow == 0 时。因此，不能简单写成：

  resultIsNull = isEmpty || overflow != 0;  // 不等价

  如果要让 DecimalSum 自己管理所有状态，我更倾向于把状态表达清楚，例如：

  struct DecimalSum {
    int128_t sum{0};
    int64_t overflow{0};          // 保留算术溢出计数
    bool isEmpty{true};
    bool invalidPartial{false};  // 曾收到已溢出、无法恢复数值的 partial
  };

  相应规则是：

  - 原始输入更新 sum / overflow / isEmpty。
  - 收到 (NULL, false) 的 partial 时设置 invalidPartial = true，后续合并保留这个状态。
  - 最终输出根据 invalidPartial、isEmpty 和 computeFinalValue() 判断 NULL。
  - 中间输出区分空状态 (0, true)、有效状态 (value, false) 和失效状态 (NULL, false)。

  也可以给现有字段设计特殊编码，避免增加成员，但必须把特殊值与正常计数运算隔离，不能继续直接 overflow += ...。

  再区分两种改动范围：

  1. DecimalSum 不再读写这个 bit：可行。 需要一起改初始化、更新、合并、最终输出、中间输出，以及依赖 numNulls_ 的快路径。聚合结果本来就是通过 aggregate
     自己的提取函数生成；例如 bolt/functions/prestosql/aggregates/CountAggregate.cpp:71 初始化时就不调用 setAllNulls()。

  2. RowContainer 不再为它预留这个 bit：需要改框架。 当前 bolt/exec/RowContainer.cpp:280 无条件为每个 accumulator 分配一个 null bit，没有“该
     accumulator 自己管理 NULL”的能力声明。而且去掉一个 bit，也未必减少最终行大小，bitmap 按字节存放，后面还有对齐 padding。

  所以你的设计方向成立：null bit 并非 DecimalSum 语义上的必需品，当前它承担的信息可以迁入 accumulator。只是现有 overflow / isEmpty 的更新逻辑尚未完整承
  接这些信息，不能只删除 null bit 的操作。