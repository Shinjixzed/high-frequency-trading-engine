#pragma once

#include "types.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include <cstddef>

#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#else
#endif

namespace trading_engine {
    // Branch prediction hints
#ifdef __GNUC__
#define LIKELY(x)       __builtin_expect(!!(x), 1)
#define UNLIKELY(x)     __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)       (x)
#define UNLIKELY(x)     (x)
#endif

    // Force inline for critical functions
#ifdef _MSC_VER
#define FORCE_INLINE __forceinline
#else
#define FORCE_INLINE __attribute__((always_inline)) inline
#endif

    // Memory prefetching
    class PrefetchOptimizer {
    public:
        template<typename T>
        static FORCE_INLINE void prefetch_read(const T *ptr) noexcept {
#ifdef __GNUC__
            __builtin_prefetch(ptr, 0, 3); // Read, high temporal locality
#endif
        }

        template<typename T>
        static FORCE_INLINE void prefetch_write(T *ptr) noexcept {
#ifdef __GNUC__
            __builtin_prefetch(ptr, 1, 3); // Write, high temporal locality
#endif
        }
    };

    // Lock-free memory pool
    template<typename T, size_t PoolSize = 1024>
    class LockFreeMemoryPool {
        struct alignas(CacheLineSize) Node {
            T data{};
            Node *next{};
        };

        alignas(CacheLineSize) std::atomic<Node *> free_list_head{nullptr};
        alignas(CacheLineSize) std::array<Node, PoolSize> pool;
        alignas(CacheLineSize) std::atomic<size_t> allocated_count{0};

    public:
        LockFreeMemoryPool() {
            // Initialize free list
            for (size_t i = 0; i < PoolSize - 1; ++i) {
                pool[i].next = &pool[i + 1];
            }
            pool[PoolSize - 1].next = nullptr;
            free_list_head.store(&pool[0], std::memory_order_relaxed);
        }

        T *acquire() noexcept {
            Node *head = free_list_head.load(std::memory_order_acquire);
            while (head != nullptr) {
                Node *next = head->next;
                if (free_list_head.compare_exchange_weak(
                    head, next, std::memory_order_release, std::memory_order_acquire)) {
                    allocated_count.fetch_add(1, std::memory_order_relaxed);
                    return &head->data;
                }
            }
            return nullptr; // Pool exhausted
        }

        void release(T *ptr) noexcept {
            if (ptr == nullptr) return;

            Node *node = reinterpret_cast<Node *>(
                reinterpret_cast<char *>(ptr) - offsetof(Node, data));

            Node *head = free_list_head.load(std::memory_order_acquire);
            do {
                node->next = head;
            } while (!free_list_head.compare_exchange_weak(
                head, node, std::memory_order_release, std::memory_order_acquire));

            allocated_count.fetch_sub(1, std::memory_order_relaxed);
        }

        [[nodiscard]] size_t size() const noexcept {
            return allocated_count.load(std::memory_order_relaxed);
        }

        [[nodiscard]] static size_t capacity() noexcept {
            return PoolSize;
        }

        [[nodiscard]] bool empty() const noexcept {
            return allocated_count.load(std::memory_order_relaxed) == 0;
        }

        [[nodiscard]] bool full() const noexcept {
            return allocated_count.load(std::memory_order_relaxed) == PoolSize;
        }
    };

    // NUMA-aware allocator
    class NUMAAllocator {
    private:
        struct alignas(CacheLineSize) PerCorePool {
            LockFreeMemoryPool<Order, 1024> orders;
            LockFreeMemoryPool<MarketTick, 2048> ticks;
            LockFreeMemoryPool<Trade, 512> trades;
        };

        inline static thread_local PerCorePool *local_pool = nullptr;
        std::vector<std::unique_ptr<PerCorePool> > pools;
        std::atomic<uint32_t> pool_count{0};

    public:
        static NUMAAllocator &instance() {
            static NUMAAllocator allocator;
            return allocator;
        }

        void initialize(uint32_t num_cores) {
            pools.reserve(num_cores);
            for (uint32_t i = 0; i < num_cores; ++i) {
                pools.emplace_back(std::make_unique<PerCorePool>());
            }
            pool_count.store(num_cores, std::memory_order_release);
        }

        template<typename T>
        T *allocate() {
            if (local_pool == nullptr) {
                uint32_t core_id = get_current_core_id();
                if (core_id < pool_count.load(std::memory_order_acquire)) {
                    local_pool = pools[core_id].get();
                } else {
                    local_pool = pools[0].get(); // Fallback
                }
            }

            if constexpr (std::is_same_v<T, Order>) {
                return local_pool->orders.acquire();
            } else if constexpr (std::is_same_v<T, MarketTick>) {
                return local_pool->ticks.acquire();
            } else if constexpr (std::is_same_v<T, Trade>) {
                return local_pool->trades.acquire();
            }
            return nullptr;
        }

        template<typename T>
        void deallocate(T *ptr) {
            if (ptr == nullptr) return;

            if constexpr (std::is_same_v<T, Order>) {
                local_pool->orders.release(ptr);
            } else if constexpr (std::is_same_v<T, MarketTick>) {
                local_pool->ticks.release(ptr);
            } else if constexpr (std::is_same_v<T, Trade>) {
                local_pool->trades.release(ptr);
            }
        }

    private:
        static uint32_t get_current_core_id() {
            // Platform-specific core ID detection
#ifdef _WIN32
            return GetCurrentProcessorNumber();
#else
            return sched_getcpu();
#endif
        }
    };

    // Circular buffer for strategy data
    template<typename T, size_t Size>
    class CircularBuffer {
        static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
        static constexpr size_t mask = Size - 1;

        alignas(CacheLineSize) std::array<T, Size> buffer;
        alignas(CacheLineSize) std::atomic<size_t> head{0};
        alignas(CacheLineSize) std::atomic<size_t> tail{0};

    public:
        void push(const T &item) noexcept {
            size_t current_tail = tail.load(std::memory_order_relaxed);
            buffer[current_tail & mask] = item;
            tail.store(current_tail + 1, std::memory_order_release);
        }

        bool try_pop(T &item) noexcept {
            size_t current_head = head.load(std::memory_order_relaxed);
            size_t current_tail = tail.load(std::memory_order_acquire);

            if (current_head == current_tail) {
                return false; // Empty
            }

            item = buffer[current_head & mask];
            head.store(current_head + 1, std::memory_order_release);
            return true;
        }

        [[nodiscard]] size_t size() const noexcept {
            size_t current_tail = tail.load(std::memory_order_acquire);
            size_t current_head = head.load(std::memory_order_acquire);
            return current_tail - current_head;
        }

        [[nodiscard]] bool empty() const noexcept {
            return head.load(std::memory_order_acquire) ==
                   tail.load(std::memory_order_acquire);
        }

        T &operator[](size_t index) noexcept;

        const T &operator[](size_t index) const noexcept;
    };

    template<typename T, size_t Size>
    T &CircularBuffer<T, Size>::operator[](const size_t index) noexcept {
        const size_t current_head = head.load(std::memory_order_acquire);
        return buffer[(current_head + index) & mask];
    }

    template<typename T, size_t Size>
    const T &CircularBuffer<T, Size>::operator[](const size_t index) const noexcept {
        const size_t current_head = head.load(std::memory_order_acquire);
        return buffer[(current_head + index) & mask];
    }

    // Cache-aligned structure wrapper
    template<typename T>
    struct alignas(CacheLineSize) CacheAligned {
        T data;

        template<typename... Args>
        explicit CacheAligned(Args &&... args) : data(std::forward<Args>(args)...) {
        }

        T &operator*() noexcept { return data; }
        const T &operator*() const noexcept { return data; }

        T *operator->() noexcept { return &data; }
        const T *operator->() const noexcept { return &data; }
    };
}
