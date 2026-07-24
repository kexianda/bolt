/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
 *
 * --------------------------------------------------------------------------
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

#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

#include <folly/FBString.h>
#include <folly/Format.h>
#include <folly/Range.h>
#include <folly/dynamic.h>

#include <fmt/format.h>

#include "bolt/common/base/BitUtil.h"
#include "bolt/common/base/Exceptions.h"

#include <folly/Portability.h>
namespace bytedance::bolt {

// Variable length string or binary type for use in vectors. This has
// semantics similar to std::string_view or folly::StringPiece and
// exposes a subset of the interface. If the string is 12 characters
// or less, it is inlined and no reference is held. If it is longer, a
// reference to the string is held and the 4 first characters are
// cached in the StringViewBase. This allows failing comparisons early and
// reduces the CPU cache working set when dealing with short strings.
//
// Adapted from TU Munich Umbra and CWI DuckDB.
//
// TODO: Extend the interface to parity with folly::StringPiece as needed.
template <bool ForRow>
struct StringViewBase {
 public:
  using value_type = char;

  static constexpr size_t kPrefixSize = 4 * sizeof(char);
  static constexpr size_t kInlineSize = 12;

  StringViewBase() {
    static_assert(sizeof(StringViewBase) == 16);
    if constexpr (sizeof(StringViewBase) == 16) {
      auto* words = reinterpret_cast<int64_t*>(this);
      words[0] = 0;
      words[1] = 0;
    } else {
      memset(this, 0, sizeof(StringViewBase));
    }
  }

  StringViewBase(const char* data, int32_t len) {
    set(data, len);
  }

  template <bool OtherForRow, std::enable_if_t<ForRow && !OtherForRow, int> = 0>
  /* implicit */ StringViewBase(const StringViewBase<OtherForRow>& other)
      : StringViewBase(other.data(), other.size()) {}

  void set(const char* data, int32_t len) {
    BOLT_CHECK_GE(len, 0);
    BOLT_DCHECK(data || len == 0);
    size_ = len;
    if (isInline()) {
      // Zero the inline part.
      // this makes sure that inline strings can be compared for equality with 2
      // int64 compares.
      if constexpr (kPrefixSize == 4) {
        *reinterpret_cast<int32_t*>(prefix_) = 0;
      } else {
        memset(prefix_, 0, kPrefixSize);
      }
      if (size_ == 0) {
        return;
      }
      // small string: inlined. Zero the last 8 bytes first to allow for whole
      // word comparison.
      value_.data = nullptr;
      memcpy(prefix_, data, size_);
    } else {
      // large string: store pointer
      if constexpr (kPrefixSize == 4) {
        *reinterpret_cast<int32_t*>(prefix_) =
            *reinterpret_cast<const int32_t*>(data);
      } else {
        memcpy(prefix_, data, kPrefixSize);
      }
      value_.data = data;
    }
  }

  static StringViewBase makeInline(std::string str) {
    BOLT_DCHECK(isInline(str.size()));
    return StringViewBase{str};
  }

  // Making StringViewBase implicitly constructible/convertible from char* and
  // string literals, in order to allow for a more flexible API and optional
  // interoperability. E.g:
  //
  //   StringViewBase sv = "literal";
  //   std::optional<StringViewBase> osv = "literal";
  //
  /* implicit */ StringViewBase(const char* data)
      : StringViewBase(data, strlen(data)) {}

  explicit StringViewBase(const folly::fbstring& value)
      : StringViewBase(value.data(), value.size()) {}
  explicit StringViewBase(folly::fbstring&& value) = delete;

  explicit StringViewBase(const std::string& value)
      : StringViewBase(value.data(), value.size()) {}
  explicit StringViewBase(std::string&& value) = delete;

  explicit StringViewBase(std::string_view value)
      : StringViewBase(value.data(), value.size()) {}

  FOLLY_ALWAYS_INLINE bool isInline() const {
    return isInline(size_);
  }

  FOLLY_ALWAYS_INLINE static constexpr bool isInline(uint32_t size) {
    return size <= kInlineSize;
  }

  FOLLY_ALWAYS_INLINE bool isNonContiguous() const {
    if constexpr (ForRow) {
      return !isInline() &&
          (reinterpret_cast<uintptr_t>(value_.data) & kNonContiguousMask) != 0;
    }
    return false;
  }

  FOLLY_ALWAYS_INLINE void setNonContiguous() {
    static_assert(ForRow, "Only row StringViews can be non-contiguous");
    BOLT_CHECK(!isInline());
    value_.data = reinterpret_cast<const char*>(
        reinterpret_cast<uintptr_t>(value_.data) | kNonContiguousMask);
  }

  const char* data() && = delete;
  const char* data() const& {
    if (isInline()) {
      return prefix_;
    }
    if constexpr (ForRow) {
      return reinterpret_cast<const char*>(
          reinterpret_cast<uintptr_t>(value_.data) & ~kNonContiguousMask);
    }
    return value_.data;
  }

  size_t size() const {
    return size_;
  }

  size_t capacity() const {
    return size_;
  }

  friend std::ostream& operator<<(
      std::ostream& os,
      const StringViewBase& stringView) {
    os.write(stringView.data(), stringView.size());
    return os;
  }

  bool operator==(const StringViewBase& other) const {
    // Compare lengths and first 4 characters.
    if (sizeAndPrefixAsInt64() != other.sizeAndPrefixAsInt64()) {
      return false;
    }
    if (isInline()) {
      // The inline part is zeroed at construction, so we can compare
      // a word at a time if data extends past 'prefix_'.
      return size_ <= kPrefixSize || inlinedAsInt64() == other.inlinedAsInt64();
    }
    // Sizes are equal and this is not inline, therefore both are out
    // of line and have kPrefixSize first in common.
    return memcmp(
               data() + kPrefixSize,
               other.data() + kPrefixSize,
               size_ - kPrefixSize) == 0;
  }

  bool operator!=(const StringViewBase& other) const {
    return !(*this == other);
  }

  // Returns 0, if this == other
  //       < 0, if this < other
  //       > 0, if this > other
  int32_t compare(const StringViewBase& other) const {
    uint32_t prefix = prefixAsInt();
    uint32_t otherPrefix = other.prefixAsInt();
    if (prefix != otherPrefix) {
      // The result is decided on prefix. The shorter will be less
      // because the prefix is padded with zeros.
      if constexpr (folly::kIsLittleEndian) {
        prefix = __builtin_bswap32(prefix);
        otherPrefix = __builtin_bswap32(otherPrefix);
      }
      return prefix < otherPrefix ? -1 : 1;
    }
    int32_t size = std::min(size_, other.size_) - kPrefixSize;
    if (size <= 0) {
      // One ends within the prefix.
      return size_ - other.size_;
    }
    if (size <= kInlineSize && isInline() && other.isInline()) {
      uint64_t inlined = inlinedAsInt64();
      uint64_t otherInlined = other.inlinedAsInt64();
      if constexpr (folly::kIsLittleEndian) {
        inlined = __builtin_bswap64(inlined);
        otherInlined = __builtin_bswap64(otherInlined);
      }
      if (inlined == otherInlined) {
        return size_ - other.size_;
      }
      return (inlined < otherInlined) ? -1 : 1;
    }
    int32_t result =
        memcmp(data() + kPrefixSize, other.data() + kPrefixSize, size);
    return (result != 0) ? result : size_ - other.size_;
  }

  bool operator<(const StringViewBase& other) const {
    return compare(other) < 0;
  }

  bool operator<=(const StringViewBase& other) const {
    return compare(other) <= 0;
  }

  bool operator>(const StringViewBase& other) const {
    return compare(other) > 0;
  }

  bool operator>=(const StringViewBase& other) const {
    return compare(other) >= 0;
  }

  operator folly::StringPiece() && = delete;
  operator folly::StringPiece() const& {
    return folly::StringPiece(data(), size());
  }

  operator std::string() const {
    return std::string(data(), size());
  }

  std::string str() const {
    return *this;
  }

  std::string getString() const {
    return *this;
  }

  std::string materialize() const {
    return *this;
  }

  operator folly::dynamic() && = delete;
  operator folly::dynamic() const& {
    return folly::dynamic(folly::StringPiece(data(), size()));
  }

  operator std::string_view() && = delete;
  explicit operator std::string_view() const& {
    return std::string_view(data(), size());
  }

  const char* begin() && = delete;
  const char* begin() const& {
    return data();
  }

  const char* end() && = delete;
  const char* end() const& {
    return data() + size();
  }

  bool empty() const {
    return size() == 0;
  }

  /// Searches for 'key == strings[i]'for i >= 0 < numStrings. If
  /// 'indices' is given. searches for 'key ==
  /// strings[indices[i]]. Returns the first i for which the strings
  /// match or -1 if no match is found. Uses SIMD to accelerate the
  /// search. Accesses StringViewBase bodies in 32 byte vectors, thus
  /// expects up to 31 bytes of addressable padding after out of
  /// line strings. This is the case for bolt Buffers.
  static int32_t linearSearch(
      StringViewBase key,
      const StringViewBase* strings,
      const int32_t* indices,
      int32_t numStrings);

  const char* prefix() const {
    return prefix_;
  }

  const char* value() const {
    return value_.data;
  }

 private:
  static constexpr uintptr_t kNonContiguousMask = uintptr_t{1} << 63;

  inline int64_t sizeAndPrefixAsInt64() const {
    return reinterpret_cast<const int64_t*>(this)[0];
  }

  inline int64_t inlinedAsInt64() const {
    return reinterpret_cast<const int64_t*>(this)[1];
  }

  int32_t prefixAsInt() const {
    return *reinterpret_cast<const int32_t*>(&prefix_);
  }

  // We rely on all members being laid out top to bottom . C++
  // guarantees this.
  uint32_t size_{0};
  char prefix_[4]{0};
  union {
    char inlined[8];
    const char* data;
  } value_{.data = nullptr};
};

using StringView = StringViewBase<false>;
using RowStringView = StringViewBase<true>;

// This creates a user-defined literal for StringView. You can use it as:
//
//   auto myStringView = "my string"_sv;
//   auto vec = {"str1"_sv, "str2"_sv};
inline StringView operator""_sv(const char* str, size_t len) {
  return StringView(str, len);
}

} // namespace bytedance::bolt

namespace std {
template <bool ForRow>
struct hash<::bytedance::bolt::StringViewBase<ForRow>> {
  size_t operator()(
      const ::bytedance::bolt::StringViewBase<ForRow> view) const {
    return bytedance::bolt::bits::hashBytes(1, view.data(), view.size());
  }
};
} // namespace std

namespace folly {
template <bool ForRow>
struct hasher<::bytedance::bolt::StringViewBase<ForRow>> {
  size_t operator()(
      const ::bytedance::bolt::StringViewBase<ForRow> view) const {
    return bytedance::bolt::bits::hashBytes(1, view.data(), view.size());
  }
};

} // namespace folly

namespace fmt {
template <bool ForRow>
struct formatter<bytedance::bolt::StringViewBase<ForRow>>
    : private formatter<string_view> {
  using formatter<string_view>::parse;

  template <typename Context>
  typename Context::iterator format(
      bytedance::bolt::StringViewBase<ForRow> s,
      Context& ctx) const {
    return formatter<string_view>::format(string_view{s.data(), s.size()}, ctx);
  }
};
} // namespace fmt
