#pragma once

#include "orderbook/types.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace orderbook {

using MarketEvent = std::variant<NewOrder, CancelOrder, ReplaceOrder>;

struct ParseResult {
    ParseStatus status{ParseStatus::Ok};
    MarketEvent event{CancelOrder{}};
    std::string message;
};

[[nodiscard]] ParseResult parse_event_jsonl(std::string_view line);
[[nodiscard]] std::string to_jsonl(const Trade& trade);
[[nodiscard]] std::string to_jsonl(const BookSnapshot& snapshot);
[[nodiscard]] std::string to_jsonl(const Metrics& metrics);

}  // namespace orderbook
