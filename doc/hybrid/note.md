
可以把完整布局画成下面这样，[] 表示可选部分：

```text
低地址 → 高地址

[normalized key 前缀及其 padding]
                            ← char* row 指向这里
keys
至少补足到 sizeof(void*) 的空间
null bits + [probed bit] + free bit
[聚合 accumulator，按各自 alignment 插入 padding]
payload / dependent columns
[variable-size：uint32_t]
[next：同 key 的下一行指针]
尾部 alignment padding
```

各部分的作用如下：

| 字段 / 属性 | 大小 | 存在条件与用途 |
| --- | --- | --- |
| Null bits | 每字段逻辑上 1 bit，按字节存储 | key 由 nullableKeys 决定是否有有效 null bit；每个 accumulator 和 dependent column 都有 null bit。bit 为 1 表示 null |
| Probed flag | 1 bit | hasProbedFlag=true 时保留。用于记录 join build 行是否被 probe 命中，例如 right/full outer join；在 spilled aggregation 中也可表示当前行紧跟同 key 行 |
| Free flag | 1 bit | 当前实现始终保留。表示该行已删除、进入空闲链表，可以复用 |
| Accumulator | 每个聚合函数决定固定区大小 | 聚合场景保存聚合中间状态，例如 sum、count、avg 的状态；还可能引用行外内存 |
| Variable-size | 4 bytes，uint32_t | 存在可变长列或可变长 accumulator 时保存该行关联的可变长内存大小，用于内存统计。它不是行内固定区域的长度 |
| Next pointer | sizeof(void*)，本机通常 8 bytes | hasNext=true 时存在，连接 hash join build 中 相同 key 的多行 |
| Normalized key | 64 bit 值，前缀空间按 alignment 补齐 | 启用时放在 row 指针之前，将 key 编码成一个 word 来加速比较；关闭后，新分配行可以不再保留这个前缀 |
| Padding / alignment | 可变 | accumulator 按各自对齐要求放置，整行固定区也会补齐；key 区至少占一个指针大小，供删除后复用 |

这里还有两个容易混淆的点：

空闲链表指针不额外占一个尾部字段。 行删除后，nextFree 复用行起始位置的空间，因此会覆盖原来的 key 数据；这也是 key 区至少补足到 sizeof(void*) 的原因。实现见 bolt/exec/RowContainer.h:1172。

可变长内容通常放在行外。 行内保留固定大小的描述符：字符串使用 StringView，短字符串可以内联；ARRAY / MAP / ROW 使用 std::string_view 指向序列化数据。因此，同一个容器的行内槽位大小固定，行外内存占用可以不同。

如果你关注的是之前的 hybrid layout，它保留上述适用的元数据，但把 dependent columns 换成唯一的 8-byte BIGINT rowId，真实 payload 放在 HybridContainer 的输入 vectors 中。rowIdOffset_ 只是容器记录的字段偏移，不是行内额外再存一个 offset，见 bolt/exec/RowContainer.cpp:317。

容器本身还保存 offsets_ / rowColumns_、fixedRowSize_、alignment_、行数、空闲链表头、allocator，以及可选的行指针索引等属性。这些由整个容器共享，不会在每行里重复存储，定义见 bolt/exec/RowContainer.h:1640。

## RowContainer 的 erase

RowContainer 的 erase 主要出现在 窗口计算释放已消费的数据、partial 聚合放弃后释放临时状态，以及容器整体清理。

先明确语义：bolt/exec/RowContainer.cpp:440 会释放行的变长字段、销毁聚合状态，将行标记为 free 并挂到空闲链表。固定行槽位保留，后续 newRow() 可以复用。如果启用了 monotonic string allocation，字符串内存也会保留到 clear() 或析构。

| 场景 | 触发时机 | 代码位置 |
| --- | --- | --- |
| 流式窗口切换 partition | 已完成的旧 partition 不再需要，批量删除其行 | bolt/exec/StreamingWindowBuild.cpp:90 |
| 窗口分段计算 | 推进到下一段时删除上一段；结束时删除剩余行。仅 kRowContainer 格式调用 erase | bolt/exec/RowsStreamingWindowPartition.cpp:83 |
| 窗口 spill | 数据写入 spill 后，若容器中还有需要保留的行，就只 erase 已 spill 的行；若全部已 spill，则 clear() | bolt/exec/SpillableWindowBuild.cpp:148 |
| partial aggregation 被放弃 | 对不支持直接 toIntermediate() 的聚合，临时创建每行 accumulator，提取 intermediate 结果后调用 eraseRowsSkippingKeys() | bolt/exec/GroupingSet.cpp:2173 |
| 哈希表显式删除条目 | 先删除 hash table 中的条目，再删除对应容器行；当前搜到的直接调用方是 HashTable 单测 | bolt/exec/HashTable.cpp:2346 |
| clear() 或析构 | 当共享 string allocator、存在外部聚合内存，或启用 checkFree 时，先批量 eraseRows()，再整体释放 | bolt/exec/RowContainer.cpp:1134 |
