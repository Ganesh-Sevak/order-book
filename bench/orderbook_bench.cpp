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
    std::size_t runs{5};
};

struct BenchmarkResult {
    std::string scenario;
    std::size_t events{};
    std::uint64_t elapsed_ns{};
    std::uint64_t batch_per_op_ns{};
    std::uint64_t throughput_per_sec{};
    std::uint64_t raw_p50_ns{};
    std::uint64_t raw_p99_ns{};
    std::uint64_t raw_p999_ns{};
    std::uint64_t corrected_p50_ns{};
    std::uint64_t corrected_p99_ns{};
    std::uint64_t corrected_p999_ns{};
};

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--orders") args.orders = static_cast<std::size_t>(std::stoull(value));
        else if (key == "--seed") args.seed = static_cast<std::uint64_t>(std::stoull(value));
        else if (key == "--runs") args.runs = std::max<std::size_t>(1, static_cast<std::size_t>(std::stoull(value)));
    }
    return args;
}

std::uint64_t percentile(std::vector<std::uint64_t>& values, double p) {
    if (values.empty()) return 0;
    const auto index = static_cast<std::size_t>((values.size() - 1) * p);
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

std::uint64_t median(std::vector<std::uint64_t> values) { return percentile(values, 0.50); }

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

BenchmarkResult make_result(std::string scenario,
                            std::size_t events,
                            std::chrono::nanoseconds elapsed,
                            const std::vector<std::uint64_t>& latencies,
                            std::uint64_t clock_overhead_ns) {
    auto raw_p50_values = latencies;
    auto raw_p99_values = latencies;
    auto raw_p999_values = latencies;
    std::vector<std::uint64_t> corrected;
    corrected.reserve(latencies.size());
    for (const auto value : latencies) corrected.push_back(value > clock_overhead_ns ? value - clock_overhead_ns : 0);
    auto corrected_p50_values = corrected;
    auto corrected_p99_values = corrected;
    auto corrected_p999_values = corrected;
    const double seconds = static_cast<double>(elapsed.count()) / 1'000'000'000.0;
    return {.scenario = std::move(scenario),
            .events = events,
            .elapsed_ns = static_cast<std::uint64_t>(elapsed.count()),
            .batch_per_op_ns = events == 0 ? 0 : static_cast<std::uint64_t>(elapsed.count() / events),
            .throughput_per_sec = seconds == 0.0 ? 0 : static_cast<std::uint64_t>(static_cast<double>(events) / seconds),
            .raw_p50_ns = percentile(raw_p50_values, 0.50),
            .raw_p99_ns = percentile(raw_p99_values, 0.99),
            .raw_p999_ns = percentile(raw_p999_values, 0.999),
            .corrected_p50_ns = percentile(corrected_p50_values, 0.50),
            .corrected_p99_ns = percentile(corrected_p99_values, 0.99),
            .corrected_p999_ns = percentile(corrected_p999_values, 0.999)};
}

void print_result(const BenchmarkResult& result, std::size_t runs, std::uint64_t clock_overhead_ns) {
    std::cout << "{\"scenario\":\"" << result.scenario << "\",\"runs\":" << runs
              << ",\"events\":" << result.events
              << ",\"elapsed_ns\":" << result.elapsed_ns
              << ",\"batch_per_op_ns\":" << result.batch_per_op_ns
              << ",\"throughput_per_sec\":" << result.throughput_per_sec
              << ",\"clock_overhead_ns\":" << clock_overhead_ns
              << ",\"raw_p50_ns\":" << result.raw_p50_ns
              << ",\"raw_p99_ns\":" << result.raw_p99_ns
              << ",\"raw_p999_ns\":" << result.raw_p999_ns
              << ",\"corrected_p50_ns\":" << result.corrected_p50_ns
              << ",\"corrected_p99_ns\":" << result.corrected_p99_ns
              << ",\"corrected_p999_ns\":" << result.corrected_p999_ns << "}\n";
}

BenchmarkResult bench_submit(const char* name, const std::vector<NewOrder>& orders, std::uint64_t clock_overhead_ns) {
    OrderBook book({.min_price = 1, .max_price = 20'000, .max_orders = orders.size() + 1024});
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
    return make_result(name, orders.size(), std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start), latencies, clock_overhead_ns);
}

BenchmarkResult bench_mixed(std::size_t count, std::uint64_t clock_overhead_ns) {
    OrderBook book({.min_price = 1, .max_price = 20'000, .max_orders = count + 1024});
    std::vector<OrderId> live;
    live.reserve(count);
    std::vector<std::uint64_t> latencies;
    latencies.reserve(count);
    OrderId next_id = 1;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const auto before = Clock::now();
        if (live.empty() || i % 10 < 6) {
            NewOrder order{.id = next_id++, .side = Side::Buy, .price = 9'000 - static_cast<Price>(i % 100),
                           .quantity = static_cast<Quantity>(1 + (i % 250))};
            const auto result = book.submit(order);
            if (result.status == SubmitStatus::Accepted && result.resting_quantity > 0) live.push_back(order.id);
        } else if (i % 10 < 8) {
            const auto id = live.back();
            live.pop_back();
            (void)book.cancel(id);
        } else {
            const auto old_id = live.back();
            live.pop_back();
            const auto new_id = next_id++;
            const auto result = book.replace({.old_id = old_id, .new_id = new_id,
                                               .new_price = 8'900 - static_cast<Price>(i % 100),
                                               .new_quantity = static_cast<Quantity>(1 + (i % 300))});
            if (result.status == ReplaceStatus::Replaced) live.push_back(new_id);
        }
        const auto after = Clock::now();
        latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
    }
    return make_result("mixed_add_cancel_replace", count,
                       std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start), latencies, clock_overhead_ns);
}

BenchmarkResult bench_deep_wide(std::size_t count, std::uint64_t clock_overhead_ns) {
    constexpr Price min_price = 1;
    constexpr Price max_price = 131'072;
    const auto initial_levels = std::min<std::size_t>(32'768, std::max<std::size_t>(1, count / 2));
    OrderBook book({.min_price = min_price, .max_price = max_price, .max_orders = initial_levels + 1024});
    std::vector<OrderId> live;
    live.reserve(initial_levels);
    OrderId next_id = 1;
    for (std::size_t i = 0; i < initial_levels; ++i) {
        const auto id = next_id++;
        (void)book.submit({.id = id, .side = Side::Buy, .price = min_price + static_cast<Price>(i * 4U), .quantity = 1});
        live.push_back(id);
    }

    std::vector<std::uint64_t> latencies;
    latencies.reserve(count);
    const auto start = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const auto before = Clock::now();
        if ((i & 1U) == 0) {
            const auto id = live.back();
            live.pop_back();
            (void)book.cancel(id);
        } else {
            const auto lane = (i / 2U) % initial_levels;
            const auto id = next_id++;
            (void)book.submit({.id = id, .side = Side::Buy,
                               .price = min_price + 1 + static_cast<Price>(lane * 4U), .quantity = 1});
            live.push_back(id);
        }
        const auto after = Clock::now();
        latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
    }
    return make_result("deep_wide_level_churn", count,
                       std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start), latencies, clock_overhead_ns);
}

BenchmarkResult bench_snapshot(std::size_t count, std::uint64_t seed, std::uint64_t clock_overhead_ns) {
    const auto orders = make_synthetic_orders(count, seed);
    OrderBook book({.min_price = 1, .max_price = 20'000, .max_orders = count + 1024});
    for (const auto& order : orders) (void)book.submit(order);
    std::vector<std::uint64_t> latencies;
    latencies.reserve(10'000);
    const auto start = Clock::now();
    for (std::size_t i = 0; i < 10'000; ++i) {
        const auto before = Clock::now();
        const auto snapshot = book.snapshot(10);
        const auto after = Clock::now();
        latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
        if (snapshot.sequence == 0 && book.sequence() != 0) std::abort();
    }
    return make_result("snapshot_top10", 10'000,
                       std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start), latencies, clock_overhead_ns);
}

BenchmarkResult bench_spsc(std::size_t count, std::uint64_t clock_overhead_ns) {
    SpscRing<std::uint64_t> ring(65536);
    std::vector<std::uint64_t> received;
    received.reserve(count);
    std::vector<std::uint64_t> latencies;
    latencies.reserve(count);
    const auto start = Clock::now();
    std::thread consumer([&] {
        while (received.size() < count) {
            if (auto value = ring.pop()) received.push_back(*value);
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
    return make_result("spsc_ring_throughput", count,
                       std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start), latencies, clock_overhead_ns);
}

std::vector<BenchmarkResult> run_suite(const Args& args, std::uint64_t clock_overhead_ns) {
    auto add_only = make_synthetic_orders(args.orders, args.seed);
    for (std::size_t i = 0; i < add_only.size(); ++i) {
        add_only[i].id = i + 1;
        add_only[i].type = OrderType::Limit;
        add_only[i].tif = TimeInForce::Gtc;
        add_only[i].side = Side::Buy;
        add_only[i].price = 9'000 - static_cast<Price>(i % 100);
    }
    auto crossing = make_synthetic_orders(args.orders, args.seed + 1);
    for (std::size_t i = 0; i < crossing.size(); ++i) {
        crossing[i].id = i + 1;
        crossing[i].type = OrderType::Limit;
        crossing[i].tif = TimeInForce::Ioc;
        crossing[i].side = (i % 2 == 0) ? Side::Sell : Side::Buy;
        crossing[i].price = (i % 2 == 0) ? 10'000 : 10'001;
    }
    return {bench_submit("add_only", add_only, clock_overhead_ns),
            bench_mixed(args.orders, clock_overhead_ns),
            bench_submit("aggressive_crossing", crossing, clock_overhead_ns),
            bench_deep_wide(args.orders, clock_overhead_ns),
            bench_snapshot(std::min<std::size_t>(args.orders, 200'000), args.seed, clock_overhead_ns),
            bench_spsc(args.orders, clock_overhead_ns)};
}

BenchmarkResult median_result(const std::vector<BenchmarkResult>& values) {
    BenchmarkResult out{.scenario = values.front().scenario, .events = values.front().events};
    std::vector<std::uint64_t> elapsed, batch, throughput, raw50, raw99, raw999, corrected50, corrected99, corrected999;
    for (const auto& value : values) {
        elapsed.push_back(value.elapsed_ns); batch.push_back(value.batch_per_op_ns); throughput.push_back(value.throughput_per_sec);
        raw50.push_back(value.raw_p50_ns); raw99.push_back(value.raw_p99_ns); raw999.push_back(value.raw_p999_ns);
        corrected50.push_back(value.corrected_p50_ns); corrected99.push_back(value.corrected_p99_ns); corrected999.push_back(value.corrected_p999_ns);
    }
    out.elapsed_ns = median(elapsed); out.batch_per_op_ns = median(batch); out.throughput_per_sec = median(throughput);
    out.raw_p50_ns = median(raw50); out.raw_p99_ns = median(raw99); out.raw_p999_ns = median(raw999);
    out.corrected_p50_ns = median(corrected50); out.corrected_p99_ns = median(corrected99); out.corrected_p999_ns = median(corrected999);
    return out;
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
              << "\",\"cpp\":" << __cplusplus << ",\"orders\":" << args.orders
              << ",\"seed\":" << args.seed << ",\"runs\":" << args.runs
              << ",\"warmup_runs\":1,\"clock_overhead_ns\":" << clock_overhead_ns << "}\n";

    (void)run_suite(args, clock_overhead_ns);
    std::vector<std::vector<BenchmarkResult>> samples;
    for (std::size_t run = 0; run < args.runs; ++run) {
        auto suite = run_suite(args, clock_overhead_ns);
        if (samples.empty()) samples.resize(suite.size());
        for (std::size_t i = 0; i < suite.size(); ++i) samples[i].push_back(std::move(suite[i]));
    }
    for (const auto& scenario_samples : samples) print_result(median_result(scenario_samples), args.runs, clock_overhead_ns);
}
