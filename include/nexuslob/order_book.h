#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "nexuslob/memory_pool.h"
#include "nexuslob/types.h"

namespace nexuslob {

struct OrderBookConfig {
    std::size_t max_resting_orders{1'000'000U};
    std::size_t max_order_id{4'000'000U};
    std::size_t price_level_count{50'001U};
};

struct OrderBookStats {
    std::uint64_t accepted_orders{0};
    std::uint64_t cancelled_orders{0};
    std::uint64_t rejected_orders{0};
    std::uint64_t executions{0};
};

// A hierarchical bitmap replaces price scans after a best level is depleted.
// Its depth is bounded by six for the full uint32_t price domain; for a fixed
// configured price range, best-price refresh is therefore constant-time.
class NonEmptyPriceSet {
public:
    explicit NonEmptyPriceSet(std::size_t bit_count);

    void set(PriceTick price) noexcept;
    void reset(PriceTick price) noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] PriceTick first() const noexcept;
    [[nodiscard]] PriceTick last() const noexcept;

private:
    static constexpr std::size_t kBitsPerWord = 64U;
    static constexpr std::size_t kMaxTiers = 6U;

    [[nodiscard]] std::uint64_t& word(std::size_t tier, std::size_t index) noexcept;
    [[nodiscard]] const std::uint64_t& word(std::size_t tier, std::size_t index) const noexcept;

    std::vector<std::uint64_t> words_;
    std::array<std::size_t, kMaxTiers> offsets_{};
    std::array<std::size_t, kMaxTiers> counts_{};
    std::size_t tier_count_{0};
};

class FlatSideBook {
public:
    FlatSideBook(std::size_t price_level_count, Side side);

    [[nodiscard]] PriceLevel& level(PriceTick price) noexcept;
    [[nodiscard]] const PriceLevel& level(PriceTick price) const noexcept;
    [[nodiscard]] bool valid_price(PriceTick price) const noexcept;
    [[nodiscard]] PriceTick best_price() const noexcept { return best_price_; }

    void became_non_empty(PriceTick price) noexcept;
    void became_empty(PriceTick price) noexcept;

private:
    std::vector<PriceLevel> levels_;
    NonEmptyPriceSet non_empty_;
    Side side_;
    PriceTick best_price_{kInvalidPrice};
};

using ExecutionSink = bool (*)(void* context, const Execution& execution) noexcept;

class OrderBook {
public:
    explicit OrderBook(const OrderBookConfig& config);

    // Returns false only when downstream execution output is backpressured.
    // In that case message.quantity holds the unexecuted aggressor residual and
    // the caller must retry this same message before reading the next one.
    [[nodiscard]] bool process(IncomingMessage& message,
                               ExecutionSink sink,
                               void* sink_context) noexcept;

    [[nodiscard]] PriceTick best_bid() const noexcept { return bids_.best_price(); }
    [[nodiscard]] PriceTick best_ask() const noexcept { return asks_.best_price(); }
    [[nodiscard]] const OrderBookStats& stats() const noexcept { return stats_; }
    [[nodiscard]] RejectReason last_reject_reason() const noexcept { return last_reject_reason_; }

private:
    [[nodiscard]] bool process_new(IncomingMessage& message,
                                   ExecutionSink sink,
                                   void* sink_context) noexcept;
    void process_cancel(OrderId order_id) noexcept;
    [[nodiscard]] bool can_cross(const IncomingMessage& incoming, PriceTick resting_price) const noexcept;
    [[nodiscard]] bool execute_available(IncomingMessage& incoming,
                                         ExecutionSink sink,
                                         void* sink_context) noexcept;
    void insert_resting(const IncomingMessage& incoming) noexcept;
    void unlink_and_release(PoolIndex index) noexcept;

    [[nodiscard]] FlatSideBook& side_book(Side side) noexcept;
    [[nodiscard]] const FlatSideBook& side_book(Side side) const noexcept;
    [[nodiscard]] FlatSideBook& opposite_book(Side side) noexcept;

    OrderPool pool_;
    std::vector<PoolIndex> order_id_to_pool_index_;
    FlatSideBook bids_;
    FlatSideBook asks_;
    OrderBookStats stats_{};
    RejectReason last_reject_reason_{RejectReason::None};
    std::uint64_t next_execution_sequence_{1};
};

}  // namespace nexuslob
