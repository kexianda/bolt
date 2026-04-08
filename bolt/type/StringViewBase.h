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
 * --------------------------------------------------------------------------
 */

#pragma once

#include <concepts>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

#include <folly/FBString.h>
#include <folly/Format.h>
#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/dynamic.h>

#include <fmt/format.h>

#include "bolt/common/base/Exceptions.h"

namespace bytedance::bolt {

template <uint32_t PrefixLen>
concept ValidStringViewPrefixLen = (PrefixLen == 4 || PrefixLen == 12);

template <uint32_t PrefixLen>
  requires ValidStringViewPrefixLen<PrefixLen>
struct StringViewBase {
 public:
  using value_type = char;

  static constexpr size_t kPrefixSize = PrefixLen * sizeof(char);
  static constexpr size_t kInlinePayloadSize = 8;
  static constexpr size_t kInlineSize = kPrefixSize + kInlinePayloadSize;

  StringViewBase() {
    clear();
  }

  StringViewBase(const char* data, int32_t len) {
    set(data, len);
  }

  void clear() {
    auto* p = reinterpret_cast<int64_t*>(this);
    *p++ = 0L;
    *p = 0L;
    if constexpr (PrefixLen == 12) {
      *(reinterpret_cast<int32_t*>(p) + (2 * sizeof(int64_t))) = 0;
    }
  }

  void set(const char* data, int32_t len) {
    BOLT_CHECK_GE(len, 0);
    BOLT_DCHECK(data || len == 0);

    clear();

    size_ = len;

    if (size_ == 0) {
      return;
    }

    if (isInline()) {
      const auto prefixBytes = std::min<uint32_t>(size_, kPrefixSize);
      std::memcpy(prefix_, data, prefixBytes);
      if (size_ > kPrefixSize) {
        std::memcpy(value_.inlined, data + kPrefixSize, size_ - kPrefixSize);
      }
    } else {
      std::memcpy(prefix_, data, kPrefixSize);
      value_.data = data;
    }
  }

  static StringViewBase makeInline(std::string str) {
    BOLT_DCHECK(isInline(str.size()));
    return StringViewBase{str};
  }

  /* implicit */ StringViewBase(const char* data)
      : StringViewBase(data, std::strlen(data)) {}

  explicit StringViewBase(const folly::fbstring& value)
      : StringViewBase(value.data(), value.size()) {}
  explicit StringViewBase(folly::fbstring&& value) = delete;

  explicit StringViewBase(const std::string& value)
      : StringViewBase(value.data(), value.size()) {}
  explicit StringViewBase(std::string&& value) = delete;

  explicit StringViewBase(std::string_view value)
      : StringViewBase(value.data(), value.size()) {}

  bool isInline() const {
    return isInline(size_);
  }

  static constexpr bool isInline(uint32_t size) {
    return size <= kInlineSize;
  }

  const char* data() && = delete;
  const char* data() const& {
    return isInline() ? prefix_ : value_.data;
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
    if (sizeAndPrefixAsInt64() != other.sizeAndPrefixAsInt64()) {
      return false;
    }

    if (size_ <= kPrefixSize) {
      return true;
    }

    if (isInline()) {
      return std::memcmp(
                 value_.inlined, other.value_.inlined, size_ - kPrefixSize) ==
          0;
    }

    return std::memcmp(
               value_.data + kPrefixSize,
               other.value_.data + kPrefixSize,
               size_ - kPrefixSize) == 0;
  }

  bool operator!=(const StringViewBase& other) const {
    return !(*this == other);
  }

  int32_t compare(const StringViewBase& other) const {
    auto prefix = prefixAsInt();
    auto otherPrefix = other.prefixAsInt();
    if (prefix != otherPrefix) {
      if constexpr (folly::kIsLittleEndian) {
        prefix = __builtin_bswap32(prefix);
        otherPrefix = __builtin_bswap32(otherPrefix);
      }
      return prefix < otherPrefix ? -1 : 1;
    }

    const int32_t remaining = std::min(size_, other.size_) - kPrefixSize;
    if (remaining <= 0) {
      return size_ - other.size_;
    }

    const auto result = std::memcmp(
        data() + kPrefixSize, other.data() + kPrefixSize, remaining);
    return result != 0 ? result : size_ - other.size_;
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

  static int32_t linearSearch(
      StringViewBase key,
      const StringViewBase* strings,
      const int32_t* indices,
      int32_t numStrings) {
    if (indices) {
      for (auto i = 0; i < numStrings; ++i) {
        if (strings[indices[i]] == key) {
          return i;
        }
      }
    } else {
      for (auto i = 0; i < numStrings; ++i) {
        if (strings[i] == key) {
          return i;
        }
      }
    }
    return -1;
  }

  const char* prefix() const {
    return prefix_;
  }

  const char* value() const {
    return value_.data;
  }

 private:
  int64_t sizeAndPrefixAsInt64() const {
    return reinterpret_cast<const int64_t*>(this)[0];
  }

  int32_t prefixAsInt() const {
    return *reinterpret_cast<const int32_t*>(prefix_);
  }

  uint32_t size_{0};
  char prefix_[kPrefixSize]{0};
  union {
    char inlined[kInlinePayloadSize];
    const char* data;
  } value_{.data = nullptr};
};

using StringView4 = StringViewBase<4>;
using StringView12 = StringViewBase<12>;

using StringView2 = StringViewBase<12>;

} // namespace bytedance::bolt

namespace std {
template <>
struct hash<::bytedance::bolt::StringView4> {
  size_t operator()(const ::bytedance::bolt::StringView4& view) const {
    return std::hash<std::string_view>{}(
        std::string_view(view.data(), view.size()));
  }
};

template <>
struct hash<::bytedance::bolt::StringView12> {
  size_t operator()(const ::bytedance::bolt::StringView12& view) const {
    return std::hash<std::string_view>{}(
        std::string_view(view.data(), view.size()));
  }
};
} // namespace std

namespace folly {
template <>
struct hasher<::bytedance::bolt::StringView4> {
  size_t operator()(const ::bytedance::bolt::StringView4& view) const {
    return std::hash<::bytedance::bolt::StringView4>{}(view);
  }
};

template <>
struct hasher<::bytedance::bolt::StringView12> {
  size_t operator()(const ::bytedance::bolt::StringView12& view) const {
    return std::hash<::bytedance::bolt::StringView12>{}(view);
  }
};
} // namespace folly

template <>
struct fmt::formatter<bytedance::bolt::StringView4>
    : private fmt::formatter<std::string_view> {
  template <typename FormatContext>
  auto format(bytedance::bolt::StringView4 s, FormatContext& ctx) const {
    return fmt::formatter<std::string_view>::format(
        std::string_view(s.data(), s.size()), ctx);
  }
};

template <>
struct fmt::formatter<bytedance::bolt::StringView12>
    : private fmt::formatter<std::string_view> {
  template <typename FormatContext>
  auto format(bytedance::bolt::StringView12 s, FormatContext& ctx) const {
    return fmt::formatter<std::string_view>::format(
        std::string_view(s.data(), s.size()), ctx);
  }
};
