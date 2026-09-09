#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace nexuslob {

using OrderId = std::uint64_t;
using PriceTick = std::uint32_t;
using Quantity = std::uint32_t;
using PoolIndex = std::uint32_t;

inline constexpr PoolIndex kInvalidIndex = std::numeric_limits<PoolIndex>::max();
inline constexpr PriceTick kInvalidPrice = std::numeric_limits<PriceTick>::max();

enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

enum class OrderType : std::uint8_t {
    LimitGtc = 1,
    LimitIoc = 2,
    Market = 3,
};

enum class MessageType : std::uint8_t {
    NewOrder = 1,
    CancelOrder = 2,
};

enum class RejectReason : std::uint8_t {
    None = 0,
    InvalidOrder = 1,
    DuplicateOrderId = 2,
    PoolExhausted = 3,
    UnknownOrderId = 4,
};

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

// An allocated order is also its own queue node.  Indices address one contiguous
// OrderPool, so no list node or heap allocation exists on the matching path.
struct Order {
    OrderId order_id{0};
    PriceTick price_tick{0};
    Quantity remaining_quantity{0};
    PoolIndex next_index{kInvalidIndex};
    PoolIndex prev_index{kInvalidIndex};
    Side side{Side::Buy};
    OrderType type{OrderType::LimitGtc};
    std::uint16_t reserved{0};
};

// A price level is compact and stored in one of two flat vectors (bids/asks).
struct PriceLevel {
    PoolIndex head_index{kInvalidIndex};
    PoolIndex tail_index{kInvalidIndex};
    std::uint64_t total_quantity{0};
    std::uint32_t order_count{0};
    std::uint32_t reserved{0};
};

struct IncomingMessage {
    MessageType message_type{MessageType::NewOrder};
    Side side{Side::Buy};
    OrderType order_type{OrderType::LimitGtc};
    std::uint8_t reserved{0};
    OrderId order_id{0};
    PriceTick price_tick{0};
    Quantity quantity{0};
    std::uint64_t client_sequence{0};
};

struct Execution {
    std::uint64_t execution_sequence{0};
    OrderId aggressor_order_id{0};
    OrderId resting_order_id{0};
    PriceTick price_tick{0};
    Quantity quantity{0};
    Side aggressor_side{Side::Buy};
    std::uint8_t reserved[7]{};
};

static_assert(std::is_trivially_copyable_v<Order>);
static_assert(std::is_trivially_copyable_v<PriceLevel>);
static_assert(std::is_trivially_copyable_v<IncomingMessage>);
static_assert(std::is_trivially_copyable_v<Execution>);
static_assert(sizeof(Order) == 32U, "two order nodes fit in one 64-byte cache line");
static_assert(sizeof(PriceLevel) == 24U);

}  // namespace nexuslob
