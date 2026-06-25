#include "orderbook/order_book.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>

namespace orderbook {

namespace {

std::size_t tick_count(const OrderBookConfig& config) {
    if (config.min_price <= 0 || config.max_price < config.min_price) {
        throw std::invalid_argument("OrderBookConfig requires positive, ordered price bounds");
    }
    if (config.max_orders > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("max_orders exceeds the supported order-index range");
    }
    const auto width = static_cast<std::uint64_t>(config.max_price) - static_cast<std::uint64_t>(config.min_price) + 1U;
    if (width > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("price band exceeds addressable size");
    }
    return static_cast<std::size_t>(width);
}

bool is_valid_limit_price(const NewOrder& order, const OrderBookConfig& config) noexcept {
    return order.type == OrderType::Market ||
           (order.price >= config.min_price && order.price <= config.max_price);
}

ReplaceStatus map_submit_status(SubmitStatus status) noexcept {
    switch (status) {
        case SubmitStatus::Accepted: return ReplaceStatus::Replaced;
        case SubmitStatus::RejectedDuplicateId: return ReplaceStatus::RejectedDuplicateId;
        case SubmitStatus::RejectedInvalidPrice: return ReplaceStatus::RejectedInvalidPrice;
        case SubmitStatus::RejectedInvalidQuantity: return ReplaceStatus::RejectedInvalidQuantity;
        case SubmitStatus::RejectedCapacity: return ReplaceStatus::RejectedCapacity;
        case SubmitStatus::RejectedFokNotFillable: return ReplaceStatus::RejectedFokNotFillable;
    }
    return ReplaceStatus::RejectedCapacity;
}

void collect_trade(void* context, const Trade& trade) {
    static_cast<TradeList*>(context)->push_back(trade);
}

void emit_to_sink(void* context, const Trade& trade) noexcept {
    const auto* sink = static_cast<const TradeSink*>(context);
    sink->on_trade(sink->context, trade);
}

}  // namespace

const char* to_string(Side side) noexcept { return side == Side::Buy ? "buy" : "sell"; }
const char* to_string(OrderType type) noexcept { return type == OrderType::Limit ? "limit" : "market"; }

const char* to_string(TimeInForce tif) noexcept {
    switch (tif) {
        case TimeInForce::Gtc: return "gtc";
        case TimeInForce::Ioc: return "ioc";
        case TimeInForce::Fok: return "fok";
    }
    return "unknown";
}

const char* to_string(SubmitStatus status) noexcept {
    switch (status) {
        case SubmitStatus::Accepted: return "accepted";
        case SubmitStatus::RejectedDuplicateId: return "rejected_duplicate_id";
        case SubmitStatus::RejectedInvalidPrice: return "rejected_invalid_price";
        case SubmitStatus::RejectedInvalidQuantity: return "rejected_invalid_quantity";
        case SubmitStatus::RejectedCapacity: return "rejected_capacity";
        case SubmitStatus::RejectedFokNotFillable: return "rejected_fok_not_fillable";
    }
    return "unknown";
}

const char* to_string(CancelStatus status) noexcept {
    switch (status) {
        case CancelStatus::Canceled: return "canceled";
        case CancelStatus::UnknownOrder: return "unknown_order";
        case CancelStatus::AlreadyInactive: return "already_inactive";
    }
    return "unknown";
}

const char* to_string(ReplaceStatus status) noexcept {
    switch (status) {
        case ReplaceStatus::Replaced: return "replaced";
        case ReplaceStatus::Filled: return "filled";
        case ReplaceStatus::Canceled: return "canceled";
        case ReplaceStatus::UnknownOrder: return "unknown_order";
        case ReplaceStatus::RejectedInvalidPrice: return "rejected_invalid_price";
        case ReplaceStatus::RejectedInvalidQuantity: return "rejected_invalid_quantity";
        case ReplaceStatus::RejectedDuplicateId: return "rejected_duplicate_id";
        case ReplaceStatus::RejectedCapacity: return "rejected_capacity";
        case ReplaceStatus::RejectedFokNotFillable: return "rejected_fok_not_fillable";
    }
    return "unknown";
}

const char* to_string(ParseStatus status) noexcept {
    switch (status) {
        case ParseStatus::Ok: return "ok";
        case ParseStatus::EmptyLine: return "empty_line";
        case ParseStatus::Malformed: return "malformed";
        case ParseStatus::UnknownField: return "unknown_field";
        case ParseStatus::MissingField: return "missing_field";
        case ParseStatus::InvalidEnum: return "invalid_enum";
        case ParseStatus::InvalidNumber: return "invalid_number";
        case ParseStatus::Overflow: return "overflow";
        case ParseStatus::UnsupportedEscape: return "unsupported_escape";
    }
    return "unknown";
}

OrderBook::OccupancyBitmap::OccupancyBitmap(std::size_t bit_count) : bit_count_(bit_count) {
    std::size_t words = (bit_count + 63U) / 64U;
    layers_.emplace_back(words);
    while (words > 1) {
        words = (words + 63U) / 64U;
        layers_.emplace_back(words);
    }
}

void OrderBook::OccupancyBitmap::set(std::size_t bit) noexcept {
    for (std::size_t layer = 0;; ++layer) {
        const auto word_index = bit / 64U;
        const auto mask = std::uint64_t{1} << (bit % 64U);
        auto& word = layers_[layer][word_index];
        const auto was_zero = word == 0;
        word |= mask;
        if (!was_zero || layer + 1 == layers_.size()) return;
        bit = word_index;
    }
}

void OrderBook::OccupancyBitmap::clear(std::size_t bit) noexcept {
    for (std::size_t layer = 0;; ++layer) {
        const auto word_index = bit / 64U;
        const auto mask = std::uint64_t{1} << (bit % 64U);
        auto& word = layers_[layer][word_index];
        word &= ~mask;
        if (word != 0 || layer + 1 == layers_.size()) return;
        bit = word_index;
    }
}

std::optional<std::size_t> OrderBook::OccupancyBitmap::find_from(std::size_t layer, std::size_t bit) const noexcept {
    const auto& words = layers_[layer];
    auto word_index = bit / 64U;
    if (word_index >= words.size()) return std::nullopt;
    auto word = words[word_index] & (~std::uint64_t{0} << (bit % 64U));
    if (word == 0) {
        if (layer + 1 == layers_.size()) return std::nullopt;
        const auto next_word = find_from(layer + 1, word_index + 1U);
        if (!next_word) return std::nullopt;
        word_index = *next_word;
        word = words[word_index];
    }
    return word_index * 64U + std::countr_zero(word);
}

std::optional<std::size_t> OrderBook::OccupancyBitmap::find_before(std::size_t layer, std::size_t bit) const noexcept {
    const auto& words = layers_[layer];
    auto word_index = bit / 64U;
    if (word_index >= words.size()) word_index = words.size() - 1U;
    const auto offset = bit % 64U;
    const auto mask = offset == 63U ? ~std::uint64_t{0} : ((std::uint64_t{1} << (offset + 1U)) - 1U);
    auto word = words[word_index] & mask;
    if (word == 0) {
        if (word_index == 0 || layer + 1 == layers_.size()) return std::nullopt;
        const auto previous_word = find_before(layer + 1, word_index - 1U);
        if (!previous_word) return std::nullopt;
        word_index = *previous_word;
        word = words[word_index];
    }
    return word_index * 64U + (63U - std::countl_zero(word));
}

std::optional<std::size_t> OrderBook::OccupancyBitmap::first() const noexcept {
    const auto bit = find_from(0, 0);
    return bit && *bit < bit_count_ ? bit : std::nullopt;
}

std::optional<std::size_t> OrderBook::OccupancyBitmap::last() const noexcept {
    return bit_count_ == 0 ? std::nullopt : find_before(0, bit_count_ - 1U);
}

std::optional<std::size_t> OrderBook::OccupancyBitmap::next(std::size_t bit) const noexcept {
    return bit + 1U >= bit_count_ ? std::nullopt : find_from(0, bit + 1U);
}

std::optional<std::size_t> OrderBook::OccupancyBitmap::previous(std::size_t bit) const noexcept {
    return bit == 0 ? std::nullopt : find_before(0, bit - 1U);
}

OrderBook::BookSide::BookSide(Side side, std::size_t tick_count)
    : side_(side), levels_(tick_count), occupied_(tick_count) {}

void OrderBook::BookSide::mark_active(std::size_t index) noexcept {
    occupied_.set(index);
    ++active_levels_;
}

void OrderBook::BookSide::mark_inactive_if_empty(std::size_t index) noexcept {
    if (levels_[index].order_count == 0) {
        occupied_.clear(index);
        --active_levels_;
    }
}

std::optional<std::size_t> OrderBook::BookSide::best_index() const noexcept {
    return side_ == Side::Buy ? occupied_.last() : occupied_.first();
}

std::optional<std::size_t> OrderBook::BookSide::first_index() const noexcept { return occupied_.first(); }
std::optional<std::size_t> OrderBook::BookSide::next_index(std::size_t index) const noexcept { return occupied_.next(index); }
std::optional<std::size_t> OrderBook::BookSide::previous_index(std::size_t index) const noexcept { return occupied_.previous(index); }

std::uint64_t OrderBook::OrderIndex::mix(OrderId id) noexcept {
    id += 0x9e3779b97f4a7c15ULL;
    id = (id ^ (id >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    id = (id ^ (id >> 27U)) * 0x94d049bb133111ebULL;
    return id ^ (id >> 31U);
}

std::size_t OrderBook::OrderIndex::table_capacity(std::size_t max_entries) {
    if (max_entries > (std::numeric_limits<std::size_t>::max() / 2U)) {
        throw std::length_error("order index capacity overflow");
    }
    std::size_t capacity = 2;
    const auto required = std::max<std::size_t>(2, max_entries * 2U);
    while (capacity < required) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2U) {
            throw std::length_error("order index capacity overflow");
        }
        capacity *= 2U;
    }
    return capacity;
}

OrderBook::OrderIndex::OrderIndex(std::size_t max_entries)
    : entries_(table_capacity(max_entries)), mask_(entries_.size() - 1U) {}

std::size_t OrderBook::OrderIndex::slot_for(OrderId id) const noexcept {
    return static_cast<std::size_t>(mix(id)) & mask_;
}

std::optional<std::uint32_t> OrderBook::OrderIndex::find(OrderId id) const noexcept {
    auto slot = slot_for(id);
    while (entries_[slot].index != npos) {
        if (entries_[slot].id == id) return entries_[slot].index;
        slot = (slot + 1U) & mask_;
    }
    return std::nullopt;
}

bool OrderBook::OrderIndex::contains(OrderId id) const noexcept { return find(id).has_value(); }

void OrderBook::OrderIndex::insert(OrderId id, std::uint32_t index) noexcept {
    auto slot = slot_for(id);
    while (entries_[slot].index != npos) slot = (slot + 1U) & mask_;
    entries_[slot] = {.id = id, .index = index};
    ++size_;
}

void OrderBook::OrderIndex::erase(OrderId id) noexcept {
    auto slot = slot_for(id);
    while (entries_[slot].index != npos && entries_[slot].id != id) slot = (slot + 1U) & mask_;
    if (entries_[slot].index == npos) return;

    --size_;
    auto hole = slot;
    auto next = (slot + 1U) & mask_;
    while (entries_[next].index != npos) {
        const auto ideal = slot_for(entries_[next].id);
        const auto distance_to_hole = (hole - ideal) & mask_;
        const auto distance_to_next = (next - ideal) & mask_;
        if (distance_to_hole < distance_to_next) {
            entries_[hole] = entries_[next];
            hole = next;
        }
        next = (next + 1U) & mask_;
    }
    entries_[hole].index = npos;
}

OrderBook::OrderBook(OrderBookConfig config)
    : config_(config),
      bids_(Side::Buy, tick_count(config)),
      asks_(Side::Sell, tick_count(config)),
      order_index_(config.max_orders) {
    orders_.reserve(config_.max_orders);
    free_orders_.reserve(config_.max_orders);
}

SubmitResult OrderBook::submit(const NewOrder& order) {
    TradeList trades;
    const auto summary = submit_impl(order, {.context = &trades, .emit = collect_trade});
    return {.status = summary.status,
            .accepted_quantity = summary.accepted_quantity,
            .resting_quantity = summary.resting_quantity,
            .trades = std::move(trades)};
}

SubmitSummary OrderBook::submit(const NewOrder& order, TradeSink sink) {
    assert(sink.on_trade != nullptr);
    return submit_impl(order, {.context = &sink, .emit = emit_to_sink});
}

SubmitSummary OrderBook::submit_impl(NewOrder input, TradeEmitter emitter) {
    ++metrics_.submitted;
    if (input.quantity == 0) {
        ++metrics_.rejected;
        return {.status = SubmitStatus::RejectedInvalidQuantity};
    }
    if (!is_valid_limit_price(input, config_)) {
        ++metrics_.rejected;
        return {.status = SubmitStatus::RejectedInvalidPrice};
    }
    if (order_index_.contains(input.id)) {
        ++metrics_.rejected;
        return {.status = SubmitStatus::RejectedDuplicateId};
    }
    return input.type == OrderType::Market ? submit_market(input, emitter) : submit_limit(input, emitter);
}

SubmitSummary OrderBook::submit_market(NewOrder order, TradeEmitter emitter) {
    SubmitSummary result{.status = SubmitStatus::Accepted, .accepted_quantity = order.quantity};
    match(order, emitter, result.trade_count);
    ++metrics_.accepted;
    return result;
}

SubmitSummary OrderBook::submit_limit(NewOrder order, TradeEmitter emitter) {
    const bool fully_fillable = can_fully_fill(order);
    if (order.tif == TimeInForce::Fok && !fully_fillable) {
        ++metrics_.rejected;
        return {.status = SubmitStatus::RejectedFokNotFillable};
    }

    if (order.tif == TimeInForce::Gtc && !fully_fillable && !has_order_slot_capacity()) {
        ++metrics_.rejected;
        ++metrics_.capacity_rejections;
        return {.status = SubmitStatus::RejectedCapacity};
    }

    SubmitSummary result{.status = SubmitStatus::Accepted, .accepted_quantity = order.quantity};
    match(order, emitter, result.trade_count);
    if (order.quantity > 0 && order.tif == TimeInForce::Gtc) {
        if (!rest_order(order, order.quantity)) {
            ++metrics_.rejected;
            ++metrics_.capacity_rejections;
            result.status = SubmitStatus::RejectedCapacity;
            result.accepted_quantity -= order.quantity;
            return result;
        }
        result.resting_quantity = order.quantity;
    }
    ++metrics_.accepted;
    return result;
}

CancelResult OrderBook::cancel(OrderId id) {
    const auto found = order_index_.find(id);
    if (!found) return {.status = CancelStatus::UnknownOrder};
    const auto index = *found;
    if (index >= orders_.size() || !orders_[index].active) return {.status = CancelStatus::AlreadyInactive};
    const auto quantity = orders_[index].remaining;
    unlink_order(index);
    ++metrics_.canceled;
    return {.status = CancelStatus::Canceled, .canceled_quantity = quantity};
}

ReplaceResult OrderBook::replace(const ReplaceOrder& order) {
    TradeList trades;
    const auto summary = replace_impl(order, {.context = &trades, .emit = collect_trade});
    return {.status = summary.status, .resting_quantity = summary.resting_quantity, .trades = std::move(trades)};
}

ReplaceSummary OrderBook::replace(const ReplaceOrder& order, TradeSink sink) {
    assert(sink.on_trade != nullptr);
    return replace_impl(order, {.context = &sink, .emit = emit_to_sink});
}

ReplaceSummary OrderBook::replace_impl(const ReplaceOrder& order, TradeEmitter emitter) {
    const auto found = order_index_.find(order.old_id);
    if (!found) return {.status = ReplaceStatus::UnknownOrder};
    if (order.new_quantity == 0) return {.status = ReplaceStatus::RejectedInvalidQuantity};
    if (!is_valid_price(order.new_price)) return {.status = ReplaceStatus::RejectedInvalidPrice};
    if (order.new_id != order.old_id && order_index_.contains(order.new_id)) {
        return {.status = ReplaceStatus::RejectedDuplicateId};
    }

    const auto old_index = *found;
    const auto side = orders_[old_index].side;
    unlink_order(old_index);
    const SubmitSummary submitted = submit_impl({.id = order.new_id,
                                                  .side = side,
                                                  .price = order.new_price,
                                                  .quantity = order.new_quantity,
                                                  .type = OrderType::Limit,
                                                  .tif = order.tif},
                                                 emitter);
    ReplaceSummary result{.status = map_submit_status(submitted.status),
                          .resting_quantity = submitted.resting_quantity,
                          .trade_count = submitted.trade_count};
    if (submitted.status == SubmitStatus::Accepted) {
        ++metrics_.replaced;
        if (result.resting_quantity == 0 && result.trade_count != 0) result.status = ReplaceStatus::Filled;
    }
    return result;
}

std::optional<Price> OrderBook::best_bid() const noexcept {
    const auto index = bids_.best_index();
    return index ? std::optional<Price>(price_at(*index)) : std::nullopt;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    const auto index = asks_.best_index();
    return index ? std::optional<Price>(price_at(*index)) : std::nullopt;
}

std::optional<Price> OrderBook::spread() const noexcept {
    const auto bid = best_bid();
    const auto ask = best_ask();
    return bid && ask ? std::optional<Price>(*ask - *bid) : std::nullopt;
}

std::optional<double> OrderBook::midprice() const noexcept {
    const auto bid = best_bid();
    const auto ask = best_ask();
    return bid && ask ? std::optional<double>((static_cast<double>(*bid) + static_cast<double>(*ask)) / 2.0) : std::nullopt;
}

std::optional<double> OrderBook::microprice() const noexcept {
    const auto bid_index = bids_.best_index();
    const auto ask_index = asks_.best_index();
    if (!bid_index || !ask_index) return std::nullopt;
    const auto& bid = bids_.level(*bid_index);
    const auto& ask = asks_.level(*ask_index);
    const auto total = static_cast<double>(bid.total_quantity) + static_cast<double>(ask.total_quantity);
    if (total == 0.0) return std::nullopt;
    return (static_cast<double>(price_at(*ask_index)) * bid.total_quantity +
            static_cast<double>(price_at(*bid_index)) * ask.total_quantity) / total;
}

double OrderBook::imbalance(std::size_t depth) const noexcept {
    std::uint64_t bid_volume = 0;
    std::uint64_t ask_volume = 0;
    std::size_t seen = 0;
    for (auto index = bids_.best_index(); index && seen < depth; index = bids_.previous_index(*index), ++seen) {
        bid_volume += bids_.level(*index).total_quantity;
    }
    seen = 0;
    for (auto index = asks_.best_index(); index && seen < depth; index = asks_.next_index(*index), ++seen) {
        ask_volume += asks_.level(*index).total_quantity;
    }
    const auto total = bid_volume + ask_volume;
    return total == 0 ? 0.0 : (static_cast<double>(bid_volume) - static_cast<double>(ask_volume)) / static_cast<double>(total);
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
    for (auto index = bids_.best_index(); index && out.bids.size() < depth; index = bids_.previous_index(*index)) {
        const auto& level = bids_.level(*index);
        out.bids.push_back({price_at(*index), level.total_quantity, level.order_count});
    }
    for (auto index = asks_.best_index(); index && out.asks.size() < depth; index = asks_.next_index(*index)) {
        const auto& level = asks_.level(*index);
        out.asks.push_back({price_at(*index), level.total_quantity, level.order_count});
    }
    return out;
}

InvariantReport OrderBook::check_invariants() const {
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (bid && ask && *bid >= *ask) return {false, "resting book is crossed"};

    std::size_t active_seen = 0;
    for (const auto side : {Side::Buy, Side::Sell}) {
        const auto& book = side_book(side);
        std::size_t levels_seen = 0;
        for (auto level_index = book.first_index(); level_index; level_index = book.next_index(*level_index)) {
            const auto& level = book.level(*level_index);
            if (level.order_count == 0) return {false, "occupied bitmap contains an empty level"};
            const auto price = price_at(*level_index);
            std::uint64_t sum = 0;
            std::uint32_t count = 0;
            auto current = level.head;
            auto previous = npos;
            while (current != npos) {
                if (current >= orders_.size()) return {false, "order link out of range"};
                const auto& order = orders_[current];
                if (!order.active || order.side != side || order.price != price) return {false, "level contains wrong order"};
                if (order.prev != previous) return {false, "broken prev link"};
                const auto lookup = order_index_.find(order.id);
                if (!lookup || *lookup != current) return {false, "lookup does not point at active order"};
                sum += order.remaining;
                ++count;
                ++active_seen;
                previous = current;
                current = order.next;
            }
            if (previous != level.tail) return {false, "tail link mismatch"};
            if (sum != level.total_quantity || count != level.order_count) return {false, "level aggregate mismatch"};
            ++levels_seen;
        }
        if (levels_seen != book.size()) return {false, "occupied level count mismatch"};
    }
    return active_seen == order_index_.size() ? InvariantReport{} : InvariantReport{false, "lookup contains inactive orders"};
}

bool OrderBook::can_fully_fill(const NewOrder& order) const {
    Quantity remaining = order.quantity;
    const auto& opposite = side_book(order.side == Side::Buy ? Side::Sell : Side::Buy);
    if (order.side == Side::Buy) {
        for (auto index = opposite.first_index(); index; index = opposite.next_index(*index)) {
            if (order.type == OrderType::Limit && price_at(*index) > order.price) break;
            const auto quantity = opposite.level(*index).total_quantity;
            remaining = remaining > quantity ? remaining - quantity : 0;
            if (remaining == 0) return true;
        }
    } else {
        for (auto index = opposite.best_index(); index; index = opposite.previous_index(*index)) {
            if (order.type == OrderType::Limit && price_at(*index) < order.price) break;
            const auto quantity = opposite.level(*index).total_quantity;
            remaining = remaining > quantity ? remaining - quantity : 0;
            if (remaining == 0) return true;
        }
    }
    return remaining == 0;
}

void OrderBook::match(NewOrder& taker, TradeEmitter emitter, std::size_t& trade_count) {
    while (taker.quantity > 0 && crosses(taker)) {
        const auto level_index = best_opposite_index(taker.side);
        if (!level_index) break;
        auto& level = side_book(taker.side == Side::Buy ? Side::Sell : Side::Buy).level(*level_index);
        if (level.head == npos) break;
        const auto maker_index = level.head;
        auto& maker = orders_[maker_index];
        const auto fill = std::min(taker.quantity, maker.remaining);
        taker.quantity -= fill;
        maker.remaining -= fill;
        level.total_quantity -= fill;
        const Trade trade{.maker_id = maker.id,
                          .taker_id = taker.id,
                          .price = maker.price,
                          .quantity = fill,
                          .sequence = ++sequence_};
        ++metrics_.trades;
        ++trade_count;
        emitter.emit(emitter.context, trade);
        if (maker.remaining == 0) unlink_order(maker_index);
    }
}

bool OrderBook::crosses(const NewOrder& order) const noexcept {
    const auto index = best_opposite_index(order.side);
    if (!index) return false;
    if (order.type == OrderType::Market) return true;
    const auto opposite_price = price_at(*index);
    return order.side == Side::Buy ? order.price >= opposite_price : order.price <= opposite_price;
}

bool OrderBook::rest_order(const NewOrder& order, Quantity remaining) { return allocate_order(order, remaining).has_value(); }

std::optional<std::uint32_t> OrderBook::allocate_order(const NewOrder& order, Quantity remaining) {
    if (!has_order_slot_capacity()) return std::nullopt;
    const auto level_index = price_index(order.price);
    auto& side = side_book(order.side);
    auto& level = side.level(level_index);
    const auto was_empty = level.order_count == 0;

    const OrderNode node{.id = order.id,
                         .price = order.price,
                         .remaining = remaining,
                         .prev = npos,
                         .next = npos,
                         .side = order.side,
                         .active = true};
    std::uint32_t index = npos;
    if (!free_orders_.empty()) {
        index = free_orders_.back();
        free_orders_.pop_back();
        orders_[index] = node;
    } else {
        index = static_cast<std::uint32_t>(orders_.size());
        orders_.push_back(node);
    }
    if (level.tail != npos) {
        orders_[level.tail].next = index;
        orders_[index].prev = level.tail;
    } else {
        level.head = index;
    }
    level.tail = index;
    level.total_quantity += remaining;
    ++level.order_count;
    if (was_empty) side.mark_active(level_index);
    ++sequence_;
    order_index_.insert(order.id, index);
    return index;
}

void OrderBook::unlink_order(std::uint32_t index) {
    auto& order = orders_[index];
    auto& side = side_book(order.side);
    const auto level_index = price_index(order.price);
    auto& level = side.level(level_index);
    if (order.prev != npos) orders_[order.prev].next = order.next;
    else level.head = order.next;
    if (order.next != npos) orders_[order.next].prev = order.prev;
    else level.tail = order.prev;
    level.total_quantity -= order.remaining;
    --level.order_count;
    order.remaining = 0;
    order.prev = npos;
    order.next = npos;
    order.active = false;
    order_index_.erase(order.id);
    free_orders_.push_back(index);
    side.mark_inactive_if_empty(level_index);
}

bool OrderBook::has_order_slot_capacity() const noexcept {
    return !free_orders_.empty() || orders_.size() < orders_.capacity();
}

bool OrderBook::is_valid_price(Price price) const noexcept {
    return price >= config_.min_price && price <= config_.max_price;
}

std::size_t OrderBook::price_index(Price price) const noexcept {
    return static_cast<std::size_t>(static_cast<std::uint64_t>(price) - static_cast<std::uint64_t>(config_.min_price));
}

Price OrderBook::price_at(std::size_t index) const noexcept { return config_.min_price + static_cast<Price>(index); }

OrderBook::BookSide& OrderBook::side_book(Side side) noexcept { return side == Side::Buy ? bids_ : asks_; }
const OrderBook::BookSide& OrderBook::side_book(Side side) const noexcept { return side == Side::Buy ? bids_ : asks_; }

std::optional<std::size_t> OrderBook::best_opposite_index(Side taker_side) const noexcept {
    return side_book(taker_side == Side::Buy ? Side::Sell : Side::Buy).best_index();
}

const OrderBook::PriceLevel* OrderBook::find_level(Side side, Price price) const noexcept {
    if (!is_valid_price(price)) return nullptr;
    const auto& level = side_book(side).level(price_index(price));
    return level.order_count == 0 ? nullptr : &level;
}

}  // namespace orderbook
