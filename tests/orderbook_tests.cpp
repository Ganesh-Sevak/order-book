#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "orderbook/jsonl.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/spsc_ring.hpp"
#include "orderbook/synthetic.hpp"

#include <variant>

using namespace orderbook;

TEST_CASE("fifo priority at one price") {
    OrderBook book;
    CHECK(book.submit({.id = 1, .side = Side::Sell, .price = 101, .quantity = 10}).status == SubmitStatus::Accepted);
    CHECK(book.submit({.id = 2, .side = Side::Sell, .price = 101, .quantity = 10}).status == SubmitStatus::Accepted);

    auto result = book.submit({.id = 3, .side = Side::Buy, .price = 101, .quantity = 15, .tif = TimeInForce::Ioc});

    REQUIRE(result.trades.size() == 2);
    CHECK(result.trades[0].maker_id == 1);
    CHECK(result.trades[0].quantity == 10);
    CHECK(result.trades[1].maker_id == 2);
    CHECK(result.trades[1].quantity == 5);
    CHECK(book.depth_at(Side::Sell, 101) == 5);
}

TEST_CASE("price priority across levels") {
    OrderBook book;
    (void)book.submit({.id = 1, .side = Side::Sell, .price = 105, .quantity = 10});
    (void)book.submit({.id = 2, .side = Side::Sell, .price = 103, .quantity = 10});

    auto result = book.submit({.id = 3, .side = Side::Buy, .price = 106, .quantity = 10, .tif = TimeInForce::Ioc});

    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].maker_id == 2);
}

TEST_CASE("ioc fok cancel and replace behavior") {
    OrderBook book;
    (void)book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 10});

    auto fok = book.submit({.id = 2, .side = Side::Buy, .price = 100, .quantity = 20, .tif = TimeInForce::Fok});
    CHECK(fok.status == SubmitStatus::RejectedFokNotFillable);
    CHECK(book.depth_at(Side::Sell, 100) == 10);

    auto ioc = book.submit({.id = 3, .side = Side::Buy, .price = 100, .quantity = 20, .tif = TimeInForce::Ioc});
    REQUIRE(ioc.trades.size() == 1);
    CHECK(ioc.resting_quantity == 0);

    (void)book.submit({.id = 4, .side = Side::Buy, .price = 99, .quantity = 7});
    CHECK(book.cancel(4).status == CancelStatus::Canceled);
    CHECK(book.cancel(4).status == CancelStatus::UnknownOrder);

    (void)book.submit({.id = 5, .side = Side::Buy, .price = 98, .quantity = 7});
    auto replace = book.replace({.old_id = 5, .new_id = 6, .new_price = 97, .new_quantity = 9});
    CHECK(replace.status == ReplaceStatus::Replaced);
    CHECK(book.depth_at(Side::Buy, 97) == 9);
}

TEST_CASE("invalid inputs and capacity rejection") {
    OrderBook book({.max_orders = 1});
    CHECK(book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 0}).status == SubmitStatus::RejectedInvalidQuantity);
    CHECK(book.submit({.id = 1, .side = Side::Buy, .price = -1, .quantity = 1}).status == SubmitStatus::RejectedInvalidPrice);
    CHECK(book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 1}).status == SubmitStatus::Accepted);
    CHECK(book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 1}).status == SubmitStatus::RejectedDuplicateId);
    CHECK(book.submit({.id = 2, .side = Side::Buy, .price = 99, .quantity = 1}).status == SubmitStatus::RejectedCapacity);
}

TEST_CASE("cancelled and filled slots are reused") {
    OrderBook cancel_book({.max_orders = 1});
    CHECK(cancel_book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 1}).status == SubmitStatus::Accepted);
    CHECK(cancel_book.cancel(1).status == CancelStatus::Canceled);
    CHECK(cancel_book.reusable_slots() == 1);
    CHECK(cancel_book.submit({.id = 2, .side = Side::Buy, .price = 101, .quantity = 1}).status == SubmitStatus::Accepted);

    OrderBook fill_book({.max_orders = 1});
    CHECK(fill_book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 1}).status == SubmitStatus::Accepted);
    auto fill = fill_book.submit({.id = 2, .side = Side::Buy, .price = 100, .quantity = 1, .tif = TimeInForce::Ioc});
    REQUIRE(fill.trades.size() == 1);
    CHECK(fill_book.reusable_slots() == 1);
    CHECK(fill_book.submit({.id = 3, .side = Side::Sell, .price = 101, .quantity = 1}).status == SubmitStatus::Accepted);
}

TEST_CASE("residual quantity is not silently dropped when capacity is exhausted") {
    OrderBook book({.max_orders = 1});
    CHECK(book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 5}).status == SubmitStatus::Accepted);

    auto result = book.submit({.id = 2, .side = Side::Buy, .price = 100, .quantity = 10});

    CHECK(result.status == SubmitStatus::RejectedCapacity);
    CHECK(result.trades.empty());
    CHECK(book.depth_at(Side::Sell, 100) == 5);
}

TEST_CASE("sorted vector levels preserve top of book and snapshots") {
    OrderBook book;
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 10});
    (void)book.submit({.id = 2, .side = Side::Buy, .price = 102, .quantity = 20});
    (void)book.submit({.id = 3, .side = Side::Buy, .price = 101, .quantity = 30});
    (void)book.submit({.id = 4, .side = Side::Sell, .price = 106, .quantity = 10});
    (void)book.submit({.id = 5, .side = Side::Sell, .price = 104, .quantity = 20});
    (void)book.submit({.id = 6, .side = Side::Sell, .price = 105, .quantity = 30});

    CHECK(book.best_bid().value() == 102);
    CHECK(book.best_ask().value() == 104);
    auto snapshot = book.snapshot(3);
    REQUIRE(snapshot.bids.size() == 3);
    REQUIRE(snapshot.asks.size() == 3);
    CHECK(snapshot.bids[0].price == 102);
    CHECK(snapshot.bids[1].price == 101);
    CHECK(snapshot.bids[2].price == 100);
    CHECK(snapshot.asks[0].price == 104);
    CHECK(snapshot.asks[1].price == 105);
    CHECK(snapshot.asks[2].price == 106);
}

TEST_CASE("parser handles valid events and rejects bad fields") {
    auto add = parse_event_jsonl(R"({"op":"add","id":1,"side":"buy","price":100,"quantity":5,"type":"limit","tif":"gtc"})");
    CHECK(add.status == ParseStatus::Ok);
    CHECK(std::holds_alternative<NewOrder>(add.event));

    auto cancel = parse_event_jsonl(R"({"op":"cancel","id":1})");
    CHECK(cancel.status == ParseStatus::Ok);
    CHECK(std::holds_alternative<CancelOrder>(cancel.event));

    auto bad = parse_event_jsonl(R"({"op":"add","id":"not-a-number","side":"buy","price":100,"quantity":5})");
    CHECK(bad.status == ParseStatus::InvalidNumber);
}

TEST_CASE("spsc ring preserves fifo order") {
    SpscRing<int> ring(8);
    CHECK(ring.push(1));
    CHECK(ring.push(2));
    CHECK(ring.pop().value_or(0) == 1);
    CHECK(ring.pop().value_or(0) == 2);
    CHECK(!ring.pop().has_value());
}

TEST_CASE("random event streams preserve book invariants") {
    for (std::uint64_t seed = 1; seed <= 50; ++seed) {
        OrderBook book({.max_orders = 20'000, .max_price_levels = 4096});
        auto orders = make_synthetic_orders(10'000, seed);
        for (const auto& order : orders) {
            (void)book.submit(order);
            const auto invariant = book.check_invariants();
            INFO("seed=" << seed << " message=" << invariant.message);
            CHECK(invariant.ok);
        }
    }
}
