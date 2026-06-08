#pragma once

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>

#include "bolt/common/base/Exceptions.h"
#include "bolt/common/memory/MemoryPool.h"

namespace bytedance::bolt::memory {

struct alignas(64) SlabAllocatorState {
  MemoryPool* pool{nullptr};
  inline static thread_local std::size_t freeBytes{0};
};

template <typename T>
class SlabAllocator {
 public:
  using value_type = T;
  using pointer = T*;
  using const_pointer = const T*;
  using void_pointer = void*;
  using const_void_pointer = const void*;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;

  template <typename U>
  struct rebind {
    using other = SlabAllocator<U>;
  };

  SlabAllocator() noexcept = default;

  explicit SlabAllocator(MemoryPool* pool) noexcept : pool_(pool) {
    state_.pool = pool;
  }

  SlabAllocator(const SlabAllocator&) noexcept = default;

  template <typename U>
  SlabAllocator(const SlabAllocator<U>& other) noexcept
      : pool_(other.pool_), state_(other.state_) {}

  SlabAllocator& operator=(const SlabAllocator&) noexcept = default;

  [[nodiscard]] T* allocate(std::size_t n) {
    return allocate(n, 1);
  }

  [[nodiscard]] T* allocate(std::size_t n, std::size_t alignment) {
    const std::size_t bytes = sizeAlign(n * sizeof(T), alignment);
    if (bytes >= kLargeAllocationThreshold) [[unlikely]] {
      return static_cast<T*>(pool_->allocate(bytes, alignment));
    }

    const auto freeBytes = state_.freeBytes;
    std::size_t newReservedBytes{0};
    while (bytes > freeBytes + newReservedBytes) [[unlikely]] {
      newReservedBytes += kReservationQuantum;
    }

    if (newReservedBytes > 0) [[unlikely]] {
      poolImpl()->reserve(newReservedBytes);
      state_.freeBytes += newReservedBytes;
    }

    void* p = std::malloc(bytes);
    if (p == nullptr) [[unlikely]] {
      if (newReservedBytes > 0) {
        poolImpl()->release(newReservedBytes);
        state_.freeBytes -= newReservedBytes;
      }
      throw std::bad_alloc();
    }

    state_.freeBytes -= bytes;
    return static_cast<T*>(p);
  }

  void deallocate(T* p, std::size_t n) noexcept {
    deallocate(p, n, 1);
  }

  void deallocate(T* p, std::size_t n, std::size_t alignment) noexcept {
    if (p == nullptr) [[unlikely]] {
      return;
    }

    const std::size_t bytes = sizeAlign(n * sizeof(T), alignment);
    if (bytes >= kLargeAllocationThreshold) [[unlikely]] {
      pool_->free(p, bytes, alignment);
      return;
    }

    const auto freeBytes = state_.freeBytes;

    std::size_t recycleBytes{0};
    while (freeBytes + bytes >= recycleBytes + kReservationQuantum) [[unlikely]] {
      recycleBytes += kReservationQuantum;
    }

    state_.freeBytes += bytes;
    if (recycleBytes > 0) [[unlikely]] {
      state_.freeBytes -= recycleBytes;
      poolImpl()->release(recycleBytes);
    }
    std::free(p);
  }

  MemoryPool* pool() const noexcept {
    return pool_;
  }

  std::size_t freeBytes() const noexcept {
    return state_.freeBytes;
  }

  template <typename U>
  bool operator==(const SlabAllocator<U>& other) const noexcept {
    return pool_ == other.pool_;
  }

  template <typename U>
  bool operator!=(const SlabAllocator<U>& other) const noexcept {
    return !(*this == other);
  }

 private:
  template <typename U>
  friend class SlabAllocator;

  static constexpr std::size_t kReservationQuantum = 1 << 20;

  // for <=32K allocations, jemalloc performs well.
  // For larger allocations, we delegate to the memory pool.
  static constexpr std::size_t kLargeAllocationThreshold = 32 << 10;

  static std::size_t sizeAlign(std::size_t size, std::size_t alignment) {
    const auto remainder = size % alignment;
    return (remainder == 0) ? size : size + alignment - remainder;
  }

  MemoryPoolImpl* poolImpl() const {
    BOLT_CHECK_NOT_NULL(pool_);
    auto* impl = dynamic_cast<MemoryPoolImpl*>(pool_);
    BOLT_CHECK_NOT_NULL(impl);
    return impl;
  }

  MemoryPool* pool_{nullptr};
  SlabAllocatorState state_;
};

} // namespace bytedance::bolt::memory
