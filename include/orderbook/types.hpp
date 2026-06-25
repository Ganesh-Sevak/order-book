#pragma once

#include <cstdint>
#include <array>
#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

namespace orderbook {

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint32_t;
using Sequence = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };
enum class TimeInForce : std::uint8_t { Gtc, Ioc, Fok };

enum class SubmitStatus : std::uint8_t {
    Accepted,
    RejectedDuplicateId,
    RejectedInvalidPrice,
    RejectedInvalidQuantity,
    RejectedCapacity,
    RejectedFokNotFillable
};

enum class CancelStatus : std::uint8_t {
    Canceled,
    UnknownOrder,
    AlreadyInactive
};

enum class ReplaceStatus : std::uint8_t {
    Replaced,
    Filled,
    Canceled,
    UnknownOrder,
    RejectedInvalidPrice,
    RejectedInvalidQuantity,
    RejectedDuplicateId,
    RejectedCapacity,
    RejectedFokNotFillable
};

enum class ParseStatus : std::uint8_t {
    Ok,
    EmptyLine,
    Malformed,
    UnknownField,
    MissingField,
    InvalidEnum,
    InvalidNumber,
    Overflow,
    UnsupportedEscape
};

struct NewOrder {
    OrderId id{};
    Side side{};
    Price price{};
    Quantity quantity{};
    OrderType type{OrderType::Limit};
    TimeInForce tif{TimeInForce::Gtc};
};

struct CancelOrder {
    OrderId id{};
};

struct ReplaceOrder {
    OrderId old_id{};
    OrderId new_id{};
    Price new_price{};
    Quantity new_quantity{};
    TimeInForce tif{TimeInForce::Gtc};
};

struct Trade {
    OrderId maker_id{};
    OrderId taker_id{};
    Price price{};
    Quantity quantity{};
    Sequence sequence{};
};

struct TradeSink {
    void* context{};
    void (*on_trade)(void*, const Trade&) noexcept{};
};

class TradeList {
public:
    using Spill = std::vector<Trade>;

    void push_back(const Trade& trade) {
        if (inline_size_ < inline_.size()) {
            inline_[inline_size_++] = trade;
            return;
        }
        if (spill_.empty()) {
            spill_.reserve(inline_.size() * 2);
            for (std::size_t i = 0; i < inline_size_; ++i) {
                spill_.push_back(inline_[i]);
            }
        }
        spill_.push_back(trade);
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return spill_.empty() ? inline_size_ : spill_.size(); }
    [[nodiscard]] const Trade& operator[](std::size_t index) const noexcept {
        return spill_.empty() ? inline_[index] : spill_[index];
    }

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Trade;
        using difference_type = std::ptrdiff_t;
        using pointer = const Trade*;
        using reference = const Trade&;

        const_iterator(const TradeList* list, std::size_t index) : list_(list), index_(index) {}
        reference operator*() const noexcept { return (*list_)[index_]; }
        pointer operator->() const noexcept { return &(*list_)[index_]; }
        const_iterator& operator++() noexcept {
            ++index_;
            return *this;
        }
        bool operator==(const const_iterator& other) const noexcept {
            return list_ == other.list_ && index_ == other.index_;
        }

    private:
        const TradeList* list_{};
        std::size_t index_{};
    };

    [[nodiscard]] const_iterator begin() const noexcept { return {this, 0}; }
    [[nodiscard]] const_iterator end() const noexcept { return {this, size()}; }

private:
    std::array<Trade, 4> inline_{};
    std::size_t inline_size_{};
    Spill spill_;
};

struct PriceLevelSnapshot {
    Price price{};
    Quantity quantity{};
    std::uint32_t order_count{};
};

struct BookSnapshot {
    std::vector<PriceLevelSnapshot> bids;
    std::vector<PriceLevelSnapshot> asks;
    Sequence sequence{};
};

struct SubmitResult {
    SubmitStatus status{SubmitStatus::Accepted};
    Quantity accepted_quantity{};
    Quantity resting_quantity{};
    TradeList trades;
};

struct SubmitSummary {
    SubmitStatus status{SubmitStatus::Accepted};
    Quantity accepted_quantity{};
    Quantity resting_quantity{};
    std::size_t trade_count{};
};

struct CancelResult {
    CancelStatus status{CancelStatus::UnknownOrder};
    Quantity canceled_quantity{};
};

struct ReplaceResult {
    ReplaceStatus status{ReplaceStatus::UnknownOrder};
    Quantity resting_quantity{};
    TradeList trades;
};

struct ReplaceSummary {
    ReplaceStatus status{ReplaceStatus::UnknownOrder};
    Quantity resting_quantity{};
    std::size_t trade_count{};
};

struct Metrics {
    std::uint64_t submitted{};
    std::uint64_t accepted{};
    std::uint64_t rejected{};
    std::uint64_t canceled{};
    std::uint64_t replaced{};
    std::uint64_t trades{};
    std::uint64_t capacity_rejections{};
};

struct PipelineMetrics {
    std::uint64_t parser_failures{};
    std::uint64_t queue_drops{};
    std::uint64_t backpressure_events{};
};

const char* to_string(Side side) noexcept;
const char* to_string(OrderType type) noexcept;
const char* to_string(TimeInForce tif) noexcept;
const char* to_string(SubmitStatus status) noexcept;
const char* to_string(CancelStatus status) noexcept;
const char* to_string(ReplaceStatus status) noexcept;
const char* to_string(ParseStatus status) noexcept;

}  // namespace orderbook
