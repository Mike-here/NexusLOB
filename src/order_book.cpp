#include "nexuslob/order_book.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace nexuslob {
namespace {

[[nodiscard]] constexpr std::size_t ceil_div_64(std::size_t value) noexcept {
    return (value + 63U) / 64U;
}

[[nodiscard]] unsigned first_set_bit(std::uint64_t value) noexcept {
    assert(value != 0U);
    return static_cast<unsigned>(__builtin_ctzll(value));
}

[[nodiscard]] unsigned last_set_bit(std::uint64_t value) noexcept {
    assert(value != 0U);
    return 63U - static_cast<unsigned>(__builtin_clzll(value));
}

}  // namespace

NonEmptyPriceSet::NonEmptyPriceSet(std::size_t bit_count) {
    assert(bit_count > 0U);
    std::size_t current_count = ceil_div_64(bit_count);
    while (true) {
        assert(tier_count_ < kMaxTiers);
        offsets_[tier_count_] = words_.size();
        counts_[tier_count_] = current_count;
        words_.resize(words_.size() + current_count, 0U);
        ++tier_count_;
        if (current_count == 1U) {
            break;
        }
        current_count = ceil_div_64(current_count);
    }
}

std::uint64_t& NonEmptyPriceSet::word(std::size_t tier, std::size_t index) noexcept {
    assert(tier < tier_count_ && index < counts_[tier]);
    return words_[offsets_[tier] + index];
}

const std::uint64_t& NonEmptyPriceSet::word(std::size_t tier, std::size_t index) const noexcept {
    assert(tier < tier_count_ && index < counts_[tier]);
    return words_[offsets_[tier] + index];
}

void NonEmptyPriceSet::set(PriceTick price) noexcept {
    std::size_t child_word_index = static_cast<std::size_t>(price) / kBitsPerWord;
    const unsigned child_bit = static_cast<unsigned>(price % kBitsPerWord);
    std::uint64_t& leaf = word(0U, child_word_index);
    const std::uint64_t leaf_mask = std::uint64_t{1} << child_bit;
    if ((leaf & leaf_mask) != 0U) {
        return;
    }
    const bool leaf_was_empty = leaf == 0U;
    leaf |= leaf_mask;
    if (!leaf_was_empty) {
        return;
    }

    for (std::size_t tier = 1U; tier < tier_count_; ++tier) {
        const std::size_t parent_word_index = child_word_index / kBitsPerWord;
        const unsigned parent_bit = static_cast<unsigned>(child_word_index % kBitsPerWord);
        std::uint64_t& parent = word(tier, parent_word_index);
        const std::uint64_t parent_mask = std::uint64_t{1} << parent_bit;
        if ((parent & parent_mask) != 0U) {
            return;
        }
        const bool parent_was_empty = parent == 0U;
        parent |= parent_mask;
        if (!parent_was_empty) {
            return;
        }
        child_word_index = parent_word_index;
    }
}

void NonEmptyPriceSet::reset(PriceTick price) noexcept {
    std::size_t child_word_index = static_cast<std::size_t>(price) / kBitsPerWord;
    const unsigned child_bit = static_cast<unsigned>(price % kBitsPerWord);
    std::uint64_t& leaf = word(0U, child_word_index);
    leaf &= ~(std::uint64_t{1} << child_bit);
    if (leaf != 0U) {
        return;
    }

    for (std::size_t tier = 1U; tier < tier_count_; ++tier) {
        const std::size_t parent_word_index = child_word_index / kBitsPerWord;
        const unsigned parent_bit = static_cast<unsigned>(child_word_index % kBitsPerWord);
        std::uint64_t& parent = word(tier, parent_word_index);
        parent &= ~(std::uint64_t{1} << parent_bit);
        if (parent != 0U) {
            return;
        }
        child_word_index = parent_word_index;
    }
}

bool NonEmptyPriceSet::empty() const noexcept {
    return word(tier_count_ - 1U, 0U) == 0U;
}

PriceTick NonEmptyPriceSet::first() const noexcept {
    if (empty()) {
        return kInvalidPrice;
    }
    if (tier_count_ == 1U) {
        return static_cast<PriceTick>(first_set_bit(word(0U, 0U)));
    }

    std::size_t child_word_index = first_set_bit(word(tier_count_ - 1U, 0U));
    for (std::size_t tier = tier_count_ - 1U; tier-- > 0U;) {
        child_word_index = child_word_index * kBitsPerWord + first_set_bit(word(tier, child_word_index));
    }
    return static_cast<PriceTick>(child_word_index);
}

PriceTick NonEmptyPriceSet::last() const noexcept {
    if (empty()) {
        return kInvalidPrice;
    }
    if (tier_count_ == 1U) {
        return static_cast<PriceTick>(last_set_bit(word(0U, 0U)));
    }

    std::size_t child_word_index = last_set_bit(word(tier_count_ - 1U, 0U));
    for (std::size_t tier = tier_count_ - 1U; tier-- > 0U;) {
        child_word_index = child_word_index * kBitsPerWord + last_set_bit(word(tier, child_word_index));
    }
    return static_cast<PriceTick>(child_word_index);
}

FlatSideBook::FlatSideBook(std::size_t price_level_count, Side side)
    : levels_(price_level_count), non_empty_(price_level_count), side_(side) {}

PriceLevel& FlatSideBook::level(PriceTick price) noexcept {
    assert(valid_price(price));
    return levels_[price];
}

const PriceLevel& FlatSideBook::level(PriceTick price) const noexcept {
    assert(valid_price(price));
    return levels_[price];
}

bool FlatSideBook::valid_price(PriceTick price) const noexcept {
    return static_cast<std::size_t>(price) < levels_.size();
}

void FlatSideBook::became_non_empty(PriceTick price) noexcept {
    non_empty_.set(price);
    if (best_price_ == kInvalidPrice ||
        (side_ == Side::Buy && price > best_price_) ||
        (side_ == Side::Sell && price < best_price_)) {
        best_price_ = price;
    }
}

void FlatSideBook::became_empty(PriceTick price) noexcept {
    non_empty_.reset(price);
    if (best_price_ != price) {
        return;
    }
    best_price_ = side_ == Side::Buy ? non_empty_.last() : non_empty_.first();
}

OrderBook::OrderBook(const OrderBookConfig& config)
    : pool_(config.max_resting_orders),
      order_id_to_pool_index_(config.max_order_id, kInvalidIndex),
      bids_(config.price_level_count, Side::Buy),
      asks_(config.price_level_count, Side::Sell) {
    assert(config.max_order_id > 0U);
    assert(config.price_level_count > 0U);
}

bool OrderBook::process(IncomingMessage& message,
                        ExecutionSink sink,
                        void* sink_context) noexcept {
    last_reject_reason_ = RejectReason::None;
    if (message.message_type == MessageType::CancelOrder) {
        process_cancel(message.order_id);
        return true;
    }
    return process_new(message, sink, sink_context);
}

bool OrderBook::process_new(IncomingMessage& message,
                            ExecutionSink sink,
                            void* sink_context) noexcept {
    if (message.quantity == 0U || message.order_id >= order_id_to_pool_index_.size() ||
        (message.order_type != OrderType::LimitGtc && message.order_type != OrderType::LimitIoc &&
         message.order_type != OrderType::Market) ||
        (message.order_type != OrderType::Market && !side_book(message.side).valid_price(message.price_tick))) {
        last_reject_reason_ = RejectReason::InvalidOrder;
        ++stats_.rejected_orders;
        return true;
    }
    if (order_id_to_pool_index_[message.order_id] != kInvalidIndex) {
        last_reject_reason_ = RejectReason::DuplicateOrderId;
        ++stats_.rejected_orders;
        return true;
    }

    if (!execute_available(message, sink, sink_context)) {
        return false;
    }

    if (message.quantity != 0U && message.order_type == OrderType::LimitGtc) {
        if (pool_.available() == 0U) {
            last_reject_reason_ = RejectReason::PoolExhausted;
            ++stats_.rejected_orders;
            return true;
        }
        insert_resting(message);
    }
    // IOC and market remainders are intentionally discarded after matching.
    message.quantity = 0U;
    ++stats_.accepted_orders;
    return true;
}

void OrderBook::process_cancel(OrderId order_id) noexcept {
    if (order_id >= order_id_to_pool_index_.size()) {
        last_reject_reason_ = RejectReason::UnknownOrderId;
        ++stats_.rejected_orders;
        return;
    }
    const PoolIndex index = order_id_to_pool_index_[order_id];
    if (index == kInvalidIndex) {
        last_reject_reason_ = RejectReason::UnknownOrderId;
        ++stats_.rejected_orders;
        return;
    }
    unlink_and_release(index);
    ++stats_.cancelled_orders;
}

bool OrderBook::can_cross(const IncomingMessage& incoming, PriceTick resting_price) const noexcept {
    if (incoming.order_type == OrderType::Market) {
        return true;
    }
    return incoming.side == Side::Buy ? incoming.price_tick >= resting_price
                                      : incoming.price_tick <= resting_price;
}

bool OrderBook::execute_available(IncomingMessage& incoming,
                                  ExecutionSink sink,
                                  void* sink_context) noexcept {
    FlatSideBook& opposing = opposite_book(incoming.side);
    while (incoming.quantity != 0U) {
        const PriceTick price = opposing.best_price();
        if (price == kInvalidPrice || !can_cross(incoming, price)) {
            break;
        }

        PriceLevel& level = opposing.level(price);
        assert(level.head_index != kInvalidIndex);
        const PoolIndex resting_index = level.head_index;
        Order& resting = pool_.at(resting_index);
        const Quantity executed = std::min(incoming.quantity, resting.remaining_quantity);
        const Execution execution{
            next_execution_sequence_, incoming.order_id, resting.order_id, price, executed, incoming.side, {}};

        // Output capacity is checked before book state changes. A failed emit
        // leaves the current incoming residual and queue head untouched.
        if (!sink(sink_context, execution)) {
            return false;
        }

        ++next_execution_sequence_;
        ++stats_.executions;
        incoming.quantity -= executed;
        resting.remaining_quantity -= executed;
        level.total_quantity -= executed;
        if (resting.remaining_quantity == 0U) {
            unlink_and_release(resting_index);
        }
    }
    return true;
}

void OrderBook::insert_resting(const IncomingMessage& incoming) noexcept {
    const PoolIndex index = pool_.acquire();
    assert(index != kInvalidIndex);
    Order& order = pool_.at(index);
    order.order_id = incoming.order_id;
    order.price_tick = incoming.price_tick;
    order.remaining_quantity = incoming.quantity;
    order.next_index = kInvalidIndex;
    order.prev_index = kInvalidIndex;
    order.side = incoming.side;
    order.type = incoming.order_type;

    FlatSideBook& book = side_book(order.side);
    PriceLevel& level = book.level(order.price_tick);
    const bool was_empty = level.order_count == 0U;
    if (level.tail_index == kInvalidIndex) {
        level.head_index = index;
    } else {
        pool_.at(level.tail_index).next_index = index;
        order.prev_index = level.tail_index;
    }
    level.tail_index = index;
    level.total_quantity += order.remaining_quantity;
    ++level.order_count;
    order_id_to_pool_index_[order.order_id] = index;
    if (was_empty) {
        book.became_non_empty(order.price_tick);
    }
}

void OrderBook::unlink_and_release(PoolIndex index) noexcept {
    Order& order = pool_.at(index);
    FlatSideBook& book = side_book(order.side);
    PriceLevel& level = book.level(order.price_tick);

    if (order.prev_index == kInvalidIndex) {
        level.head_index = order.next_index;
    } else {
        pool_.at(order.prev_index).next_index = order.next_index;
    }
    if (order.next_index == kInvalidIndex) {
        level.tail_index = order.prev_index;
    } else {
        pool_.at(order.next_index).prev_index = order.prev_index;
    }
    level.total_quantity -= order.remaining_quantity;
    --level.order_count;
    order_id_to_pool_index_[order.order_id] = kInvalidIndex;
    if (level.order_count == 0U) {
        book.became_empty(order.price_tick);
    }
    pool_.release(index);
}

FlatSideBook& OrderBook::side_book(Side side) noexcept {
    return side == Side::Buy ? bids_ : asks_;
}

const FlatSideBook& OrderBook::side_book(Side side) const noexcept {
    return side == Side::Buy ? bids_ : asks_;
}

FlatSideBook& OrderBook::opposite_book(Side side) noexcept {
    return side == Side::Buy ? asks_ : bids_;
}

}  // namespace nexuslob
