#pragma once

#include "a0/phase0/preview_run_journal.hpp"
#include "a0/phase0/preview_worker_owner.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace a0::phase0::experimental {

// Commands PreviewCommissioning/PreviewWorkerOwner pass as
// PreviewWorkerFailureObservation::operation. Journaled as
// "failure_operation_<name>"; anything else becomes "failure_operation_unknown".
inline constexpr std::string_view kPreviewWorkerJournalOperations[] = {
    "enumerate", "select", "start", "frame", "suspend", "resume", "close", "disarm",
};

// Every failure category the parent recognizes and copies verbatim into the
// journal as its own event. Sources: the owner/IPC layer
// (preview_worker_owner.cpp, preview_worker_reply.hpp), the worker protocol
// layer (worker_preview_dispatcher.hpp, worker_preview_selection.hpp), and the
// SDK transport layer (nikon_sdk_transport.cpp): its worker preview path, its
// gated (SDK-stub) build, and 7 capture-path categories that the preview path
// cannot reach but that stay here as fixed vocabulary. A category reported by
// the worker but absent here is journaled as "failure_category_unknown", never
// verbatim.
//
// When a TransportError category is added on any of those worker paths, add it
// here as well; the "--category-table" mode of a0_preview_worker_owner_tests
// checks that every entry is unique and accepted by PreviewRunJournal.
inline constexpr std::string_view kPreviewWorkerFailureCategories[] = {
    // Owner / IPC layer (parent side).
    "delegation_disarm_unconfirmed",
    "owner_operation_deadline_expired",
    "worker_ack_unconfirmed",
    "worker_exit_unconfirmed",
    "worker_ipc_unconfirmed",
    "worker_operation_failed",
    "worker_owner_failed",
    "worker_prior_exit",
    "worker_reply_invalid",
    "worker_stop_in_progress",
    // Worker protocol layer (dispatcher and candidate selection).
    "worker_authority",
    "worker_authority_expired",
    "worker_authority_missing",
    "worker_candidate",
    "worker_candidate_unavailable",
    "worker_envelope",
    "worker_frame_size",
    "worker_handoff_unavailable",
    "worker_inventory",
    "worker_inventory_not_pair",
    "worker_operation_unavailable",
    "worker_selection_invalidated",
    "worker_sequence",
    "worker_terminal",
    "worker_unexpected",
    // SDK transport layer, worker preview path.
    "close_failed",
    "live_view_frame_failed",
    "live_view_invalid_frame",
    "live_view_not_started",
    "live_view_prohibited",
    "live_view_recovery_failed",
    "live_view_start_failed",
    "live_view_stop_already_attempted",
    "live_view_stop_failed",
    "live_view_unavailable",
    "open_failed",
    "sdk_load_failed",
    "session_busy",
    "session_mode_mismatch",
    "session_not_open",
    "session_poisoned",
    "worker_camera_type_failed",
    "worker_camera_type_mismatch",
    "worker_handoff_unconfirmed",
    "worker_inventory_failed",
    "worker_live_view_already_on",
    "worker_selection_required",
    "worker_session_required",
    "worker_source_required",
    "worker_stop_required",
    "worker_topology_changed",
    "worker_topology_failed",
    // SDK transport layer, gated build: every adapter call of a build without
    // the licensed SDK throws this.
    "licensed_adapter_unavailable",
    // SDK transport layer, capture path: not reachable from the worker preview
    // path, kept as fixed vocabulary.
    "ambiguous_image_event",
    "baseline_mismatch",
    "download_failed",
    "identity_collision",
    "image_event_timeout",
    "invalid_jpeg",
    "transaction_watchdog",
};

// Membership in kPreviewWorkerJournalOperations.
//
// Every caller of Exchange() passes the operation as a string literal from our
// own source (PreviewCommissioning, CloseChild, the disarm path), so this check
// is defense in depth that keeps the journal vocabulary closed, not a filter for
// untrusted text. If Exchange() ever accepts an operation derived from input
// (UI, IPC, configuration), revisit this assumption together with that change.
[[nodiscard]] bool IsKnownOperation(std::string_view operation) noexcept;

// Membership in kPreviewWorkerFailureCategories.
[[nodiscard]] bool IsKnownFailureCategory(std::string_view category) noexcept;

// Writes a bounded, fixed-vocabulary trace of a worker failure: which worker,
// which command, which known failure category, the response/status/ACK state,
// and the worker-reported close receipt. Every value is either a fixed ASCII
// event name accepted by PreviewRunJournal or a plain numeric value already
// carried on PreviewWorkerFailureObservation. Never writes SDK free-form text,
// nonces, PIDs, serials, or paths. Throws whatever PreviewRunJournal::Record
// throws; the first failed line stops the block.
void RecordWorkerFailureToJournal(PreviewRunJournal& journal, const PreviewWorkerFailureObservation& failure);

// Writes the OS-observed terminal state of one worker process (see
// PreviewWorkerExitObservation): which exit window was applied, the result
// inside it, worker_exit_window_capped_by_session_limit and
// worker_exit_looked_after_window_end when those flags are set, the later 0 ms
// rechecks when present, and then a summary of the close reply the parent
// received from this worker (worker_close_not_sent when close was never
// attempted, worker_close_not_delivered when the request was not written in
// full, otherwise status, ACK, and the five worker-reported receipt fields).
// Events are only appended; a recheck never rewrites an earlier line.
// worker_index is expected to be 0 or 1. Throws like
// RecordWorkerFailureToJournal.
void RecordWorkerExitObservationToJournal(PreviewRunJournal& journal, std::size_t worker_index,
                                           const PreviewWorkerExitObservation& exit_observation);

// What RecordCloseOutcomeToJournal has already written for one run.
struct PreviewCloseJournalState {
    bool details_recorded{};
    // PreviewWorkerExitObservation::repeated_close_looks already journaled, per worker.
    std::array<std::uint32_t, 2> repeated_close_looks_recorded{};
};

// The journal part of the commissioning display's close handling, shared by
// preview_commissioning_main.cpp and its tests so that the tested order is the
// production order:
//   closed:     both_workers_close_verified, then the exit block of worker 0 and 1
//   not closed: close_unconfirmed, then the failure block (or
//               failure_observation_missing), then the exit block of worker 0 and 1
// The verdict line is written on every call. The failure and exit blocks are
// written only on the first call (state.details_recorded), so repeated close
// attempts never duplicate them. On a later call, a worker whose repeated
// Close() took a new 0 ms look since the last write gets a short block after
// the verdict: worker_exit_observation_index, worker_exit_recheck_at_repeated_close,
// and its result.
// Never throws: a journal failure must not interfere with the camera close that
// already happened. Returns true only if every line attempted by this call was
// written.
[[nodiscard]] bool RecordCloseOutcomeToJournal(PreviewRunJournal& journal, bool closed,
                                               const std::optional<PreviewWorkerFailureObservation>& failure,
                                               const std::array<PreviewWorkerExitObservation, 2>& exits,
                                               PreviewCloseJournalState& state) noexcept;

} // namespace a0::phase0::experimental
