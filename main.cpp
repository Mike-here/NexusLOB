#include "nexuslob/network_listener.h"
#include "nexuslob/order_book.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>

#include <sched.h>

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void request_stop(int) noexcept {
    g_stop_requested = 1;
}

[[nodiscard]] bool publish_execution(void* context, const nexuslob::Execution& execution) noexcept {
    auto* queue = static_cast<nexuslob::EgressRing*>(context);
    return queue->try_push(execution);
}

void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

[[nodiscard]] bool pin_current_thread(unsigned int cpu) noexcept {
    if (cpu >= CPU_SETSIZE) {
        return false;
    }
    cpu_set_t affinity{};
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    return sched_setaffinity(0, sizeof(affinity), &affinity) == 0;
}

[[nodiscard]] unsigned long parse_argument(const char* value, const char* name) {
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        std::cerr << "Invalid " << name << ": " << value << '\n';
        std::exit(EXIT_FAILURE);
    }
    return parsed;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 9000U;
    unsigned int cpu = 0U;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--port" && index + 1 < argc) {
            const unsigned long parsed_port = parse_argument(argv[++index], "port");
            if (parsed_port == 0U || parsed_port > 65'535U) {
                std::cerr << "Port must be in [1, 65535]\n";
                return EXIT_FAILURE;
            }
            port = static_cast<std::uint16_t>(parsed_port);
        } else if (std::string_view(argv[index]) == "--cpu" && index + 1 < argc) {
            cpu = static_cast<unsigned int>(parse_argument(argv[++index], "cpu"));
        } else {
            std::cerr << "Usage: nexuslob_server [--port 9000] [--cpu 0]\n";
            return EXIT_FAILURE;
        }
    }

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    nexuslob::IngressRing ingress;
    nexuslob::EgressRing egress;
    std::atomic<bool> running{true};
    nexuslob::OrderBook book(nexuslob::OrderBookConfig{});
    nexuslob::NetworkListener listener(ingress, egress, running, port);
    std::thread network_thread(&nexuslob::NetworkListener::run, &listener);

    if (!pin_current_thread(cpu)) {
        std::cerr << "Warning: unable to pin matching thread to CPU " << cpu << '\n';
    }
    std::cout << "NexusLOB listening on TCP port " << port << "; matching thread pinned to CPU " << cpu << '\n';

    nexuslob::IncomingMessage current{};
    bool have_current = false;
    while (running.load(std::memory_order_acquire) && g_stop_requested == 0) {
        if (!have_current) {
            have_current = ingress.try_pop(current);
        }
        if (!have_current) {
            cpu_relax();
            continue;
        }
        if (book.process(current, publish_execution, &egress)) {
            have_current = false;
        } else {
            // The execution queue is full. Retain the exact incoming residual
            // and wait until the network thread drains output.
            cpu_relax();
        }
    }

    running.store(false, std::memory_order_release);
    network_thread.join();
    const nexuslob::OrderBookStats& stats = book.stats();
    std::cout << "accepted=" << stats.accepted_orders << " cancelled=" << stats.cancelled_orders
              << " rejected=" << stats.rejected_orders << " executions=" << stats.executions << '\n';
    return EXIT_SUCCESS;
}
