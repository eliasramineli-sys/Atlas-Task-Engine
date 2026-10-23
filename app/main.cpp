#include "atlas/engine.hpp"

#include <charconv>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using namespace std::chrono_literals;
struct Options {
    std::size_t workers{4}, jobs{32}, capacity{64};
    std::uint32_t limit{20'000};
    std::uint32_t cancel_after_ms{};
    bool json{};
};

std::size_t number(std::string_view text, std::size_t minimum, std::size_t maximum) {
    std::size_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < minimum || value > maximum)
        throw std::invalid_argument("numeric argument outside its allowed range");
    return value;
}

void help() {
    std::cout << "Atlas Task Engine 0.1.0 - C++20\n"
              << "CPU demonstration: each job counts prime numbers up to --limit.\n\n"
              << "atlas-cli [--workers 1..64] [--jobs 1..1000] [--queue 1..100000]\n"
              << "          [--limit 2..2000000] [--cancel-after-ms 1..10000] [--json]\n"
              << "Cancellation is cooperative. Timer starts after submission completes.\n";
}

std::uint64_t count_primes(std::stop_token token, std::uint32_t limit) {
    std::uint64_t count{};
    for (std::uint32_t value = 2; value <= limit; ++value) {
        if ((value & 255U) == 0) atlas::cancellation_point(token);
        bool prime = true;
        for (std::uint32_t divisor = 2; divisor <= value / divisor; ++divisor)
            if (value % divisor == 0) { prime = false; break; }
        count += prime ? 1U : 0U;
    }
    atlas::cancellation_point(token);
    return count;
}
} // namespace

int main(int argc, char** argv) {
    Options options;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--help") { help(); return 0; }
            if (arg == "--json") { options.json = true; continue; }
            if (i + 1 == argc) throw std::invalid_argument("option requires a value");
            const std::string_view value = argv[++i];
            if (arg == "--workers") options.workers = number(value, 1, 64);
            else if (arg == "--jobs") options.jobs = number(value, 1, 1000);
            else if (arg == "--queue") options.capacity = number(value, 1, 100'000);
            else if (arg == "--limit") options.limit = static_cast<std::uint32_t>(number(value, 2, 2'000'000));
            else if (arg == "--cancel-after-ms") options.cancel_after_ms = static_cast<std::uint32_t>(number(value, 1, 10'000));
            else throw std::invalid_argument("unknown option: " + std::string(arg));
        }
    } catch (const std::exception& error) {
        std::cerr << "Input error: " << error.what() << "\nUse --help for options.\n"; return 2;
    }

    try {
        atlas::Engine engine({.workers = options.workers, .capacity = options.capacity});
        std::vector<atlas::TaskHandle<std::uint64_t>> handles;
        handles.reserve(options.jobs);
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < options.jobs; ++i) {
            const auto priority = static_cast<atlas::Priority>(i % 3);
            handles.push_back(engine.submit_for(5s, priority, count_primes, options.limit));
        }

        std::mutex timer_mutex;
        std::condition_variable timer_done;
        bool finished = false;
        std::jthread timer;
        if (options.cancel_after_ms != 0) timer = std::jthread([&] {
            std::unique_lock lock(timer_mutex);
            if (timer_done.wait_for(lock, std::chrono::milliseconds{options.cancel_after_ms}, [&] { return finished; })) return;
            lock.unlock();
            for (const auto& handle : handles) handle.request_cancel();
        });

        std::uint64_t checksum{};
        for (auto& handle : handles) {
            try { checksum += handle.get(); }
            catch (const atlas::task_cancelled&) { /* Cancelamento é um resultado esperado. */ }
            catch (const std::exception& error) { std::cerr << "Task failed: " << error.what() << '\n'; }
        }
        {
            std::lock_guard lock(timer_mutex); finished = true;
        }
        timer_done.notify_one(); if (timer.joinable()) timer.join();
        engine.shutdown(atlas::Shutdown::drain);
        const auto metrics = engine.metrics();
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::cout << std::fixed << std::setprecision(3);
        if (options.json) {
            std::cout << "{\n  \"workers\": " << options.workers
                      << ",\n  \"accepted\": " << metrics.accepted << ",\n  \"succeeded\": " << metrics.succeeded
                      << ",\n  \"failed\": " << metrics.failed << ",\n  \"cancelled\": " << metrics.cancelled
                      << ",\n  \"rejected\": " << metrics.rejected << ",\n  \"queued\": " << metrics.queued
                      << ",\n  \"running\": " << metrics.running << ",\n  \"peak_queued\": " << metrics.peak_queued
                      << ",\n  \"checksum\": " << checksum << ",\n  \"elapsed_ms\": " << elapsed << "\n}\n";
        } else {
            std::cout << "Atlas Task Engine - actual execution metrics\n"
                      << "Workers: " << options.workers << " | Accepted: " << metrics.accepted
                      << " | Succeeded: " << metrics.succeeded << " | Cancelled: " << metrics.cancelled
                      << " | Failed: " << metrics.failed << '\n'
                      << "Peak pending queue: " << metrics.peak_queued << " | Elapsed: " << elapsed << " ms\n"
                      << "Checksum (sum of completed prime counts): " << checksum << '\n';
        }
        return metrics.failed == 0 && metrics.consistent() ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << "Engine error: " << error.what() << '\n'; return 1; }
}
