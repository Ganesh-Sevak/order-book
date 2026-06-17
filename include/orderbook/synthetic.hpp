#pragma once

#include "orderbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace orderbook {

std::vector<NewOrder> make_synthetic_orders(std::size_t count, std::uint64_t seed);

}  // namespace orderbook
