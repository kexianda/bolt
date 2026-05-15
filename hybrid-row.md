RowContainer2 设计

## 1. RowContainer 简述 和存在的问题：

### 1. RowContainer的memory layout如下：
```
    +--------- keys / dependents / accumulators(fixed-width) -----------------------+
    | f0 | f1 | f2 | null bytes | acc1 | acc2 | acc3 | ... | acc100 | next ptr      |
    +-------------------------------------------------------------------------------+

    row buffers（HashStringAllocator）:
    +--------- variable-width bytes ---------+
    | long string payload / serialized blob  |
    +----------------------------------------+
```
memory layout 存在问题：
- null flags 和 value 相距甚远，对于Agg计算，影响cache hit，尤其列比较多的情况。
- * 指针不能序列化 *


### 2. RowContainer的 StringView 和 HashStringAllocator:

对于RowContainer里的StringView, HashStringAllocator分配的buffer如下：
```
    ┌──────────────────────────────────────────────┐
    │ [size=20][prefix='abcd'][ptr=0x1000]         │
    └──────────────────────────────────────────────┘

    0x1000
    ┌──────────────┬───────────────────────┬────────────────────┐
    │ header       │ usable payload bytes  │ Header* to         │
    │ continued=1  │ "abcdefghijkl"        │ 0x3000             │
    └──────────────┴───────────────────────┴────────────────────┘

    0x3000
    ┌──────────────┬───────────────────────┐
    │ header       │ usable payload bytes  │
    │ continued=0  │ "mnopqrst"            │
    └──────────────┴───────────────────────┘
```

HashStringAllocator的管理结构：

- 内存膨胀问题：
对于每一个小的buffer：
  - folly::F14FastMap<void*, size_t> 保存每一个buffer指针和大小。
  - buffer Header: 4 bytes
对于每一个小buffer，管理数据浪费的内存至少有： sizeof(void*) + sizeof(size_t) + 4，已经20字节了，还得再加上 hashtable的开销。
线上的业务，尤其是中文utf8，常见长度20左右字符串，管理数据结构消耗内存已经是超过数据本身。更严重的是，这些管理数据占用的大量内存不在memorypool的控制之内。
这种buffer管理，这个对于Sort/Agg/Join等算子的内存消耗会比较大。

- 性能问题：
  - alloc()/free() 实现非常简陋，线性扫描bits，复杂度是O(n), 数据越多，性能越差。
  - 读RowContainer的StringView 变成一个很重操作， StringView的compare(), 逻辑复杂，甚至需要复制和构造一个新的StringView

### 3. memory::AllocationPool
目前，bolt使用AllocationPool给RowContainer分配内存。
AllocationPool的管理结构如下：
AllocationPool {
    Allocation[] allocations;
}
Allocation {
    PageRun[] pageRuns; // 每个PageRun只有 16 * 4K = 64K。 这样几乎存放不了几行。Row碎片化了。
}
抽象层次太多，AllocationPool里多个Allocation， 每个Allocation里多个PageRun；并且，每个连续的块太小了， 只有16个Page。
碎片化就没有了data locality。


## 2. RowContainer2 设计

### 2.1 新的 memory layout

cache-friendly memory layout:
```
+-----------------------------------------------------------------------------------------------------------+
| f0 | f1 | f2 | null byte | f4 ｜ acc1 | acc2 | acc3 | acc4 ｜ acc5 ｜ null byte｜ ... | acc100 |  rowId    |
+-----------------------------------------------------------------------------------------------------------+
```
- encoded null flag,
  StringView/Double/float，还有大多数 accumulator 直接把null flag编码到value 里.
  除了节省空间，这是实现的原因是，减少对内存的访问。

- 按照 cache size 切分为多个segment，每个segment最多有一个byte用来存储null flags.
  为了节省内存，不做强制cache line对齐。这个byte的插入的位置，动态规划算出来，它和segment之内的字段距离之和为最小，目的是尽量命中同一个cache line，最差也要相邻的一个cache line。

- rowId 是整数，方便整个序列化

- meta 信息:
用一个 int64_t 来存储字段的meta信息，包括 type kind，type parameters, 在Row中offset。
以及其他属性 nullable/null first/ascending / encoding。对于复杂类型，也可以表达，深度优先访问平铺即可。


```cpp
struct RowFieldMeta {
    ///    bit 0-7: type kind
    ///    bit 8: is nullable
    ///    bit 9: is ascending order (for sort keys)
    ///    bit 10: is nulls first (for sort keys)
    ///    bit 11: is long prefix StringView
    ///    bit 12: dictionary encoding
    ///    bit 13-15: reserved for future use
    ///    bit 16-23: precision for decimal
    ///    bit 24-31: scale for decimal
    ///    bit 16-31: children count for complex types
    ///    bit 32-51: field bytes offset in the row. 2 ^^ 20 = 1M
    ///    bit 52-63: null flag offset from field.
    int64_t data;
};
```
这么设计目的是，schema信息高度压缩，非常轻量，可直接序列化。

### 2.2 RowContainer2的 null flag encoding

- StringView: null flag 编码到len字段的高位。31位来支持最大长度2G的字符串。
- float/double类型:
  - 使用 signaling NaN 来指代 null。 CPU 运算结果只可能是 quiet NaN，用signaling NaN 来指代 null。
  - 一个固定 quiet NaN常量代表其他的 NaN
- Timestamp:
  - null flag 编码到nanos字段的高位。

Accumulator情况比较多：

Accumulator in RowContainer2
RowContainer中，null flags和Accumulator相距过远，污染CPU cache。对于很多Accumulator，其实可以把 null flag编码进Accumulator的value里。
需要给 Aggregate 增加接口 hasBuiltInNullFlag()/isNull(). 对于没有办法编码null flag的Accumulator。null flag byte在内存中的安放位置的算法，如同前面对keys的算法。

第一步先支持常用聚合函数：
  - avg/sum/count/min/max/variance
  - collect_list / collect_set 常用的复杂类型聚合

AverageAggregate
  - 用 SumCount.count 高位表示null flag
DecimalAverageAggregate
  - 用 LongDecimalWithOverflowState.count 高位表示null flag
DecimalSumAggregate
  - DecimalSum.isEmpty 已经用来表示null flag了
(SimpleNumericAggregate)
  ArbitraryAggregate
  NonNumericArbitrary
  FirstLastAggregateBase
  MinMaxAggregate
  SumAggregateBase
NonNumericMinAggregate
NonNumericMaxAggregate
  - float/double类型，用 signaling NaN 来指代 null
  - 其他类型，用额外的bit表示null flag
CountAggregate
  - 高位表示null flag
VarianceAggregate
  StdDevPopAggregate
  StdDevSampAggregate
  VarPopAggregate
  VarSampAggregate
  - VarianceAccumulator.count_ 高位表示null flag
collect_list
  - 用 ValueList.size_ 高位表示null flag
collect_set
  - 用额外的bit表示null flag
其他聚合函数，以后再考虑覆盖。


## 3. allocator for RowContainer2

### 3.1 Row的 allocator：
  对于输出的batch，对RowContainer2的分配，与vecotr的分配一样，直接使用 pool->allocateBytes()分配。

### 3.2 Slab Allocator 设计
引入 小块内存的分配器 SlabAllocator 替换 HashStringAllocator，目的：
- 为了节省内存，不需要每个小对象单独做复杂管理，节约大量内存。
- 有thread local的 tcache，两层过滤快速定位，性能比HashStringAllocator那中简单线性扫描好。
[] TODO: benchmark HashStringAllocator vs  SlabAllocator












## 4. StringView for RowContainer2
与之前RowContainer中的StringView对比，

- buffer ptr 同时兼容 offset 和 raw pointer。支持offset， 这是实现Buffer Manager的pin/unpin功能，优化序列化的大前提。
- null bit 编码到len字段的高位。
- 分场景使用不同的allocator
- 支持变长prefix，对于长字符串，prefix增加到12，提升性能 [optional] P1

场景一，readonly的StringView buffer。
除了 Accumulator场景中的StringView需要被更新，其他场景，都是readonly的。直接用类型FlatVector<StringView>的buffer即可，节省内存和性能更好。

场景二，Accumulator场景，变长数据需要被更新，比如 StringView。
对于 变长的buffer，需要使用SlabAllocator，且必须使用raw pointer。


实现:
```cpp
template <uint32_t PREFIX_LEN = 4>
  requires (PREFIX_LEN == 4 || PREFIX_LEN == 12)
class StringView {
  uint32_t size_{0};
  char prefix_[PREFIX_LEN]{0};
  union {
    char inlined[8];
    const char* data;
    int64_t offset;
  } value_{.data = nullptr};
};
```
兼容模式，当offset高位是1，表示是offset，否则是raw pointer。

## 5.JIT
RowContainer2 的
- row-row compare()
- row-column compare()
- row-row equal()
- row-column equal()

还有:
- rowToColumn()
- columnToRow()

##  评估 / 落地节奏
评估：
[] SlabAllocator 与 HashStringAllocator 性能对比
[] benchmark RowContainer2 新的memory layout 对 agg 行计算的影响。 PMU perf

SQL模仿 线上资源top2的 agg 计算为例子：
