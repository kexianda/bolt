
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

#pragma once

#include <cstddef>
#include <vector>
#include "bolt/exec/RowContainer.h"

namespace bytedance::bolt::exec {

struct RowFieldMeta {
    // bit 0-7
    constexpr int8_t typeKind() const {
      return data & 0xFF;
    }

    constexpr RowFieldMeta& setTypeKind(int8_t kind) {
      auto u = static_cast<uint64_t>(data);
      u = (u & ~uint64_t{0xFF}) | (static_cast<uint64_t>(kind) & 0xFF);
      data = static_cast<int64_t>(u);
      return *this;
    }

    // bit 8-10
    // bit 11-15 reserved for future use
    constexpr bool isNullable()  const { return (data >> 8) & 0x1; }
    constexpr bool isAsc()  const { return (data >> 9) & 0x1; }
    constexpr bool isNullsFirst()  const { return (data >> 10) & 0x1; }

    constexpr RowFieldMeta& setNullable(bool nullable) {
      auto u = static_cast<uint64_t>(data);
      constexpr uint64_t mask = uint64_t{1} << 8;
      u = nullable ? (u | mask) : (u & ~mask);
      data = static_cast<int64_t>(u);
      return *this;
    }

    constexpr RowFieldMeta& setAsc(bool asc) {
      auto u = static_cast<uint64_t>(data);
      constexpr uint64_t mask = uint64_t{1} << 9;
      u = asc ? (u | mask) : (u & ~mask);
      data = static_cast<int64_t>(u);
      return *this;
    }

    constexpr RowFieldMeta& setNullsFirst(bool nullsFirst) {
      auto u = static_cast<uint64_t>(data);
      constexpr uint64_t mask = uint64_t{1} << 10;
      u = nullsFirst ? (u | mask) : (u & ~mask);
      data = static_cast<int64_t>(u);
      return *this;
    }

    // bit 16-23
    constexpr int8_t getPrecision()  const { return (data >> 16) & 0xFF; }
    constexpr int8_t getScale()  const { return (data >> 24) & 0xFF; }
    constexpr int8_t getSvPrefixLen()  const { return (data >> 16) & 0xFF; }

    constexpr RowFieldMeta& setPrecision(int8_t precision) {
      auto u = static_cast<uint64_t>(data);
      constexpr uint64_t mask = uint64_t{0xFF} << 16;
      u = (u & ~mask) | ((static_cast<uint64_t>(precision) & 0xFF) << 16);
      data = static_cast<int64_t>(u);
      return *this;
    }

    constexpr RowFieldMeta& setScale(int8_t scale) {
      auto u = static_cast<uint64_t>(data);
      constexpr uint64_t mask = uint64_t{0xFF} << 24;
      u = (u & ~mask) | ((static_cast<uint64_t>(scale) & 0xFF) << 24);
      data = static_cast<int64_t>(u);
      return *this;
    }

    constexpr RowFieldMeta& setSvPrefixLen(int8_t prefixLen) {
      // Shares the same bit field with precision (bit 16-23).
      return setPrecision(prefixLen);
    }

    // bit 31-51: 2 ^^ 20 = 1M, that is enough
    constexpr int32_t getFieldOffset()  const { return (data >> 32) & 0xFFFFF; }
    // bit 52-63 2 ^^ 12 = 4096, bit offset
    // how to get null flag:
    //  *(row + getFieldOffset() + getNullFlagOffset() / 8) & (1 << (getNullFlagOffset() % 8))
    constexpr int32_t getNullFlagOffset()  const { return (data >> 42) & 0xFFFFF; }

    constexpr RowFieldMeta& setFieldOffset(int32_t fieldOffset) {
      auto u = static_cast<uint64_t>(data);
      constexpr uint64_t mask = uint64_t{0xFFFFF} << 32;
      u = (u & ~mask) | ((static_cast<uint64_t>(fieldOffset) & 0xFFFFF) << 32);
      data = static_cast<int64_t>(u);
      return *this;
    }

    constexpr RowFieldMeta& setNullFlagOffset(int32_t nullFlagOffset) {
      auto u = static_cast<uint64_t>(data);
      constexpr uint64_t mask = uint64_t{0xFFFFF} << 42;
      u = (u & ~mask) | ((static_cast<uint64_t>(nullFlagOffset) & 0xFFFFF) << 42);
      data = static_cast<int64_t>(u);
      return *this;
    }

  int64_t data{0};
};


class RowContainer2 {
 public:
  RowContainer2(
      const std::vector<TypePtr>& types,
      memory::MemoryPool* FOLLY_NONNULL pool);

  const std::vector<RowFieldMeta>& fieldMetas() const {
    return fieldMetas_;
  }

  size_t fixedRowSize() const {
    return fixedRowSize_;
  }

 private:
  std::vector<RowFieldMeta> fieldMetas_;
  size_t fixedRowSize_{0};
  size_t rowNum_{0};

  std::vector<Accumulator> accumulators_;

  // Contiguous values.
  //   values_ = AlignedBuffer::allocate<uint8_t>(fixedRowSize_ * rowNum_ +
  //     (simd::kPadding / sizeof(T)), pool_);
  BufferPtr values_{nullptr};

  std::vector<BufferPtr> variadicBuffers_;

  memory::MemoryPool* pool_{nullptr};
};

} // namespace bytedance::bolt::exec
