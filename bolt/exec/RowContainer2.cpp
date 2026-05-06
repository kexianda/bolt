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

static bool nullBitBuiltInType(TypeKind kind) {
  return kind == TypeKind::VARCHAR || kind == TypeKind::VARBINARY;
}

} // namespace

RowContainer2::RowContainer2(
    const std::vector<TypePtr>& types,
    memory::MemoryPool* FOLLY_NONNULL pool)
    : pool_(pool) {
  fieldMetas_.clear();
  fieldMetas_.reserve(types.size());
  constexpr int32_t kCacheLineSize = 64;

  int32_t offset = 0;
  int32_t nullOffset = 0;

  int32_t cacheBeginingFieldIndex = 0;
  // memory layout 如下：
  // 一个 byte作为8个fields的null flag，紧接着是8个field数据
  for (size_t i = 0; i < types.size(); ++i) {
    RowFieldMeta meta;
    auto typeSize = typeKindSize(types[i]->kind());


    meta.setTypeKind(static_cast<int8_t>(types[i]->kind()));
    meta.setFieldOffset(offset);
    meta.setNullable(true);

    if (types[i]->kind() == TypeKind::VARCHAR || types[i]->kind() == TypeKind::VARBINARY) {
      meta.setSvPrefixLen(4);
    }

    fieldMetas_.push_back(meta);
    offset += typeSize;
    ++nullOffset;
  }

  // Make offset at least sizeof pointer so there is always room for a pointer
  // field and to keep the null flags at a predictable offset.
  offset = std::max<int32_t>(offset, sizeof(void*));
  const int32_t nullBytesStart = offset;

  // Fixup null flag offsets to be bit numbers from the start of the row.
  for (auto& meta : fieldMetas_) {
    meta.setNullFlagOffset(meta.getNullFlagOffset() + nullBytesStart * 8);
  }

  const int32_t nullBytes = bits::nbytes(static_cast<int32_t>(fieldMetas_.size()));
  offset += nullBytes;

  fixedRowSize_ = bits::roundUp<size_t>(offset, alignof(void*));
}

} // namespace bytedance::bolt::exec

