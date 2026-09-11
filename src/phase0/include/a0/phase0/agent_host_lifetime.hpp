#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

namespace a0::phase0 {

// One monotonic origin and immutable budget, shared across a binding-to-capture
// transition. No wall clock, rolling deadline, addition overflow, or hard kill.
class AgentHostLifetime final {
public:
    using Clock = std::function<std::uint64_t()>;
    static constexpr std::uint64_t kDefaultBudgetMilliseconds = 600000;

    explicit AgentHostLifetime(
        std::uint64_t budget_milliseconds = kDefaultBudgetMilliseconds,
        Clock clock = {})
        : budget_(budget_milliseconds), clock_(clock ? std::move(clock) : Clock{[] {
              return static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
          }}) {
        try { started_ = clock_(); }
        catch (...) { expired_ = true; }
    }

    [[nodiscard]] std::uint64_t RemainingMilliseconds() noexcept {
        if (expired_) return 0;
        try {
            // Modular subtraction permits a tick wrap during this short budget.
            // A backwards clock or any clock exception is terminal, not an
            // opportunity to replenish a previously spent host budget.
            const std::uint64_t elapsed = clock_() - started_;
            if (elapsed < last_elapsed_ || elapsed >= budget_) {
                expired_ = true;
                return 0;
            }
            last_elapsed_ = elapsed;
            return budget_ - elapsed;
        } catch (...) {
            expired_ = true;
            return 0;
        }
    }

    [[nodiscard]] const Clock& TickSource() const noexcept { return clock_; }

private:
    std::uint64_t budget_{};
    Clock clock_;
    std::uint64_t started_{};
    std::uint64_t last_elapsed_{};
    bool expired_{};
};

} // namespace a0::phase0
