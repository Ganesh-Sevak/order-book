#include "orderbook/jsonl.hpp"

#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

std::string random_line(std::mt19937_64& rng) {
    std::uniform_int_distribution<int> len_dist(0, 256);
    std::uniform_int_distribution<int> char_dist(1, 126);
    std::string line;
    line.resize(static_cast<std::size_t>(len_dist(rng)));
    for (char& c : line) {
        c = static_cast<char>(char_dist(rng));
    }
    return line;
}

}  // namespace

int main() {
    std::vector<std::string> corpus = {
        "",
        "{",
        R"({"op":"add"})",
        R"({"op":"add","id":184467440737095516160,"side":"buy","price":1,"quantity":1})",
        R"({"op":"cancel","id":1,"extra":2})",
        R"({"op":"replace","old_id":1,"new_id":2,"price":100,"quantity":5})",
        R"({"op":"add","id":1,"side":"nope","price":1,"quantity":1})",
    };
    for (const auto& line : corpus) {
        (void)orderbook::parse_event_jsonl(line);
    }
    std::mt19937_64 rng(123);
    for (int i = 0; i < 20'000; ++i) {
        (void)orderbook::parse_event_jsonl(random_line(rng));
    }
    std::cout << "parser_fuzz: ok\n";
}
