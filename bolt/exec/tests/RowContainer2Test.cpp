/*
 * Copyright (c) ByteDance Ltd. and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * This file has been modified by ByteDance Ltd. and/or its affiliates on
 * 2025-11-11.
 *
 * Original file was released under the Apache License 2.0,
 * with the full license text available at:
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * This modified file is released under the same license.
 * --------------------------------------------------------------------------
 */

#include "bolt/exec/RowContainer2.h"

#include <gtest/gtest.h>

#include "bolt/common/base/BitUtil.h"
#include "bolt/type/Type.h"
#include "bolt/vector/VectorTypeUtils.h"
#include "bolt/vector/tests/utils/VectorTestBase.h"

using namespace bytedance::bolt;
using namespace bytedance::bolt::exec;

namespace {

class RowContainer2Test : public testing::Test, public test::VectorTestBase {
 protected:
  static void SetUpTestCase() {
    memory::MemoryManager::testingSetInstance(memory::MemoryManager::Options{});
  }
};

TEST_F(RowContainer2Test, constructorPopulatesFieldMetas) {
  std::vector<TypePtr> types{BIGINT(), VARCHAR(), INTEGER()};
  RowContainer2 container(types, pool());

  ASSERT_EQ(container.fieldMetas().size(), 3);

  const int32_t bigintSize =
      sizeof(typename KindToFlatVector<TypeKind::BIGINT>::HashRowType);
  const int32_t varcharSize =
      sizeof(typename KindToFlatVector<TypeKind::VARCHAR>::HashRowType);
  const int32_t intSize =
      sizeof(typename KindToFlatVector<TypeKind::INTEGER>::HashRowType);

  const int32_t fixedBytes = bigintSize + varcharSize + intSize;
  const int32_t nullBytesStart = std::max<int32_t>(fixedBytes, sizeof(void*));
  const int32_t nullBytes =
      static_cast<int32_t>(bits::nbytes(static_cast<int32_t>(types.size())));

  const size_t expectedFixedRowSize =
      bits::roundUp<size_t>(nullBytesStart + nullBytes, alignof(void*));
  EXPECT_EQ(container.fixedRowSize(), expectedFixedRowSize);

  const auto& m0 = container.fieldMetas()[0];
  EXPECT_EQ(m0.typeKind(), static_cast<int8_t>(TypeKind::BIGINT));
  EXPECT_EQ(m0.getFieldOffset(), 0);
  EXPECT_EQ(m0.getNullFlagOffset(), nullBytesStart * 8 + 0);
  EXPECT_EQ(m0.getSvPrefixLen(), 0);

  const auto& m1 = container.fieldMetas()[1];
  EXPECT_EQ(m1.typeKind(), static_cast<int8_t>(TypeKind::VARCHAR));
  EXPECT_EQ(m1.getFieldOffset(), bigintSize);
  EXPECT_EQ(m1.getNullFlagOffset(), nullBytesStart * 8 + 1);
  EXPECT_EQ(m1.getSvPrefixLen(), 4);

  const auto& m2 = container.fieldMetas()[2];
  EXPECT_EQ(m2.typeKind(), static_cast<int8_t>(TypeKind::INTEGER));
  EXPECT_EQ(m2.getFieldOffset(), bigintSize + varcharSize);
  EXPECT_EQ(m2.getNullFlagOffset(), nullBytesStart * 8 + 2);
  EXPECT_EQ(m2.getSvPrefixLen(), 0);
}

} // namespace

