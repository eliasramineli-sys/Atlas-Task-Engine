#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace atlas {

enum class Priority { low, normal, high };
enum class Shutdown { drain, cancel };

struct task_cancelled : std::runtime_error { task_cancelled() : runtime_error("task cancelled") {} };
struct queue_full : std::runtime_error { queue_full() : runtime_error("pending queue is full") {} };
struct queue_timeout : std::runtime_error { queue_timeout() : runtime_error("submission timed out") {} };
struct engine_closed : std::runtime_error { engine_closed() : runtime_error("engine is closed") {} };

inline void cancellation_point(std::stop_token token) {
    if (token.stop_requested()) throw task_cancelled{};
}

struct Config {
    std::size_t workers{4};
    std::size_t capacity{256}; // Somente tarefas pendentes; não inclui as que estão executando.
};

struct Metrics {
    std::uint64_t accepted{}, succeeded{}, failed{}, cancelled{}, rejected{};
    std::size_t queued{}, running{}, peak_queued{};
    [[nodiscard]] bool consistent() const noexcept {
        return accepted == succeeded + failed + cancelled + queued + running;
    }
};

template<class T>
class TaskHandle {
public:
    TaskHandle(TaskHandle&&) noexcept = default;
    TaskHandle& operator=(TaskHandle&&) noexcept = default;
    TaskHandle(const TaskHandle&) = delete;
    TaskHandle& operator=(const TaskHandle&) = delete;

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] bool valid() const noexcept { return future_.valid(); }
    bool request_cancel() const noexcept { return stop_.request_stop(); }
    void wait() const { future_.wait(); }
    template<class Rep, class Period>
    std::future_status wait_for(std::chrono::duration<Rep, Period> timeout) const {
        return future_.wait_for(timeout);
    }
    T get() { return future_.get(); }

private:
    friend class Engine;
    TaskHandle(std::uint64_t id, std::future<T> future, std::stop_source stop)
        : id_(id), future_(std::move(future)), stop_(std::move(stop)) {}
    std::uint64_t id_;
    std::future<T> future_;
    std::stop_source stop_;
};

namespace detail {
enum class Outcome { succeeded, failed, cancelled };

struct Job {
    explicit Job(Priority priority) : priority(priority) {}
    virtual ~Job() = default;
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    virtual Outcome execute() noexcept = 0;
    virtual void cancel(std::exception_ptr reason) noexcept = 0;
    Priority priority;
    std::uint64_t sequence{};
    std::stop_source stop;
};

template<class F, class Tuple, class R>
struct JobModel final : Job {
    JobModel(Priority priority, F callable, Tuple arguments)
        : Job(priority), callable(std::move(callable)), arguments(std::move(arguments)) {}

    Outcome execute() noexcept override {
        try {
            cancellation_point(stop.get_token());
            auto invoke = [this]() -> R {
                return std::apply([this](auto&... args) -> R {
                    return std::invoke(callable, stop.get_token(), std::move(args)...);
                }, arguments);
            };
            if constexpr (std::is_void_v<R>) { invoke(); promise.set_value(); }
            else promise.set_value(invoke());
            return Outcome::succeeded;
        } catch (const task_cancelled&) {
            promise.set_exception(std::current_exception()); return Outcome::cancelled;
        } catch (...) {
            promise.set_exception(std::current_exception()); return Outcome::failed;
        }
    }

    void cancel(std::exception_ptr reason) noexcept override { promise.set_exception(std::move(reason)); }
    F callable;
    Tuple arguments;
    std::promise<R> promise;
};
} // namespace detail

class Engine final {
public:
    explicit Engine(Config config = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    template<class F, class... Args>
        requires std::invocable<std::decay_t<F>&, std::stop_token, std::decay_t<Args>&&...>
    auto submit(Priority priority, F&& callable, Args&&... args) {
        return make_task(priority, std::nullopt, std::forward<F>(callable), std::forward<Args>(args)...);
    }

    template<class F, class... Args>
        requires std::invocable<std::decay_t<F>&, std::stop_token, std::decay_t<Args>&&...>
    auto submit_for(std::chrono::milliseconds timeout, Priority priority, F&& callable, Args&&... args) {
        if (timeout < std::chrono::milliseconds::zero() || timeout > std::chrono::hours{24})
            throw std::invalid_argument("submission timeout must be between 0 and 24 hours");
        return make_task(priority, timeout, std::forward<F>(callable), std::forward<Args>(args)...);
    }

    // close() recusa novas submissões. drain executa a fila; cancel cancela a fila e sinaliza tarefas ativas.
    void close(Shutdown mode = Shutdown::drain);
    // shutdown() também espera as threads. Não deve ser chamado por uma tarefa deste motor.
    void shutdown(Shutdown mode = Shutdown::drain);
    void wait_idle();
    [[nodiscard]] bool accepting() const;
    [[nodiscard]] Metrics metrics() const;

private:
    template<class F, class... Args>
    auto make_task(Priority priority, std::optional<std::chrono::milliseconds> timeout, F&& callable, Args&&... args) {
        using Function = std::decay_t<F>;
        using Arguments = std::tuple<std::decay_t<Args>...>;
        using Result = std::invoke_result_t<Function&, std::stop_token, std::decay_t<Args>&&...>;
        static_assert(!std::is_reference_v<Result>, "return an owned value, not a reference to task storage");
        static_assert(std::is_void_v<Result> || std::is_move_constructible_v<Result>, "result must be movable");
        auto job = std::make_unique<detail::JobModel<Function, Arguments, Result>>(
            priority, Function(std::forward<F>(callable)), Arguments(std::forward<Args>(args)...));
        auto future = job->promise.get_future();
        auto stop = job->stop;
        const auto id = enqueue(std::move(job), timeout);
        return TaskHandle<Result>{id, std::move(future), std::move(stop)};
    }

    struct State;
    static thread_local const State* current_worker_;
    static void worker_loop(const std::shared_ptr<State>& state);
    std::uint64_t enqueue(std::unique_ptr<detail::Job> job, std::optional<std::chrono::milliseconds> timeout);
    void join_workers();
    void require_external_thread() const;

    std::shared_ptr<State> state_;
    std::vector<std::jthread> workers_;
    std::mutex join_mutex_;
};

} // namespace atlas
