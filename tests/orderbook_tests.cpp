#include "orderbook/jsonl.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/spsc_ring.hpp"
#include "orderbook/synthetic.hpp"

#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>

namespace {

using namespace orderbook;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_fifo_priority() {
    OrderBook book;
    require(book.submit({.id = 1, .side = Side::Sell, .price = 101, .quantity = 10}).status == SubmitStatus::Accepted, "first sell accepted");
    require(book.submit({.id = 2, .side = Side::Sell, .price = 101, .quantity = 10}).status == SubmitStatus::Accepted, "second sell accepted");
    auto result = book.submit({.id = 3, .side = Side::Buy, .price = 101, .quantity = 15, .tif = TimeInForce::Ioc});
    require(result.trades.size() == 2, "two fills");
    require(result.trades[0].maker_id == 1 && result.trades[0].quantity == 10, "first order fills first");
    require(result.trades[1].maker_id == 2 && result.trades[1].quantity == 5, "second order partially fills");
    require(book.depth_at(Side::Sell, 101) == 5, "remaining depth");
}

void test_price_priority() {
    OrderBook book;
    (void)book.submit({.id = 1, .side = Side::Sell, .price = 105, .quantity = 10});
    (void)book.submit({.id = 2, .side = Side::Sell, .price = 103, .quantity = 10});
    auto result = book.submit({.id = 3, .side = Side::Buy, .price = 106, .quantity = 10, .tif = TimeInForce::Ioc});
    require(result.trades.size() == 1, "one fill");
    require(result.trades[0].maker_id == 2, "best ask fills before worse ask");
}

void test_ioc_fok_cancel_replace() {
    OrderBook book;
    (void)book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 10});
    auto fok = book.submit({.id = 2, .side = Side::Buy, .price = 100, .quantity = 20, .tif = TimeInForce::Fok});
    require(fok.status == SubmitStatus::RejectedFokNotFillable, "fok rejects if not fully fillable");
    require(book.depth_at(Side::Sell, 100) == 10, "fok leaves book unchanged");
    auto ioc = book.submit({.id = 3, .side = Side::Buy, .price = 100, .quantity = 20, .tif = TimeInForce::Ioc});
    require(ioc.trades.size() == 1 && ioc.resting_quantity == 0, "ioc fills available only");
    (void)book.submit({.id = 4, .side = Side::Buy, .price = 99, .quantity = 7});
    require(book.cancel(4).status == CancelStatus::Canceled, "cancel active");
    require(book.cancel(4).status == CancelStatus::UnknownOrder, "cancel unknown after removal");
    (void)book.submit({.id = 5, .side = Side::Buy, .price = 98, .quantity = 7});
    auto replace = book.replace({.old_id = 5, .new_id = 6, .new_price = 97, .new_quantity = 9});
    require(replace.status == ReplaceStatus::Replaced, "replace accepted");
    require(book.depth_at(Side::Buy, 97) == 9, "replace rests new order");
}

void test_invalid_inputs() {
    OrderBook book({.max_orders = 1});
    require(book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 0}).status == SubmitStatus::RejectedInvalidQuantity, "reject zero qty");
    require(book.submit({.id = 1, .side = Side::Buy, .price = -1, .quantity = 1}).status == SubmitStatus::RejectedInvalidPrice, "reject bad price");
    require(book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 1}).status == SubmitStatus::Accepted, "accept first");
    require(book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 1}).status == SubmitStatus::RejectedDuplicateId, "reject duplicate");
    require(book.submit({.id = 2, .side = Side::Buy, .price = 99, .quantity = 1}).status == SubmitStatus::RejectedCapacity, "reject capacity");
}

void test_capacity_reuse_after_cancel_and_fill() {
    OrderBook cancel_book({.max_orders = 1});
    require(cancel_book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 1}).status == SubmitStatus::Accepted, "accept one");
    require(cancel_book.cancel(1).status == CancelStatus::Canceled, "cancel frees slot");
    require(cancel_book.reusable_slots() == 1, "slot is reusable after cancel");
    require(cancel_book.submit({.id = 2, .side = Side::Buy, .price = 101, .quantity = 1}).status == SubmitStatus::Accepted, "reuse canceled slot");

    OrderBook fill_book({.max_orders = 1});
    require(fill_book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 1}).status == SubmitStatus::Accepted, "accept resting sell");
    auto fill = fill_book.submit({.id = 2, .side = Side::Buy, .price = 100, .quantity = 1, .tif = TimeInForce::Ioc});
    require(fill.trades.size() == 1, "fill consumes resting order");
    require(fill_book.reusable_slots() == 1, "slot is reusable after fill");
    require(fill_book.submit({.id = 3, .side = Side::Sell, .price = 101, .quantity = 1}).status == SubmitStatus::Accepted, "reuse filled slot");
}

void test_no_partial_fill_when_residual_cannot_rest() {
    OrderBook book({.max_orders = 1});
    require(book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 5}).status == SubmitStatus::Accepted, "accept resting sell");
    auto result = book.submit({.id = 2, .side = Side::Buy, .price = 100, .quantity = 10});
    require(result.status == SubmitStatus::RejectedCapacity, "reject before creating residual that cannot rest");
    require(result.trades.empty(), "no partial fill before residual capacity failure");
    require(book.depth_at(Side::Sell, 100) == 5, "resting order unchanged");
}

void test_parser() {
    auto add = parse_event_jsonl(R"({"op":"add","id":1,"side":"buy","price":100,"quantity":5,"type":"limit","tif":"gtc"})");
    require(add.status == ParseStatus::Ok && std::holds_alternative<NewOrder>(add.event), "parse add");
    auto cancel = parse_event_jsonl(R"({"op":"cancel","id":1})");
    require(cancel.status == ParseStatus::Ok && std::holds_alternative<CancelOrder>(cancel.event), "parse cancel");
    auto bad = parse_event_jsonl(R"({"op":"add","id":"not-a-number","side":"buy","price":100,"quantity":5})");
    require(bad.status == ParseStatus::InvalidNumber, "reject bad number");
}

void test_spsc() {
    SpscRing<int> ring(8);
    require(ring.push(1), "push");
    require(ring.push(2), "push2");
    require(ring.pop().value_or(0) == 1, "pop1");
    require(ring.pop().value_or(0) == 2, "pop2");
    require(!ring.pop().has_value(), "empty");
}

void property_random_invariants() {
    for (std::uint64_t seed = 1; seed <= 50; ++seed) {
        OrderBook book({.max_orders = 20'000, .max_price_levels = 4096});
        auto orders = make_synthetic_orders(10'000, seed);
        for (const auto& order : orders) {
            (void)book.submit(order);
            const auto invariant = book.check_invariants();
            require(invariant.ok, invariant.message.c_str());
        }
    }
}

}  // namespace

int main() {
    test_fifo_priority();
    test_price_priority();
    test_ioc_fok_cancel_replace();
    test_invalid_inputs();
    test_capacity_reuse_after_cancel_and_fill();
    test_no_partial_fill_when_residual_cannot_rest();
    test_parser();
    test_spsc();
    property_random_invariants();
    std::cout << "orderbook_tests: ok\n";
}
