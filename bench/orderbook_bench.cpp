#include "orderbook/order_book.hpp"
#include "orderbook/spsc_ring.hpp"
#include "orderbook/synthetic.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace orderbook;
using Clock = std::chrono::steady_clock;

struct Args {
    std::size_t orders{1'000'000};
    std::uint64_t seed{42};
};

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--orders") args.orders = static_cast<std::size_t>(std::stoull(value));
        else if (key == "--seed") args.seed = static_cast<std::uint64_t>(std::stoull(value));
    }
    return args;
}

std::uint64_t percentile(std::vector<std::uint64_t>& values, double p) {
    if (values.empty()) return 0;
    const auto index = static_cast<std::size_t>((values.size() - 1) * p);
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

std::uint64_t calibrate_clock_overhead() {
    std::vector<std::uint64_t> samples;
    samples.reserve(100'000);
    for (int i = 0; i < 100'000; ++i) {
        const auto before = Clock::now();
        const auto after = Clock::now();
        samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
    }
    return percentile(samples, 0.50);
}

std::vector<std::uint64_t> corrected_latencies(const std::vector<std::uint64_t>& raw, std::uint64_t clock_overhead_ns) {
    std::vector<std::uint64_t> corrected;
    corrected.reserve(raw.size());
    for (const auto value : raw) {
        corrected.push_back(value > clock_overhead_ns ? value - clock_overhead_ns : 0);
    }
    return corrected;
}

void print_result(const char* name,
                  std::size_t events,
                  std::chrono::nanoseconds elapsed,
                  const std::vector<std::uint64_t>& latencies,
                  std::uint64_t clock_overhead_ns) {
    auto raw_p50_values = latencies;
    auto raw_p99_values = latencies;
    auto raw_p999_values = latencies;
    auto corrected = corrected_latencies(latencies, clock_overhead_ns);
    auto corrected_p50_values = corrected;
    auto corrected_p99_values = corrected;
    auto corrected_p999_values = corrected;
    const double seconds = static_cast<double>(elapsed.count()) / 1'000'000'000.0;
    const double throughput = seconds == 0.0 ? 0.0 : static_cast<double>(events) / seconds;
    const auto batch_per_op = events == 0 ? 0 : static_cast<std::uint64_t>(elapsed.count() / events);
    std::cout << "{\"scenario\":\"" << name << "\",\"events\":" << events
              << ",\"elapsed_ns\":" << elapsed.count()
              << ",\"batch_per_op_ns\":" << batch_per_op
              << ",\"throughput_per_sec\":" << static_cast<std::uint64_t>(throughput)
              << ",\"clock_overhead_ns\":" << clock_overhead_ns
              << ",\"raw_p50_ns\":" << percentile(raw_p50_values, 0.50)
              << ",\"raw_p99_ns\":" << percentile(raw_p99_values, 0.99)
              << ",\"raw_p999_ns\":" << percentile(raw_p999_values, 0.999)
              << ",\"corrected_p50_ns\":" << percentile(corrected_p50_values, 0.50)
              << ",\"corrected_p99_ns\":" << percentile(corrected_p99_values, 0.99)
              << ",\"corrected_p999_ns\":" << percentile(corrected_p999_values, 0.999) << "}\n";
}

void bench_submit(const char* name, const std::vector<NewOrder>& orders, std::uint64_t clock_overhead_ns) {
    OrderBook book({.max_orders = orders.size() + 1024, .max_price_levels = 4096});
    std::vector<std::uint64_t> latencies;
    latencies.reserve(orders.size());
    const auto start = Clock::now();
    for (const auto& order : orders) {
        const auto before = Clock::now();
        (void)book.submit(order);
        const auto after = Clock::now();
        latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
    print_result(name, orders.size(), elapsed, latencies, clock_overhead_ns);
}

void bench_mixed(std::size_t count, std::uint64_t clock_overhead_ns) {
    OrderBook book({.max_orders = count + 1024, .max_price_levels = 4096});
    std::vector<OrderId> live;
    live.reserve(count);
    std::vector<std::uint64_t> latencies;
    latencies.reserve(count);
    OrderId next_id = 1;

    const auto start = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const auto before = Clock::now();
        if (live.empty() || i % 10 < 6) {
            NewOrder order{
                .id = next_id++,
                .side = Side::Buy,
                .price = 9'000 - static_cast<Price>(i % 100),
                .quantity = static_cast<Quantity>(1 + (i % 250)),
            };
            auto result = book.submit(order);
            if (result.status == SubmitStatus::Accepted && result.resting_quantity > 0) {
                live.push_back(order.id);
            }
        } else if (i % 10 < 8) {
            const OrderId id = live.back();
            live.pop_back();
            (void)book.cancel(id);
        } else {
            const OrderId old_id = live.back();
            live.pop_back();
            const OrderId new_id = next_id++;
            auto result = book.replace({
                .old_id = old_id,
                .new_id = new_id,
                .new_price = 8'900 - static_cast<Price>(i % 100),
                .new_quantity = static_cast<Quantity>(1 + (i % 300)),
            });
            if (result.status == ReplaceStatus::Replaced) {
                live.push_back(new_id);
            }
        }
        const auto after = Clock::now();
        latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
    print_result("mixed_add_cancel_replace", count, elapsed, latencies, clock_overhead_ns);
}

void bench_snapshot(std::size_t count, std::uint64_t seed, std::uint64_t clock_overhead_ns) {
    auto orders = make_synthetic_orders(count, seed);
    OrderBook book({.max_orders = count + 1024, .max_price_levels = 4096});
    for (const auto& order : orders) {
        (void)book.submit(order);
    }
    std::vector<std::uint64_t> latencies;
    latencies.reserve(10'000);
    const auto start = Clock::now();
    for (std::size_t i = 0; i < 10'000; ++i) {
        const auto before = Clock::now();
        auto snapshot = book.snapshot(10);
        const auto after = Clock::now();
        latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
        if (snapshot.sequence == 0 && book.sequence() != 0) {
            std::abort();
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
    print_result("snapshot_top10", 10'000, elapsed, latencies, clock_overhead_ns);
}

void bench_spsc(std::size_t count, std::uint64_t clock_overhead_ns) {
    SpscRing<std::uint64_t> ring(65536);
    std::vector<std::uint64_t> received;
    received.reserve(count);
    std::vector<std::uint64_t> latencies;
    latencies.reserve(count);
    const auto start = Clock::now();
    std::thread consumer([&] {
        while (received.size() < count) {
            if (auto value = ring.pop()) {
                received.push_back(*value);
            }
        }
    });
    std::size_t pushed = 0;
    while (pushed < count) {
        const auto before = Clock::now();
        if (ring.push(static_cast<std::uint64_t>(pushed))) {
            const auto after = Clock::now();
            latencies.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
            ++pushed;
        }
    }
    consumer.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
    print_result("spsc_ring_throughput", count, elapsed, latencies, clock_overhead_ns);
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const auto clock_overhead_ns = calibrate_clock_overhead();
    std::cout << "{\"compiler\":\""
#if defined(__clang__)
              << "clang-" << __clang_major__ << '.' << __clang_minor__
#elif defined(__GNUC__)
              << "gcc-" << __GNUC__ << '.' << __GNUC_MINOR__
#else
              << "unknown"
#endif
              << "\",\"cpp\":\"" << __cplusplus << "\",\"orders\":" << args.orders
              << ",\"seed\":" << args.seed
              << ",\"clock_overhead_ns\":" << clock_overhead_ns << "}\n";

    auto add_only = make_synthetic_orders(args.orders, args.seed);
    for (std::size_t i = 0; i < add_only.size(); ++i) {
        add_only[i].id = i + 1;
        add_only[i].type = OrderType::Limit;
        add_only[i].tif = TimeInForce::Gtc;
        add_only[i].side = Side::Buy;
        add_only[i].price = 9'000 - static_cast<Price>(i % 100);
    }
    bench_submit("add_only", add_only, clock_overhead_ns);

    bench_mixed(args.orders, clock_overhead_ns);

    auto crossing = make_synthetic_orders(args.orders, args.seed + 1);
    for (std::size_t i = 0; i < crossing.size(); ++i) {
        crossing[i].id = i + 1;
        crossing[i].type = OrderType::Limit;
        crossing[i].tif = TimeInForce::Ioc;
        crossing[i].side = (i % 2 == 0) ? Side::Sell : Side::Buy;
        crossing[i].price = (i % 2 == 0) ? 10'000 : 10'001;
    }
    bench_submit("aggressive_crossing", crossing, clock_overhead_ns);
    bench_snapshot(std::min<std::size_t>(args.orders, 200'000), args.seed, clock_overhead_ns);
    bench_spsc(args.orders, clock_overhead_ns);
}
