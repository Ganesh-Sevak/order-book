#include "orderbook/order_book.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>

namespace orderbook {

namespace {

bool is_valid_limit_price(const NewOrder& order) noexcept {
    return order.type == OrderType::Market || order.price > 0;
}

ReplaceStatus map_submit_status(SubmitStatus status) noexcept {
    switch (status) {
        case SubmitStatus::Accepted:
            return ReplaceStatus::Replaced;
        case SubmitStatus::RejectedDuplicateId:
            return ReplaceStatus::RejectedDuplicateId;
        case SubmitStatus::RejectedInvalidPrice:
            return ReplaceStatus::RejectedInvalidPrice;
        case SubmitStatus::RejectedInvalidQuantity:
            return ReplaceStatus::RejectedInvalidQuantity;
        case SubmitStatus::RejectedCapacity:
            return ReplaceStatus::RejectedCapacity;
        case SubmitStatus::RejectedFokNotFillable:
            return ReplaceStatus::RejectedFokNotFillable;
    }
    return ReplaceStatus::RejectedCapacity;
}

}  // namespace

const char* to_string(Side side) noexcept {
    return side == Side::Buy ? "buy" : "sell";
}

const char* to_string(OrderType type) noexcept {
    return type == OrderType::Limit ? "limit" : "market";
}

const char* to_string(TimeInForce tif) noexcept {
    switch (tif) {
        case TimeInForce::Gtc:
            return "gtc";
        case TimeInForce::Ioc:
            return "ioc";
        case TimeInForce::Fok:
            return "fok";
    }
    return "unknown";
}

const char* to_string(SubmitStatus status) noexcept {
    switch (status) {
        case SubmitStatus::Accepted:
            return "accepted";
        case SubmitStatus::RejectedDuplicateId:
            return "rejected_duplicate_id";
        case SubmitStatus::RejectedInvalidPrice:
            return "rejected_invalid_price";
        case SubmitStatus::RejectedInvalidQuantity:
            return "rejected_invalid_quantity";
        case SubmitStatus::RejectedCapacity:
            return "rejected_capacity";
        case SubmitStatus::RejectedFokNotFillable:
            return "rejected_fok_not_fillable";
    }
    return "unknown";
}

const char* to_string(CancelStatus status) noexcept {
    switch (status) {
        case CancelStatus::Canceled:
            return "canceled";
        case CancelStatus::UnknownOrder:
            return "unknown_order";
        case CancelStatus::AlreadyInactive:
            return "already_inactive";
    }
    return "unknown";
}

const char* to_string(ReplaceStatus status) noexcept {
    switch (status) {
        case ReplaceStatus::Replaced:
            return "replaced";
        case ReplaceStatus::Filled:
            return "filled";
        case ReplaceStatus::Canceled:
            return "canceled";
        case ReplaceStatus::UnknownOrder:
            return "unknown_order";
        case ReplaceStatus::RejectedInvalidPrice:
            return "rejected_invalid_price";
        case ReplaceStatus::RejectedInvalidQuantity:
            return "rejected_invalid_quantity";
        case ReplaceStatus::RejectedDuplicateId:
            return "rejected_duplicate_id";
        case ReplaceStatus::RejectedCapacity:
            return "rejected_capacity";
        case ReplaceStatus::RejectedFokNotFillable:
            return "rejected_fok_not_fillable";
    }
    return "unknown";
}

const char* to_string(ParseStatus status) noexcept {
    switch (status) {
        case ParseStatus::Ok:
            return "ok";
        case ParseStatus::EmptyLine:
            return "empty_line";
        case ParseStatus::Malformed:
            return "malformed";
        case ParseStatus::UnknownField:
            return "unknown_field";
        case ParseStatus::MissingField:
            return "missing_field";
        case ParseStatus::InvalidEnum:
            return "invalid_enum";
        case ParseStatus::InvalidNumber:
            return "invalid_number";
        case ParseStatus::Overflow:
            return "overflow";
    }
    return "unknown";
}

OrderBook::OrderBook(OrderBookConfig config) : config_(config) {
    orders_.reserve(config_.max_orders);
    free_orders_.reserve(config_.max_orders);
    order_index_.reserve(config_.max_orders);
}

std::vector<OrderBook::PriceLevel>::iterator OrderBook::BookSide::lower_bound(Price price) noexcept {
    return std::lower_bound(levels_.begin(), levels_.end(), price, [](const PriceLevel& level, Price value) {
        return level.price < value;
    });
}

std::vector<OrderBook::PriceLevel>::const_iterator OrderBook::BookSide::lower_bound(Price price) const noexcept {
    return std::lower_bound(levels_.begin(), levels_.end(), price, [](const PriceLevel& level, Price value) {
        return level.price < value;
    });
}

OrderBook::PriceLevel* OrderBook::BookSide::find(Price price) noexcept {
    auto it = lower_bound(price);
    if (it == levels_.end() || it->price != price) {
        return nullptr;
    }
    return &*it;
}

const OrderBook::PriceLevel* OrderBook::BookSide::find(Price price) const noexcept {
    auto it = lower_bound(price);
    if (it == levels_.end() || it->price != price) {
        return nullptr;
    }
    return &*it;
}

OrderBook::PriceLevel* OrderBook::BookSide::find_or_insert(Price price) {
    auto it = lower_bound(price);
    if (it == levels_.end() || it->price != price) {
        it = levels_.insert(it, PriceLevel{.price = price});
    }
    return &*it;
}

void OrderBook::BookSide::erase_if_empty(Price price) {
    auto it = lower_bound(price);
    if (it != levels_.end() && it->price == price && it->order_count == 0) {
        levels_.erase(it);
    }
}

OrderBook::PriceLevel* OrderBook::BookSide::best() noexcept {
    if (levels_.empty()) {
        return nullptr;
    }
    return side_ == Side::Buy ? &levels_.back() : &levels_.front();
}

const OrderBook::PriceLevel* OrderBook::BookSide::best() const noexcept {
    if (levels_.empty()) {
        return nullptr;
    }
    return side_ == Side::Buy ? &levels_.back() : &levels_.front();
}

SubmitResult OrderBook::submit(const NewOrder& input) {
    ++metrics_.submitted;
    if (input.quantity == 0) {
        ++metrics_.rejected;
        return {.status = SubmitStatus::RejectedInvalidQuantity};
    }
    if (!is_valid_limit_price(input)) {
        ++metrics_.rejected;
        return {.status = SubmitStatus::RejectedInvalidPrice};
    }
    if (order_index_.contains(input.id)) {
        ++metrics_.rejected;
        return {.status = SubmitStatus::RejectedDuplicateId};
    }
    if (input.type == OrderType::Market) {
        return submit_market(input);
    }
    return submit_limit(input);
}

SubmitResult OrderBook::submit_market(NewOrder order) {
    SubmitResult result{.status = SubmitStatus::Accepted, .accepted_quantity = order.quantity};
    match(order, result.trades);
    result.resting_quantity = 0;
    ++metrics_.accepted;
    return result;
}

SubmitResult OrderBook::submit_limit(NewOrder order) {
    const bool fully_fillable = can_fully_fill(order);
    if (order.tif == TimeInForce::Fok && !fully_fillable) {
        ++metrics_.rejected;
        return {.status = SubmitStatus::RejectedFokNotFillable};
    }

    const bool may_rest = order.tif == TimeInForce::Gtc && !fully_fillable;
    if (may_rest) {
        const auto& own_levels = side_book(order.side);
        if (!has_order_slot_capacity()) {
            ++metrics_.rejected;
            ++metrics_.capacity_rejections;
            return {.status = SubmitStatus::RejectedCapacity};
        }
        if (!own_levels.contains(order.price) && own_levels.size() >= config_.max_price_levels) {
            ++metrics_.rejected;
            ++metrics_.capacity_rejections;
            return {.status = SubmitStatus::RejectedCapacity};
        }
    }

    SubmitResult result{.status = SubmitStatus::Accepted, .accepted_quantity = order.quantity};
    match(order, result.trades);
    if (order.quantity > 0 && order.tif == TimeInForce::Gtc) {
        if (!rest_order(order, order.quantity)) {
            ++metrics_.rejected;
            ++metrics_.capacity_rejections;
            result.status = SubmitStatus::RejectedCapacity;
            result.accepted_quantity -= order.quantity;
            result.resting_quantity = 0;
            return result;
        }
        result.resting_quantity = order.quantity;
    }
    ++metrics_.accepted;
    return result;
}

CancelResult OrderBook::cancel(OrderId id) {
    const auto found = order_index_.find(id);
    if (found == order_index_.end()) {
        return {.status = CancelStatus::UnknownOrder};
    }
    const auto index = found->second;
    if (index >= orders_.size() || !orders_[index].active) {
        return {.status = CancelStatus::AlreadyInactive};
    }
    const auto quantity = orders_[index].remaining;
    unlink_order(index);
    ++metrics_.canceled;
    return {.status = CancelStatus::Canceled, .canceled_quantity = quantity};
}

ReplaceResult OrderBook::replace(const ReplaceOrder& order) {
    const auto found = order_index_.find(order.old_id);
    if (found == order_index_.end()) {
        return {.status = ReplaceStatus::UnknownOrder};
    }
    if (order.new_quantity == 0) {
        return {.status = ReplaceStatus::RejectedInvalidQuantity};
    }
    if (order.new_price <= 0) {
        return {.status = ReplaceStatus::RejectedInvalidPrice};
    }
    if (order.new_id != order.old_id && order_index_.contains(order.new_id)) {
        return {.status = ReplaceStatus::RejectedDuplicateId};
    }

    const auto old_index = found->second;
    const auto side = orders_[old_index].side;
    unlink_order(old_index);

    NewOrder replacement{
        .id = order.new_id,
        .side = side,
        .price = order.new_price,
        .quantity = order.new_quantity,
        .type = OrderType::Limit,
        .tif = order.tif,
    };
    SubmitResult submitted = submit(replacement);
    ReplaceResult result{
        .status = map_submit_status(submitted.status),
        .resting_quantity = submitted.resting_quantity,
        .trades = std::move(submitted.trades),
    };
    if (submitted.status == SubmitStatus::Accepted) {
        ++metrics_.replaced;
        if (result.resting_quantity == 0 && !result.trades.empty()) {
            result.status = ReplaceStatus::Filled;
        }
    }
    return result;
}

std::optional<Price> OrderBook::best_bid() const noexcept {
    const auto* level = bids_.best();
    if (level == nullptr) {
        return std::nullopt;
    }
    return level->price;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    const auto* level = asks_.best();
    if (level == nullptr) {
        return std::nullopt;
    }
    return level->price;
}

std::optional<Price> OrderBook::spread() const noexcept {
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return *ask - *bid;
}

std::optional<double> OrderBook::midprice() const noexcept {
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return (static_cast<double>(*bid) + static_cast<double>(*ask)) / 2.0;
}

std::optional<double> OrderBook::microprice() const noexcept {
    const auto* bid = bids_.best();
    const auto* ask = asks_.best();
    if (bid == nullptr || ask == nullptr) {
        return std::nullopt;
    }
    const auto total = static_cast<double>(bid->total_quantity) + static_cast<double>(ask->total_quantity);
    if (total == 0.0) {
        return std::nullopt;
    }
    return (static_cast<double>(ask->price) * bid->total_quantity +
            static_cast<double>(bid->price) * ask->total_quantity) /
           total;
}

double OrderBook::imbalance(std::size_t depth) const noexcept {
    std::uint64_t bid_volume = 0;
    std::uint64_t ask_volume = 0;
    std::size_t seen = 0;
    const auto& bid_levels = bids_.levels();
    for (auto it = bid_levels.rbegin(); it != bid_levels.rend() && seen < depth; ++it, ++seen) {
        bid_volume += it->total_quantity;
    }
    seen = 0;
    const auto& ask_levels = asks_.levels();
    for (auto it = ask_levels.begin(); it != ask_levels.end() && seen < depth; ++it, ++seen) {
        ask_volume += it->total_quantity;
    }
    const auto total = bid_volume + ask_volume;
    if (total == 0) {
        return 0.0;
    }
    return (static_cast<double>(bid_volume) - static_cast<double>(ask_volume)) / static_cast<double>(total);
}

Quantity OrderBook::depth_at(Side side, Price price) const noexcept {
    const auto* level = find_level(side, price);
    return level == nullptr ? 0 : level->total_quantity;
}

BookSnapshot OrderBook::snapshot(std::size_t depth) const {
    BookSnapshot out;
    out.sequence = sequence_;
    out.bids.reserve(depth);
    out.asks.reserve(depth);
    std::size_t seen = 0;
    const auto& bid_levels = bids_.levels();
    for (auto it = bid_levels.rbegin(); it != bid_levels.rend() && seen < depth; ++it, ++seen) {
        out.bids.push_back({it->price, it->total_quantity, it->order_count});
    }
    seen = 0;
    const auto& ask_levels = asks_.levels();
    for (auto it = ask_levels.begin(); it != ask_levels.end() && seen < depth; ++it, ++seen) {
        out.asks.push_back({it->price, it->total_quantity, it->order_count});
    }
    return out;
}

InvariantReport OrderBook::check_invariants() const {
    const auto* best_bid_level = bids_.best();
    const auto* best_ask_level = asks_.best();
    if (best_bid_level != nullptr && best_ask_level != nullptr && best_bid_level->price >= best_ask_level->price) {
        return {false, "resting book is crossed"};
    }

    std::size_t active_seen = 0;
    for (const auto side : {Side::Buy, Side::Sell}) {
        const auto& level_vector = side_book(side).levels();
        for (const auto& level : level_vector) {
            const auto price = level.price;
            std::uint64_t sum = 0;
            std::uint32_t count = 0;
            auto current = level.head;
            auto prev = npos;
            while (current != npos) {
                if (current >= orders_.size()) {
                    return {false, "order link out of range"};
                }
                const auto& order = orders_[current];
                if (!order.active || order.side != side || order.price != price) {
                    return {false, "level contains wrong order"};
                }
                if (order.prev != prev) {
                    return {false, "broken prev link"};
                }
                const auto lookup = order_index_.find(order.id);
                if (lookup == order_index_.end() || lookup->second != current) {
                    return {false, "lookup does not point at active order"};
                }
                sum += order.remaining;
                ++count;
                ++active_seen;
                prev = current;
                current = order.next;
            }
            if (prev != level.tail) {
                return {false, "tail link mismatch"};
            }
            if (sum != level.total_quantity || count != level.order_count) {
                return {false, "level aggregate mismatch"};
            }
        }
    }
    if (active_seen != order_index_.size()) {
        return {false, "lookup contains inactive orders"};
    }
    return {};
}

bool OrderBook::can_fully_fill(const NewOrder& order) const {
    Quantity remaining = order.quantity;
    const auto& opposite = side_book(order.side == Side::Buy ? Side::Sell : Side::Buy).levels();
    if (order.side == Side::Buy) {
        for (const auto& level : opposite) {
            if (order.type == OrderType::Limit && level.price > order.price) {
                break;
            }
            remaining = remaining > level.total_quantity ? remaining - level.total_quantity : 0;
            if (remaining == 0) {
                return true;
            }
        }
    } else {
        for (auto it = opposite.rbegin(); it != opposite.rend(); ++it) {
            if (order.type == OrderType::Limit && it->price < order.price) {
                break;
            }
            remaining = remaining > it->total_quantity ? remaining - it->total_quantity : 0;
            if (remaining == 0) {
                return true;
            }
        }
    }
    return remaining == 0;
}

void OrderBook::match(NewOrder& taker, TradeList& trades) {
    while (taker.quantity > 0 && crosses(taker)) {
        PriceLevel* level = best_opposite_level(taker.side);
        if (level == nullptr || level->head == npos) {
            break;
        }
        const auto maker_index = level->head;
        auto& maker = orders_[maker_index];
        const auto fill = std::min(taker.quantity, maker.remaining);
        taker.quantity -= fill;
        maker.remaining -= fill;
        level->total_quantity -= fill;
        ++sequence_;
        ++metrics_.trades;
        trades.push_back({
            .maker_id = maker.id,
            .taker_id = taker.id,
            .price = maker.price,
            .quantity = fill,
            .sequence = sequence_,
        });
        if (maker.remaining == 0) {
            unlink_order(maker_index);
        }
    }
}

bool OrderBook::crosses(const NewOrder& order) const noexcept {
    const auto* opposite = side_book(order.side == Side::Buy ? Side::Sell : Side::Buy).best();
    if (opposite == nullptr) {
        return false;
    }
    if (order.type == OrderType::Market) {
        return true;
    }
    return order.side == Side::Buy ? order.price >= opposite->price : order.price <= opposite->price;
}

bool OrderBook::rest_order(const NewOrder& order, Quantity remaining) {
    auto allocated = allocate_order(order, remaining);
    if (!allocated) {
        return false;
    }
    return true;
}

std::optional<std::uint32_t> OrderBook::allocate_order(const NewOrder& order, Quantity remaining) {
    if (!has_order_slot_capacity()) {
        return std::nullopt;
    }
    auto& side_levels = side_book(order.side);
    const bool new_level = !side_levels.contains(order.price);
    if (new_level && side_levels.size() >= config_.max_price_levels) {
        return std::nullopt;
    }
    auto* level = side_levels.find_or_insert(order.price);

    OrderNode node{
        .id = order.id,
        .side = order.side,
        .price = order.price,
        .remaining = remaining,
        .time = ++sequence_,
        .active = true,
    };
    std::uint32_t index = npos;
    if (!free_orders_.empty()) {
        index = free_orders_.back();
        free_orders_.pop_back();
        orders_[index] = node;
    } else {
        index = static_cast<std::uint32_t>(orders_.size());
        orders_.push_back(node);
    }

    if (level->tail != npos) {
        orders_[level->tail].next = index;
        orders_[index].prev = level->tail;
    } else {
        level->head = index;
    }
    level->tail = index;
    level->total_quantity += remaining;
    ++level->order_count;
    order_index_.emplace(order.id, index);
    return index;
}

void OrderBook::unlink_order(std::uint32_t index) {
    auto& order = orders_[index];
    auto& side_levels = side_book(order.side);
    auto* level = side_levels.find(order.price);
    if (level == nullptr) {
        order.active = false;
        order_index_.erase(order.id);
        free_orders_.push_back(index);
        return;
    }
    if (order.prev != npos) {
        orders_[order.prev].next = order.next;
    } else {
        level->head = order.next;
    }
    if (order.next != npos) {
        orders_[order.next].prev = order.prev;
    } else {
        level->tail = order.prev;
    }
    level->total_quantity -= order.remaining;
    --level->order_count;
    const auto price = order.price;
    const auto side = order.side;
    order.remaining = 0;
    order.prev = npos;
    order.next = npos;
    order.active = false;
    order_index_.erase(order.id);
    free_orders_.push_back(index);
    erase_level_if_empty(side, price);
}

void OrderBook::erase_level_if_empty(Side side, Price price) {
    side_book(side).erase_if_empty(price);
}

bool OrderBook::has_order_slot_capacity() const noexcept {
    return !free_orders_.empty() || orders_.size() < orders_.capacity();
}

OrderBook::BookSide& OrderBook::side_book(Side side) noexcept {
    return side == Side::Buy ? bids_ : asks_;
}

const OrderBook::BookSide& OrderBook::side_book(Side side) const noexcept {
    return side == Side::Buy ? bids_ : asks_;
}

OrderBook::PriceLevel* OrderBook::best_opposite_level(Side taker_side) noexcept {
    return side_book(taker_side == Side::Buy ? Side::Sell : Side::Buy).best();
}

const OrderBook::PriceLevel* OrderBook::find_level(Side side, Price price) const noexcept {
    return side_book(side).find(price);
}

}  // namespace orderbook
