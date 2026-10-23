#include "atlas/engine.hpp"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <unordered_map>

namespace atlas {
namespace {
struct LowerPriority {
    bool operator()(const std::unique_ptr<detail::Job>& a, const std::unique_ptr<detail::Job>& b) const noexcept {
        if (a->priority != b->priority) return a->priority < b->priority;
        return a->sequence > b->sequence; // FIFO entre tarefas da mesma prioridade.
    }
};
} // namespace

struct Engine::State {
    explicit State(Config value) : config(value) { queue.reserve(config.capacity); }
    Config config;
    mutable std::mutex mutex;
    std::condition_variable ready, space, idle;
    std::vector<std::unique_ptr<detail::Job>> queue;
    std::unordered_map<std::uint64_t, std::stop_source> controls;
    Metrics totals;
    std::uint64_t next_id{1};
    bool accepting{true};
};

thread_local const Engine::State* Engine::current_worker_ = nullptr;

Engine::Engine(Config config) {
    if (config.workers == 0 || config.workers > 256 || config.capacity == 0 || config.capacity > 1'000'000)
        throw std::invalid_argument("workers must be 1..256 and capacity must be 1..1000000");
    state_ = std::make_shared<State>(config);
    workers_.reserve(config.workers);
    try {
        for (std::size_t i = 0; i < config.workers; ++i)
            workers_.emplace_back([state = state_](std::stop_token) { worker_loop(state); });
    } catch (...) {
        close(Shutdown::cancel); join_workers(); throw;
    }
}

Engine::~Engine() {
    // Destruir o motor dentro de uma de suas tarefas viola o contrato de vida útil.
    if (current_worker_ == state_.get()) std::terminate();
    close(Shutdown::cancel);
    join_workers();
}

std::uint64_t Engine::enqueue(std::unique_ptr<detail::Job> job,
                              std::optional<std::chrono::milliseconds> timeout) {
    if (job->priority != Priority::low && job->priority != Priority::normal && job->priority != Priority::high)
        throw std::invalid_argument("invalid priority");
    std::unique_lock lock(state_->mutex);
    if (!state_->accepting) { ++state_->totals.rejected; throw engine_closed{}; }
    if (state_->queue.size() == state_->config.capacity) {
        if (!timeout) { ++state_->totals.rejected; throw queue_full{}; }
        if (current_worker_ == state_.get()) {
            ++state_->totals.rejected;
            throw std::logic_error("a worker must not wait for space in its own full queue");
        }
        if (!state_->space.wait_for(lock, *timeout, [this] {
            return !state_->accepting || state_->queue.size() < state_->config.capacity;
        })) { ++state_->totals.rejected; throw queue_timeout{}; }
        if (!state_->accepting) { ++state_->totals.rejected; throw engine_closed{}; }
    }
    if (state_->next_id == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("task id space exhausted");
    const auto id = state_->next_id++;
    job->sequence = id;
    state_->controls.emplace(id, job->stop); // Pode alocar; ainda não alteramos fila/contadores.
    state_->queue.push_back(std::move(job)); // Vetor já reservado até capacity.
    std::push_heap(state_->queue.begin(), state_->queue.end(), LowerPriority{});
    ++state_->totals.accepted;
    state_->totals.peak_queued = std::max(state_->totals.peak_queued, state_->queue.size());
    lock.unlock(); state_->ready.notify_one();
    return id;
}

void Engine::worker_loop(const std::shared_ptr<State>& state) {
    current_worker_ = state.get();
    for (;;) {
        std::unique_ptr<detail::Job> job;
        {
            std::unique_lock lock(state->mutex);
            state->ready.wait(lock, [&] { return !state->accepting || !state->queue.empty(); });
            if (state->queue.empty()) break;
            std::pop_heap(state->queue.begin(), state->queue.end(), LowerPriority{});
            job = std::move(state->queue.back()); state->queue.pop_back();
            ++state->totals.running;
        }
        state->space.notify_one();
        const auto id = job->sequence;
        const auto outcome = job->execute();
        job.reset(); // Destrutores de objetos do usuário são executados fora do mutex do motor.
        {
            std::lock_guard lock(state->mutex);
            state->controls.erase(id);
            --state->totals.running;
            switch (outcome) {
                case detail::Outcome::succeeded: ++state->totals.succeeded; break;
                case detail::Outcome::failed: ++state->totals.failed; break;
                case detail::Outcome::cancelled: ++state->totals.cancelled; break;
            }
        }
        state->idle.notify_all();
    }
    current_worker_ = nullptr;
}

void Engine::close(Shutdown mode) {
    if (mode != Shutdown::drain && mode != Shutdown::cancel) throw std::invalid_argument("invalid shutdown mode");
    std::vector<std::unique_ptr<detail::Job>> abandoned;
    std::unordered_map<std::uint64_t, std::stop_source> stops;
    const auto reason = mode == Shutdown::cancel ? std::make_exception_ptr(task_cancelled{}) : std::exception_ptr{};
    {
        std::lock_guard lock(state_->mutex);
        state_->accepting = false;
        if (mode == Shutdown::cancel) {
            abandoned.swap(state_->queue);
            stops.swap(state_->controls);
            // Somente promises são resolvidas aqui; callables não são executados ou destruídos sob o lock.
            for (auto& job : abandoned) job->cancel(reason);
            state_->totals.cancelled += abandoned.size();
        }
    }
    state_->ready.notify_all(); state_->space.notify_all(); state_->idle.notify_all();
    // stop_callback pode ser chamado sincronamente; nunca segure state_->mutex nesta chamada.
    for (auto& [id, source] : stops) { (void) id; source.request_stop(); }
}

void Engine::require_external_thread() const {
    if (current_worker_ == state_.get())
        throw std::logic_error("wait_idle/shutdown must not be called by a worker of this engine");
}

void Engine::join_workers() {
    std::lock_guard lock(join_mutex_);
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
}

void Engine::shutdown(Shutdown mode) { require_external_thread(); close(mode); join_workers(); }

void Engine::wait_idle() {
    require_external_thread();
    std::unique_lock lock(state_->mutex);
    state_->idle.wait(lock, [this] { return state_->queue.empty() && state_->totals.running == 0; });
}

bool Engine::accepting() const { std::lock_guard lock(state_->mutex); return state_->accepting; }

Metrics Engine::metrics() const {
    std::lock_guard lock(state_->mutex);
    auto snapshot = state_->totals; snapshot.queued = state_->queue.size(); return snapshot;
}
} // namespace atlas
