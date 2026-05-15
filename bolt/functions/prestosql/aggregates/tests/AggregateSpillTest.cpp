/*
 * Copyright (c) ByteDance Ltd. and/or its affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bolt/exec/PlanNodeStats.h"
#include "bolt/exec/tests/utils/AssertQueryBuilder.h"
#include "bolt/exec/tests/utils/PlanBuilder.h"
#include "bolt/exec/tests/utils/TempDirectoryPath.h"
#include "bolt/exec/tests/utils/TempFilePath.h"
#include "bolt/functions/lib/aggregates/tests/utils/AggregationTestBase.h"

#include "bolt/connectors/hive/HiveConnectorSplit.h"
#include "bolt/dwio/common/FileSink.h"
#include "bolt/dwio/common/Options.h"
#include "bolt/dwio/dwrf/writer/Writer.h"

#include "bolt/common/file/FileSystems.h"

#include <array>

using namespace bytedance::bolt::exec;
using namespace bytedance::bolt::exec::test;
using namespace bytedance::bolt::functions::aggregate::test;

namespace bytedance::bolt::aggregate::test {

namespace {

std::unordered_map<std::string, std::string> spillConfig() {
  return {
      {core::QueryConfig::kSpillEnabled, "true"},
      {core::QueryConfig::kAggregationSpillEnabled, "true"},
      {core::QueryConfig::kPreferPartialAggregationSpill, "true"},
      {core::QueryConfig::kTestingSpillPct, "100"},
      // Force spilling quickly.
      {core::QueryConfig::kAggregationSpillMemoryThreshold, "1"}};
}

RowVectorPtr runPlan(
    const core::PlanNodePtr& plan,
    const std::unordered_map<std::string, std::string>& config,
    const std::string& spillDirectory,
    memory::MemoryPool* pool,
    std::shared_ptr<exec::Task>& task) {
  AssertQueryBuilder builder(plan);
  builder.configs(config);
  if (!spillDirectory.empty()) {
    builder.spillDirectory(spillDirectory);
  }
  return builder.copyResults(pool, task);
}

void writeToDwrfFile(
    const std::string& path,
    const VectorPtr& vector,
    memory::MemoryPool* pool) {
  dwrf::WriterOptions options;
  options.schema = vector->type();
  options.memoryPool = pool;
  auto writeFile = std::make_unique<LocalWriteFile>(path, true, false);
  auto sink =
      std::make_unique<dwio::common::WriteFileSink>(std::move(writeFile), path);
  dwrf::Writer writer(std::move(sink), options);
  writer.write(vector);
  writer.close();
}

class AggregateSpillTest : public AggregationTestBase {
 protected:
  void SetUp() override {
    AggregationTestBase::SetUp();
  }
};

// Comprehensive test covering all bolt aggregate functions with spill enabled
// in a single execution plan.
// Covers: avg, sum, count, count_if, min, max, bool_and, bool_or,
//         bitwise_and_agg, bitwise_or_agg, bitwise_xor_agg,
//         variance, var_samp, var_pop, stddev, stddev_samp, stddev_pop,
//         skewness, kurtosis,
//         covar_samp, covar_pop, corr,
//         arbitrary, first, last, checksum, approx_distinct,
//         array_agg, collect_list, set_agg, collect_set, reduce_agg,
//         entropy, min_by, max_by, histogram, map_agg, multimap_agg,
//         geometric_mean, regr_intercept, regr_slope, regr_count,
//         regr_avgy, regr_avgx, regr_sxy, regr_sxx, regr_syy, regr_r2
TEST_F(AggregateSpillTest, allAggregatesWithSpill) {
  // Two 10-row batches with group key c0 (values 1 or 2).
  auto batch1 = makeRowVector({
      makeFlatVector<int64_t>({1, 1, 1, 1, 1, 2, 2, 2, 2, 2}), // c0: group key
      makeFlatVector<int32_t>({10, 20, 30, 40, 50, 15, 25, 35, 45, 55}), // c1: int
      makeFlatVector<int64_t>({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}), // c2: bigint
      makeNullableFlatVector<int32_t>(
          {1, 2, std::nullopt, 4, 5, 6, std::nullopt, 8, 9, 10}), // c3: nullable
      makeFlatVector<double>({1.1, 2.2, 3.3, 4.4, 5.5, 6.6, 7.7, 8.8, 9.9, 10.10}), // c4: double
      makeFlatVector<double>({2.0, 4.0, 6.0, 8.0, 10.0, 3.0, 6.0, 9.0, 12.0, 15.0}), // c5: for covariance
      makeFlatVector<bool>({true, false, true, false, true, false, true, false, true, false}), // c6: bool
      makeFlatVector<int32_t>({1, 2, 3, 4, 5, 1, 2, 3, 4, 5}), // c7: for array_agg
      makeFlatVector<int32_t>({100, 200, 300, 400, 500, 150, 250, 350, 450, 550}), // c8: for min_by/max_by
      makeFlatVector<StringView>({
          "m"_sv,
          "a"_sv,
          "c"_sv,
          "b"_sv,
          "d"_sv,
          "z"_sv,
          "y"_sv,
          "x"_sv,
          "w"_sv,
          "v"_sv,
      }), // c9: varchar
  });

  auto batch2 = makeRowVector({
      makeFlatVector<int64_t>({1, 1, 1, 1, 1, 2, 2, 2, 2, 2}), // c0
      makeFlatVector<int32_t>({60, 70, 80, 90, 100, 65, 75, 85, 95, 105}), // c1
      makeFlatVector<int64_t>({11, 12, 13, 14, 15, 16, 17, 18, 19, 20}), // c2
      makeNullableFlatVector<int32_t>(
          {11, std::nullopt, 13, 14, 15, 16, 17, std::nullopt, 19, 20}), // c3
      makeFlatVector<double>({11.11, 12.12, 13.13, 14.14, 15.15, 16.16, 17.17, 18.18, 19.19, 20.20}), // c4
      makeFlatVector<double>({1.0, 2.0, 3.0, 4.0, 5.0, 2.0, 4.0, 6.0, 8.0, 10.0}), // c5
      makeFlatVector<bool>({false, true, false, true, false, true, false, true, false, true}), // c6
      makeFlatVector<int32_t>({6, 7, 8, 9, 10, 6, 7, 8, 9, 10}), // c7
      makeFlatVector<int32_t>({600, 700, 800, 900, 1000, 650, 750, 850, 950, 1050}), // c8
      makeFlatVector<StringView>({
          "aa"_sv,
          "ab"_sv,
          "ac"_sv,
          "ad"_sv,
          "ae"_sv,
          "vv"_sv,
          "ww"_sv,
          "xx"_sv,
          "yy"_sv,
          "zz"_sv,
      }), // c9
  });

  std::vector<RowVectorPtr> inputs = {batch1, batch2};

  std::unordered_map<std::string, std::string> config = {
      {core::QueryConfig::kSpillEnabled, "true"},
      {core::QueryConfig::kAggregationSpillEnabled, "true"},
      {core::QueryConfig::kPreferPartialAggregationSpill, "true"},
      {core::QueryConfig::kTestingSpillPct, "100"},
      {core::QueryConfig::kAggregationSpillMemoryThreshold, "1"}};

  core::PlanNodeId partialAggNodeId;
  auto plan = PlanBuilder()
                  .values(inputs)
                  .partialAggregation(
                      {"c0"},
                      {
                          // Basic aggregates
                          "avg(c1)",
                          "sum(c2)",
                          "count(c3)",
                          "count_if(c6)",
                          "min(c1)",
                          "min(c9)",
                          "max(c1)",

                          // Boolean aggregates
                          "bool_and(c6)",
                          "bool_or(c6)",

                          // Bitwise aggregates
                          "bitwise_and_agg(c1)",
                          "bitwise_or_agg(c1)",
                          "bitwise_xor_agg(c1)",

                          // Variance/Stddev family
                          "variance(c4)",
                          "var_samp(c4)",
                          "var_pop(c4)",
                          "stddev(c4)",
                          "stddev_samp(c4)",
                          "stddev_pop(c4)",

                          // Central moments
                          "skewness(c4)",
                          "kurtosis(c4)",

                          // Covariance/Regression family
                          "covar_samp(c4, c5)",
                          "covar_pop(c4, c5)",
                          "corr(c4, c5)",

                          // Regression aggregates
                          "regr_intercept(c4, c5)",
                          "regr_slope(c4, c5)",
                          "regr_count(c4, c5)",
                          "regr_avgy(c4, c5)",
                          "regr_avgx(c4, c5)",
                          "regr_sxy(c4, c5)",
                          "regr_sxx(c4, c5)",
                          "regr_syy(c4, c5)",
                          "regr_r2(c4, c5)",

                          // Arbitrary/first/last
                          "arbitrary(c1)",
                          "first(c1)",
                          "last(c1)",

                          // Min/Max by
                          "min_by(c1, c8)",
                          "max_by(c1, c8)",

                          // Checksum
                          "checksum(c1)",

                          // Approx distinct
                          "approx_distinct(c1)",

                          // Array/Set aggregates
                          "array_agg(c7)",
                          "collect_list(c7)",
                          "set_agg(c7)",
                          "collect_set(c7)",

                          // Histogram
                          "histogram(c1)",

                          // Map aggregates
                          "map_agg(c1, c2)",
                          "multimap_agg(c1, c2)",

                          // Geometric mean
                          "geometric_mean(c4)",

                          // Entropy
                          "entropy(c1)",

                          // Reduce agg
                          "reduce_agg(c7, 0, (s, x) -> (s + x), (s, x) -> (s + x))",
                      })
                  .capturePlanNodeId(partialAggNodeId)
                  .finalAggregation()
                  .planNode();

  auto spillDirectory = TempDirectoryPath::create();
  std::shared_ptr<exec::Task> task;
  auto results = AssertQueryBuilder(plan)
                     .configs(config)
                     .spillDirectory(spillDirectory->path)
                     .copyResults(pool(), task);

  auto stats = exec::toPlanStats(task->taskStats());
  EXPECT_GT(stats.at(partialAggNodeId).spilledBytes, 0);
  EXPECT_GT(stats.at(partialAggNodeId).customStats.count("spillRuns"), 0);
  EXPECT_GT(stats.at(partialAggNodeId).customStats.at("spillRuns").sum, 0);
  OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
}

TEST_F(AggregateSpillTest, arraySetAndMapAggregatesWithSpill) {
  // Focused test for complex return types (ARRAY/SET/MAP) with spill enabled.
  const vector_size_t size = 5'000;
  auto batch1 = makeRowVector({
      makeFlatVector<int64_t>(size, [](auto row) { return row % 17; }),
      makeFlatVector<int32_t>(size, [](auto row) { return row; }),
      makeFlatVector<int64_t>(size, [](auto row) { return row * 10LL; }),
  });
  auto batch2 = makeRowVector({
      makeFlatVector<int64_t>(size, [](auto row) { return row % 17; }),
      makeFlatVector<int32_t>(size, [](auto row) { return row + 1'000; }),
      makeFlatVector<int64_t>(size, [](auto row) { return (row + 1'000) * 10LL; }),
  });
  std::vector<RowVectorPtr> inputs = {batch1, batch2};

  core::PlanNodeId partialAggNodeId;
  auto plan = PlanBuilder()
                  .values(inputs)
                  .partialAggregation(
                      {"c0"},
                      {
                          "array_agg(c1)",
                          "collect_list(c1)",
                          "set_agg(c1)",
                          "collect_set(c1)",
                          // Ensure keys are unique within each group.
                          "map_agg(c1, c2)",
                          "multimap_agg(c1, c2)",
                      })
                  .capturePlanNodeId(partialAggNodeId)
                  .finalAggregation()
                  .planNode();

  auto spillDirectory = TempDirectoryPath::create();
  std::shared_ptr<exec::Task> spillTask;
  auto config = spillConfig();
  config[core::QueryConfig::kHashAdaptivityEnabled] = "false";
  auto actual = runPlan(
      plan, config, spillDirectory->path, pool(), spillTask);

  // Basic correctness invariants.
  ASSERT_EQ(17, actual->size());
  ASSERT_EQ(7, actual->childrenSize());
  auto* groupIds = actual->childAt(0)->asFlatVector<int64_t>();
  auto* arrayAgg = actual->childAt(1)->as<ArrayVector>();
  auto* collectList = actual->childAt(2)->as<ArrayVector>();
  auto* setAgg = actual->childAt(3)->as<ArrayVector>();
  auto* collectSet = actual->childAt(4)->as<ArrayVector>();
  auto* mapAgg = actual->childAt(5)->as<MapVector>();
  auto* multiMapAgg = actual->childAt(6)->as<MapVector>();
  auto* multiMapValues = multiMapAgg->mapValues()->as<ArrayVector>();

  for (auto i = 0; i < actual->size(); ++i) {
    const auto group = groupIds->valueAt(i);
    // size % 17 == 2 -> groups 0 and 1 have 295 rows per batch.
    const vector_size_t expectedCount = (group < 2) ? 590 : 588;
    EXPECT_EQ(expectedCount, arrayAgg->sizeAt(i));
    EXPECT_EQ(expectedCount, collectList->sizeAt(i));
    EXPECT_EQ(expectedCount, setAgg->sizeAt(i));
    EXPECT_EQ(expectedCount, collectSet->sizeAt(i));
    // Keys are unique by construction (c1), so map sizes match row counts.
    EXPECT_EQ(expectedCount, mapAgg->sizeAt(i));
    EXPECT_EQ(expectedCount, multiMapAgg->sizeAt(i));

    // multimap_agg produces map<K, array<V>> and we used unique keys, hence
    // each array<V> should contain exactly one element.
    const auto offset = multiMapAgg->offsetAt(i);
    const auto count = multiMapAgg->sizeAt(i);
    for (auto j = 0; j < count; ++j) {
      EXPECT_EQ(1, multiMapValues->sizeAt(offset + j));
    }
  }

  auto stats = exec::toPlanStats(spillTask->taskStats());
  EXPECT_GT(stats.at(partialAggNodeId).spilledBytes, 0);
  EXPECT_GT(stats.at(partialAggNodeId).customStats.count("spillRuns"), 0);
  EXPECT_GT(stats.at(partialAggNodeId).customStats.at("spillRuns").sum, 0);
  OperatorTestBase::deleteTaskAndCheckSpillDirectory(spillTask);
}

TEST_F(AggregateSpillTest, groupByThreeStringsAvgAndMaxWithSpill) {
  // group by str1, str2, str3; avg(double), max(str4) with spill enabled.
  const std::array<StringView, 10> s1 = {
      "aaaaaaaaaaaaaaaaaaa"_sv,
      "bbbbbbbbbbbbbbbb"_sv,
      "cccccccccccccccc"_sv,
      "ddddddddddddddddddd"_sv,
      "eeeeeeeeeeeeeeee"_sv,
      "ffffffffffffffff"_sv,
      "ggggggggggggggggggg"_sv,
      "hhhhhhhhhhhhhhhh"_sv,
      "iiiiiiiiiiiiiiii"_sv,
      "jjjjjjjjjjjjjjjj"_sv,
  };
  const std::array<StringView, 10> s2 = {
      "kkkkkkkkkkkkkkkk"_sv,
      "llllllllllllllll"_sv,
      "mmmmmmmmmmmmmmmm "_sv,
      "nnnnnnnnnnnnnnnn"_sv,
      "oooooooooooooooo"_sv,
      "pppppppppppppppp"_sv,
      "qqqqqqqqqqqqqqqq"_sv,
      "rrrrrrrrrrrrrrrr"_sv,
      "ssssssssssssssss"_sv,
      "tttttttttttttttt"_sv,
  };
  const std::array<StringView, 10> s3 = {
      "u"_sv,
      "v"_sv,
      "w"_sv,
      "x"_sv,
      "y"_sv,
      "z"_sv,
      "uuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuu1"_sv,
      "v1"_sv,
      "wwwwwwwwwwwwwwwwwwwwww1"_sv,
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx1"_sv,
  };
  const std::array<StringView, 6> s4 = {
      "aa"_sv,
      "bb"_sv,
      "cc"_sv,
      "dd"_sv,
      "ee"_sv,
      "ff"_sv,
  };

  const vector_size_t size = 12'000;
  auto makeBatch = [&](int32_t base) {
    return makeRowVector({
        makeFlatVector<StringView>(
            size, [&](auto row) { return s1[(row + base) % s1.size()]; }),
        makeFlatVector<StringView>(
            size, [&](auto row) { return s2[((row + base) / 10) % s2.size()]; }),
        makeFlatVector<StringView>(
            size,
            [&](auto row) { return s3[((row + base) / 100) % s3.size()]; }),
        makeFlatVector<double>(
            size, [&](auto row) { return (row + base) * 0.125; }),
        makeFlatVector<StringView>(
            size, [&](auto row) { return s4[(row + base) % s4.size()]; }),
    });
  };

  std::vector<RowVectorPtr> inputs = {makeBatch(0), makeBatch(7)};

  core::PlanNodeId partialAggNodeId;
  auto plan = PlanBuilder()
                  .values(inputs)
                  .partialAggregation(
                      {"c0", "c1", "c2"}, {"avg(c3)", "max(c4)"})
                  .capturePlanNodeId(partialAggNodeId)
                  .finalAggregation()
                  .planNode();

  std::shared_ptr<exec::Task> noSpillTask;
  auto expected = runPlan(plan, {}, /*spillDirectory*/ "", pool(), noSpillTask);

  auto spillDirectory = TempDirectoryPath::create();
  std::shared_ptr<exec::Task> spillTask;
  auto actual = runPlan(
      plan, spillConfig(), spillDirectory->path, pool(), spillTask);

  assertEqualResults({expected}, {actual});

  auto stats = exec::toPlanStats(spillTask->taskStats());
  EXPECT_GT(stats.at(partialAggNodeId).spilledBytes, 0);
  EXPECT_GT(stats.at(partialAggNodeId).customStats.count("spillRuns"), 0);
  EXPECT_GT(stats.at(partialAggNodeId).customStats.at("spillRuns").sum, 0);
  noSpillTask.reset();
  OperatorTestBase::deleteTaskAndCheckSpillDirectory(spillTask);
}

TEST_F(AggregateSpillTest, aggregationPushdownSimple) {
  // Aggregation pushdown requires LazyVector input (e.g. from TableScan).
  const vector_size_t size = 1'000;
  auto input = makeRowVector({
      makeFlatVector<int64_t>(size, [](auto row) { return row % 7; }),
      makeFlatVector<int64_t>(size, [](auto row) { return row; }),
  });
  createDuckDbTable({input});

  auto writerPool = rootPool_->addAggregateChild("AggregateSpillTest.writer");
  auto file = exec::test::TempFilePath::create();
  writeToDwrfFile(file->path, input, writerPool.get());
  std::vector<exec::Split> splits;
  splits.emplace_back(std::make_shared<connector::hive::HiveConnectorSplit>(
      "test-hive", file->path, dwio::common::FileFormat::DWRF));

  auto plan = PlanBuilder(pool())
                  .tableScan(asRowType(input->type()))
                  .singleAggregation({"c0"}, {"sum(c1)"})
                  .planNode();
  auto task = assertQuery(plan, std::move(splits), "SELECT c0, sum(c1) FROM tmp GROUP BY 1");

  int64_t loadedToValueHook = 0;
  for (const auto& pipeline : task->taskStats().pipelineStats) {
    for (const auto& op : pipeline.operatorStats) {
      auto it = op.runtimeStats.find("loadedToValueHook");
      if (it != op.runtimeStats.end()) {
        loadedToValueHook += it->second.sum;
      }
    }
  }
  // One aggregate (sum) processes all rows via ValueHook.
  EXPECT_EQ(size, loadedToValueHook);

  remove(file->path.c_str());
}

} // namespace

} // namespace bytedance::bolt::aggregate::test
