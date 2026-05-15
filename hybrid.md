1. row-column Hybrid computing：
动机：
计算引擎里，除了 project/filter/reader/writer。其他算子Sort/Agg/Join逻辑上基于行格式计算的， 全部用行，或全部用列，对算子来说，并不是最佳的数据结构。
所以引入行列混合计算，目的是：
- 减少行列转换；
- 更好的data locality, 提升算子性能;
- 解决序列化问题

核心想法：
1. keys放在Row中，并且和payload 分开，目的是访问keys有好的cache hit rate，避免不必要的payload转换。ruochen的PR 已经实现和合入。
2. 算子之间的 data batch，采用行列混合格式，可以避免很多不必要的行列转换，算子可以按需动态地抉择cache-friendly的数据结构。
3. 工程上，重写RowContainer/allocator，更加cache-friendly，顺便解决序列化问题。
bolt已经做过的尝试：
1. CompositeVector 在Aggregation算子里用RowContainer计算，输出shuffle时用 Row格式，但是：
  - shuffle时还是有 Row to Row 转换，有type dispatching。
  - final Agg 读取Row时，还得做Row to Column 的转换之后，继续基于列计算。

2. Row based spill， 但局限在于：
  - 仅限于 min/max(string)类型的聚合，场景覆盖非常少。
  - 并不是直接序列化，还有row 到另外一种 row 的转换，有type dispatching。
2. Hybrid computing的设计
2.1 接口设计
- HybridRowVector
- RowViewVector
- RowContainer2
增加新的HybridRowVector，继承自RowVector。 这样就不需要修改addInput/getOutput接口。
```cpp
class HybridRowVector : public RowVector {
 std::vector<BaseVectorPtr> childrens_;
 RowContainer2  childrenInRow_;
};
```
为了兼容 Vector接口, 对于存储在Row中的数据列，新增加一个 class RowViewVector : public SimpleVector, 它并不存储真实数据，只是一个Vector视图，用于访问在RowContainer中的数据。 对于Row中的数据处理，核心的接口。
```cpp
template<typename T>
class RowViewVector : public SimpleVector<T>
{
public:
    bool isNullAt(vector_size_t idx) const override;
    const T valueAt(vector_size_t idx) const override;
};
```
对于复杂类型：
- 采用xiaofeng方案的encoding，当作binary处理
- 或，row里不处理复杂类型，复杂类型继续保持列格式。

3. 行列混合算子

- 在local plan时，给算子添加属性，preferRowLayout 还是 preferColumnLayout。每个算子，根据上游算子的输出格式，下游算子对格式的偏好，以及算子本身的逻辑来灵活采用算法和尽量避免行列转换。
- Be lazy. 行列转换尽量推迟，只有在需要的时候，才转换数据格式。
- 对于同一个算子，在getOutput()的layout保持一致。这样有个好处：下游算子根据input来codegen，可以生成固定的JIT。比如：compare(), rowToColumn()等。每个batch格式不固定的话，会过于复杂。

3.1  Sort
3.1.1 Sort行列混合计算的算法：
- addInput()：
  - 若input全是columns，keys部分转成Row，payload分开，格式不做改动。Ruochen基于旧的RowContainer已经实现。基于新的RowContainer2，重构代码即可。
  - 若input的row部分，刚好与sort keys相同，且有空间存放row id，直接复用。payload部分不做任何改动。比如上游是group by或join 的keys与sort keys相同，且row里的next字段可以复用为排序的row id。
  - 其他情况，重新构建keys row。对于payload部分不做改动。

对于BoltML，sort是重度使用算子，业务场景里payload往往是非常重的array类型，对payload部分不做行列转换很重要。

- getOutput():
sort的getOutput(), 不管是否有外部排序，都是逻辑上按行复制，一行一行的攒一个batch给下游。 可以根据下游算子的需求，选择按行输出或按列输出。
  - 如果下游算子是Sink(Parquet Writer)/ProjectFilter， 单行输出时，直接输出为列格式。
  - 如果下游算子是(hash) shuffle ，那么全部整行输出。
  - sort merge agg / sort merge join / windows等算子，按行输出。 对于agg/join，把keys/payload分开。好处是：sort在拼凑输出行write 和下游读数据，都有更好data locality。

3.1.2 sort算法改进：

目前bolt动态选择使用 TimSort / PdqSort算法，两个算法共同的思路是先qsort分区，分区后在局部使用insert sort算法。数据的局部性对sort性能至关重要。
重构RowContainer之后，把keys payload拆开之后，可以通过直接swap keys的值，而不是用pointer，来提升select sort算法性能。 mirco-benchmark 显示基于value而不是pointer的排序，对性能有3～4x的提升。

实现：
需要轻微修改下sort算法的源代码，使用swap接口实现。
用C++ ADL，bolt内部实现的swap实现替换。

3.2 ProjectFilter
ProjectFilter算子中，行列混合计算的逻辑考虑如下因素：
  - input batch的layout
  - 下游算子的类型和对格式的偏好
  - ProjectFilter的 的 expr引用字段 和 projection字段
具体算法如下：
  - 如果input batch全部是列格式，沿用现在的逻辑。
  - 如果input是行列混合的batch，且expr引用到在row里的数据列。
    - 只有filter expr，那么用RowViewVector接口计算，不做行列转换。
    - 有projection expr，引用到的字段在Row中，除非row中所有字段也在projection中，否则把这个row拆开，行转列后参与计算。举例说明：
      - select expr(c1), c1, c2 , c1,c2是行格式，那么可以不做行列转换。
      - select expr(c1), c2, c1, c2是行格式，但c1不在projections中，那么需要把这个row拆开转成列。

- getOutput()
对于 有expr的projections，输出为列格式，其他没有expr的列输出保持原样。
3.3 HashAgg
3.3.1 HashAgg hybrid算法：
- 如果input里keys是行格式，且与 group by的keys相同，直接使用。 比如,final agg中，数据来自上游的partial agg
- 其他情况，重新构造Row存放keys。与projectfilter算子的算法不一样，虽然构造了新row(k1,k2..), payload部分不做任何改动，因为payload字段参与agg计算后就会被释放。并不需要做行列转换。
3.3.2 HashAgg的按行计算
目前bolt HashAgg 逻辑上是按列计算的，对accumulator访问是随机访问。即使输入是vector格式，也是没有办法很好的simd向量化，即使用scatter/gather指令收益也不明显。
按行计算的好处是：
- 更新accumulator保存计算结果，数据局部性更好。
- 如果input是行格式(比如，final agg)，更新accumulator保存计算结果也是行格式，这样读和写都有很好的data locality。
更进一步，可以用JIT消除多层函数调用成本，把整行的计算合并为一个jit函数，很多agg计算本身很简单，overhead在多层的函数调用和memory random access。

3.3.3  Accummulator的Spill:
- Row中的accumulate按照 immediate types，跟着row直接落盘。变长部分，复制到 buffers里落盘。 对于ValueList和ValueSet，沿用现在的序列化方式。
- shuffle 逻辑类似。
(btw, 顺便一提，HashAgg里Spill时，进行了外排，外排成本很高。不知为何bolt目前这么设计。 可以考虑按hash值分区 spill?)

3.4 HashBuild
- 对keys的处理逻辑类似 HashAgg.
- 对payload的处理：不对payload做行列转换。

优化：以前zhuhe也提到过一个idea，spill时， keys部分可以保存内存中，把payload spill。 join的时候，只从磁盘加载命中的payload。
3.5 HashProbe
- 对于keys部分，不管input是row还是column，jit生成compare()函数，用于比较keys是否相等。
- 对payload 的处理：不对payload做行列转换
4.  RowContainer工程优化
 4.1 RowContainer2
4.1.1  Why RowContainer2
RowContainer，是行列混合计算的核心数据结构，为什么需要重新设计：
- RowContainer的memory layout 需要改进提升数据局部性，null flags和value必须大概率在一个cache line内。
- 现在的RowContainer的内存分配使用memory::AllocationPool 分配，每个chunk太小，碎片化。
- 不支持直接序列化，而序列化成本很高，
- HashStringAllocator 处理 StringView 的buffer，性能很差。
- Sort性能至关重要， 而PdqSort/TimSort算法对数据的局部性要求很高

4.1.2  不使用memory::AllocationPool分配

        bolt 原来意图 用AllocationPool 分配较小的chuck，意图是可以复用删除的row或尽快释放内存。  但实际上，sort之后，随机性分布导致chuck很难释放。 结果导致一个chuck存放不了几个row，row在内存中随机分布局部性差。
         从另外一个视角看，把row<field1, field2, field3> 看做一个特殊列即可。内存分配和vector的分配一致。这样data locality更好。

4.1.2 RowContainer2 Memory Layout：
- memory layout动态根据类型确定，按cache size切分，让null flags 和value尽量在一个cache line内。用meta 描述memory layout。
- 复杂类型，沿用xiaofeng的编码方式。
- 支持大部分的accumulator的序列化,  对于ValueList和ValueSet，可以沿用现在的序列化方式。
部分类型，直接把null flag编码到value里：
- StringView: null flag 编码到len字段的高位。
- float/double类型:
  - 使用 signaling NaN 来指代 null
  - 一个固定 quiet NaN常量代表其他的 NaN
- accumulator情况比较多，见后文的梳理。
- 动态规划 null flags位置。 根据字段是否支持null flag encoded，用动态规划算法，插入距离最近的一个byte作为null flag byte。 目的是尽可能null flag 和value在同一个cache line之内。同理，带来的实现复杂度可以用JIT解决。

4.1.3 Accumulator in RowContainer2
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

4.2  StringView for RowContainer2
为什么需要重新设计StringView？
- 指针不能直接序列化，影响内存管理的设计，需要支持offset方便序列化，计算时也兼容指针。
- 目前RowContainer中StringView 和 Vector里StringView的memory layout 并不一致，RowContainer的StringView， buffer不能保证连续，这个设计缺陷导致 RowContainer的StringView和Vector里StringView实际不是一个类型，需要转换，哪怕一次比较操作，需要构造新的对象。
- 互联网规模/utf8的 group by keys，往往长度 > 12.  可以把prefix长度从4增长到12。但是对于短字符串可以用 prefix=4； 带来的是实现上的复杂度，但是对于 JIT codgen来说，没有本质区别，JIT可以消除这个复杂度。

3. HashStringAllocator -> SlabAllocator
4.3.1 为什么要引入 SlabAllocator?
      在vecotr的格式里，一般是读和生成新的vecotr。变长部分放在一个大的buffer里。但是在Row格式里，在agg场景里，比如max/min(varchar)的计算：
  - 1，需要频繁的分配/释放小块内存的分配器，
  - 2，并需要把小块内存的管理纳入bolt的memory pool的管理中去。
如果直接用 MemoryPool->allocateBytes()：
  1. 对于分配小块内存，这个操作太重，不仅有锁(锁还特别重，需要遍历 MemoryPool tree);
  2. 还有强制的 cache line对齐.
这个对于非常小的内存分配，显然非常不合适。 这也是bolt中原有HashStringAllocator被引入的原因。但是，HashStringAllocator 的工程实现很简陋，和kernel slab系统或jemalloc有很大的差距，在多个查询oncall里，还有boltML作业里，HashStringAllocator是perf热点瓶颈。

4.3.2 SlabAllocator 设计和实现：
核心思想： 针对小块内存分配，比如几个byte的内存分配，直接代理给jemalloc去实现，因为jemalloc实现类似linux slab系统，对碎片的处理/线程局部缓存的实现已经非常高效。
我们需要把内存的分配/释放纳入bolt的memory pool的管理中去。

设计：
- SlabAllocator兼容 HashStringAllocator的 allocate() / allocateFromPool() 两个接口。
- 给MemoryPool 增加一种 UpdateStatAllocator，只用来统计内存分配释放。并不做真正的分配。
- SmallAllocator里就以某个chuck(4/8M)批量向 MemoryPool(UpdateStatAllocator)申请，并不会真正向系统要求内存，只是更新 Reserver/UsedBytes 信息。 在SlabAllocator 里 allocate()/free()直接代理给jemalloc处理，只更新自己的usedBytes信息。 一旦需要新的chuck，向 MemoryPool申请批量更新统计信息。这样就可以替换 HashStringAllocator。
4.3  序列化/Row based Shuffle/PartitionOutput
实现思路:
  - meta: 轻量，保存类型信息和文件offset信息
  - 对 StringView 变长部分指针序列化时使用pointer swizzling，变成idx+offset。
  - 对列的处理，类似 arrow flight，buffer直接序列化。
  - buffer直接flush到文件，反序列，直接从文件读取buffer构建row或vector
  - 复杂类型：可以参考arrow递归展开，或者xiaofeng的编码方案。

5. 前期需要做的评估工作
需要做性能评估的几个点:
1. [x] keys 和 payload 分开。 这个ruochen已经实现和benchmark (无spill情况下，HashJoin收益x2)
2. [] 算子间避免行列转换，可以用 sort /windows的合并来验证，或 partial agg / final agg。
4. [x] RowContainer memory layout和 allocator改变，对内存sort算法的体现 mirco-benchmark
5. [] Agg的按行计算。把 agg function JIT成一个函数，逻辑上按一整行计算。可以从top100的线上业务挑选SQL模式，mircobenchmark看性能收益。
6. [] RowContainer2的序列化 和 旧的RowContainer的序列化对比。

6. 风险/执行步骤
核心底层数据结构，已经变成行列混合，算子HashAgg/HashBuild/Serializer/Spill/Shuffle，近乎重写算子，改动非常大，风险也很大。在实现完整之前，可以把改动限定在算子之内，不同算子分批实施。

- 第一阶段: 实现RowContainer2 （P0）
  - 作为核心数据结构，是其他实现的地基，首先实现RowContainer2，改进sort算法，并实现JIT做compare()/RowToCol()/ColToRow(). 在sort算子中测试和验证性能收益。
  - 同时，进行其他mirco-benchmark 评估工作，poc验证收益

- 第二阶段，row-column Hybrid computing的基础设施 （P0）
  - row-column hybrid computing的核心接口 HybridRowVector / RowViewVector 实现和正确性验证。
  - SlabAllocator 替换 HashStringAllocator，验证性能收益.
  - HybridRowVector / RowContainer2 的直接序列化，在sort 算子中验证Spill的性能收益。

- 第三阶段：拓展到 SortMergeAgg / HashAgg  （P0）
  - 基于行的计算逻辑，重新实现 Agg，先覆盖avg/sum/count/min/max等高频聚合函数的 JIT / Spill, 选一个top 100的查询，验证性能收益。(P0)
  - 还有统计类的variance (P1)， 复杂类型的 collect_list / collect_set (P1)

- 第四阶段：拓展到剩余算子
  - shuffle/PartitionOutput
  - HashBuild / HashJoin / Shuffle / PartitionOutput / Window（P0）
