#include "orderbook/jsonl.hpp"

#include <charconv>
#include <array>
#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

namespace orderbook {

namespace {

using Fields = std::unordered_map<std::string, std::string>;

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

bool consume(std::string_view& input, char expected) {
    input = trim(input);
    if (input.empty() || input.front() != expected) {
        return false;
    }
    input.remove_prefix(1);
    return true;
}

bool is_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

ParseStatus parse_string(std::string_view& input, std::string& out) {
    input = trim(input);
    if (input.empty() || input.front() != '"') {
        return ParseStatus::Malformed;
    }
    input.remove_prefix(1);
    out.clear();
    while (!input.empty()) {
        const char c = input.front();
        input.remove_prefix(1);
        if (c == '"') {
            return ParseStatus::Ok;
        }
        if (c == '\\') {
            if (input.empty()) {
                return ParseStatus::Malformed;
            }
            const char escaped = input.front();
            input.remove_prefix(1);
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(escaped);
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'u':
                    if (input.size() < 4 || !is_hex(input[0]) || !is_hex(input[1]) || !is_hex(input[2]) ||
                        !is_hex(input[3])) {
                        return ParseStatus::Malformed;
                    }
                    return ParseStatus::UnsupportedEscape;
                default:
                    return ParseStatus::UnsupportedEscape;
            }
        } else {
            out.push_back(c);
        }
    }
    return ParseStatus::Malformed;
}

ParseStatus parse_value(std::string_view& input, std::string& out) {
    input = trim(input);
    if (input.empty()) {
        return ParseStatus::Malformed;
    }
    if (input.front() == '"') {
        return parse_string(input, out);
    }
    const auto end = input.find_first_of(",}");
    if (end == std::string_view::npos) {
        out = std::string(trim(input));
        input = {};
        return ParseStatus::Ok;
    }
    out = std::string(trim(input.substr(0, end)));
    input.remove_prefix(end);
    return out.empty() ? ParseStatus::Malformed : ParseStatus::Ok;
}

ParseStatus parse_object(std::string_view line, Fields& fields) {
    line = trim(line);
    if (line.empty()) {
        return ParseStatus::EmptyLine;
    }
    if (!consume(line, '{')) {
        return ParseStatus::Malformed;
    }
    line = trim(line);
    if (!line.empty() && line.front() == '}') {
        line.remove_prefix(1);
        return trim(line).empty() ? ParseStatus::Ok : ParseStatus::Malformed;
    }
    while (true) {
        std::string key;
        std::string value;
        const auto key_status = parse_string(line, key);
        if (key_status != ParseStatus::Ok) {
            return key_status;
        }
        if (!consume(line, ':')) {
            return ParseStatus::Malformed;
        }
        const auto value_status = parse_value(line, value);
        if (value_status != ParseStatus::Ok) {
            return value_status;
        }
        fields.emplace(std::move(key), std::move(value));
        line = trim(line);
        if (line.empty()) {
            return ParseStatus::Malformed;
        }
        if (line.front() == '}') {
            line.remove_prefix(1);
            return trim(line).empty() ? ParseStatus::Ok : ParseStatus::Malformed;
        }
        if (line.front() != ',') {
            return ParseStatus::Malformed;
        }
        line.remove_prefix(1);
    }
}

template <typename T>
ParseStatus parse_unsigned(const Fields& fields, std::string_view key, T& out) {
    const auto it = fields.find(std::string(key));
    if (it == fields.end()) {
        return ParseStatus::MissingField;
    }
    if (it->second.empty() || it->second.front() == '-') {
        return ParseStatus::InvalidNumber;
    }
    unsigned long long parsed = 0;
    const auto* begin = it->second.data();
    const auto* end = begin + it->second.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec == std::errc::result_out_of_range) {
        return ParseStatus::Overflow;
    }
    if (ec != std::errc{} || ptr != end) {
        return ParseStatus::InvalidNumber;
    }
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<T>::max())) {
        return ParseStatus::Overflow;
    }
    out = static_cast<T>(parsed);
    return ParseStatus::Ok;
}

ParseStatus parse_price(const Fields& fields, Price& out) {
    const auto it = fields.find("price");
    if (it == fields.end()) {
        return ParseStatus::MissingField;
    }
    long long parsed = 0;
    const auto* begin = it->second.data();
    const auto* end = begin + it->second.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec == std::errc::result_out_of_range) {
        return ParseStatus::Overflow;
    }
    if (ec != std::errc{} || ptr != end) {
        return ParseStatus::InvalidNumber;
    }
    out = static_cast<Price>(parsed);
    return ParseStatus::Ok;
}

ParseStatus parse_side(const Fields& fields, Side& out) {
    const auto it = fields.find("side");
    if (it == fields.end()) {
        return ParseStatus::MissingField;
    }
    if (it->second == "buy") {
        out = Side::Buy;
        return ParseStatus::Ok;
    }
    if (it->second == "sell") {
        out = Side::Sell;
        return ParseStatus::Ok;
    }
    return ParseStatus::InvalidEnum;
}

ParseStatus parse_type(const Fields& fields, OrderType& out) {
    const auto it = fields.find("type");
    if (it == fields.end()) {
        out = OrderType::Limit;
        return ParseStatus::Ok;
    }
    if (it->second == "limit") {
        out = OrderType::Limit;
        return ParseStatus::Ok;
    }
    if (it->second == "market") {
        out = OrderType::Market;
        return ParseStatus::Ok;
    }
    return ParseStatus::InvalidEnum;
}

ParseStatus parse_tif(const Fields& fields, TimeInForce& out) {
    const auto it = fields.find("tif");
    if (it == fields.end()) {
        out = TimeInForce::Gtc;
        return ParseStatus::Ok;
    }
    if (it->second == "gtc") {
        out = TimeInForce::Gtc;
        return ParseStatus::Ok;
    }
    if (it->second == "ioc") {
        out = TimeInForce::Ioc;
        return ParseStatus::Ok;
    }
    if (it->second == "fok") {
        out = TimeInForce::Fok;
        return ParseStatus::Ok;
    }
    return ParseStatus::InvalidEnum;
}

template <std::size_t N>
bool has_only_fields(const Fields& fields, const std::array<std::string_view, N>& allowed) {
    for (const auto& [key, _] : fields) {
        if (!std::binary_search(allowed.begin(), allowed.end(), std::string_view(key))) {
            return false;
        }
    }
    return true;
}

}  // namespace

ParseResult parse_event_jsonl(std::string_view line) {
    Fields fields;
    const ParseStatus object_status = parse_object(line, fields);
    if (object_status != ParseStatus::Ok) {
        return {.status = object_status, .message = to_string(object_status)};
    }
    const auto op_it = fields.find("op");
    if (op_it == fields.end()) {
        return {.status = ParseStatus::MissingField, .message = "missing op"};
    }

    if (op_it->second == "add") {
        static constexpr std::array<std::string_view, 7> allowed{"id", "op", "price", "quantity", "side", "tif", "type"};
        if (!has_only_fields(fields, allowed)) {
            return {.status = ParseStatus::UnknownField, .message = "unknown add field"};
        }
        NewOrder order;
        if (auto status = parse_unsigned(fields, "id", order.id); status != ParseStatus::Ok) return {.status = status, .message = "bad id"};
        if (auto status = parse_side(fields, order.side); status != ParseStatus::Ok) return {.status = status, .message = "bad side"};
        if (auto status = parse_price(fields, order.price); status != ParseStatus::Ok) return {.status = status, .message = "bad price"};
        if (auto status = parse_unsigned(fields, "quantity", order.quantity); status != ParseStatus::Ok) return {.status = status, .message = "bad quantity"};
        if (auto status = parse_type(fields, order.type); status != ParseStatus::Ok) return {.status = status, .message = "bad type"};
        if (auto status = parse_tif(fields, order.tif); status != ParseStatus::Ok) return {.status = status, .message = "bad tif"};
        return {.status = ParseStatus::Ok, .event = order};
    }

    if (op_it->second == "cancel") {
        static constexpr std::array<std::string_view, 2> allowed{"id", "op"};
        if (!has_only_fields(fields, allowed)) {
            return {.status = ParseStatus::UnknownField, .message = "unknown cancel field"};
        }
        CancelOrder cancel;
        if (auto status = parse_unsigned(fields, "id", cancel.id); status != ParseStatus::Ok) return {.status = status, .message = "bad id"};
        return {.status = ParseStatus::Ok, .event = cancel};
    }

    if (op_it->second == "replace") {
        static constexpr std::array<std::string_view, 6> allowed{"new_id", "old_id", "op", "price", "quantity", "tif"};
        if (!has_only_fields(fields, allowed)) {
            return {.status = ParseStatus::UnknownField, .message = "unknown replace field"};
        }
        ReplaceOrder replace;
        if (auto status = parse_unsigned(fields, "old_id", replace.old_id); status != ParseStatus::Ok) return {.status = status, .message = "bad old_id"};
        if (auto status = parse_unsigned(fields, "new_id", replace.new_id); status != ParseStatus::Ok) return {.status = status, .message = "bad new_id"};
        if (auto status = parse_price(fields, replace.new_price); status != ParseStatus::Ok) return {.status = status, .message = "bad price"};
        if (auto status = parse_unsigned(fields, "quantity", replace.new_quantity); status != ParseStatus::Ok) return {.status = status, .message = "bad quantity"};
        if (auto status = parse_tif(fields, replace.tif); status != ParseStatus::Ok) return {.status = status, .message = "bad tif"};
        return {.status = ParseStatus::Ok, .event = replace};
    }

    return {.status = ParseStatus::InvalidEnum, .message = "bad op"};
}

std::string to_jsonl(const Trade& trade) {
    std::string out;
    out.reserve(128);
    out += "{\"type\":\"trade\",\"sequence\":";
    out += std::to_string(trade.sequence);
    out += ",\"maker_id\":";
    out += std::to_string(trade.maker_id);
    out += ",\"taker_id\":";
    out += std::to_string(trade.taker_id);
    out += ",\"price\":";
    out += std::to_string(trade.price);
    out += ",\"quantity\":";
    out += std::to_string(trade.quantity);
    out += "}";
    return out;
}

std::string to_jsonl(const BookSnapshot& snapshot) {
    std::string out;
    out.reserve(128 + (snapshot.bids.size() + snapshot.asks.size()) * 64);
    out += "{\"type\":\"snapshot\",\"sequence\":";
    out += std::to_string(snapshot.sequence);
    out += ",\"bids\":[";
    for (std::size_t i = 0; i < snapshot.bids.size(); ++i) {
        if (i != 0) out += ',';
        const auto& level = snapshot.bids[i];
        out += "{\"price\":";
        out += std::to_string(level.price);
        out += ",\"quantity\":";
        out += std::to_string(level.quantity);
        out += ",\"orders\":";
        out += std::to_string(level.order_count);
        out += "}";
    }
    out += "],\"asks\":[";
    for (std::size_t i = 0; i < snapshot.asks.size(); ++i) {
        if (i != 0) out += ',';
        const auto& level = snapshot.asks[i];
        out += "{\"price\":";
        out += std::to_string(level.price);
        out += ",\"quantity\":";
        out += std::to_string(level.quantity);
        out += ",\"orders\":";
        out += std::to_string(level.order_count);
        out += "}";
    }
    out += "]}";
    return out;
}

std::string to_jsonl(const Metrics& metrics) {
    std::string out;
    out.reserve(192);
    out += "{\"type\":\"metrics\",\"submitted\":";
    out += std::to_string(metrics.submitted);
    out += ",\"accepted\":";
    out += std::to_string(metrics.accepted);
    out += ",\"rejected\":";
    out += std::to_string(metrics.rejected);
    out += ",\"canceled\":";
    out += std::to_string(metrics.canceled);
    out += ",\"replaced\":";
    out += std::to_string(metrics.replaced);
    out += ",\"trades\":";
    out += std::to_string(metrics.trades);
    out += ",\"capacity_rejections\":";
    out += std::to_string(metrics.capacity_rejections);
    out += "}";
    return out;
}

}  // namespace orderbook
