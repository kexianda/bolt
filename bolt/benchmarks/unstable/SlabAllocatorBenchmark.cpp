/*
 * Copyright (c) ByteDance Ltd. and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <folly/Benchmark.h>
#include <folly/init/Init.h>

#include "bolt/common/memory/HashStringAllocator.h"
#include "bolt/common/memory/Memory.h"
#include "bolt/common/memory/SlabAllocator.h"

DEFINE_int64(
    slab_allocator_benchmark_count,
    2'000'000,
    "The number of small allocations in allocator benchmark");

using namespace bytedance::bolt;
using namespace bytedance::bolt::memory;

namespace {

constexpr int64_t kBenchmarkCapacity = 1L << 30;
constexpr size_t kCachedAllocations256K = 256 * 1024;
constexpr size_t kCachedAllocations512K = 512 * 1024;
constexpr size_t kCachedAllocations1M = 1024 * 1024;
constexpr size_t kCachedAllocations2M = 2 * 1024 * 1024;

constexpr std::array<int32_t, 128> kAllocationSizes = {
    9,  15, 18, 20, 15, 13, 21, 20, 19, 23, 16, 12, 15, 29, 5,  21,
    16, 10, 15, 15, 11, 17, 30, 13, 23, 26, 16, 19, 15, 24, 3,  7,
    10, 13, 20, 13, 11, 12, 13, 22, 28, 7,  14, 16, 30, 23, 21, 16,
    19, 20, 6,  11, 27, 17, 9,  21, 14, 16, 18, 2,  4,  11, 22, 10,
    13, 16, 22, 29, 25, 18, 26, 15, 12, 25, 12, 14, 14, 22, 24, 16,
    14, 17, 17, 8,  17, 11, 18, 15, 18, 8,  12, 14, 1,  20, 7,  13,
    17, 12, 24, 18, 18, 8,  19, 17, 11, 19, 21, 9,  18, 12, 16, 13,
    28, 9,  19, 5,  14, 20, 6,  16, 10, 14, 15, 17, 10, 15, 27, 19};

static_assert(kAllocationSizes.size() == 128);

class SlabAllocatorBenchmark {
 public:
  SlabAllocatorBenchmark() {
    MemoryManager::Options options;
    options.allocatorCapacity = kBenchmarkCapacity;
    options.arbitratorCapacity = kBenchmarkCapacity;
    options.useMmapAllocator = false;
    manager_ = std::make_unique<MemoryManager>(options);
    pool_ = manager_->addLeafPool("slab_allocator_benchmark");
    hashAllocator_ = std::make_unique<HashStringAllocator>(pool_.get());
  }

  template <size_t kCachedAllocations>
  int64_t runSlabAllocFree() {
    SlabAllocator<char> allocator(pool_.get());
    return runFifoAllocFree<kCachedAllocations>(allocator);
  }

  template <size_t kCachedAllocations>
  int64_t runHashStringAllocFree() {
    bytedance::bolt::StlAllocator<char> allocator(hashAllocator_.get());
    return runFifoAllocFree<kCachedAllocations>(allocator);
  }

 private:
  template <size_t kCachedAllocations, typename Allocator>
  int64_t runFifoAllocFree(Allocator& allocator) {
    auto allocations = std::make_unique<char*[]>(kCachedAllocations);
    auto sizes = std::make_unique<int32_t[]>(kCachedAllocations);
    size_t cachedAllocations{0};
    size_t nextToFree{0};

    for (int64_t i = 0; i < FLAGS_slab_allocator_benchmark_count; ++i) {
      const auto size = kAllocationSizes[i % kAllocationSizes.size()];
      auto* p = allocator.allocate(size);
      folly::doNotOptimizeAway(p);
      if ((i & 3) == 0) {
        allocator.deallocate(p, size);
        continue;
      }

      if (cachedAllocations < kCachedAllocations) {
        allocations[cachedAllocations] = p;
        sizes[cachedAllocations] = size;
        ++cachedAllocations;
      } else {
        allocator.deallocate(allocations[nextToFree], sizes[nextToFree]);
        allocations[nextToFree] = p;
        sizes[nextToFree] = size;
        nextToFree = (nextToFree + 1) % kCachedAllocations;
      }
    }

    for (size_t i = 0; i < cachedAllocations; ++i) {
      const auto index = (nextToFree + i) % cachedAllocations;
      allocator.deallocate(allocations[index], sizes[index]);
    }
    return FLAGS_slab_allocator_benchmark_count;
  }

  std::unique_ptr<MemoryManager> manager_;
  std::shared_ptr<MemoryPool> pool_;
  std::unique_ptr<HashStringAllocator> hashAllocator_;
};

BENCHMARK_MULTI(HashStringAllocatorFifoAllocFree256K) {
  SlabAllocatorBenchmark benchmark;
  return benchmark.runHashStringAllocFree<kCachedAllocations256K>();
}

BENCHMARK_RELATIVE_MULTI(SlabAllocatorFifoAllocFree256K) {
  SlabAllocatorBenchmark benchmark;
  return benchmark.runSlabAllocFree<kCachedAllocations256K>();
}

BENCHMARK_DRAW_LINE();

BENCHMARK_MULTI(HashStringAllocatorFifoAllocFree512K) {
  SlabAllocatorBenchmark benchmark;
  return benchmark.runHashStringAllocFree<kCachedAllocations512K>();
}

BENCHMARK_RELATIVE_MULTI(SlabAllocatorFifoAllocFree512K) {
  SlabAllocatorBenchmark benchmark;
  return benchmark.runSlabAllocFree<kCachedAllocations512K>();
}

BENCHMARK_DRAW_LINE();

BENCHMARK_MULTI(HashStringAllocatorFifoAllocFree1M) {
  SlabAllocatorBenchmark benchmark;
  return benchmark.runHashStringAllocFree<kCachedAllocations1M>();
}

BENCHMARK_RELATIVE_MULTI(SlabAllocatorFifoAllocFree1M) {
  SlabAllocatorBenchmark benchmark;
  return benchmark.runSlabAllocFree<kCachedAllocations1M>();
}

BENCHMARK_DRAW_LINE();

BENCHMARK_MULTI(HashStringAllocatorFifoAllocFree2M) {
  SlabAllocatorBenchmark benchmark;
  return benchmark.runHashStringAllocFree<kCachedAllocations2M>();
}

BENCHMARK_RELATIVE_MULTI(SlabAllocatorFifoAllocFree2M) {
  SlabAllocatorBenchmark benchmark;
  return benchmark.runSlabAllocFree<kCachedAllocations2M>();
}

} // namespace

int main(int argc, char* argv[]) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
