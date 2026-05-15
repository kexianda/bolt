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

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "bolt/common/base/BitUtil.h"
#include "bolt/type/Type.h"

namespace bytedance::bolt::exec {
namespace {

template <TypeKind Kind>
static int32_t kindSize() {
  return sizeof(typename KindToFlatVector<Kind>::HashRowType);
}

static int32_t typeKindSize(TypeKind kind) {
  if (kind == TypeKind::UNKNOWN) {
    return sizeof(UnknownValue);
  }
  return BOLT_DYNAMIC_TYPE_DISPATCH(kindSize, kind);
}

bool isNullFlagEncoded(TypeKind kind) noexcept {
  return kind == TypeKind::VARCHAR
  || kind == TypeKind::VARBINARY
  || kind == TypeKind::REAL
  || kind == TypeKind::DOUBLE
  || kind == TypeKind::TIMESTAMP;
}



} // namespace

size_t findBestNullByteIdx(std::vector<size_t>& widths, const std::vector<uint8_t>& nullFlagEncoded) {
  assert(widths.size() == nullFlagEncoded.size() && widths.size() <= 8);
  size_t bestNullByteIdx = 0;

  std::vector<size_t> offsets(widths.size());
  for (auto i = 0; i < widths.size(); i++) {
    if (i ==0 ) {
      offsets[i] = widths[i];
    } else {
      offsets[i] = offsets[i-1] + widths[i];
    }
  }

  size_t minDistanceSum = std::numeric_limits<size_t>::max();

  for (size_t k = 1; k < nullFlagEncoded.size(); k++) {
    size_t distanceSum = 0;
    for (auto i = 0; i < nullFlagEncoded.size(); i++) {
      if (nullFlagEncoded[i] == 0) {
        if (i < k) {
          distanceSum += (widths[k - 1] - widths[i]);
        } else if (i > k) {
          distanceSum += (widths[i] - widths[k]);
        }
      }
    }
    if (minDistanceSum > distanceSum) {
      minDistanceSum = distanceSum;
      bestNullByteIdx = k;
    }
  }
  return bestNullByteIdx;
}

RowContainer2::RowContainer2(
    const std::vector<TypePtr>& types,
    memory::MemoryPool* FOLLY_NONNULL pool)
    : pool_(pool) {
  fieldMetas_.clear();
  constexpr int32_t kCacheLineSize = 64;

  int32_t offset = 0;
  int32_t nullOffset = 0;


  auto findNullByteIdx = [&](size_t startIdx) {
    int32_t nullBytePosition = 0;
    for (const auto& type : types) {
      if (isNullFlagEncoded(type->kind())) {
        nullBytePosition = nullOffset / 8;
        nullOffset += 1;
      }
    }
    return nullBytePosition;
  };

  for (auto i = 0; i < types.size(); i++) {
    auto kind = types[i]->kind();
    while (isNullFlagEncoded(kind) && i < types.size()) {
      RowFieldMeta fieldMeta;
      offset += typeKindSize(kind);
      fieldMeta.setTypeKind(kind);
      fieldMeta.setFieldOffset(offset);
      fieldMetas_.emplace_back(fieldMeta);
      ++i;
    }
    int32_t nullByteIdx = findNullByteIdx(i);
  }

  std::vector<int32_t> offsets;

  offsets.insert(offsets.end(), fieldMetas_.size(), 0);
}

} // namespace bytedance::bolt::exec
