#include "orderbook/jsonl.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/synthetic.hpp"

#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <variant>

namespace {

using namespace orderbook;

struct Args {
    std::string mode;
    std::string input;
    std::string snapshots;
    std::string trades;
    std::size_t orders{100'000};
    std::uint64_t seed{42};
};

std::optional<std::string> value_after(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        return std::nullopt;
    }
    ++i;
    return argv[i];
}

Args parse_args(int argc, char** argv) {
    Args args;
    if (argc >= 2) {
        args.mode = argv[1];
    }
    for (int i = 2; i < argc; ++i) {
        const std::string key = argv[i];
        auto value = value_after(i, argc, argv);
        if (!value) {
            break;
        }
        if (key == "--input") args.input = *value;
        else if (key == "--snapshots") args.snapshots = *value;
        else if (key == "--trades") args.trades = *value;
        else if (key == "--orders") args.orders = static_cast<std::size_t>(std::stoull(*value));
        else if (key == "--seed") args.seed = static_cast<std::uint64_t>(std::stoull(*value));
    }
    return args;
}

void print_usage() {
    std::cerr << "usage:\n"
              << "  orderbook_demo synthetic --orders N --seed S [--snapshots file] [--trades file]\n"
              << "  orderbook_demo replay --input events.jsonl [--snapshots file] [--trades file]\n";
}

void write_trades(std::ostream* out, const TradeList& trades) {
    if (out == nullptr) {
        return;
    }
    for (const auto& trade : trades) {
        *out << to_jsonl(trade) << '\n';
    }
}

int run_events(const std::vector<NewOrder>& orders, const Args& args) {
    OrderBook book({.max_orders = std::max<std::size_t>(orders.size() + 1024, 4096)});
    std::ofstream snapshot_file;
    std::ofstream trade_file;
    if (!args.snapshots.empty()) snapshot_file.open(args.snapshots);
    if (!args.trades.empty()) trade_file.open(args.trades);

    for (std::size_t i = 0; i < orders.size(); ++i) {
        auto result = book.submit(orders[i]);
        write_trades(trade_file.is_open() ? &trade_file : nullptr, result.trades);
        if (snapshot_file.is_open() && i % 1000 == 0) {
            snapshot_file << to_jsonl(book.snapshot(10)) << '\n';
        }
    }
    if (snapshot_file.is_open()) {
        snapshot_file << to_jsonl(book.snapshot(10)) << '\n';
        snapshot_file << to_jsonl(book.metrics()) << '\n';
    }

    const auto invariant = book.check_invariants();
    std::cout << "{\"mode\":\"synthetic\",\"orders\":" << orders.size()
              << ",\"accepted\":" << book.metrics().accepted
              << ",\"rejected\":" << book.metrics().rejected
              << ",\"trades\":" << book.metrics().trades
              << ",\"live_orders\":" << book.live_orders()
              << ",\"invariants_ok\":" << (invariant.ok ? "true" : "false") << "}\n";
    return invariant.ok ? 0 : 2;
}

int run_replay(const Args& args) {
    if (args.input.empty()) {
        print_usage();
        return 1;
    }
    std::ifstream input(args.input);
    if (!input) {
        std::cerr << "{\"level\":\"error\",\"event\":\"open_failed\",\"path\":\"" << args.input << "\"}\n";
        return 1;
    }
    std::ofstream snapshot_file;
    std::ofstream trade_file;
    if (!args.snapshots.empty()) snapshot_file.open(args.snapshots);
    if (!args.trades.empty()) trade_file.open(args.trades);

    OrderBook book;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(input, line)) {
        ++line_no;
        auto parsed = parse_event_jsonl(line);
        if (parsed.status != ParseStatus::Ok) {
            std::cerr << "{\"level\":\"error\",\"event\":\"parse_failed\",\"line\":" << line_no
                      << ",\"status\":\"" << to_string(parsed.status) << "\"}\n";
            return 2;
        }
        std::visit(
            [&](const auto& event) {
                using T = std::decay_t<decltype(event)>;
                if constexpr (std::is_same_v<T, NewOrder>) {
                    auto result = book.submit(event);
                    write_trades(trade_file.is_open() ? &trade_file : nullptr, result.trades);
                } else if constexpr (std::is_same_v<T, CancelOrder>) {
                    (void)book.cancel(event.id);
                } else if constexpr (std::is_same_v<T, ReplaceOrder>) {
                    auto result = book.replace(event);
                    write_trades(trade_file.is_open() ? &trade_file : nullptr, result.trades);
                }
            },
            parsed.event);
        if (snapshot_file.is_open() && line_no % 1000 == 0) {
            snapshot_file << to_jsonl(book.snapshot(10)) << '\n';
        }
    }
    if (snapshot_file.is_open()) {
        snapshot_file << to_jsonl(book.snapshot(10)) << '\n';
        snapshot_file << to_jsonl(book.metrics()) << '\n';
    }
    const auto invariant = book.check_invariants();
    std::cout << "{\"mode\":\"replay\",\"lines\":" << line_no
              << ",\"accepted\":" << book.metrics().accepted
              << ",\"rejected\":" << book.metrics().rejected
              << ",\"trades\":" << book.metrics().trades
              << ",\"live_orders\":" << book.live_orders()
              << ",\"invariants_ok\":" << (invariant.ok ? "true" : "false") << "}\n";
    return invariant.ok ? 0 : 3;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    if (args.mode == "synthetic") {
        return run_events(orderbook::make_synthetic_orders(args.orders, args.seed), args);
    }
    if (args.mode == "replay") {
        return run_replay(args);
    }
    print_usage();
    return 1;
}
