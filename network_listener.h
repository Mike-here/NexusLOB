#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "nexuslob/ring_buffer.h"
#include "nexuslob/types.h"
#include "nexuslob/wire_protocol.h"

namespace nexuslob {

inline constexpr std::size_t kIngressRingCapacity = 1U << 20U;
inline constexpr std::size_t kEgressRingCapacity = 1U << 21U;

using IngressRing = SpscRingBuffer<IncomingMessage, kIngressRingCapacity>;
using EgressRing = SpscRingBuffer<Execution, kEgressRingCapacity>;

class NetworkListener {
public:
    NetworkListener(IngressRing& ingress,
                    EgressRing& egress,
                    std::atomic<bool>& running,
                    std::uint16_t port) noexcept;
    NetworkListener(const NetworkListener&) = delete;
    NetworkListener& operator=(const NetworkListener&) = delete;

    // Runs only on the network producer thread.
    void run() noexcept;

private:
    [[nodiscard]] bool open_listener() noexcept;
    void close_client() noexcept;
    void accept_client() noexcept;
    void receive_messages() noexcept;
    void consume_read_buffer() noexcept;
    void flush_executions() noexcept;
    void set_client_events(bool include_write) noexcept;

    IngressRing& ingress_;
    EgressRing& egress_;
    std::atomic<bool>& running_;
    std::uint16_t port_;
    int epoll_fd_{-1};
    int listen_fd_{-1};
    int client_fd_{-1};
    bool client_registered_{false};
    bool client_wants_write_{false};
    std::array<std::byte, 64U * 1024U> read_buffer_{};
    std::size_t read_size_{0};
    WireExecution pending_wire_{};
    std::size_t pending_wire_offset_{0};
    bool has_pending_wire_{false};
};

}  // namespace nexuslob
