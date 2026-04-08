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

#include <gtest/gtest.h>
#include <algorithm>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "bolt/type/StringViewBase.h"

using namespace bytedance::bolt;

template <typename TView>
class StringViewBaseTypedTest : public ::testing::Test {};

using StringViewBaseTypes = ::testing::Types<StringView4, StringView12>;
TYPED_TEST_SUITE(StringViewBaseTypedTest, StringViewBaseTypes);

TYPED_TEST(StringViewBaseTypedTest, BasicInlineAndOutlineBehavior) {
  const std::string text = "We are stardust, we are golden...";
  for (int32_t i = 0; i <= static_cast<int32_t>(text.size()); ++i) {
    std::string subText(text.data(), i);
    TypeParam view(subText.data(), subText.size());

    EXPECT_EQ(view.size(), i);
    EXPECT_EQ(view.isInline(), i <= TypeParam::kInlineSize);
    if (view.isInline()) {
      EXPECT_NE(view.data(), subText.data());
    } else {
      EXPECT_EQ(view.data(), subText.data());
    }
    EXPECT_EQ(view.materialize(), subText);
    EXPECT_EQ(view.getString(), subText);

    std::stringstream out;
    out << view;
    EXPECT_EQ(out.str(), subText);
  }
}

TYPED_TEST(StringViewBaseTypedTest, Comparison) {
  EXPECT_LT(TypeParam(""), TypeParam("ab"));
  EXPECT_LT(TypeParam("ab"), TypeParam("abc"));
  EXPECT_LT(TypeParam("abc"), TypeParam("abd"));
  EXPECT_EQ(TypeParam("pref").compare(TypeParam("pref")), 0);
  EXPECT_EQ(
      TypeParam("pref01234567extend").compare(TypeParam("pref01234567extend")),
      0);
  EXPECT_NE(TypeParam("same"), TypeParam("sama"));
}

TYPED_TEST(StringViewBaseTypedTest, ContainerSupport) {
  std::vector<std::string> strings = {
      "May",
      "I walk",
      "beside you",
      "I've come here to lose",
      "the smog"};

  std::vector<TypeParam> views;
  std::unordered_map<TypeParam, int32_t> map;
  for (int32_t i = 0; i < strings.size(); ++i) {
    views.emplace_back(strings[i]);
    map[views.back()] = i;
  }

  for (int32_t i = 0; i < strings.size(); ++i) {
    auto it = map.find(TypeParam(strings[i]));
    ASSERT_NE(it, map.end());
    EXPECT_EQ(it->second, i);
  }

  std::sort(views.begin(), views.end());
  for (int32_t i = 0; i + 1 < views.size(); ++i) {
    EXPECT_LE(views[i], views[i + 1]);
  }
}

TYPED_TEST(StringViewBaseTypedTest, ImplicitConstructionAndConversion) {
  TypeParam sv1("literal");
  EXPECT_EQ(sv1, TypeParam("literal"));

  TypeParam sv2 = "literal";
  EXPECT_EQ(sv2, TypeParam("literal"));

  std::optional<TypeParam> sv3 = "literal";
  ASSERT_TRUE(sv3.has_value());
  EXPECT_EQ(*sv3, TypeParam("literal"));

  auto testRegularConversion = [](TypeParam sv) {
    EXPECT_EQ(sv, TypeParam("literal"));
  };
  testRegularConversion("literal");
}

TYPED_TEST(StringViewBaseTypedTest, NegativeSizes) {
  EXPECT_THROW(TypeParam("abc", -10), BoltException);
  EXPECT_NO_THROW(TypeParam(nullptr, 0));
}

TYPED_TEST(StringViewBaseTypedTest, LinearSearch) {
  std::vector<TypeParam> values = {
      TypeParam("zero"),
      TypeParam("one"),
      TypeParam("two"),
      TypeParam("three"),
      TypeParam("four")};

  EXPECT_EQ(
      TypeParam::linearSearch(TypeParam("three"), values.data(), nullptr, 5),
      3);
  EXPECT_EQ(
      TypeParam::linearSearch(TypeParam("missing"), values.data(), nullptr, 5),
      -1);

  const int32_t indices[] = {4, 2, 1, 3, 0};
  EXPECT_EQ(
      TypeParam::linearSearch(TypeParam("three"), values.data(), indices, 5),
      3);
  EXPECT_EQ(
      TypeParam::linearSearch(TypeParam("zero"), values.data(), indices, 5),
      4);
}

TEST(StringViewBaseTest, LayoutAndInlineBoundary) {
  EXPECT_EQ(sizeof(StringView4), 16);
  EXPECT_GE(sizeof(StringView12), 24);

  EXPECT_TRUE(StringView4::isInline(12));
  EXPECT_FALSE(StringView4::isInline(13));
  EXPECT_TRUE(StringView12::isInline(20));
  EXPECT_FALSE(StringView12::isInline(21));
}
