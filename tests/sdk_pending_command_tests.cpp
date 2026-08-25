// AbandonPendingCommand contracts (GitHub Issue #145, stage 1).
//
// The MAID implementation that calls this policy lives behind
// A0_NIKON_SDK_AVAILABLE and is not compiled without the licensed Nikon SDK,
// so these tests exercise the policy directly instead: exactly one Abort,
// bounded pumping for completion evidence, and no exception from abort,
// pump, done, or sleep escaping the (noexcept) function.
//
// Not covered here: the decision of *whether* to poll at all based on
// whether the SDK actually confirmed the async command started. That
// decision lives one layer up, in nikon_sdk_transport.cpp's AbandonPending
// wrapper (MAID-gated, untestable without the licensed SDK), because it
// depends on the MAID immediate-result contract, not on this policy.

#include "a0/phase0/sdk_pending_command.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace a0::phase0;

namespace {

int failures = 0;

void Check(const bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void AbortIsCalledExactlyOnce() {
    int abort_calls = 0;
    int pump_calls = 0;
    const AbandonOutcome outcome = AbandonPendingCommand(
        [&] { ++abort_calls; },
        [&] { ++pump_calls; },
        [&] { return pump_calls >= 5; },
        [] {},
        10);
    Check(abort_calls == 1, "abort is invoked exactly once regardless of how long pumping runs");
    Check(outcome == AbandonOutcome::completed, "sanity: completion evidence within the budget yields completed");
}

void DoneOnThirdIterationReturnsCompletedAndStopsPumping() {
    int pump_calls = 0;
    const AbandonOutcome outcome = AbandonPendingCommand(
        [] {},
        [&] { ++pump_calls; },
        [&] { return pump_calls == 3; },
        [] {},
        25);
    Check(outcome == AbandonOutcome::completed, "completion evidence observed after the third pump yields completed");
    Check(pump_calls == 3, "pumping stops as soon as completion evidence is observed, not before or after");
}

void DoneNeverTrueExhaustsMaxIterationsAndQuarantines() {
    int pump_calls = 0;
    constexpr int kMaxIterations = 7;
    const AbandonOutcome outcome = AbandonPendingCommand(
        [] {},
        [&] { ++pump_calls; },
        [] { return false; },
        [] {},
        kMaxIterations);
    Check(outcome == AbandonOutcome::quarantined, "completion evidence that never arrives yields quarantined");
    Check(pump_calls == kMaxIterations, "pumping runs exactly max_iterations times before giving up");
}

void AbortExceptionIsSwallowedAndPumpingStillProceeds() {
    bool abort_invoked = false;
    int pump_calls = 0;
    const AbandonOutcome outcome = AbandonPendingCommand(
        [&]() {
            abort_invoked = true;
            throw std::runtime_error("simulated Abort failure");
        },
        [&] { ++pump_calls; },
        [&] { return pump_calls >= 2; },
        [] {},
        10);
    Check(abort_invoked, "abort is called even though this run makes it throw");
    Check(outcome == AbandonOutcome::completed,
        "an abort exception is swallowed rather than aborting the policy itself");
}

void PumpExceptionEveryIterationStillExhaustsMaxIterationsAndQuarantines() {
    int pump_calls = 0;
    constexpr int kMaxIterations = 6;
    const AbandonOutcome outcome = AbandonPendingCommand(
        [] {},
        [&]() {
            ++pump_calls;
            throw std::runtime_error("simulated Pump failure");
        },
        [] { return false; },
        [] {},
        kMaxIterations);
    Check(outcome == AbandonOutcome::quarantined,
        "a pump that always throws still ends in quarantined rather than a propagated exception");
    Check(pump_calls == kMaxIterations, "every iteration still pumps despite pump throwing each time");
}

void DoneBecomingTrueAfterTheFinalSleepIsStillObservedByTheBoundaryRecheck() {
    int pump_calls = 0;
    int sleep_calls = 0;
    bool completion_arrived = false;
    constexpr int kMaxIterations = 4;
    const AbandonOutcome outcome = AbandonPendingCommand(
        [] {},
        [&] { ++pump_calls; },
        [&] { return completion_arrived; },
        [&] {
            ++sleep_calls;
            // Simulate the completion callback firing during the grace-period
            // sleep that follows the very last pump, i.e. after every
            // in-loop done() check has already run.
            if (sleep_calls == kMaxIterations) completion_arrived = true;
        },
        kMaxIterations);
    Check(outcome == AbandonOutcome::completed,
        "completion evidence arriving during the last sleep is still caught by the post-loop recheck");
    Check(pump_calls == kMaxIterations, "all max_iterations pumps ran before the boundary recheck fired");
    Check(sleep_calls == kMaxIterations, "the final grace-period sleep still ran before the boundary recheck");
}

void DoneExceptionEveryIterationStillExhaustsMaxIterationsAndQuarantines() {
    int pump_calls = 0;
    constexpr int kMaxIterations = 5;
    const AbandonOutcome outcome = AbandonPendingCommand(
        [] {},
        [&] { ++pump_calls; },
        []() -> bool { throw std::runtime_error("simulated done() failure"); },
        [] {},
        kMaxIterations);
    Check(outcome == AbandonOutcome::quarantined,
        "a done() that always throws is treated as not-yet-observed, not propagated out of a noexcept function");
    Check(pump_calls == kMaxIterations, "pumping still runs to completion when done() always throws");
}

void SleepExceptionEveryIterationStillExhaustsMaxIterationsAndQuarantines() {
    int pump_calls = 0;
    int sleep_calls = 0;
    constexpr int kMaxIterations = 5;
    const AbandonOutcome outcome = AbandonPendingCommand(
        [] {},
        [&] { ++pump_calls; },
        [] { return false; },
        [&]() {
            ++sleep_calls;
            throw std::runtime_error("simulated sleep() failure");
        },
        kMaxIterations);
    Check(outcome == AbandonOutcome::quarantined,
        "a sleep() that always throws does not propagate out of a noexcept function");
    Check(pump_calls == kMaxIterations, "pumping still runs to completion when sleep() always throws");
    Check(sleep_calls == kMaxIterations, "sleep is still invoked each iteration despite throwing every time");
}

} // namespace

int main() {
    AbortIsCalledExactlyOnce();
    DoneOnThirdIterationReturnsCompletedAndStopsPumping();
    DoneNeverTrueExhaustsMaxIterationsAndQuarantines();
    AbortExceptionIsSwallowedAndPumpingStillProceeds();
    PumpExceptionEveryIterationStillExhaustsMaxIterationsAndQuarantines();
    DoneBecomingTrueAfterTheFinalSleepIsStillObservedByTheBoundaryRecheck();
    DoneExceptionEveryIterationStillExhaustsMaxIterationsAndQuarantines();
    SleepExceptionEveryIterationStillExhaustsMaxIterationsAndQuarantines();

    if (failures != 0) {
        std::cerr << failures << " AbandonPendingCommand contract failures\n";
        return 1;
    }
    std::cout << "AbandonPendingCommand contracts passed\n";
    return 0;
}
