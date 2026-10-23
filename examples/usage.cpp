#include "atlas/engine.hpp"

#include <iostream>
#include <memory>
#include <string>

int main() {
    atlas::Engine engine({.workers = 2, .capacity = 8});

    // Callable e argumento move-only; resultado também é um unique_ptr.
    auto owned = engine.submit(atlas::Priority::high,
        [base = std::make_unique<int>(40)](std::stop_token token, std::unique_ptr<int> increment) {
            atlas::cancellation_point(token);
            return std::make_unique<int>(*base + *increment);
        }, std::make_unique<int>(2));

    auto text = engine.submit(atlas::Priority::normal,
        [](std::stop_token, std::string name) { return "Hello, " + name; }, std::string{"C++20"});

    auto failure = engine.submit(atlas::Priority::low, [](std::stop_token) -> int {
        throw std::domain_error("example of exception transported through future");
    });

    if (*owned.get() != 42 || text.get() != "Hello, C++20") return 1;
    try { (void) failure.get(); return 1; }
    catch (const std::domain_error& error) { std::cout << "Expected exception: " << error.what() << '\n'; }
    engine.shutdown(atlas::Shutdown::drain);
    const auto metrics = engine.metrics();
    std::cout << "Move-only result: 42\nSucceeded: " << metrics.succeeded << " | Failed: " << metrics.failed << '\n';
    return metrics.succeeded == 2 && metrics.failed == 1 && metrics.consistent() ? 0 : 1;
}
