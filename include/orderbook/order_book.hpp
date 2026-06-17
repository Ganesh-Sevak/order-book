#pragma once

#include "orderbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace orderbook {

struct OrderBookConfig {
    std::size_t max_orders{1'000'000};
    std::size_t max_price_levels{100'000};
};

struct InvariantReport {
    bool ok{true};
    std::string message;
};

class OrderBook {
public:
    explicit OrderBook(OrderBookConfig config = {});

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;

    [[nodiscard]] SubmitResult submit(const NewOrder& order);
    [[nodiscard]] CancelResult cancel(OrderId id);
    [[nodiscard]] ReplaceResult replace(const ReplaceOrder& order);

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::optional<Price> spread() const noexcept;
    [[nodiscard]] std::optional<double> midprice() const noexcept;
    [[nodiscard]] std::optional<double> microprice() const noexcept;
    [[nodiscard]] double imbalance(std::size_t depth) const noexcept;
    [[nodiscard]] Quantity depth_at(Side side, Price price) const noexcept;
    [[nodiscard]] BookSnapshot snapshot(std::size_t depth) const;
    [[nodiscard]] InvariantReport check_invariants() const;

    [[nodiscard]] const Metrics& metrics() const noexcept { return metrics_; }
    [[nodiscard]] Sequence sequence() const noexcept { return sequence_; }
    [[nodiscard]] std::size_t live_orders() const noexcept { return order_index_.size(); }
    [[nodiscard]] std::size_t order_capacity() const noexcept { return orders_.capacity(); }

private:
    static constexpr std::uint32_t npos = UINT32_MAX;

    struct OrderNode {
        OrderId id{};
        Side side{};
        Price price{};
        Quantity remaining{};
        Sequence time{};
        std::uint32_t prev{npos};
        std::uint32_t next{npos};
        bool active{false};
    };

    struct PriceLevel {
        Price price{};
        Quantity total_quantity{};
        std::uint32_t order_count{};
        std::uint32_t head{npos};
        std::uint32_t tail{npos};
    };

    using LevelMap = std::map<Price, PriceLevel>;

    SubmitResult submit_limit(NewOrder order);
    SubmitResult submit_market(NewOrder order);
    bool can_fully_fill(const NewOrder& order) const;
    void match(NewOrder& taker, std::vector<Trade>& trades);
    bool crosses(const NewOrder& order) const noexcept;
    void rest_order(const NewOrder& order, Quantity remaining);
    void unlink_order(std::uint32_t index);
    void erase_level_if_empty(Side side, Price price);
    std::optional<std::uint32_t> allocate_order(const NewOrder& order, Quantity remaining);

    [[nodiscard]] LevelMap& levels(Side side) noexcept;
    [[nodiscard]] const LevelMap& levels(Side side) const noexcept;
    [[nodiscard]] PriceLevel* best_opposite_level(Side taker_side) noexcept;
    [[nodiscard]] const PriceLevel* find_level(Side side, Price price) const noexcept;

    OrderBookConfig config_;
    std::vector<OrderNode> orders_;
    LevelMap bids_;
    LevelMap asks_;
    std::unordered_map<OrderId, std::uint32_t> order_index_;
    Sequence sequence_{};
    Metrics metrics_{};
};

}  // namespace orderbook
