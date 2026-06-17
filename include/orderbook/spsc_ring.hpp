#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace orderbook {

template <typename T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity)
        : capacity_(next_power_of_two(capacity)), mask_(capacity_ - 1), slots_(capacity_) {}

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    [[nodiscard]] bool push(const T& value) noexcept {
        const auto head = head_.value.load(std::memory_order_relaxed);
        const auto next = head + 1;
        if (next - tail_.value.load(std::memory_order_acquire) > capacity_) {
            return false;
        }
        slots_[head & mask_] = value;
        // Release publishes the slot write before the consumer observes the new head.
        head_.value.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool push(T&& value) noexcept {
        const auto head = head_.value.load(std::memory_order_relaxed);
        const auto next = head + 1;
        if (next - tail_.value.load(std::memory_order_acquire) > capacity_) {
            return false;
        }
        slots_[head & mask_] = std::move(value);
        head_.value.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<T> pop() noexcept {
        const auto tail = tail_.value.load(std::memory_order_relaxed);
        if (tail == head_.value.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        T value = std::move(slots_[tail & mask_]);
        // Release keeps the slot read before producer observes freed capacity.
        tail_.value.store(tail + 1, std::memory_order_release);
        return value;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    struct alignas(64) PaddedAtomic {
        std::atomic<std::uint64_t> value{0};
    };

    static std::size_t next_power_of_two(std::size_t value) noexcept {
        if (value < 2) {
            return 2;
        }
        --value;
        for (std::size_t shift = 1; shift < sizeof(std::size_t) * 8; shift <<= 1) {
            value |= value >> shift;
        }
        return value + 1;
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<T> slots_;
    // Producer and consumer write different counters; padding prevents false sharing.
    PaddedAtomic head_;
    PaddedAtomic tail_;
};

}  // namespace orderbook
