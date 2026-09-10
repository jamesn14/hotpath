//
// Created by jamesn on 8/16/26.
//

#ifndef HOTPATH_SPSC_RING_H
#define HOTPATH_SPSC_RING_H
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <algorithm>
#include <span>

inline constexpr size_t kCacheLineSize = 64;

template <typename T>
struct alignas(kCacheLineSize) CacheLinePadded { T value{}; };

template <typename T, std::size_t Capacity>
class spsc_ring {
    alignas(64) std::array<T, Capacity> buf_;
    CacheLinePadded<std::atomic<std::size_t>> head_;
    CacheLinePadded<std::atomic<std::size_t>> tail_;
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::has_single_bit(Capacity));
public:
    [[nodiscard]] bool try_push(const T& item) noexcept {
        size_t head =  head_.value.load(std::memory_order_relaxed);
        size_t tail =  tail_.value.load(std::memory_order_acquire);
        if (head - tail == Capacity) return false;
        size_t insert_index = head & (Capacity - 1);
        buf_[insert_index] = item;
        head++;
        head_.value.store(head, std::memory_order_release);
        return true;
    };
    [[nodiscard]] bool try_pop(T& out) noexcept {
        size_t tail =  tail_.value.load(std::memory_order_relaxed);
        size_t head =  head_.value.load(std::memory_order_acquire);
        if (head == tail) return false;
        size_t pop_index =  tail & (Capacity - 1);
        out = buf_[pop_index];
        tail++;
        tail_.value.store(tail, std::memory_order_release);
        return true;
    };
    [[nodiscard]] std::size_t try_push_batch(std::span<const T> items) noexcept {
        size_t head =  head_.value.load(std::memory_order_relaxed);
        size_t tail =  tail_.value.load(std::memory_order_acquire);
        size_t insert_index = head & (Capacity - 1);
        size_t free_slots = Capacity - (head - tail);
        size_t count = std::min(items.size(), free_slots);
        size_t first_chunk = std::min(count, Capacity - insert_index);
        size_t second_chunk = count-first_chunk;
        if (first_chunk > 0)
        std::memcpy(&buf_[insert_index], items.data(), first_chunk * sizeof(T));
        if (second_chunk > 0)
        std::memcpy(&buf_[0], items.data() + first_chunk, second_chunk * sizeof(T));
        head += first_chunk + second_chunk;
        head_.value.store(head, std::memory_order_release);
        return first_chunk + second_chunk;
    };
    [[nodiscard]] std::size_t try_pop_batch(std::span<T> output) noexcept {
        size_t tail =  tail_.value.load(std::memory_order_relaxed);
        size_t head =  head_.value.load(std::memory_order_acquire);
        size_t items_left = head - tail;
        size_t count = std::min(output.size(), items_left);
        size_t pop_index =  tail & (Capacity - 1);
        size_t first_chunk = std::min(count, Capacity - pop_index);
        size_t second_chunk = count-first_chunk;
        if (first_chunk > 0)
        std::memcpy(output.data(), &buf_[pop_index], first_chunk * sizeof(T));
        if (second_chunk > 0)
        std::memcpy(output.data() + first_chunk, &buf_[0], second_chunk * sizeof(T));
        tail += first_chunk + second_chunk;
        tail_.value.store(tail, std::memory_order_release);
        return first_chunk + second_chunk;
    };
    static constexpr std::size_t capacity() noexcept { return Capacity; };
    std::size_t size() noexcept {
        size_t tail =  tail_.value.load(std::memory_order_relaxed);
        size_t head =  head_.value.load(std::memory_order_acquire);
        return head - tail;
    };
};
#endif //HOTPATH_SPSC_RING_H
