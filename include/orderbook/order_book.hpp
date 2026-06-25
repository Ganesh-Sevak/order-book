#pragma once

#include "orderbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace orderbook {

struct OrderBookConfig {
    Price min_price{};
    Price max_price{};
    std::size_t max_orders{1'000'000};
};

struct InvariantReport {
    bool ok{true};
    std::string message;
};

class OrderBook {
public:
    explicit OrderBook(OrderBookConfig config);

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;

    [[nodiscard]] SubmitResult submit(const NewOrder& order);
    [[nodiscard]] SubmitSummary submit(const NewOrder& order, TradeSink sink);
    [[nodiscard]] CancelResult cancel(OrderId id);
    [[nodiscard]] ReplaceResult replace(const ReplaceOrder& order);
    [[nodiscard]] ReplaceSummary replace(const ReplaceOrder& order, TradeSink sink);

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
    [[nodiscard]] std::size_t reusable_slots() const noexcept { return free_orders_.size(); }

private:
    static constexpr std::uint32_t npos = UINT32_MAX;

    struct OrderNode {
        OrderId id{};
        Price price{};
        Quantity remaining{};
        std::uint32_t prev{npos};
        std::uint32_t next{npos};
        Side side{};
        bool active{false};
    };

    struct PriceLevel {
        Quantity total_quantity{};
        std::uint32_t order_count{};
        std::uint32_t head{npos};
        std::uint32_t tail{npos};
    };

    class OccupancyBitmap {
    public:
        explicit OccupancyBitmap(std::size_t bit_count);

        void set(std::size_t bit) noexcept;
        void clear(std::size_t bit) noexcept;
        [[nodiscard]] std::optional<std::size_t> first() const noexcept;
        [[nodiscard]] std::optional<std::size_t> last() const noexcept;
        [[nodiscard]] std::optional<std::size_t> next(std::size_t bit) const noexcept;
        [[nodiscard]] std::optional<std::size_t> previous(std::size_t bit) const noexcept;

    private:
        [[nodiscard]] std::optional<std::size_t> find_from(std::size_t layer, std::size_t bit) const noexcept;
        [[nodiscard]] std::optional<std::size_t> find_before(std::size_t layer, std::size_t bit) const noexcept;

        std::size_t bit_count_{};
        std::vector<std::vector<std::uint64_t>> layers_;
    };

    class BookSide {
    public:
        BookSide(Side side, std::size_t tick_count);

        [[nodiscard]] bool empty() const noexcept { return active_levels_ == 0; }
        [[nodiscard]] std::size_t size() const noexcept { return active_levels_; }
        [[nodiscard]] PriceLevel& level(std::size_t index) noexcept { return levels_[index]; }
        [[nodiscard]] const PriceLevel& level(std::size_t index) const noexcept { return levels_[index]; }
        void mark_active(std::size_t index) noexcept;
        void mark_inactive_if_empty(std::size_t index) noexcept;
        [[nodiscard]] std::optional<std::size_t> best_index() const noexcept;
        [[nodiscard]] std::optional<std::size_t> first_index() const noexcept;
        [[nodiscard]] std::optional<std::size_t> next_index(std::size_t index) const noexcept;
        [[nodiscard]] std::optional<std::size_t> previous_index(std::size_t index) const noexcept;

    private:
        Side side_;
        std::vector<PriceLevel> levels_;
        OccupancyBitmap occupied_;
        std::size_t active_levels_{};
    };

    class OrderIndex {
    public:
        explicit OrderIndex(std::size_t max_entries);

        [[nodiscard]] bool contains(OrderId id) const noexcept;
        [[nodiscard]] std::optional<std::uint32_t> find(OrderId id) const noexcept;
        void insert(OrderId id, std::uint32_t index) noexcept;
        void erase(OrderId id) noexcept;
        [[nodiscard]] std::size_t size() const noexcept { return size_; }

    private:
        struct Entry {
            OrderId id{};
            std::uint32_t index{npos};
        };

        [[nodiscard]] std::size_t slot_for(OrderId id) const noexcept;
        [[nodiscard]] static std::uint64_t mix(OrderId id) noexcept;
        [[nodiscard]] static std::size_t table_capacity(std::size_t max_entries);

        std::vector<Entry> entries_;
        std::size_t mask_{};
        std::size_t size_{};
    };

    struct TradeEmitter {
        void* context{};
        void (*emit)(void*, const Trade&){};
    };

    [[nodiscard]] SubmitSummary submit_impl(NewOrder order, TradeEmitter emitter);
    [[nodiscard]] SubmitSummary submit_limit(NewOrder order, TradeEmitter emitter);
    [[nodiscard]] SubmitSummary submit_market(NewOrder order, TradeEmitter emitter);
    [[nodiscard]] ReplaceSummary replace_impl(const ReplaceOrder& order, TradeEmitter emitter);
    bool can_fully_fill(const NewOrder& order) const;
    void match(NewOrder& taker, TradeEmitter emitter, std::size_t& trade_count);
    bool crosses(const NewOrder& order) const noexcept;
    bool rest_order(const NewOrder& order, Quantity remaining);
    void unlink_order(std::uint32_t index);
    [[nodiscard]] std::optional<std::uint32_t> allocate_order(const NewOrder& order, Quantity remaining);
    bool has_order_slot_capacity() const noexcept;
    [[nodiscard]] bool is_valid_price(Price price) const noexcept;
    [[nodiscard]] std::size_t price_index(Price price) const noexcept;
    [[nodiscard]] Price price_at(std::size_t index) const noexcept;

    [[nodiscard]] BookSide& side_book(Side side) noexcept;
    [[nodiscard]] const BookSide& side_book(Side side) const noexcept;
    [[nodiscard]] std::optional<std::size_t> best_opposite_index(Side taker_side) const noexcept;
    [[nodiscard]] const PriceLevel* find_level(Side side, Price price) const noexcept;

    OrderBookConfig config_;
    std::vector<OrderNode> orders_;
    std::vector<std::uint32_t> free_orders_;
    BookSide bids_;
    BookSide asks_;
    OrderIndex order_index_;
    Sequence sequence_{};
    Metrics metrics_{};
};

}  // namespace orderbook
