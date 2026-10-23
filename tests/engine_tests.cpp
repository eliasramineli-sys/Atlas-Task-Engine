#include "atlas/engine.hpp"

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <latch>
#include <numeric>
#include <string>

namespace {
using namespace std::chrono_literals;
using atlas::Priority;
using atlas::Shutdown;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("check failed: " #condition); } while (false)

template<class Exception, class F>
void rejects(F&& action) {
    try { action(); }
    catch (const Exception&) { return; }
    throw std::runtime_error("expected exception was not thrown");
}

struct Gate {
    std::mutex mutex;
    std::condition_variable_any ready;
    bool open{};
    void release() { { std::lock_guard lock(mutex); open = true; } ready.notify_all(); }
    void wait(std::stop_token token, bool throw_on_stop = true) {
        std::unique_lock lock(mutex);
        ready.wait(lock, token, [&] { return open; });
        if (throw_on_stop) atlas::cancellation_point(token);
    }
};

void ready(std::future<void>& future) { CHECK(future.wait_for(5s) == std::future_status::ready); future.get(); }

void returns_and_void() {
    atlas::Engine engine({2, 8});
    auto result = engine.submit(Priority::normal, [](std::stop_token, int a, int b) { return a + b; }, 20, 22);
    std::atomic<int> count{};
    auto effect = engine.submit(Priority::normal, [&](std::stop_token) { count.fetch_add(1); });
    CHECK(result.get() == 42); effect.get(); engine.shutdown();
    CHECK(count == 1); CHECK(engine.metrics().succeeded == 2); CHECK(engine.metrics().consistent());
}

void move_only() {
    atlas::Engine engine({1, 4});
    auto input = std::make_unique<int>(5);
    auto handle = engine.submit(Priority::normal,
        [base = std::make_unique<int>(7)](std::stop_token, std::unique_ptr<int> value) {
            return std::make_unique<int>(*base + *value);
        }, std::move(input));
    CHECK(!input); CHECK(*handle.get() == 12);
}

void reference_wrapper() {
    atlas::Engine engine({1, 4}); int value = 3;
    auto handle = engine.submit(Priority::normal, [](std::stop_token, int& n) { n += 2; }, std::ref(value));
    handle.get(); CHECK(value == 5);
}

void exceptions_and_recovery() {
    atlas::Engine engine({1, 8});
    auto standard = engine.submit(Priority::normal, [](std::stop_token) -> int { throw std::domain_error("domain"); });
    auto nonstandard = engine.submit(Priority::normal, [](std::stop_token) -> int { throw 17; });
    rejects<std::domain_error>([&] { (void) standard.get(); });
    bool caught = false; try { (void) nonstandard.get(); } catch (int n) { caught = n == 17; }
    CHECK(caught);
    auto next = engine.submit(Priority::normal, [](std::stop_token) { return 9; }); CHECK(next.get() == 9);
    engine.shutdown(); CHECK(engine.metrics().failed == 2); CHECK(engine.metrics().succeeded == 1);
}

void priority_fifo() {
    Gate gate; atlas::Engine engine({1, 8}); std::promise<void> started; auto began = started.get_future();
    auto blocker = engine.submit(Priority::normal, [&](std::stop_token token) { started.set_value(); gate.wait(token); });
    ready(began); std::vector<int> order;
    std::vector<atlas::TaskHandle<void>> tasks;
    const std::vector<Priority> priorities{Priority::low, Priority::normal, Priority::high, Priority::high, Priority::normal, Priority::low};
    for (std::size_t i = 0; i < priorities.size(); ++i)
        tasks.push_back(engine.submit(priorities[i], [&, id = static_cast<int>(i + 1)](std::stop_token) { order.push_back(id); }));
    CHECK(engine.metrics().queued == 6); gate.release(); blocker.get(); engine.shutdown();
    for (auto& task : tasks) task.get();
    CHECK((order == std::vector<int>{3, 4, 2, 5, 1, 6}));
}

void bounded_queue() {
    Gate gate; atlas::Engine engine({1, 2}); std::promise<void> started; auto began = started.get_future();
    auto blocker = engine.submit(Priority::normal, [&](std::stop_token t) { started.set_value(); gate.wait(t); }); ready(began);
    auto a = engine.submit(Priority::normal, [](std::stop_token) { return 1; });
    auto b = engine.submit(Priority::normal, [](std::stop_token) { return 2; });
    rejects<atlas::queue_full>([&] { (void) engine.submit(Priority::normal, [](std::stop_token) {}); });
    CHECK(engine.metrics().accepted == 3); CHECK(engine.metrics().rejected == 1);
    gate.release(); blocker.get(); CHECK(a.get() + b.get() == 3); engine.shutdown();
    CHECK(engine.metrics().peak_queued <= 2);
}

void timed_submit_timeout() {
    Gate gate; atlas::Engine engine({1, 1}); std::promise<void> started; auto began = started.get_future();
    auto blocker = engine.submit(Priority::normal, [&](std::stop_token t) { started.set_value(); gate.wait(t); }); ready(began);
    auto queued = engine.submit(Priority::normal, [](std::stop_token) {});
    rejects<atlas::queue_timeout>([&] { (void) engine.submit_for(0ms, Priority::normal, [](std::stop_token) {}); });
    gate.release(); blocker.get(); queued.get(); engine.shutdown(); CHECK(engine.metrics().rejected == 1);
}

void timed_submit_recovers() {
    Gate gate; atlas::Engine engine({1, 1}); std::promise<void> started; auto began = started.get_future();
    auto blocker = engine.submit(Priority::normal, [&](std::stop_token t) { started.set_value(); gate.wait(t); }); ready(began);
    auto queued = engine.submit(Priority::normal, [](std::stop_token) { return 1; });
    std::promise<atlas::TaskHandle<int>> submitted; auto accepted = submitted.get_future();
    std::jthread producer([&] {
        try { submitted.set_value(engine.submit_for(5s, Priority::normal, [](std::stop_token) { return 42; })); }
        catch (...) { submitted.set_exception(std::current_exception()); }
    });
    gate.release(); CHECK(accepted.wait_for(5s) == std::future_status::ready);
    auto result = accepted.get(); CHECK(result.get() == 42); blocker.get(); CHECK(queued.get() == 1);
}

void pending_cancel() {
    Gate gate; atlas::Engine engine({1, 2}); std::promise<void> started; auto began = started.get_future();
    auto blocker = engine.submit(Priority::normal, [&](std::stop_token t) { started.set_value(); gate.wait(t); }); ready(began);
    std::atomic<bool> ran{};
    auto pending = engine.submit(Priority::normal, [&](std::stop_token) { ran = true; return 5; });
    CHECK(pending.request_cancel()); CHECK(!pending.request_cancel());
    CHECK(pending.wait_for(0ms) == std::future_status::timeout);
    gate.release(); blocker.get(); rejects<atlas::task_cancelled>([&] { (void) pending.get(); });
    engine.shutdown(); CHECK(!ran); CHECK(engine.metrics().cancelled == 1);
}

void active_cancel() {
    Gate gate; atlas::Engine engine({1, 2}); std::promise<void> started; auto began = started.get_future();
    auto running = engine.submit(Priority::normal, [&](std::stop_token t) { started.set_value(); gate.wait(t); return 1; });
    ready(began); CHECK(running.request_cancel());
    rejects<atlas::task_cancelled>([&] { (void) running.get(); }); engine.shutdown();
    CHECK(engine.metrics().cancelled == 1);
}

void normal_result_after_stop() {
    Gate gate; atlas::Engine engine({1, 2}); std::promise<void> started; auto began = started.get_future();
    auto running = engine.submit(Priority::normal, [&](std::stop_token t) { started.set_value(); gate.wait(t, false); return 42; });
    ready(began); running.request_cancel(); CHECK(running.get() == 42); engine.shutdown();
    CHECK(engine.metrics().succeeded == 1); CHECK(engine.metrics().cancelled == 0);
}

void shutdown_cancels_pending() {
    Gate gate; atlas::Engine engine({1, 4}); std::promise<void> started; auto began = started.get_future();
    auto running = engine.submit(Priority::normal, [&](std::stop_token t) { started.set_value(); gate.wait(t); }); ready(began);
    std::atomic<bool> ran{};
    auto pending = engine.submit(Priority::high, [&](std::stop_token) { ran = true; });
    engine.shutdown(Shutdown::cancel);
    CHECK(pending.wait_for(0ms) == std::future_status::ready);
    rejects<atlas::task_cancelled>([&] { pending.get(); });
    rejects<atlas::task_cancelled>([&] { running.get(); });
    CHECK(!ran); CHECK(engine.metrics().cancelled == 2); CHECK(engine.metrics().consistent());
}

void drain_and_closed() {
    atlas::Engine engine({2, 64}); std::vector<atlas::TaskHandle<int>> handles;
    for (int i = 0; i < 30; ++i) handles.push_back(engine.submit(Priority::normal, [i](std::stop_token) { return i; }));
    engine.shutdown(Shutdown::drain); int sum = 0;
    for (auto& h : handles) sum += h.get();
    CHECK(sum == 435); CHECK(!engine.accepting());
    rejects<atlas::engine_closed>([&] { (void) engine.submit(Priority::normal, [](std::stop_token) {}); });
    engine.shutdown(); CHECK(engine.metrics().rejected == 1); CHECK(engine.metrics().succeeded == 30);
}

void worker_wait_guards() {
    atlas::Engine engine({1, 4});
    auto handle = engine.submit(Priority::normal, [&](std::stop_token) {
        rejects<std::logic_error>([&] { engine.wait_idle(); });
        rejects<std::logic_error>([&] { engine.shutdown(); }); return 2;
    });
    CHECK(handle.get() == 2); CHECK(engine.accepting());
}

void worker_submit_guard() {
    Gate gate; atlas::Engine engine({1, 1}); std::promise<void> started; auto began = started.get_future();
    auto running = engine.submit(Priority::normal, [&](std::stop_token t) {
        started.set_value(); gate.wait(t);
        rejects<std::logic_error>([&] { (void) engine.submit_for(1s, Priority::normal, [](std::stop_token) {}); });
    });
    ready(began); auto queued = engine.submit(Priority::normal, [](std::stop_token) {});
    gate.release(); running.get(); queued.get(); engine.shutdown(); CHECK(engine.metrics().rejected == 1);
}

void close_from_worker() {
    Gate gate; atlas::Engine engine({1, 2}); std::promise<void> started; auto began = started.get_future();
    auto running = engine.submit(Priority::normal, [&](std::stop_token t) { started.set_value(); gate.wait(t); engine.close(); });
    ready(began); auto queued = engine.submit(Priority::normal, [](std::stop_token) { return 7; });
    gate.release(); running.get(); CHECK(queued.get() == 7); engine.shutdown();
}

void stop_callback_reentry() {
    Gate gate; atlas::Engine engine({1, 4}); std::promise<void> started; auto began = started.get_future();
    std::atomic<bool> called{};
    auto handle = engine.submit(Priority::normal, [&](std::stop_token token) {
        std::stop_callback callback(token, [&] { called.store(engine.metrics().consistent()); });
        started.set_value(); gate.wait(token);
    });
    ready(began); handle.request_cancel(); rejects<atlas::task_cancelled>([&] { handle.get(); });
    engine.shutdown(); CHECK(called);
}

void capture_destructor_reentry() {
    atlas::Engine engine({1, 4}); std::atomic<bool> destroyed{};
    struct Probe {
        atlas::Engine* engine; std::atomic<bool>* destroyed;
        ~Probe() { destroyed->store(engine->metrics().consistent()); }
    };
    auto handle = engine.submit(Priority::normal,
        [probe = std::unique_ptr<Probe>(new Probe{&engine, &destroyed})](std::stop_token) { return 7; });
    CHECK(handle.get() == 7); engine.wait_idle(); CHECK(destroyed);
}

void producer_woken_on_close() {
    Gate gate; atlas::Engine engine({1, 1}); std::promise<void> started; auto began = started.get_future();
    auto running = engine.submit(Priority::normal, [&](std::stop_token t) { started.set_value(); gate.wait(t); }); ready(began);
    auto pending = engine.submit(Priority::normal, [](std::stop_token) {});
    std::promise<bool> rejected; auto observed = rejected.get_future();
    std::jthread producer([&] {
        try { (void) engine.submit_for(5s, Priority::normal, [](std::stop_token) {}); rejected.set_value(false); }
        catch (const atlas::engine_closed&) { rejected.set_value(true); }
        catch (...) { rejected.set_exception(std::current_exception()); }
    });
    engine.close(Shutdown::cancel); CHECK(observed.wait_for(5s) == std::future_status::ready); CHECK(observed.get());
    engine.shutdown(); rejects<atlas::task_cancelled>([&] { running.get(); }); rejects<atlas::task_cancelled>([&] { pending.get(); });
}

void concurrent_shutdown() {
    Gate gate; atlas::Engine engine({1, 2}); std::promise<void> started; auto began = started.get_future();
    auto running = engine.submit(Priority::normal, [&](std::stop_token t) { started.set_value(); gate.wait(t); }); ready(began);
    std::jthread draining([&] { engine.shutdown(Shutdown::drain); });
    std::jthread cancelling([&] { engine.shutdown(Shutdown::cancel); });
    draining.join(); cancelling.join(); rejects<atlas::task_cancelled>([&] { running.get(); });
    CHECK(engine.metrics().consistent());
}

void destructor_cancellation() {
    Gate gate; std::optional<atlas::TaskHandle<void>> handle; std::promise<void> started; auto began = started.get_future();
    {
        atlas::Engine engine({1, 2});
        handle.emplace(engine.submit(Priority::normal, [&](std::stop_token t) { started.set_value(); gate.wait(t); })); ready(began);
    }
    CHECK(handle->wait_for(0ms) == std::future_status::ready); rejects<atlas::task_cancelled>([&] { handle->get(); });
}

void handle_lifetime() {
    Gate gate; atlas::Engine engine({1, 2}); std::promise<void> started; auto began = started.get_future(); std::atomic<bool> ran{};
    auto blocker = engine.submit(Priority::normal, [&](std::stop_token t) { started.set_value(); gate.wait(t); }); ready(began);
    { auto discarded = engine.submit(Priority::normal, [&](std::stop_token) { ran = true; }); }
    gate.release(); blocker.get(); engine.wait_idle(); CHECK(ran);
}

void invalid_parameters() {
    rejects<std::invalid_argument>([] { atlas::Engine engine({0, 1}); });
    rejects<std::invalid_argument>([] { atlas::Engine engine({1, 0}); });
    atlas::Engine engine({1, 4});
    rejects<std::invalid_argument>([&] { (void) engine.submit(static_cast<Priority>(17), [](std::stop_token) {}); });
    rejects<std::invalid_argument>([&] { (void) engine.submit_for(-1ms, Priority::normal, [](std::stop_token) {}); });
    rejects<std::invalid_argument>([&] { engine.close(static_cast<Shutdown>(17)); });
    CHECK(engine.accepting()); CHECK(engine.metrics().accepted == 0);
}

void stress() {
    constexpr int producers = 4, per_producer = 2500;
    constexpr std::size_t capacity = 64;
    atlas::Engine engine({4, capacity});
    std::atomic<std::uint64_t> sum{}; std::atomic<bool> bad{}; std::latch start(producers + 1);
    std::jthread observer([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            const auto m = engine.metrics();
            if (!m.consistent() || m.queued > capacity || m.running > 4 || m.peak_queued > capacity) bad = true;
            std::this_thread::yield();
        }
    });
    std::vector<std::jthread> threads;
    for (int p = 0; p < producers; ++p) threads.emplace_back([&, p] {
        start.arrive_and_wait();
        try {
            std::vector<atlas::TaskHandle<int>> tasks; tasks.reserve(per_producer);
            for (int i = 1; i <= per_producer; ++i)
                tasks.push_back(engine.submit_for(5s, static_cast<Priority>(i % 3),
                    [value = p * per_producer + i](std::stop_token) { return value; }));
            for (auto& task : tasks) sum.fetch_add(static_cast<std::uint64_t>(task.get()), std::memory_order_relaxed);
        } catch (...) { bad = true; }
    });
    start.arrive_and_wait(); for (auto& thread : threads) thread.join();
    engine.shutdown(); observer.request_stop(); observer.join();
    constexpr std::uint64_t total = producers * per_producer;
    CHECK(!bad); CHECK(sum == total * (total + 1) / 2);
    const auto m = engine.metrics(); CHECK(m.accepted == total); CHECK(m.succeeded == total);
    CHECK(m.rejected == 0); CHECK(m.failed == 0); CHECK(m.queued == 0); CHECK(m.running == 0); CHECK(m.consistent());
    std::cout << "  Stress: 4 producers, 4 workers, 10000 tasks, consistent snapshots\n";
}
} // namespace

int main(int argc, char** argv) {
    std::vector<std::pair<const char*, std::function<void()>>> tests;
    if (argc == 2 && std::string_view(argv[1]) == "--stress") tests.push_back({"concurrent stress", stress});
    else tests = {
        {"values and void", returns_and_void}, {"move-only ownership", move_only}, {"explicit references", reference_wrapper},
        {"exceptions and worker recovery", exceptions_and_recovery}, {"priority and FIFO", priority_fifo},
        {"bounded queue", bounded_queue}, {"timed submission timeout", timed_submit_timeout},
        {"producer recovery", timed_submit_recovers}, {"pending cancellation", pending_cancel},
        {"active cancellation", active_cancel}, {"normal result after stop request", normal_result_after_stop},
        {"shutdown cancellation", shutdown_cancels_pending}, {"drain and closed rejection", drain_and_closed},
        {"worker wait guards", worker_wait_guards}, {"worker submission guard", worker_submit_guard},
        {"nonblocking close from worker", close_from_worker}, {"stop callback reentry", stop_callback_reentry},
        {"capture destructor reentry", capture_destructor_reentry}, {"producer wakeup on close", producer_woken_on_close},
        {"concurrent shutdown", concurrent_shutdown}, {"RAII shutdown", destructor_cancellation},
        {"discarded handle lifetime", handle_lifetime}, {"invalid configuration", invalid_parameters}
    };
    std::size_t failed{};
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << tests.size() - failed << "/" << tests.size() << " passed\n";
    return failed == 0 ? 0 : 1;
}
