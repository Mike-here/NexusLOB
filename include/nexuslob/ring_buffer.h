#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace nexuslob {

// Bounded SPSC queue. Exactly one producer calls try_push and exactly one
// consumer calls try_pop.  The acquire/release pairs publish payload writes
// without a mutex or allocation.
template <typename T, std::size_t Capacity>
class SpscRingBuffer {
    static_assert(Capacity >= 2U, "ring capacity must be at least two");
    static_assert((Capacity & (Capacity - 1U)) == 0U, "ring capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "hot-path ring payloads must be trivial");

public:
    [[nodiscard]] bool try_push(const T& value) noexcept {
        const std::size_t write = write_index_.load(std::memory_order_relaxed);
        const std::size_t next = (write + 1U) & kMask;
        if (next == read_index_.load(std::memory_order_acquire)) {
            return false;
        }
        entries_[write] = value;
        write_index_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& value) noexcept {
        const std::size_t read = read_index_.load(std::memory_order_relaxed);
        if (read == write_index_.load(std::memory_order_acquire)) {
            return false;
        }
        value = entries_[read];
        read_index_.store((read + 1U) & kMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return read_index_.load(std::memory_order_acquire) ==
               write_index_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kMask = Capacity - 1U;

    alignas(64) std::array<T, Capacity> entries_{};
    alignas(64) std::atomic<std::size_t> write_index_{0};
    alignas(64) std::atomic<std::size_t> read_index_{0};
};

}  // namespace nexuslob
