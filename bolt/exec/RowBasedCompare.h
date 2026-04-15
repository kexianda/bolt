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

#pragma once

#if defined(__APPLE__)
#include <machine/endian.h>
#else
#include <endian.h>
#endif
#include <folly/CPortability.h>
#include <folly/container/F14Set.h>
#include <cmath>
#include <cstdint>

#include "bolt/common/base/CompareFlags.h"
#include "bolt/common/memory/HashStringAllocator.h"
#include "bolt/exec/ContainerRowSerde.h"
#include "bolt/exec/RowContainer.h"
#include "bolt/type/Type.h"
#include "bolt/vector/VectorStream.h"
#include "bolt/vector/VectorTypeUtils.h"
namespace bytedance::bolt::exec {

namespace {

template <TypeKind Kind>
FOLLY_ALWAYS_INLINE int compareComplex(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  BOLT_DCHECK(flag.nullAsValue(), "not supported null handling mode");

  BOLT_DCHECK_NOT_NULL(left, "In compareSimple left is null");
  BOLT_DCHECK_NOT_NULL(right, "In compareSimple right is null");
  BOLT_DCHECK_NOT_NULL(type, "In compareSimple type is null");

  bool leftIsNull = RowContainer::isNullAt(left, leftColumn);
  bool rightIsNull = RowContainer::isNullAt(right, rightColumn);

  if (leftIsNull) {
    if (!rightIsNull) {
      return flag.nullsFirst ? -1 : 1;
    }
    // left and right are all NULL
    return 0;
  }
  if (rightIsNull) {
    return flag.nullsFirst ? 1 : -1;
  }

  const auto *leftSv = reinterpret_cast<const std::string_view*>(
           left + leftColumn.offset());
  const auto *rightSv = reinterpret_cast<const std::string_view*>(
           right + rightColumn.offset());
  int32_t leftSize = leftSv->size();
  int32_t rightSize = rightSv->size();

  auto* leftPtr =
      reinterpret_cast<uint8_t*>(const_cast<char*>(leftSv->data()));
  auto* rightPtr =
      reinterpret_cast<uint8_t*>(const_cast<char*>(rightSv->data()));

  ByteRange leftRange{.buffer=leftPtr, .size=leftSize, .position=0};
  ByteRange rightRange{.buffer=rightPtr, .size=rightSize, .position=0};

  ByteInputStream leftStream(std::vector<ByteRange>{leftRange});
  ByteInputStream rightStream(std::vector<ByteRange>{rightRange});
  int ans = ContainerRowSerde::compare(leftStream, rightStream, type, flag);
  return ans == 0 ? 0 : (ans > 0 ? 1 : -1);
}

template <TypeKind Kind>
FOLLY_ALWAYS_INLINE int compareSimple(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  using T = typename KindToFlatVector<Kind>::HashRowType;

  BOLT_DCHECK_NOT_NULL(left, "In compareSimple left is null");
  BOLT_DCHECK_NOT_NULL(right, "In compareSimple right is null");
  BOLT_DCHECK_NOT_NULL(type, "In compareSimple type is null");

  bool leftIsNull = RowContainer::isNullAt(left, leftColumn);
  bool rightIsNull = RowContainer::isNullAt(right, rightColumn);

  if (leftIsNull) {
    if (!rightIsNull) {
      return flag.nullsFirst ? -1 : 1;
    }
    // left and right are all NULL
    return 0;
  }
  if (rightIsNull) {
    return flag.nullsFirst ? 1 : -1;
  }

  T leftValue = RowContainer::valueAt<T>(left, leftColumn.offset());
  T rightValue = RowContainer::valueAt<T>(right, rightColumn.offset());

  if constexpr (Kind == TypeKind::VARCHAR || Kind == TypeKind::VARBINARY) {
    int ans = flag.ascending ? leftValue.compare(rightValue)
                             : rightValue.compare(leftValue);
    return ans == 0 ? 0 : (ans > 0 ? 1 : -1);
  }
  if constexpr (std::is_floating_point<T>::value) {
    bool isLeftNan = std::isnan(leftValue);
    bool isRightNan = std::isnan(rightValue);
    if (isLeftNan) {
      return isRightNan ? 0 : 1;
    }
    if (isRightNan) {
      return -1;
    }
  }
  auto result = leftValue < rightValue ? -1 : (leftValue == rightValue ? 0 : 1);
  return flag.ascending ? result : result * -1;
}

} // namespace

template <TypeKind Kind>
FOLLY_ALWAYS_INLINE int compareByRow(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  BOLT_NYI("Not supported type for compareByRow");
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::UNKNOWN>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareSimple<TypeKind::UNKNOWN>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::OPAQUE>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  BOLT_NYI("Not supported type for compareByRow");
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::BOOLEAN>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareSimple<TypeKind::BOOLEAN>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::TINYINT>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareSimple<TypeKind::TINYINT>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::SMALLINT>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareSimple<TypeKind::SMALLINT>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::INTEGER>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareSimple<TypeKind::INTEGER>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::BIGINT>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareSimple<TypeKind::BIGINT>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::REAL>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareSimple<TypeKind::REAL>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::DOUBLE>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareSimple<TypeKind::DOUBLE>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::HUGEINT>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareSimple<TypeKind::HUGEINT>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::TIMESTAMP>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareSimple<TypeKind::TIMESTAMP>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::VARCHAR>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareSimple<TypeKind::VARCHAR>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::VARBINARY>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareSimple<TypeKind::VARBINARY>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::ROW>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareComplex<TypeKind::ROW>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::MAP>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareComplex<TypeKind::MAP>(
      left, right, leftColumn, rightColumn, flag, type);
}

template <>
FOLLY_ALWAYS_INLINE int compareByRow<TypeKind::ARRAY>(
    const char* left,
    const char* right,
    RowColumn leftColumn,
    RowColumn rightColumn,
    CompareFlags flag,
    const Type* type) {
  return compareComplex<TypeKind::ARRAY>(
      left, right, leftColumn, rightColumn, flag, type);
}

FOLLY_ALWAYS_INLINE int32_t compareRow(
    const char* left,
    const char* right,
    int key,
    const std::vector<RowColumn>& rowColumns,
    RowTypePtr rowType,
    CompareFlags flags) {
  auto result = BOLT_DYNAMIC_TYPE_DISPATCH_ALL(
      compareByRow,
      rowType->childAt(key)->kind(),
      left,
      right,
      rowColumns[key],
      rowColumns[key],
      flags,
      rowType->childAt(key).get());
  return result;
}

} // namespace bytedance::bolt::exec
