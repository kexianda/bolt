/*
 * Copyright (c) ByteDance Ltd. and/or its affiliates.
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

#include <fmt/format.h>

#include "bolt/common/file/FileSystems.h"
#include "bolt/exec/PlanNodeStats.h"
#include "bolt/exec/tests/utils/AssertQueryBuilder.h"
#include "bolt/exec/tests/utils/OperatorTestBase.h"
#include "bolt/exec/tests/utils/TempDirectoryPath.h"

namespace bytedance::bolt::exec::test {
namespace {

class AggBasic : public OperatorTestBase {
 protected:
  void SetUp() override {
    OperatorTestBase::SetUp();
    filesystems::registerLocalFileSystem();
  }
};

TEST_F(AggBasic, partialToFinalWithSpill) {
  constexpr vector_size_t kRowsPerBatch = 8;
  constexpr int32_t kNumBatches = 4;

  const auto inputType =
      ROW({"id", "name", "info", "age", "score"},
          {BIGINT(), VARCHAR(), VARCHAR(), BIGINT(), DOUBLE()});
  std::vector<RowVectorPtr> batches;
  batches.reserve(kNumBatches);
  for (int32_t batch = 0; batch < kNumBatches; ++batch) {
    batches.push_back(makeRowVector(
        inputType->names(),
        {makeFlatVector<int64_t>(
             kRowsPerBatch, [](auto row) { return row % 4; }),
         makeFlatVector<std::string>(
             kRowsPerBatch,
             [](auto row) { return row < 4 ? "primary" : "secondary"; }),
         makeFlatVector<std::string>(
             kRowsPerBatch,
             [batch](auto row) {
               return fmt::format("info_{:02}", batch * kRowsPerBatch + row);
             }),
         makeFlatVector<int64_t>(
             kRowsPerBatch,
             [batch](auto row) { return 10 + batch * kRowsPerBatch + row; }),
         makeFlatVector<double>(kRowsPerBatch, [batch](auto row) {
           return 0.5 + batch * kRowsPerBatch + row;
         })}));
  }
  createDuckDbTable(batches);

  const auto values =
      std::make_shared<core::ValuesNode>("values", batches, false);
  const std::vector<core::FieldAccessTypedExprPtr> groupingKeys = {
      std::make_shared<core::FieldAccessTypedExpr>(BIGINT(), "id"),
      std::make_shared<core::FieldAccessTypedExpr>(VARCHAR(), "name")};

  const std::vector<core::AggregationNode::Aggregate> partialAggregates = {
      {std::make_shared<core::CallTypedExpr>(
           VARCHAR(),
           std::vector<core::TypedExprPtr>{
               std::make_shared<core::FieldAccessTypedExpr>(VARCHAR(), "info")},
           "max"),
       {VARCHAR()},
       nullptr,
       {},
       {}},
      {std::make_shared<core::CallTypedExpr>(
           ROW({DOUBLE(), BIGINT()}),
           std::vector<core::TypedExprPtr>{
               std::make_shared<core::FieldAccessTypedExpr>(BIGINT(), "age")},
           "avg"),
       {BIGINT()},
       nullptr,
       {},
       {}},
      {std::make_shared<core::CallTypedExpr>(
           DOUBLE(),
           std::vector<core::TypedExprPtr>{
               std::make_shared<core::FieldAccessTypedExpr>(DOUBLE(), "score")},
           "sum"),
       {DOUBLE()},
       nullptr,
       {},
       {}}};
  const auto partial = std::make_shared<core::AggregationNode>(
      "partial",
      core::AggregationNode::Step::kPartial,
      groupingKeys,
      std::vector<core::FieldAccessTypedExprPtr>{},
      std::vector<std::string>{"max_info", "avg_age", "sum_score"},
      partialAggregates,
      false,
      values);

  const std::vector<core::AggregationNode::Aggregate> finalAggregates = {
      {std::make_shared<core::CallTypedExpr>(
           VARCHAR(),
           std::vector<core::TypedExprPtr>{
               std::make_shared<core::FieldAccessTypedExpr>(
                   VARCHAR(), "max_info")},
           "max"),
       {VARCHAR()},
       nullptr,
       {},
       {}},
      {std::make_shared<core::CallTypedExpr>(
           DOUBLE(),
           std::vector<core::TypedExprPtr>{
               std::make_shared<core::FieldAccessTypedExpr>(
                   ROW({DOUBLE(), BIGINT()}), "avg_age")},
           "avg"),
       {BIGINT()},
       nullptr,
       {},
       {}},
      {std::make_shared<core::CallTypedExpr>(
           DOUBLE(),
           std::vector<core::TypedExprPtr>{
               std::make_shared<core::FieldAccessTypedExpr>(
                   DOUBLE(), "sum_score")},
           "sum"),
       {DOUBLE()},
       nullptr,
       {},
       {}}};
  const auto plan = std::make_shared<core::AggregationNode>(
      "final",
      core::AggregationNode::Step::kFinal,
      groupingKeys,
      std::vector<core::FieldAccessTypedExprPtr>{},
      std::vector<std::string>{"max_info", "avg_age", "sum_score"},
      finalAggregates,
      false,
      partial);

  const auto spillDirectory = TempDirectoryPath::create();
  auto task =
      AssertQueryBuilder(plan, duckDbQueryRunner_)
          .spillDirectory(spillDirectory->path)
          .config(core::QueryConfig::kSpillEnabled, true)
          .config(core::QueryConfig::kAggregationSpillEnabled, true)
          .config(core::QueryConfig::kTestingSpillPct, "100")
          .config(core::QueryConfig::kPartialAggregationSpillMaxPct, "100")
          .config(
              core::QueryConfig::kAbandonPartialAggregationMinFinalPct, "100")
          .config(core::QueryConfig::kAbandonPartialAggregationMinPct, "100")
          .config(core::QueryConfig::kAbandonPartialAggregationMinRows, "1")
          .assertResults(
              "SELECT id, name, max(info), avg(age), sum(score) "
              "FROM tmp GROUP BY id, name");

  const auto planStats = exec::toPlanStats(task->taskStats());
  const auto& partialStats = planStats.at(partial->id());
  EXPECT_GT(partialStats.spilledRows, 0);
  EXPECT_GT(partialStats.spilledBytes, 0);
  OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
}

} // namespace
} // namespace bytedance::bolt::exec::test
