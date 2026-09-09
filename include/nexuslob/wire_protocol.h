#pragma once

#include <arpa/inet.h>

#include <cstdint>
#include <cstring>

#include "nexuslob/types.h"

namespace nexuslob {

inline constexpr std::uint32_t kWireMagic = 0x4E4C4F42U;  // "NLOB"
inline constexpr std::uint8_t kWireExecution = 0x80U;

#pragma pack(push, 1)
struct WireOrder {
    std::uint32_t magic;
    std::uint8_t message_type;
    std::uint8_t side;
    std::uint8_t order_type;
    std::uint8_t reserved;
    std::uint64_t order_id;
    std::uint32_t price_tick;
    std::uint32_t quantity;
    std::uint64_t client_sequence;
};

struct WireExecution {
    std::uint32_t magic;
    std::uint8_t message_type;
    std::uint8_t aggressor_side;
    std::uint16_t reserved;
    std::uint64_t execution_sequence;
    std::uint64_t aggressor_order_id;
    std::uint64_t resting_order_id;
    std::uint32_t price_tick;
    std::uint32_t quantity;
};
#pragma pack(pop)

static_assert(sizeof(WireOrder) == 32U);
static_assert(sizeof(WireExecution) == 40U);

[[nodiscard]] inline std::uint64_t host_to_network_u64(std::uint64_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (static_cast<std::uint64_t>(htonl(static_cast<std::uint32_t>(value))) << 32U) |
           htonl(static_cast<std::uint32_t>(value >> 32U));
#else
    return value;
#endif
}

[[nodiscard]] inline std::uint64_t network_to_host_u64(std::uint64_t value) noexcept {
    return host_to_network_u64(value);
}

[[nodiscard]] inline bool decode_wire_order(const WireOrder& wire, IncomingMessage& message) noexcept {
    if (ntohl(wire.magic) != kWireMagic ||
        (wire.message_type != static_cast<std::uint8_t>(MessageType::NewOrder) &&
         wire.message_type != static_cast<std::uint8_t>(MessageType::CancelOrder)) ||
        wire.side > static_cast<std::uint8_t>(Side::Sell)) {
        return false;
    }
    if (wire.message_type == static_cast<std::uint8_t>(MessageType::NewOrder) &&
        wire.order_type != static_cast<std::uint8_t>(OrderType::LimitGtc) &&
        wire.order_type != static_cast<std::uint8_t>(OrderType::LimitIoc) &&
        wire.order_type != static_cast<std::uint8_t>(OrderType::Market)) {
        return false;
    }

    message.message_type = static_cast<MessageType>(wire.message_type);
    message.side = static_cast<Side>(wire.side);
    message.order_type = static_cast<OrderType>(wire.order_type);
    message.reserved = 0U;
    message.order_id = network_to_host_u64(wire.order_id);
    message.price_tick = ntohl(wire.price_tick);
    message.quantity = ntohl(wire.quantity);
    message.client_sequence = network_to_host_u64(wire.client_sequence);
    return true;
}

[[nodiscard]] inline WireExecution encode_wire_execution(const Execution& execution) noexcept {
    return WireExecution{
        htonl(kWireMagic),
        kWireExecution,
        static_cast<std::uint8_t>(execution.aggressor_side),
        0U,
        host_to_network_u64(execution.execution_sequence),
        host_to_network_u64(execution.aggressor_order_id),
        host_to_network_u64(execution.resting_order_id),
        htonl(execution.price_tick),
        htonl(execution.quantity),
    };
}

}  // namespace nexuslob
