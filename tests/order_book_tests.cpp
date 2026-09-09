#include "nexuslob/order_book.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

struct Capture {
    std::vector<nexuslob::Execution> fills;
};

bool capture_execution(void* context, const nexuslob::Execution& execution) noexcept {
    static_cast<Capture*>(context)->fills.push_back(execution);
    return true;
}

bool reject_execution(void*, const nexuslob::Execution&) noexcept {
    return false;
}

nexuslob::IncomingMessage add(std::uint64_t id,
                              nexuslob::Side side,
                              nexuslob::OrderType type,
                              std::uint32_t price,
                              std::uint32_t quantity) {
    return nexuslob::IncomingMessage{nexuslob::MessageType::NewOrder, side, type, 0U,
                                     id, price, quantity, id};
}

void submit(nexuslob::OrderBook& book, nexuslob::IncomingMessage message, Capture& output) {
    assert(book.process(message, capture_execution, &output));
}

constexpr nexuslob::OrderBookConfig test_config{128U, 4'096U, 1'024U};

nexuslob::OrderBookConfig new_book_config() {
    return test_config;
}

void test_price_time_priority_and_ioc() {
    nexuslob::OrderBook book(new_book_config());
    Capture output;
    submit(book, add(1U, nexuslob::Side::Sell, nexuslob::OrderType::LimitGtc, 100U, 10U), output);
    submit(book, add(2U, nexuslob::Side::Sell, nexuslob::OrderType::LimitGtc, 100U, 10U), output);
    submit(book, add(3U, nexuslob::Side::Buy, nexuslob::OrderType::LimitIoc, 100U, 15U), output);

    assert(output.fills.size() == 2U);
    assert(output.fills[0].resting_order_id == 1U && output.fills[0].quantity == 10U);
    assert(output.fills[1].resting_order_id == 2U && output.fills[1].quantity == 5U);
    assert(book.best_ask() == 100U);

    submit(book, add(4U, nexuslob::Side::Buy, nexuslob::OrderType::Market, 0U, 5U), output);
    assert(output.fills.size() == 3U);
    assert(output.fills.back().resting_order_id == 2U && output.fills.back().quantity == 5U);
    assert(book.best_ask() == nexuslob::kInvalidPrice);
}

void test_cancel_unlinks_middle_node_in_constant_time() {
    nexuslob::OrderBook book(new_book_config());
    Capture output;
    submit(book, add(11U, nexuslob::Side::Sell, nexuslob::OrderType::LimitGtc, 150U, 3U), output);
    submit(book, add(12U, nexuslob::Side::Sell, nexuslob::OrderType::LimitGtc, 150U, 4U), output);
    submit(book, add(13U, nexuslob::Side::Sell, nexuslob::OrderType::LimitGtc, 150U, 5U), output);
    nexuslob::IncomingMessage cancel{nexuslob::MessageType::CancelOrder, nexuslob::Side::Sell,
                                     nexuslob::OrderType::LimitGtc, 0U, 12U, 0U, 0U};
    submit(book, cancel, output);
    submit(book, add(14U, nexuslob::Side::Buy, nexuslob::OrderType::Market, 0U, 8U), output);

    assert(output.fills.size() == 2U);
    assert(output.fills[0].resting_order_id == 11U);
    assert(output.fills[1].resting_order_id == 13U);
    assert(output.fills[1].quantity == 5U);
}

void test_market_sweeps_multiple_prices() {
    nexuslob::OrderBook book(new_book_config());
    Capture output;
    submit(book, add(21U, nexuslob::Side::Sell, nexuslob::OrderType::LimitGtc, 200U, 5U), output);
    submit(book, add(22U, nexuslob::Side::Sell, nexuslob::OrderType::LimitGtc, 201U, 7U), output);
    submit(book, add(23U, nexuslob::Side::Buy, nexuslob::OrderType::Market, 0U, 12U), output);

    assert(output.fills.size() == 2U);
    assert(output.fills[0].price_tick == 200U && output.fills[0].quantity == 5U);
    assert(output.fills[1].price_tick == 201U && output.fills[1].quantity == 7U);
    assert(book.best_ask() == nexuslob::kInvalidPrice);
}

void test_bitmap_and_egress_backpressure() {
    nexuslob::NonEmptyPriceSet prices(100'000U);
    prices.set(5U);
    prices.set(50'000U);
    prices.set(99'999U);
    assert(prices.first() == 5U);
    assert(prices.last() == 99'999U);
    prices.reset(99'999U);
    prices.reset(5U);
    assert(prices.first() == 50'000U && prices.last() == 50'000U);

    nexuslob::OrderBook book(new_book_config());
    Capture output;
    submit(book, add(31U, nexuslob::Side::Sell, nexuslob::OrderType::LimitGtc, 300U, 8U), output);
    nexuslob::IncomingMessage incoming = add(32U, nexuslob::Side::Buy, nexuslob::OrderType::LimitIoc, 300U, 3U);
    assert(!book.process(incoming, reject_execution, nullptr));
    assert(incoming.quantity == 3U && book.best_ask() == 300U);
    assert(book.process(incoming, capture_execution, &output));
    assert(output.fills.size() == 1U && output.fills[0].quantity == 3U);
}

}  // namespace

int main() {
    test_price_time_priority_and_ioc();
    test_cancel_unlinks_middle_node_in_constant_time();
    test_market_sweeps_multiple_prices();
    test_bitmap_and_egress_backpressure();
    std::cout << "order_book_tests passed\n";
}
