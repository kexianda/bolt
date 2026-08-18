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

#include <algorithm>

#include <folly/init/Init.h>
#include <gtest/gtest.h>
#include <sys/resource.h>

namespace {

void raiseFileDescriptorLimit() {
  constexpr rlim_t kRequired = 10000;
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur >= kRequired) {
    return;
  }

  limit.rlim_cur = std::min(limit.rlim_max, kRequired);
  (void)setrlimit(RLIMIT_NOFILE, &limit);
}

} // namespace

int main(int argc, char** argv) {
  raiseFileDescriptorLimit();
  testing::InitGoogleTest(&argc, argv);
  // todo: use folly::Init init after upgrade folly lib
  folly::init(&argc, &argv, false);
  return RUN_ALL_TESTS();
}
