#pragma once

#include <cstdint>
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
    Overflow
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
    std::vector<Trade> trades;
};

struct CancelResult {
    CancelStatus status{CancelStatus::UnknownOrder};
    Quantity canceled_quantity{};
};

struct ReplaceResult {
    ReplaceStatus status{ReplaceStatus::UnknownOrder};
    Quantity resting_quantity{};
    std::vector<Trade> trades;
};

struct Metrics {
    std::uint64_t submitted{};
    std::uint64_t accepted{};
    std::uint64_t rejected{};
    std::uint64_t canceled{};
    std::uint64_t replaced{};
    std::uint64_t trades{};
    std::uint64_t parser_failures{};
    std::uint64_t queue_drops{};
    std::uint64_t capacity_rejections{};
};

const char* to_string(Side side) noexcept;
const char* to_string(OrderType type) noexcept;
const char* to_string(TimeInForce tif) noexcept;
const char* to_string(SubmitStatus status) noexcept;
const char* to_string(CancelStatus status) noexcept;
const char* to_string(ReplaceStatus status) noexcept;
const char* to_string(ParseStatus status) noexcept;

}  // namespace orderbook
