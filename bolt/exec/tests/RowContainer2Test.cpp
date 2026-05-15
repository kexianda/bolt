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
#include <cstddef>

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

// size_t findBestNullByteIdx(std::vector<size_t>& offsets, const std::vector<uint8_t>& nullFlagEncoded) ;
TEST_F(RowContainer2Test, findBestNullByteIdxTest) {
  {
    std::vector<size_t> offsets{8, 8, 8, 8, 8, 8, 8, 8};
    std::vector<std::vector<uint8_t>> data{
        {0, 0, 0, 0, 0, 0, 0, 0},
        {1, 1, 1, 1, 1, 1, 1, 1},
        {0, 1, 1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1, 1, 0},
        {1, 1, 1, 0, 1, 1, 1, 1},
        {0, 1, 1, 1, 1, 0, 0, 0},
    };

    std::vector<size_t> expected{4, 0, 0, 0, 0, 0, 0, 0};
    size_t i = 0;
    for (auto& nullFlagEncoded : data) {
      EXPECT_EQ(findBestNullByteIdx(offsets, nullFlagEncoded), expected[i++]);
    }
  }

  {
    std::vector<size_t> offsets{8, 8, 8, 8, 8, 8};
    std::vector<std::vector<uint8_t>> data{
        {0, 0, 0, 0, 0, 0, },
        {1, 1, 1, 1, 1, 1, },
        {0, 1, 1, 1, 1, 1, },
        {1, 1, 1, 1, 1, 0, },
        {1, 1, 1, 0, 1, 1, },
        {0, 1, 1, 1, 0, 0, },
    };

    std::vector<size_t> expected{4, 0, 0, 0, 0, 0, 0, 0};
    size_t i = 0;
    for (auto& nullFlagEncoded : data) {
      EXPECT_EQ(findBestNullByteIdx(offsets, nullFlagEncoded), expected[i++]);
    }
  }

   {
    std::vector<size_t> offsets{8};
    std::vector<std::vector<uint8_t>> data{
        {0,  },
        {1,  },
    };

    std::vector<size_t> expected{1, 0};
    size_t i = 0;
    for (auto& nullFlagEncoded : data) {
      EXPECT_EQ(findBestNullByteIdx(offsets, nullFlagEncoded), expected[i++]);
    }
  }
}

} // namespace
