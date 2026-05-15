
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
#include <cstdint>
#include <vector>
#include "bolt/exec/RowContainer.h"

namespace bytedance::bolt::exec {

/// @brief RowFieldMeta is used to store the metadata of a field in a row.
/// @note The metadata is stored in a 64-bit integer.
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
struct RowFieldMeta {
  static constexpr uint64_t kTypeKindShift = 0;
  static constexpr uint64_t kNullableShift = 8;
  static constexpr uint64_t kAscendingShift = 9;
  static constexpr uint64_t kNullsFirstShift = 10;
  static constexpr uint64_t kLongPrefixStringViewShift = 11;
  static constexpr uint64_t kChildrenCntShift = 16;
  static constexpr uint64_t kPrecisionShift = 16;
  static constexpr uint64_t kScaleShift = 24;
  static constexpr uint64_t kFieldOffsetShift = 32;
  static constexpr uint64_t kNullFlagOffsetShift = 52;

  static constexpr uint64_t kTypeKindMask = 0xFFULL << kTypeKindShift;
  static constexpr uint64_t kNullableMask = 0x1ULL << kNullableShift;
  static constexpr uint64_t kAscendingMask = 0x1ULL << kAscendingShift;
  static constexpr uint64_t kNullsFirstMask = 0x1ULL << kNullsFirstShift;
  static constexpr uint64_t kLongPrefixStringViewMask = 0x1ULL
      << kLongPrefixStringViewShift;
  static constexpr uint64_t kChildrenCntMask = 0xFFFFULL << kChildrenCntShift;
  static constexpr uint64_t kPrecisionMask = 0xFFULL << kPrecisionShift;
  static constexpr uint64_t kScaleMask = 0xFFULL << kScaleShift;
  static constexpr uint64_t kFieldOffsetMask = 0xFFFFFULL << kFieldOffsetShift;
  static constexpr uint64_t kNullFlagOffsetMask = 0xFFFULL
      << kNullFlagOffsetShift;

  // bit 0-7
  constexpr TypeKind typeKind() const {
    return static_cast<TypeKind>(extractBits(kTypeKindMask, kTypeKindShift));
  }

  void setTypeKind(TypeKind kind) {
    updateBits(static_cast<uint8_t>(kind), kTypeKindMask, kTypeKindShift);
  }

  constexpr bool nullable() const {
    return extractBits(kNullableMask, kNullableShift) != 0;
  }

  void setNullable(bool nullable) {
    updateBits(nullable, kNullableMask, kNullableShift);
  }

  constexpr bool ascending() const {
    return extractBits(kAscendingMask, kAscendingShift) != 0;
  }

  void setAscending(bool ascending) {
    updateBits(static_cast<uint64_t>(ascending), kAscendingMask, kAscendingShift);
  }

  constexpr bool nullsFirst() const {
    return extractBits(kNullsFirstMask, kNullsFirstShift) != 0;
  }

  void setNullsFirst(bool nullsFirst) {
    updateBits(static_cast<uint64_t>(nullsFirst), kNullsFirstMask, kNullsFirstShift);
  }

  constexpr bool longPrefixStringView() const {
    return extractBits(kLongPrefixStringViewMask, kLongPrefixStringViewShift) !=
        0;
  }

  void setLongPrefixStringView(bool enabled) {
    updateBits(static_cast<uint64_t>(enabled), kLongPrefixStringViewMask, kLongPrefixStringViewShift);
  }

  constexpr uint16_t childrenCnt() const {
    return static_cast<uint16_t>(
        extractBits(kChildrenCntMask, kChildrenCntShift));
  }

  void setChildrenCnt(uint16_t childrenCnt) {
    updateBits(childrenCnt, kChildrenCntMask, kChildrenCntShift);
  }

  constexpr uint8_t precision() const {
    return static_cast<uint8_t>(extractBits(kPrecisionMask, kPrecisionShift));
  }

  void setPrecision(uint8_t precision) {
    updateBits(precision, kPrecisionMask, kPrecisionShift);
  }

  constexpr uint8_t scale() const {
    return static_cast<uint8_t>(extractBits(kScaleMask, kScaleShift));
  }

  void setScale(uint8_t scale) {
    updateBits(scale, kScaleMask, kScaleShift);
  }

  constexpr uint32_t fieldOffset() const {
    return static_cast<uint32_t>(
        extractBits(kFieldOffsetMask, kFieldOffsetShift));
  }

  void setFieldOffset(uint32_t offset) {
    updateBits(offset, kFieldOffsetMask, kFieldOffsetShift);
  }

  constexpr uint16_t nullFlagOffset() const {
    return static_cast<uint16_t>(
        extractBits(kNullFlagOffsetMask, kNullFlagOffsetShift));
  }

  void setNullFlagOffset(uint16_t offset) {
    updateBits(offset, kNullFlagOffsetMask, kNullFlagOffsetShift);
  }

  constexpr uint64_t rawData() const {
    return static_cast<uint64_t>(data);
  }

  constexpr uint64_t extractBits(uint64_t mask, uint64_t shift) const {
    return (rawData() & mask) >> shift;
  }

  void updateBits(uint64_t value, uint64_t mask, uint64_t shift) noexcept {
    auto raw = rawData();
    raw = (raw & ~mask) | ((value << shift) & mask);
    data = static_cast<int64_t>(raw);
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


size_t findBestNullByteIdx(std::vector<size_t>& widths, const std::vector<uint8_t>& nullFlagEncoded) ;

} // namespace bytedance::bolt::exec
