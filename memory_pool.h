#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "nexuslob/types.h"

namespace nexuslob {

// This pool allocates its backing vector exactly once in its constructor.  The
// free list reuses Order::next_index while a slot is not owned by the book.
class OrderPool {
public:
    explicit OrderPool(std::size_t capacity) : storage_(capacity) {
        assert(capacity > 0U && capacity < static_cast<std::size_t>(kInvalidIndex));
        for (PoolIndex index = 0; index + 1U < capacity; ++index) {
            storage_[index].next_index = index + 1U;
        }
        storage_[capacity - 1U].next_index = kInvalidIndex;
        free_head_ = 0U;
        available_ = static_cast<PoolIndex>(capacity);
    }

    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    [[nodiscard]] PoolIndex acquire() noexcept {
        if (free_head_ == kInvalidIndex) {
            return kInvalidIndex;
        }

        const PoolIndex allocated = free_head_;
        free_head_ = storage_[allocated].next_index;
        --available_;
        return allocated;
    }

    void release(PoolIndex index) noexcept {
        assert(index < storage_.size());
        storage_[index] = Order{};
        storage_[index].next_index = free_head_;
        free_head_ = index;
        ++available_;
    }

    [[nodiscard]] Order& at(PoolIndex index) noexcept {
        assert(index < storage_.size());
        return storage_[index];
    }

    [[nodiscard]] const Order& at(PoolIndex index) const noexcept {
        assert(index < storage_.size());
        return storage_[index];
    }

    [[nodiscard]] PoolIndex available() const noexcept { return available_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }

private:
    std::vector<Order> storage_;
    PoolIndex free_head_{kInvalidIndex};
    PoolIndex available_{0};
};

}  // namespace nexuslob
