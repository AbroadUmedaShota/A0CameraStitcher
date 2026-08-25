#pragma once

// This header deliberately has no dependency on the Nikon MAID SDK (or any
// vendor headers). The abandon-on-timeout policy below is pure control flow,
// and keeping it out from behind the A0_NIKON_SDK_AVAILABLE gate is what lets
// it compile and get unit-tested without the licensed SDK installed.
//
// Naming note: kNkMAIDCommand_Abort's own return value is NOT used as the
// signal that the SDK finished unwinding. Whether MAID guarantees synchronous
// completion for Abort is unconfirmed, so this policy treats Abort as
// "fire and hope" and instead waits for the caller-supplied completion
// callback (the `done` predicate below) to observe the outcome.

namespace a0::phase0 {

// Outcome of abandoning an in-flight asynchronous SDK command.
enum class AbandonOutcome {
    // The SDK's completion callback fired before the grace period ran out.
    // Whatever buffer was handed to the SDK is safe to release.
    completed,
    // The completion callback never fired within the grace period. The
    // buffer handed to the SDK must NOT be released; the caller is
    // responsible for quarantining the session instead.
    quarantined,
};

// Early-exit policy for a pending asynchronous SDK command: issue Abort
// exactly once, then poll for the completion evidence (`done`) for up to
// `max_iterations` bounded steps, sleeping between steps via `sleep`.
//
// `abort` and `pump` are called from an error path that has no further
// recovery available if they themselves fail, so exceptions raised by either
// are swallowed here rather than propagated. `done` and `sleep` are expected
// not to throw.
template <typename AbortFn, typename PumpFn, typename DoneFn, typename SleepFn>
AbandonOutcome AbandonPendingCommand(
    AbortFn&& abort, PumpFn&& pump, DoneFn&& done, SleepFn&& sleep,
    int max_iterations) noexcept {
    try {
        abort();
    } catch (...) {
        // Already unwinding from a failure; there is no better recovery than
        // to keep going and rely on the completion poll below.
    }

    for (int iteration = 0; iteration < max_iterations; ++iteration) {
        try {
            pump();
        } catch (...) {
            // A pump failure here does not change the plan: keep polling for
            // the completion callback regardless of why pumping failed.
        }
        if (done()) return AbandonOutcome::completed;
        sleep();
    }

    // The completion callback may have fired during the last sleep, after
    // the last in-loop check. Recheck once more before giving up.
    return done() ? AbandonOutcome::completed : AbandonOutcome::quarantined;
}

} // namespace a0::phase0
