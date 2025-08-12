#pragma once

#include "types.h"
#include "memory.h"

#include <atomic>
#include <array>

namespace trading_engine {
    // Single Producer Single Consumer Queue
    template<typename T, size_t Size>
    class SPSCQueue {
        static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
        static constexpr size_t mask = Size - 1;

        alignas(CacheLineSize) std::atomic<size_t> head{0};
        alignas(CacheLineSize) std::atomic<size_t> tail{0};
        alignas(CacheLineSize) std::array<T, Size> buffer;

    public:
        bool try_push(const T &item) noexcept {
            const size_t current_tail = tail.load(std::memory_order_relaxed);
            const size_t next_tail = (current_tail + 1) & mask;

            if (next_tail == head.load(std::memory_order_acquire)) {
                return false; // Queue is full
            }

            buffer[current_tail] = item;
            tail.store(next_tail, std::memory_order_release);
            return true;
        }

        bool try_pop(T &item) noexcept {
            const size_t current_head = head.load(std::memory_order_relaxed);

            if (current_head == tail.load(std::memory_order_acquire)) {
                return false; // Queue is empty
            }

            item = buffer[current_head];
            head.store((current_head + 1) & mask, std::memory_order_release);
            return true;
        }

        void clear() noexcept {
            head.store(tail.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }

        [[nodiscard]] size_t size() const noexcept {
            const size_t current_tail = tail.load(std::memory_order_acquire);
            const size_t current_head = head.load(std::memory_order_acquire);
            return (current_tail - current_head) & mask;
        }

        [[nodiscard]] bool empty() const noexcept {
            return head.load(std::memory_order_acquire) ==
                   tail.load(std::memory_order_acquire);
        }

        [[nodiscard]] bool full() const noexcept {
            const size_t current_tail = tail.load(std::memory_order_acquire);
            const size_t next_tail = (current_tail + 1) & mask;
            return next_tail == head.load(std::memory_order_acquire);
        }

        [[nodiscard]] static constexpr size_t capacity() noexcept {
            return Size - 1; // One slot reserved for full/empty distinction
        }
    };

    // Multi Producer Single Consumer Queue
    template<typename T, size_t Size>
    class MPSCQueue {
        struct Node {
            std::atomic<Node *> next{nullptr};
            T data;
        };

        alignas(CacheLineSize) std::atomic<Node *> head{nullptr};
        alignas(CacheLineSize) std::atomic<Node *> tail{nullptr};
        LockFreeMemoryPool<Node, Size> node_pool;

        // Dummy node to simplify algorithm
        Node dummy_node;

    public:
        MPSCQueue() {
            Node *dummy = &dummy_node;
            head.store(dummy, std::memory_order_relaxed);
            tail.store(dummy, std::memory_order_relaxed);
        }

        bool try_push(const T &item) noexcept {
            Node *new_node = node_pool.acquire();
            if (UNLIKELY(new_node == nullptr)) {
                return false; // Pool exhausted
            }

            new_node->data = item;
            new_node->next.store(nullptr, std::memory_order_relaxed);

            Node *prev_tail = tail.exchange(new_node, std::memory_order_acq_rel);
            prev_tail->next.store(new_node, std::memory_order_release);
            return true;
        }

        bool try_pop(T &item) noexcept {
            Node *head_node = head.load(std::memory_order_acquire);
            Node *next = head_node->next.load(std::memory_order_acquire);

            if (UNLIKELY(next == nullptr)) {
                return false; // Queue is empty
            }

            item = next->data;
            head.store(next, std::memory_order_release);

            // Don't release dummy node
            if (head_node != &dummy_node) {
                node_pool.release(head_node);
            }

            return true;
        }

        void clear() noexcept {
            T item;
            while (try_pop(item));
        }

        [[nodiscard]] bool empty() const noexcept {
            Node *head_node = head.load(std::memory_order_acquire);
            Node *next = head_node->next.load(std::memory_order_acquire);
            return next == nullptr;
        }

        [[nodiscard]] size_t capacity() const noexcept {
            return node_pool.capacity();
        }
    };

    // Bounded Multi Producer Multi Consumer Queue
    template<typename T, size_t Size>
    class MPMCQueue {
        static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
        static constexpr size_t mask = Size - 1;

        struct alignas(CacheLineSize) Cell {
            std::atomic<size_t> sequence{0};
            T data;
        };

        alignas(CacheLineSize) std::array<Cell, Size> buffer;
        alignas(CacheLineSize) std::atomic<size_t> enqueue_pos{0};
        alignas(CacheLineSize) std::atomic<size_t> dequeue_pos{0};

    public:
        MPMCQueue() {
            for (size_t i = 0; i < Size; ++i) {
                buffer[i].sequence.store(i, std::memory_order_relaxed);
            }
        }

        bool try_push(const T &item) noexcept {
            Cell *cell;
            size_t pos = enqueue_pos.load(std::memory_order_relaxed);

            for (;;) {
                cell = &buffer[pos & mask];
                const size_t seq = cell->sequence.load(std::memory_order_acquire);

                if (const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos); dif == 0) {
                    if (enqueue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                        break;
                    }
                } else if (dif < 0) {
                    return false; // Queue is full
                } else {
                    pos = enqueue_pos.load(std::memory_order_relaxed);
                }
            }

            cell->data = item;
            cell->sequence.store(pos + 1, std::memory_order_release);
            return true;
        }

        bool try_pop(T &item) noexcept {
            Cell *cell;
            size_t pos = dequeue_pos.load(std::memory_order_relaxed);

            for (;;) {
                cell = &buffer[pos & mask];
                size_t seq = cell->sequence.load(std::memory_order_acquire);
                intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

                if (dif == 0) {
                    if (dequeue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                        break;
                    }
                } else if (dif < 0) {
                    return false; // Queue is empty
                } else {
                    pos = dequeue_pos.load(std::memory_order_relaxed);
                }
            }

            item = cell->data;
            cell->sequence.store(pos + mask + 1, std::memory_order_release);
            return true;
        }

        [[nodiscard]] bool empty() const noexcept {
            const size_t pos = dequeue_pos.load(std::memory_order_acquire);
            Cell *cell = &buffer[pos & mask];
            const size_t seq = cell->sequence.load(std::memory_order_acquire);
            return seq <= pos;
        }

        [[nodiscard]] bool full() const noexcept {
            size_t pos = enqueue_pos.load(std::memory_order_acquire);
            Cell *cell = &buffer[pos & mask];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            return seq <= pos;
        }

        [[nodiscard]] static constexpr size_t capacity() noexcept {
            return Size;
        }
    };

    // Message queue with priority support
    template<typename T, size_t Size, std::uint8_t NumPriorities = 4>
    class PriorityQueue {
        struct PriorityLevel {
            SPSCQueue<T, Size / NumPriorities> queue;
            std::atomic<std::uint32_t> count{0};
        };

        std::array<PriorityLevel, NumPriorities> levels;
        std::atomic<std::uint8_t> highest_priority{NumPriorities};

    public:
        bool try_push(const T &item, std::uint8_t priority) noexcept {
            if (UNLIKELY(priority >= NumPriorities)) {
                priority = NumPriorities - 1; // Clamp to the lowest priority
            }

            if (levels[priority].queue.try_push(item)) {
                levels[priority].count.fetch_add(1, std::memory_order_relaxed);

                // Update the highest priority atomically
                std::uint8_t current_highest = highest_priority.load(std::memory_order_acquire);
                while (priority < current_highest) {
                    if (highest_priority.compare_exchange_weak(
                        current_highest, priority, std::memory_order_release)) {
                        break;
                    }
                }

                return true;
            }

            return false;
        }

        bool try_pop(T &item) noexcept {
            // Start from the highest priority
            std::uint8_t start_priority = highest_priority.load(std::memory_order_acquire);

            for (std::uint8_t p = start_priority; p < NumPriorities; ++p) {
                if (levels[p].queue.try_pop(item)) {
                    levels[p].count.fetch_sub(1, std::memory_order_relaxed);

                    // Update the highest priority if this level is now empty
                    if (p == start_priority && levels[p].count.load(std::memory_order_acquire) == 0) {
                        find_next_highest_priority(p);
                    }

                    return true;
                }
            }

            return false;
        }

        [[nodiscard]] bool empty() const noexcept {
            for (const auto &level: levels) {
                if (level.count.load(std::memory_order_acquire) > 0) {
                    return false;
                }
            }
            return true;
        }

    private:
        void find_next_highest_priority(std::uint8_t current_priority) noexcept {
            std::uint8_t next_priority = NumPriorities;

            for (std::uint8_t p = current_priority + 1; p < NumPriorities; ++p) {
                if (levels[p].count.load(std::memory_order_acquire) > 0) {
                    next_priority = p;
                    break;
                }
            }

            highest_priority.store(next_priority, std::memory_order_release);
        }
    };
}
