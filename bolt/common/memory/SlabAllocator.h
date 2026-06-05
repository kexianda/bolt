#pragma once

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

#include "bolt/common/base/Exceptions.h"
#include "bolt/common/memory/MemoryPool.h"

namespace bytedance::bolt::memory {

struct SlabAllocatorState {
  MemoryPool* pool{nullptr};
  std::size_t usedBytes{0};
  std::size_t reservedBytes{0};
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

  explicit SlabAllocator(MemoryPool* pool) noexcept
      : pool_(pool), state_(std::make_shared<SlabAllocatorState>()) {
    state_->pool = pool;
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
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::bad_array_new_length();
    }

    const std::size_t bytes = sizeAlign(n * sizeof(T), alignment);
    if (bytes >= kLargeAllocationThreshold) {
      return static_cast<T*>(pool_->allocate(bytes, alignment));
    }

    std::size_t newReservedBytes{0};
    while (state_->usedBytes + bytes >
           state_->reservedBytes + newReservedBytes) {
      newReservedBytes += kReservationQuantum;
    }

    if (newReservedBytes > 0) {
      poolImpl()->reserve(newReservedBytes);
      state_->reservedBytes += newReservedBytes;
    }

    void* p = std::malloc(bytes);
    if (p == nullptr) {
      if (newReservedBytes > 0) {
        poolImpl()->release(newReservedBytes);
        state_->reservedBytes -= newReservedBytes;
      }
      throw std::bad_alloc();
    }

    state_->usedBytes += bytes;
    return static_cast<T*>(p);
  }

  void deallocate(T* p, std::size_t n) noexcept {
    deallocate(p, n, 1);
  }

  void deallocate(T* p, std::size_t n, std::size_t alignment) noexcept {
    if (p == nullptr) {
      return;
    }

    const std::size_t bytes = sizeAlign(n * sizeof(T), alignment);
    if (bytes >= kLargeAllocationThreshold) {
      pool_->free(p, bytes, alignment);
      return;
    }

    BOLT_DCHECK_GE(state_->usedBytes, bytes);

    std::size_t recycleBytes{0};
    while (state_->reservedBytes >= recycleBytes + kReservationQuantum &&
           state_->reservedBytes - recycleBytes - (state_->usedBytes - bytes) >=
               kReservationQuantum) {
      recycleBytes += kReservationQuantum;
    }

    if (recycleBytes > 0) {
      state_->reservedBytes -= recycleBytes;
      poolImpl()->release(recycleBytes);
    }

    state_->usedBytes -= bytes;
    std::free(p);
  }

  MemoryPool* pool() const noexcept {
    return pool_;
  }

  std::size_t usedBytes() const noexcept {
    return state_->usedBytes;
  }

  std::size_t reservedBytes() const noexcept {
    return state_->reservedBytes;
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
  std::shared_ptr<SlabAllocatorState> state_{
      std::make_shared<SlabAllocatorState>()};
};

} // namespace bytedance::bolt::memory
