#include "orderbook/synthetic.hpp"

#include <random>

namespace orderbook {

std::vector<NewOrder> make_synthetic_orders(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> qty_dist(1, 250);
    std::uniform_int_distribution<int> price_offset(-50, 50);
    std::bernoulli_distribution market_dist(0.03);
    std::bernoulli_distribution ioc_dist(0.08);
    std::bernoulli_distribution fok_dist(0.02);

    std::vector<NewOrder> orders;
    orders.reserve(count);
    constexpr Price mid = 10'000;
    for (std::size_t i = 0; i < count; ++i) {
        const Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
        const bool market = market_dist(rng);
        TimeInForce tif = TimeInForce::Gtc;
        if (market || ioc_dist(rng)) {
            tif = TimeInForce::Ioc;
        }
        if (!market && fok_dist(rng)) {
            tif = TimeInForce::Fok;
        }
        orders.push_back(NewOrder{
            .id = static_cast<OrderId>(i + 1),
            .side = side,
            .price = market ? 0 : mid + price_offset(rng),
            .quantity = static_cast<Quantity>(qty_dist(rng)),
            .type = market ? OrderType::Market : OrderType::Limit,
            .tif = tif,
        });
    }
    return orders;
}

}  // namespace orderbook
