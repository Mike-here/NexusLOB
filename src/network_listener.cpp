#include "nexuslob/network_listener.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nexuslob {

NetworkListener::NetworkListener(IngressRing& ingress,
                                 EgressRing& egress,
                                 std::atomic<bool>& running,
                                 std::uint16_t port) noexcept
    : ingress_(ingress), egress_(egress), running_(running), port_(port) {}

void NetworkListener::run() noexcept {
    if (!open_listener()) {
        running_.store(false, std::memory_order_release);
        return;
    }

    std::array<epoll_event, 8U> events{};
    while (running_.load(std::memory_order_acquire)) {
        const int ready = epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), 1);
        if (ready < 0 && errno != EINTR) {
            running_.store(false, std::memory_order_release);
            break;
        }
        for (int index = 0; index < ready; ++index) {
            const epoll_event& event = events[static_cast<std::size_t>(index)];
            if (event.data.fd == listen_fd_) {
                accept_client();
                continue;
            }
            if (event.data.fd == client_fd_) {
                if ((event.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
                    close_client();
                    continue;
                }
                if ((event.events & EPOLLIN) != 0U) {
                    receive_messages();
                }
            }
        }
        flush_executions();
    }

    close_client();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
    }
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
    }
}

bool NetworkListener::open_listener() noexcept {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (epoll_fd_ < 0 || listen_fd_ < 0) {
        return false;
    }

    const int enabled = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port_);
    if (bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(listen_fd_, SOMAXCONN) < 0) {
        return false;
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = listen_fd_;
    return epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) == 0;
}

void NetworkListener::close_client() noexcept {
    if (client_fd_ >= 0) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd_, nullptr);
        close(client_fd_);
    }
    client_fd_ = -1;
    read_size_ = 0U;
    // A partially sent report was already removed from the SPSC queue. Start
    // that complete record again for a replacement client rather than losing it.
    if (has_pending_wire_) {
        pending_wire_offset_ = 0U;
    }
    client_registered_ = false;
    client_wants_write_ = false;
}

void NetworkListener::accept_client() noexcept {
    while (true) {
        const int accepted = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (accepted < 0) {
            return;
        }
        close_client();  // The protocol deliberately has one market-data client.
        client_fd_ = accepted;
        set_client_events(false);
    }
}

void NetworkListener::set_client_events(bool include_write) noexcept {
    if (client_fd_ < 0 || (client_registered_ && client_wants_write_ == include_write)) {
        return;
    }
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | (include_write ? EPOLLOUT : 0U);
    event.data.fd = client_fd_;
    const int operation = client_registered_ ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(epoll_fd_, operation, client_fd_, &event) == 0) {
        client_registered_ = true;
        client_wants_write_ = include_write;
    }
}

void NetworkListener::receive_messages() noexcept {
    if (client_fd_ < 0) {
        return;
    }
    consume_read_buffer();
    if (read_size_ >= sizeof(WireOrder)) {
        return;
    }
    if (read_size_ >= read_buffer_.size()) {
        close_client();
        return;
    }
    while (read_size_ < read_buffer_.size()) {
        const ssize_t received = recv(client_fd_, read_buffer_.data() + read_size_,
                                       read_buffer_.size() - read_size_, 0);
        if (received > 0) {
            read_size_ += static_cast<std::size_t>(received);
            consume_read_buffer();
            if (read_size_ >= sizeof(WireOrder)) {
                return;
            }
            if (read_size_ >= read_buffer_.size()) {
                close_client();
                return;
            }
            continue;
        }
        if (received == 0) {
            close_client();
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        if (errno != EINTR) {
            close_client();
            return;
        }
    }
}

void NetworkListener::consume_read_buffer() noexcept {
    while (read_size_ >= sizeof(WireOrder)) {
        WireOrder wire{};
        std::memcpy(&wire, read_buffer_.data(), sizeof(wire));
        IncomingMessage message{};
        if (!decode_wire_order(wire, message)) {
            close_client();
            return;
        }
        if (!ingress_.try_push(message)) {
            return;  // Leave the frame in place; TCP naturally applies backpressure.
        }
        read_size_ -= sizeof(WireOrder);
        if (read_size_ != 0U) {
            std::memmove(read_buffer_.data(), read_buffer_.data() + sizeof(WireOrder), read_size_);
        }
    }
}

void NetworkListener::flush_executions() noexcept {
    if (client_fd_ < 0) {
        return;
    }
    while (true) {
        if (!has_pending_wire_) {
            Execution execution{};
            if (!egress_.try_pop(execution)) {
                set_client_events(false);
                return;
            }
            pending_wire_ = encode_wire_execution(execution);
            pending_wire_offset_ = 0U;
            has_pending_wire_ = true;
        }

        const auto* bytes = reinterpret_cast<const std::byte*>(&pending_wire_);
        const ssize_t sent = send(client_fd_, bytes + pending_wire_offset_,
                                  sizeof(pending_wire_) - pending_wire_offset_, MSG_NOSIGNAL);
        if (sent > 0) {
            pending_wire_offset_ += static_cast<std::size_t>(sent);
            if (pending_wire_offset_ == sizeof(pending_wire_)) {
                has_pending_wire_ = false;
                pending_wire_offset_ = 0U;
                continue;
            }
        }
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close_client();
            return;
        }
        set_client_events(true);
        return;
    }
}

}  // namespace nexuslob
