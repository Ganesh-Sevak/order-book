#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "orderbook/jsonl.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/spsc_ring.hpp"
#include "orderbook/synthetic.hpp"

#include <variant>
#include <stdexcept>
#include <vector>

using namespace orderbook;

OrderBookConfig test_config(std::size_t max_orders = 1'000'000) {
    return {.min_price = 1, .max_price = 20'000, .max_orders = max_orders};
}

struct CapturedTrades {
    std::vector<Trade> values;
};

void capture_trade(void* context, const Trade& trade) noexcept {
    static_cast<CapturedTrades*>(context)->values.push_back(trade);
}

TEST_CASE("fifo priority at one price") {
    OrderBook book(test_config());
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
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Sell, .price = 105, .quantity = 10});
    (void)book.submit({.id = 2, .side = Side::Sell, .price = 103, .quantity = 10});

    auto result = book.submit({.id = 3, .side = Side::Buy, .price = 106, .quantity = 10, .tif = TimeInForce::Ioc});

    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].maker_id == 2);
}

TEST_CASE("ioc fok cancel and replace behavior") {
    OrderBook book(test_config());
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
    OrderBook book(test_config(1));
    CHECK(book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 0}).status == SubmitStatus::RejectedInvalidQuantity);
    CHECK(book.submit({.id = 1, .side = Side::Buy, .price = -1, .quantity = 1}).status == SubmitStatus::RejectedInvalidPrice);
    CHECK(book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 1}).status == SubmitStatus::Accepted);
    CHECK(book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 1}).status == SubmitStatus::RejectedDuplicateId);
    CHECK(book.submit({.id = 2, .side = Side::Buy, .price = 99, .quantity = 1}).status == SubmitStatus::RejectedCapacity);
}

TEST_CASE("cancelled and filled slots are reused") {
    OrderBook cancel_book(test_config(1));
    CHECK(cancel_book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 1}).status == SubmitStatus::Accepted);
    CHECK(cancel_book.cancel(1).status == CancelStatus::Canceled);
    CHECK(cancel_book.reusable_slots() == 1);
    CHECK(cancel_book.submit({.id = 2, .side = Side::Buy, .price = 101, .quantity = 1}).status == SubmitStatus::Accepted);

    OrderBook fill_book(test_config(1));
    CHECK(fill_book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 1}).status == SubmitStatus::Accepted);
    auto fill = fill_book.submit({.id = 2, .side = Side::Buy, .price = 100, .quantity = 1, .tif = TimeInForce::Ioc});
    REQUIRE(fill.trades.size() == 1);
    CHECK(fill_book.reusable_slots() == 1);
    CHECK(fill_book.submit({.id = 3, .side = Side::Sell, .price = 101, .quantity = 1}).status == SubmitStatus::Accepted);
}

TEST_CASE("residual quantity is not silently dropped when capacity is exhausted") {
    OrderBook book(test_config(1));
    CHECK(book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 5}).status == SubmitStatus::Accepted);

    auto result = book.submit({.id = 2, .side = Side::Buy, .price = 100, .quantity = 10});

    CHECK(result.status == SubmitStatus::RejectedCapacity);
    CHECK(result.trades.empty());
    CHECK(book.depth_at(Side::Sell, 100) == 5);
}

TEST_CASE("sorted vector levels preserve top of book and snapshots") {
    OrderBook book(test_config());
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

TEST_CASE("bounded ladder rejects out-of-band limits and keeps market prices unrestricted") {
    CHECK_THROWS_AS(OrderBook(OrderBookConfig{.min_price = 10, .max_price = 9}), std::invalid_argument);

    OrderBook book({.min_price = 100, .max_price = 200, .max_orders = 16});
    CHECK(book.submit({.id = 1, .side = Side::Buy, .price = 99, .quantity = 1}).status == SubmitStatus::RejectedInvalidPrice);
    CHECK(book.submit({.id = 2, .side = Side::Buy, .price = 201, .quantity = 1}).status == SubmitStatus::RejectedInvalidPrice);
    CHECK(book.submit({.id = 3, .side = Side::Buy, .price = 100, .quantity = 1}).status == SubmitStatus::Accepted);
    CHECK(book.submit({.id = 4, .side = Side::Sell, .price = 200, .quantity = 2}).status == SubmitStatus::Accepted);
    CHECK(book.submit({.id = 5, .side = Side::Buy, .price = 0, .quantity = 1, .type = OrderType::Market}).status == SubmitStatus::Accepted);
    CHECK(book.best_bid().value() == 100);
    CHECK(book.best_ask().value() == 200);
}

TEST_CASE("sparse ladder levels update best prices without scanning empty ticks") {
    OrderBook book({.min_price = 1, .max_price = 131'072, .max_orders = 16});
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 2, .quantity = 1});
    (void)book.submit({.id = 2, .side = Side::Buy, .price = 65'537, .quantity = 1});
    (void)book.submit({.id = 3, .side = Side::Buy, .price = 131'072, .quantity = 1});
    CHECK(book.best_bid().value() == 131'072);
    CHECK(book.cancel(3).status == CancelStatus::Canceled);
    CHECK(book.best_bid().value() == 65'537);
    CHECK(book.cancel(2).status == CancelStatus::Canceled);
    CHECK(book.best_bid().value() == 2);
    CHECK(book.check_invariants().ok);
}

TEST_CASE("flat order index remains correct through repeated erase and slot reuse") {
    OrderBook book({.min_price = 1, .max_price = 512, .max_orders = 128});
    for (OrderId id = 1; id <= 128; ++id) {
        CHECK(book.submit({.id = id, .side = Side::Buy, .price = static_cast<Price>(1 + (id % 512)), .quantity = 1}).status == SubmitStatus::Accepted);
    }
    for (OrderId id = 1; id <= 128; id += 2) CHECK(book.cancel(id).status == CancelStatus::Canceled);
    for (OrderId id = 129; id <= 192; ++id) {
        CHECK(book.submit({.id = id, .side = Side::Buy, .price = static_cast<Price>(1 + (id % 512)), .quantity = 1}).status == SubmitStatus::Accepted);
    }
    CHECK(book.live_orders() == 128);
    CHECK(book.check_invariants().ok);
}

TEST_CASE("trade sink matches result-owning submit and replace APIs") {
    const OrderBookConfig config{.min_price = 1, .max_price = 1'000, .max_orders = 32};
    OrderBook result_book(config);
    OrderBook sink_book(config);
    for (const auto id : {1ULL, 2ULL, 3ULL}) {
        const NewOrder maker{.id = id, .side = Side::Sell, .price = 100, .quantity = 2};
        (void)result_book.submit(maker);
        (void)sink_book.submit(maker);
    }

    const NewOrder taker{.id = 4, .side = Side::Buy, .price = 100, .quantity = 5, .tif = TimeInForce::Ioc};
    const auto result = result_book.submit(taker);
    CapturedTrades captured;
    captured.values.reserve(3);
    const auto summary = sink_book.submit(taker, {.context = &captured, .on_trade = capture_trade});
    CHECK(summary.status == result.status);
    CHECK(summary.accepted_quantity == result.accepted_quantity);
    CHECK(summary.resting_quantity == result.resting_quantity);
    REQUIRE(summary.trade_count == result.trades.size());
    REQUIRE(captured.values.size() == result.trades.size());
    for (std::size_t i = 0; i < result.trades.size(); ++i) {
        CHECK(captured.values[i].maker_id == result.trades[i].maker_id);
        CHECK(captured.values[i].taker_id == result.trades[i].taker_id);
        CHECK(captured.values[i].price == result.trades[i].price);
        CHECK(captured.values[i].quantity == result.trades[i].quantity);
        CHECK(captured.values[i].sequence == result.trades[i].sequence);
    }

    (void)result_book.submit({.id = 5, .side = Side::Buy, .price = 90, .quantity = 2});
    (void)sink_book.submit({.id = 5, .side = Side::Buy, .price = 90, .quantity = 2});
    const auto replaced = result_book.replace({.old_id = 5, .new_id = 6, .new_price = 110, .new_quantity = 2, .tif = TimeInForce::Ioc});
    captured.values.clear();
    captured.values.reserve(1);
    const auto replaced_summary = sink_book.replace({.old_id = 5, .new_id = 6, .new_price = 110, .new_quantity = 2, .tif = TimeInForce::Ioc},
                                                    {.context = &captured, .on_trade = capture_trade});
    CHECK(replaced_summary.status == replaced.status);
    CHECK(replaced_summary.resting_quantity == replaced.resting_quantity);
    CHECK(replaced_summary.trade_count == replaced.trades.size());
    CHECK(captured.values.size() == replaced.trades.size());
    CHECK(result_book.check_invariants().ok);
    CHECK(sink_book.check_invariants().ok);
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

    auto escaped = parse_event_jsonl("{\"op\":\"add\",\"id\":1,\"side\":\"buy\",\"price\":100,\"quantity\":5,\"type\":\"lim\\nit\",\"tif\":\"gtc\"}");
    CHECK(escaped.status == ParseStatus::InvalidEnum);

    auto unsupported = parse_event_jsonl(R"({"op":"add","id":1,"side":"buy","price":100,"quantity":5,"type":"\u006cimit","tif":"gtc"})");
    CHECK(unsupported.status == ParseStatus::UnsupportedEscape);
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
        OrderBook book(test_config(20'000));
        auto orders = make_synthetic_orders(10'000, seed);
        for (const auto& order : orders) {
            (void)book.submit(order);
            const auto invariant = book.check_invariants();
            INFO("seed=" << seed << " message=" << invariant.message);
            CHECK(invariant.ok);
        }
    }
}

// ---------------------------------------------------------------------------
// Market orders
// ---------------------------------------------------------------------------

TEST_CASE("market order against empty book is accepted with no fills") {
    OrderBook book(test_config());
    auto result = book.submit({.id = 1, .side = Side::Buy, .quantity = 10, .type = OrderType::Market});
    CHECK(result.status == SubmitStatus::Accepted);
    CHECK(result.trades.empty());
    CHECK(result.resting_quantity == 0);
    CHECK(book.live_orders() == 0);
}

TEST_CASE("market buy fills available sells by price priority and does not rest") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 5});
    (void)book.submit({.id = 2, .side = Side::Sell, .price = 101, .quantity = 5});

    auto result = book.submit({.id = 3, .side = Side::Buy, .quantity = 8, .type = OrderType::Market});
    REQUIRE(result.trades.size() == 2);
    CHECK(result.trades[0].maker_id == 1);
    CHECK(result.trades[0].quantity == 5);
    CHECK(result.trades[1].maker_id == 2);
    CHECK(result.trades[1].quantity == 3);
    CHECK(result.resting_quantity == 0);
    CHECK(book.depth_at(Side::Sell, 101) == 2);
    CHECK(book.check_invariants().ok);
}

TEST_CASE("market sell fills bids at maker price and does not rest") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 200, .quantity = 5});

    auto result = book.submit({.id = 2, .side = Side::Sell, .quantity = 3, .type = OrderType::Market});
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].price == 200);
    CHECK(result.trades[0].quantity == 3);
    CHECK(result.resting_quantity == 0);
    CHECK(book.depth_at(Side::Buy, 200) == 2);
}

// ---------------------------------------------------------------------------
// Multi-level crossing
// ---------------------------------------------------------------------------

TEST_CASE("aggressive limit sweeps multiple price levels") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 10});
    (void)book.submit({.id = 2, .side = Side::Sell, .price = 101, .quantity = 10});
    (void)book.submit({.id = 3, .side = Side::Sell, .price = 102, .quantity = 10});

    auto result = book.submit({.id = 4, .side = Side::Buy, .price = 102, .quantity = 25, .tif = TimeInForce::Ioc});
    REQUIRE(result.trades.size() == 3);
    CHECK(result.trades[0].maker_id == 1);
    CHECK(result.trades[0].quantity == 10);
    CHECK(result.trades[1].maker_id == 2);
    CHECK(result.trades[1].quantity == 10);
    CHECK(result.trades[2].maker_id == 3);
    CHECK(result.trades[2].quantity == 5);
    CHECK(result.resting_quantity == 0);
    CHECK(book.depth_at(Side::Sell, 100) == 0);
    CHECK(book.depth_at(Side::Sell, 101) == 0);
    CHECK(book.depth_at(Side::Sell, 102) == 5);
    CHECK(*book.best_ask() == 102);
    CHECK(book.check_invariants().ok);
}

// ---------------------------------------------------------------------------
// IOC / FOK edge cases
// ---------------------------------------------------------------------------

TEST_CASE("ioc order with no matching liquidity is accepted without resting") {
    OrderBook book(test_config());
    auto result = book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 10, .tif = TimeInForce::Ioc});
    CHECK(result.status == SubmitStatus::Accepted);
    CHECK(result.trades.empty());
    CHECK(result.resting_quantity == 0);
    CHECK(book.live_orders() == 0);
}

TEST_CASE("fok succeeds when available liquidity exactly matches quantity") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 10});

    auto result = book.submit({.id = 2, .side = Side::Buy, .price = 100, .quantity = 10, .tif = TimeInForce::Fok});
    CHECK(result.status == SubmitStatus::Accepted);
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].quantity == 10);
    CHECK(result.resting_quantity == 0);
    CHECK(!book.best_ask().has_value());
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

TEST_CASE("cancel removes price level when it becomes empty") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 10});
    CHECK(*book.best_bid() == 100);

    auto cr = book.cancel(1);
    CHECK(cr.status == CancelStatus::Canceled);
    CHECK(cr.canceled_quantity == 10);
    CHECK(!book.best_bid().has_value());
    CHECK(book.depth_at(Side::Buy, 100) == 0);
    CHECK(book.live_orders() == 0);
    CHECK(book.check_invariants().ok);
}

TEST_CASE("cancel reduces level depth without removing the level") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 10});
    (void)book.submit({.id = 2, .side = Side::Buy, .price = 100, .quantity = 5});

    (void)book.cancel(1);
    CHECK(book.depth_at(Side::Buy, 100) == 5);
    CHECK(*book.best_bid() == 100);
    CHECK(book.live_orders() == 1);
    CHECK(book.check_invariants().ok);
}

// ---------------------------------------------------------------------------
// Replace edge cases
// ---------------------------------------------------------------------------

TEST_CASE("replace with same id reprices and resizes in place") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 10});

    auto result = book.replace({.old_id = 1, .new_id = 1, .new_price = 99, .new_quantity = 5});
    CHECK(result.status == ReplaceStatus::Replaced);
    CHECK(result.resting_quantity == 5);
    CHECK(book.depth_at(Side::Buy, 100) == 0);
    CHECK(book.depth_at(Side::Buy, 99) == 5);
    CHECK(book.live_orders() == 1);
    CHECK(book.check_invariants().ok);
}

TEST_CASE("replace that crosses the spread reports Filled") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 10});
    (void)book.submit({.id = 2, .side = Side::Buy, .price = 95, .quantity = 5});

    auto result = book.replace({.old_id = 2, .new_id = 3, .new_price = 101, .new_quantity = 5});
    CHECK(result.status == ReplaceStatus::Filled);
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].quantity == 5);
    CHECK(result.resting_quantity == 0);
    CHECK(book.depth_at(Side::Buy, 95) == 0);
    CHECK(book.depth_at(Side::Sell, 100) == 5);
    CHECK(book.check_invariants().ok);
}

TEST_CASE("replace of unknown order returns UnknownOrder without modifying book") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 5});
    auto result = book.replace({.old_id = 999, .new_id = 1000, .new_price = 100, .new_quantity = 5});
    CHECK(result.status == ReplaceStatus::UnknownOrder);
    CHECK(book.depth_at(Side::Buy, 100) == 5);
}

TEST_CASE("replace with existing new_id is rejected and old order remains") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 10});
    (void)book.submit({.id = 2, .side = Side::Buy, .price = 99, .quantity = 10});

    auto result = book.replace({.old_id = 1, .new_id = 2, .new_price = 98, .new_quantity = 5});
    CHECK(result.status == ReplaceStatus::RejectedDuplicateId);
    CHECK(book.depth_at(Side::Buy, 100) == 10);
    CHECK(book.depth_at(Side::Buy, 99) == 10);
    CHECK(book.check_invariants().ok);
}

TEST_CASE("replace with zero quantity is rejected and old order remains") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 5});

    auto result = book.replace({.old_id = 1, .new_id = 2, .new_price = 99, .new_quantity = 0});
    CHECK(result.status == ReplaceStatus::RejectedInvalidQuantity);
    CHECK(book.depth_at(Side::Buy, 100) == 5);
    CHECK(book.live_orders() == 1);
}

TEST_CASE("replace with out-of-range price is rejected and old order remains") {
    OrderBook book({.min_price = 100, .max_price = 200, .max_orders = 16});
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 150, .quantity = 5});

    auto result = book.replace({.old_id = 1, .new_id = 2, .new_price = 99, .new_quantity = 5});
    CHECK(result.status == ReplaceStatus::RejectedInvalidPrice);
    CHECK(book.depth_at(Side::Buy, 150) == 5);
    CHECK(book.live_orders() == 1);
}

// ---------------------------------------------------------------------------
// Market data queries
// ---------------------------------------------------------------------------

TEST_CASE("best bid and ask are nullopt on empty and one-sided books") {
    OrderBook book(test_config());
    CHECK(!book.best_bid().has_value());
    CHECK(!book.best_ask().has_value());
    CHECK(!book.spread().has_value());
    CHECK(!book.midprice().has_value());
    CHECK(!book.microprice().has_value());

    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 5});
    CHECK(book.best_bid().has_value());
    CHECK(!book.best_ask().has_value());
    CHECK(!book.spread().has_value());
    CHECK(!book.midprice().has_value());
    CHECK(!book.microprice().has_value());
}

TEST_CASE("midprice and spread are computed from best bid and ask") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 99, .quantity = 10});
    (void)book.submit({.id = 2, .side = Side::Sell, .price = 101, .quantity = 10});

    CHECK(*book.best_bid() == 99);
    CHECK(*book.best_ask() == 101);
    CHECK(*book.spread() == 2);
    CHECK(*book.midprice() == 100.0);
}

TEST_CASE("microprice is weighted toward the side with greater quantity") {
    OrderBook book(test_config());
    // bid@99 qty=30, ask@101 qty=10: (101*30 + 99*10) / 40 = 4020/40 = 100.5
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 99, .quantity = 30});
    (void)book.submit({.id = 2, .side = Side::Sell, .price = 101, .quantity = 10});

    auto mp = book.microprice();
    REQUIRE(mp.has_value());
    CHECK(*mp == 100.5);
}

TEST_CASE("imbalance returns 0 for symmetric depth and correct value for asymmetric") {
    OrderBook book(test_config());
    // bid@5000 qty=60, bid@4900 qty=20, ask@5100 qty=20
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 5000, .quantity = 60});
    (void)book.submit({.id = 2, .side = Side::Buy, .price = 4900, .quantity = 20});
    (void)book.submit({.id = 3, .side = Side::Sell, .price = 5100, .quantity = 20});

    // depth=1: bid=60, ask=20 → (60-20)/80 = 0.5
    CHECK(book.imbalance(1) == 0.5);
    // depth=2: bid=80, ask=20 → (80-20)/100 = 0.6
    CHECK(book.imbalance(2) == 0.6);

    OrderBook empty_book(test_config());
    CHECK(empty_book.imbalance(5) == 0.0);
}

TEST_CASE("best ask advances to next level after top level is cleared by fill") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 10});
    (void)book.submit({.id = 2, .side = Side::Sell, .price = 101, .quantity = 10});
    CHECK(*book.best_ask() == 100);

    (void)book.submit({.id = 3, .side = Side::Buy, .price = 100, .quantity = 10, .tif = TimeInForce::Ioc});
    CHECK(*book.best_ask() == 101);

    (void)book.submit({.id = 4, .side = Side::Buy, .price = 101, .quantity = 10, .tif = TimeInForce::Ioc});
    CHECK(!book.best_ask().has_value());
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

TEST_CASE("snapshot orders bids descending and asks ascending with depth limit") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 10});
    (void)book.submit({.id = 2, .side = Side::Buy, .price = 99, .quantity = 5});
    (void)book.submit({.id = 3, .side = Side::Sell, .price = 101, .quantity = 7});
    (void)book.submit({.id = 4, .side = Side::Sell, .price = 102, .quantity = 3});

    auto snap = book.snapshot(5);
    REQUIRE(snap.bids.size() == 2);
    REQUIRE(snap.asks.size() == 2);
    CHECK(snap.bids[0].price == 100);
    CHECK(snap.bids[1].price == 99);
    CHECK(snap.asks[0].price == 101);
    CHECK(snap.asks[1].price == 102);
    CHECK(snap.bids[0].quantity == 10);
    CHECK(snap.bids[0].order_count == 1);
    CHECK(snap.sequence == book.sequence());

    auto snap1 = book.snapshot(1);
    CHECK(snap1.bids.size() == 1);
    CHECK(snap1.asks.size() == 1);
    CHECK(snap1.bids[0].price == 100);
    CHECK(snap1.asks[0].price == 101);
}

TEST_CASE("snapshot aggregates quantity and order count for multiple orders at same price") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 10});
    (void)book.submit({.id = 2, .side = Side::Buy, .price = 100, .quantity = 5});
    (void)book.submit({.id = 3, .side = Side::Buy, .price = 100, .quantity = 3});

    auto snap = book.snapshot(5);
    REQUIRE(snap.bids.size() == 1);
    CHECK(snap.bids[0].price == 100);
    CHECK(snap.bids[0].quantity == 18);
    CHECK(snap.bids[0].order_count == 3);
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

TEST_CASE("metrics submitted and accepted increment on each successful order") {
    OrderBook book(test_config());
    CHECK(book.metrics().submitted == 0);
    CHECK(book.metrics().accepted == 0);
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 10});
    CHECK(book.metrics().submitted == 1);
    CHECK(book.metrics().accepted == 1);
    CHECK(book.metrics().rejected == 0);
}

TEST_CASE("metrics rejected increments on duplicate id and invalid price") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 10});
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 99, .quantity = 5}); // duplicate
    CHECK(book.metrics().submitted == 2);
    CHECK(book.metrics().rejected == 1);
    CHECK(book.metrics().accepted == 1);

    (void)book.submit({.id = 2, .side = Side::Buy, .price = 0, .quantity = 1}); // price out of range
    CHECK(book.metrics().rejected == 2);
}

TEST_CASE("metrics canceled and trades increment on cancel and fill") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 10});
    (void)book.cancel(1);
    CHECK(book.metrics().canceled == 1);

    (void)book.submit({.id = 2, .side = Side::Sell, .price = 90, .quantity = 5});
    (void)book.submit({.id = 3, .side = Side::Buy, .price = 90, .quantity = 5, .tif = TimeInForce::Ioc});
    CHECK(book.metrics().trades == 1);
}

TEST_CASE("metrics replaced increments on successful replace") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 5});
    (void)book.replace({.old_id = 1, .new_id = 2, .new_price = 99, .new_quantity = 3});
    CHECK(book.metrics().replaced == 1);
}

// ---------------------------------------------------------------------------
// Sequence number
// ---------------------------------------------------------------------------

TEST_CASE("sequence increments on resting order and trade but not on cancel") {
    OrderBook book(test_config());
    CHECK(book.sequence() == 0);

    (void)book.submit({.id = 1, .side = Side::Sell, .price = 100, .quantity = 10});
    CHECK(book.sequence() == 1);

    (void)book.submit({.id = 2, .side = Side::Sell, .price = 101, .quantity = 10});
    CHECK(book.sequence() == 2);

    auto result = book.submit({.id = 3, .side = Side::Buy, .price = 100, .quantity = 5, .tif = TimeInForce::Ioc});
    CHECK(book.sequence() == 3);
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].sequence == 3);

    (void)book.submit({.id = 4, .side = Side::Buy, .price = 98, .quantity = 5});
    const auto seq_before_cancel = book.sequence();
    (void)book.cancel(4);
    CHECK(book.sequence() == seq_before_cancel);
}

// ---------------------------------------------------------------------------
// Live orders count
// ---------------------------------------------------------------------------

TEST_CASE("live orders count tracks active resting orders") {
    OrderBook book(test_config());
    CHECK(book.live_orders() == 0);

    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 10});
    CHECK(book.live_orders() == 1);

    // sell@99 qty=5 crosses bid@100: sell fully fills, buy rests with 5 remaining
    (void)book.submit({.id = 2, .side = Side::Sell, .price = 99, .quantity = 5});
    CHECK(book.live_orders() == 1);

    (void)book.cancel(1);
    CHECK(book.live_orders() == 0);
}

// ---------------------------------------------------------------------------
// Parser: replace event
// ---------------------------------------------------------------------------

TEST_CASE("parser handles replace event with and without explicit tif") {
    auto result = parse_event_jsonl(R"({"op":"replace","old_id":1,"new_id":2,"price":100,"quantity":5})");
    CHECK(result.status == ParseStatus::Ok);
    REQUIRE(std::holds_alternative<ReplaceOrder>(result.event));
    const auto& rep = std::get<ReplaceOrder>(result.event);
    CHECK(rep.old_id == 1);
    CHECK(rep.new_id == 2);
    CHECK(rep.new_price == 100);
    CHECK(rep.new_quantity == 5);
    CHECK(rep.tif == TimeInForce::Gtc);

    auto with_tif = parse_event_jsonl(R"({"op":"replace","old_id":3,"new_id":4,"price":50,"quantity":2,"tif":"ioc"})");
    CHECK(with_tif.status == ParseStatus::Ok);
    REQUIRE(std::holds_alternative<ReplaceOrder>(with_tif.event));
    CHECK(std::get<ReplaceOrder>(with_tif.event).tif == TimeInForce::Ioc);
}

// ---------------------------------------------------------------------------
// Parser: edge cases
// ---------------------------------------------------------------------------

TEST_CASE("parser returns EmptyLine for blank input") {
    CHECK(parse_event_jsonl("").status == ParseStatus::EmptyLine);
    CHECK(parse_event_jsonl("   ").status == ParseStatus::EmptyLine);
}

TEST_CASE("parser returns Malformed for structurally invalid JSON") {
    CHECK(parse_event_jsonl("not json").status == ParseStatus::Malformed);
    CHECK(parse_event_jsonl("{").status == ParseStatus::Malformed);
    CHECK(parse_event_jsonl(R"({"op":"add","id":1)").status == ParseStatus::Malformed);
}

TEST_CASE("parser returns MissingField when op key is absent") {
    CHECK(parse_event_jsonl(R"({"id":1,"side":"buy","price":100,"quantity":5})").status == ParseStatus::MissingField);
}

TEST_CASE("parser returns InvalidEnum for unknown op value") {
    CHECK(parse_event_jsonl(R"({"op":"unknown"})").status == ParseStatus::InvalidEnum);
    CHECK(parse_event_jsonl(R"({"op":"modify"})").status == ParseStatus::InvalidEnum);
}

TEST_CASE("parser returns MissingField when add is missing required fields") {
    // missing side
    CHECK(parse_event_jsonl(R"({"op":"add","id":1,"price":100,"quantity":5})").status == ParseStatus::MissingField);
    // missing price
    CHECK(parse_event_jsonl(R"({"op":"add","id":1,"side":"buy","quantity":5})").status == ParseStatus::MissingField);
    // missing quantity
    CHECK(parse_event_jsonl(R"({"op":"add","id":1,"side":"buy","price":100})").status == ParseStatus::MissingField);
    // missing id
    CHECK(parse_event_jsonl(R"({"op":"add","side":"buy","price":100,"quantity":5})").status == ParseStatus::MissingField);
}

TEST_CASE("parser returns UnknownField for add and cancel with extra keys") {
    CHECK(parse_event_jsonl(R"({"op":"add","id":1,"side":"buy","price":100,"quantity":5,"extra":"x"})").status == ParseStatus::UnknownField);
    CHECK(parse_event_jsonl(R"({"op":"cancel","id":1,"type":"limit"})").status == ParseStatus::UnknownField);
}

TEST_CASE("parser returns MissingField when cancel is missing id") {
    CHECK(parse_event_jsonl(R"({"op":"cancel"})").status == ParseStatus::MissingField);
}

TEST_CASE("parser returns InvalidEnum for bad enum values in add fields") {
    CHECK(parse_event_jsonl(R"({"op":"add","id":1,"side":"up","price":100,"quantity":5})").status == ParseStatus::InvalidEnum);
    CHECK(parse_event_jsonl(R"({"op":"add","id":1,"side":"buy","price":100,"quantity":5,"tif":"day"})").status == ParseStatus::InvalidEnum);
    CHECK(parse_event_jsonl(R"({"op":"add","id":1,"side":"buy","price":100,"quantity":5,"type":"stop"})").status == ParseStatus::InvalidEnum);
}

TEST_CASE("parser returns Overflow for values exceeding field type range") {
    // id overflows uint64_t
    CHECK(parse_event_jsonl(R"({"op":"add","id":99999999999999999999,"side":"buy","price":100,"quantity":5})").status == ParseStatus::Overflow);
    // quantity overflows uint32_t (max ~4.3e9)
    CHECK(parse_event_jsonl(R"({"op":"add","id":1,"side":"buy","price":100,"quantity":9999999999})").status == ParseStatus::Overflow);
}

TEST_CASE("parser returns InvalidNumber for non-numeric id value") {
    CHECK(parse_event_jsonl(R"({"op":"add","id":"abc","side":"buy","price":100,"quantity":5})").status == ParseStatus::InvalidNumber);
}

TEST_CASE("parser returns MissingField when replace is missing required fields") {
    // missing new_id
    CHECK(parse_event_jsonl(R"({"op":"replace","old_id":1,"price":100,"quantity":5})").status == ParseStatus::MissingField);
    // missing price
    CHECK(parse_event_jsonl(R"({"op":"replace","old_id":1,"new_id":2,"quantity":5})").status == ParseStatus::MissingField);
}

// ---------------------------------------------------------------------------
// Invariants
// ---------------------------------------------------------------------------

TEST_CASE("invariants hold after cancel and replace on a two-sided book") {
    OrderBook book(test_config());
    (void)book.submit({.id = 1, .side = Side::Buy, .price = 100, .quantity = 10});
    (void)book.submit({.id = 2, .side = Side::Sell, .price = 102, .quantity = 5});
    CHECK(book.check_invariants().ok);

    (void)book.cancel(1);
    CHECK(book.check_invariants().ok);

    (void)book.replace({.old_id = 2, .new_id = 3, .new_price = 103, .new_quantity = 3});
    CHECK(book.check_invariants().ok);
}
