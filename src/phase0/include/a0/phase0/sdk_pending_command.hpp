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
    // The caller may treat the buffer handed to the SDK as safe to release
    // -- but note this rests on the same kind of unconfirmed vendor-behavior
    // assumption as the Abort-return-value skepticism above: that the
    // completion callback firing means the SDK is truly done touching the
    // buffer, not merely that it has scheduled more work against it. Stage 2
    // (moving buffer ownership into the transport, see #145) makes this
    // assumption moot rather than resolving it.
    completed,
    // The completion callback never fired within the grace period. The
    // buffer handed to the SDK must NOT be released; the caller is
    // responsible for quarantining the session instead.
    quarantined,
};

// Early-exit policy for a pending asynchronous SDK command: issue Abort
// exactly once, then poll for the completion evidence (`done`) for up to
// `max_iterations` bounded steps, sleeping between steps via `sleep`.
// `max_iterations <= 0` skips polling entirely and only performs the single
// post-loop completion check below.
//
// This function is noexcept: it is itself called from error/cleanup paths
// with nothing better to fall back to, and its caller must be able to rely
// on it never propagating. Every callback is invoked from inside its own
// try/catch for that reason -- including `done` and `sleep`, which are not
// contractually noexcept in general (std::this_thread::sleep_for is not, for
// instance). An exception escaping here would call std::terminate before
// the transport's own cleanup (e.g. Close()) can run, which is exactly the
// "camera left stuck" failure mode this policy exists to avoid. A callback
// that throws is treated as "no information" (not-done / step-not-taken)
// rather than propagated.
template <typename AbortFn, typename PumpFn, typename DoneFn, typename SleepFn>
[[nodiscard]] AbandonOutcome AbandonPendingCommand(
    AbortFn&& abort, PumpFn&& pump, DoneFn&& done, SleepFn&& sleep,
    int max_iterations) noexcept {
    try {
        abort();
    } catch (...) {
        // Already unwinding from a failure; there is no better recovery than
        // to keep going and rely on the completion poll below.
    }

    const auto is_done = [&done]() noexcept -> bool {
        try {
            return done();
        } catch (...) {
            // An exception from the completion predicate carries no usable
            // information either way; treat it as "not yet observed" and
            // keep polling rather than let it escape.
            return false;
        }
    };

    for (int iteration = 0; iteration < max_iterations; ++iteration) {
        try {
            pump();
        } catch (...) {
            // A pump failure here does not change the plan: keep polling for
            // the completion callback regardless of why pumping failed.
        }
        if (is_done()) return AbandonOutcome::completed;
        try {
            sleep();
        } catch (...) {
            // Same reasoning as pump(): a failed sleep does not change the
            // plan, just the pacing of the remaining iterations.
        }
    }

    // The completion callback may have fired during the last sleep, after
    // the last in-loop check. Recheck once more before giving up.
    return is_done() ? AbandonOutcome::completed : AbandonOutcome::quarantined;
}

} // namespace a0::phase0
