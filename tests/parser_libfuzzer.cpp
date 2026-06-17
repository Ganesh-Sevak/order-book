#include "orderbook/jsonl.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto* chars = reinterpret_cast<const char*>(data);
    (void)orderbook::parse_event_jsonl(std::string_view(chars, size));
    return 0;
}
