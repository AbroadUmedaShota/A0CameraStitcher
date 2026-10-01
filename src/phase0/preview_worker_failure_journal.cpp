#include "a0/phase0/preview_worker_failure_journal.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

namespace a0::phase0::experimental {
namespace {

std::uint64_t Flag(bool value) { return value ? 1U : 0U; }

std::string_view StatusEvent(const std::optional<PreviewWorkerReplyStatus>& status) {
    if (!status) return "worker_status_missing";
    switch (*status) {
    case PreviewWorkerReplyStatus::ok: return "worker_status_ok";
    case PreviewWorkerReplyStatus::failed: return "worker_status_failed";
    case PreviewWorkerReplyStatus::closed: return "worker_status_closed";
    case PreviewWorkerReplyStatus::quarantined: return "worker_status_quarantined";
    }
    return "worker_status_unknown";
}

// One process check, as a single result line. `not_exited_event` names the
// still-running result for the check that produced it, because a wait that ran
// out and a 0 ms look that found the process running mean different things.
void RecordProcessCheck(PreviewRunJournal& journal, const PreviewWorkerProcessCheck& check,
                        std::string_view not_exited_event) {
    if (check.wait_failed) {
        journal.Record("worker_exit_wait_failed");
    } else if (check.exited && check.code_available) {
        journal.Record("worker_exit_code", check.exit_code);
    } else if (check.exited) {
        journal.Record("worker_exit_code_unavailable");
    } else {
        journal.Record(not_exited_event);
    }
}

std::string_view CloseStatusEvent(PreviewWorkerReplyStatus status) {
    switch (status) {
    case PreviewWorkerReplyStatus::closed: return "worker_close_status_closed";
    case PreviewWorkerReplyStatus::quarantined: return "worker_close_status_quarantined";
    case PreviewWorkerReplyStatus::failed: return "worker_close_status_failed";
    case PreviewWorkerReplyStatus::ok: break; // Rejected for close by ParsePreviewWorkerReply.
    }
    return "worker_close_status_unknown";
}

// The close reply of one worker, right after its exit lines.
void RecordCloseReply(PreviewRunJournal& journal, const PreviewWorkerCloseReply& reply) {
    if (!reply.attempted) {
        journal.Record("worker_close_not_sent");
        return;
    }
    if (!reply.sent) {
        // Attempted, but the request never left the parent in full.
        journal.Record("worker_close_not_delivered");
        return;
    }
    if (reply.response_validated && reply.status) {
        journal.Record(CloseStatusEvent(*reply.status));
        journal.Record(reply.ack_write_completed ? "worker_close_ack_write_completed"
                                                 : "worker_close_ack_write_unconfirmed");
    } else {
        journal.Record(reply.response_received ? "worker_close_response_invalid" : "worker_close_response_missing");
    }
    if (reply.receipt) {
        const auto& receipt = *reply.receipt;
        journal.Record("worker_close_receipt_live_view_off", Flag(receipt.live_view_off));
        journal.Record("worker_close_receipt_source_closed", Flag(receipt.source_closed));
        journal.Record("worker_close_receipt_module_closed", Flag(receipt.module_closed));
        journal.Record("worker_close_receipt_process_claim_released", Flag(receipt.process_claim_released));
        journal.Record("worker_close_receipt_safe_to_exit", Flag(receipt.safe_to_exit));
    } else {
        journal.Record("worker_close_receipt_missing");
    }
}

void RecordRepeatedCloseLook(PreviewRunJournal& journal, const PreviewWorkerProcessCheck& check) {
    journal.Record("worker_exit_recheck_at_repeated_close");
    RecordProcessCheck(journal, check, "worker_exit_not_observed_at_repeated_close");
}

} // namespace

bool IsKnownOperation(std::string_view operation) noexcept {
    return std::find(std::begin(kPreviewWorkerJournalOperations), std::end(kPreviewWorkerJournalOperations),
                     operation) != std::end(kPreviewWorkerJournalOperations);
}

bool IsKnownFailureCategory(std::string_view category) noexcept {
    return std::find(std::begin(kPreviewWorkerFailureCategories), std::end(kPreviewWorkerFailureCategories),
                     category) != std::end(kPreviewWorkerFailureCategories);
}

void RecordWorkerFailureToJournal(PreviewRunJournal& journal, const PreviewWorkerFailureObservation& failure) {
    // 0 or 1 is a worker; 2 (== number of workers) is the owner-wide disarm step.
    journal.Record("failure_worker_index", static_cast<std::uint64_t>(failure.worker_index));

    if (IsKnownOperation(failure.operation)) {
        journal.Record("failure_operation_" + failure.operation);
    } else {
        journal.Record("failure_operation_unknown");
    }

    if (failure.category.empty()) {
        journal.Record("failure_category_missing");
    } else if (IsKnownFailureCategory(failure.category)) {
        journal.Record(failure.category);
    } else {
        journal.Record("failure_category_unknown");
    }

    journal.Record(failure.response_validated ? "worker_response_validated" :
                    failure.response_received ? "worker_response_invalid" : "worker_response_missing");
    journal.Record(StatusEvent(failure.response_status));
    journal.Record(failure.ack_write_completed ? "worker_ack_write_completed" : "worker_ack_write_unconfirmed");

    if (failure.reported_close) {
        const auto& receipt = *failure.reported_close;
        journal.Record("close_receipt_live_view_off", Flag(receipt.live_view_off));
        journal.Record("close_receipt_source_closed", Flag(receipt.source_closed));
        journal.Record("close_receipt_module_closed", Flag(receipt.module_closed));
        journal.Record("close_receipt_process_claim_released", Flag(receipt.process_claim_released));
        journal.Record("close_receipt_safe_to_exit", Flag(receipt.safe_to_exit));
    } else {
        journal.Record("close_receipt_missing");
    }
}

void RecordWorkerExitObservationToJournal(PreviewRunJournal& journal, std::size_t worker_index,
                                           const PreviewWorkerExitObservation& exit_observation) {
    journal.Record("worker_exit_observation_index", static_cast<std::uint64_t>(worker_index));
    switch (exit_observation.kind) {
    case PreviewWorkerExitCheckKind::not_checked:
        journal.Record("worker_exit_check_not_run");
        return;
    case PreviewWorkerExitCheckKind::waited_after_close_ack:
        journal.Record("worker_exit_check_waited_after_close_ack");
        RecordProcessCheck(journal, exit_observation.at_close, "worker_exit_wait_timed_out");
        break;
    case PreviewWorkerExitCheckKind::instant_at_close_failure:
        journal.Record("worker_exit_check_instant_at_close_failure");
        RecordProcessCheck(journal, exit_observation.at_close, "worker_exit_not_observed_at_close");
        break;
    case PreviewWorkerExitCheckKind::waited_after_failure_receipt:
        journal.Record("worker_exit_check_waited_after_failure_receipt");
        RecordProcessCheck(journal, exit_observation.at_close, "worker_exit_wait_timed_out");
        break;
    case PreviewWorkerExitCheckKind::waited_for_worker_cleanup:
        journal.Record("worker_exit_check_waited_for_worker_cleanup");
        RecordProcessCheck(journal, exit_observation.at_close, "worker_exit_wait_timed_out");
        break;
    }
    // Where the window ended and when the look started; diagnostic lines only.
    if (exit_observation.window_capped_by_session_limit) journal.Record("worker_exit_window_capped_by_session_limit");
    if (exit_observation.looked_after_window_end) journal.Record("worker_exit_looked_after_window_end");
    if (exit_observation.after_both_closes) {
        journal.Record("worker_exit_recheck_after_both_closes");
        RecordProcessCheck(journal, *exit_observation.after_both_closes, "worker_exit_not_observed_at_recheck");
    }
    if (exit_observation.at_repeated_close) RecordRepeatedCloseLook(journal, *exit_observation.at_repeated_close);
    if (exit_observation.close_reply) RecordCloseReply(journal, *exit_observation.close_reply);
}

bool RecordCloseOutcomeToJournal(PreviewRunJournal& journal, bool closed,
                                 const std::optional<PreviewWorkerFailureObservation>& failure,
                                 const std::array<PreviewWorkerExitObservation, 2>& exits,
                                 PreviewCloseJournalState& state) noexcept {
    bool complete = true;
    try {
        journal.Record(closed ? "both_workers_close_verified" : "close_unconfirmed");
    } catch (...) {
        complete = false;
    }
    if (state.details_recorded) {
        // Later calls: only new looks taken by a repeated Close().
        for (std::size_t worker = 0; worker < exits.size(); ++worker) {
            const auto& exit = exits[worker];
            if (!exit.at_repeated_close || exit.repeated_close_looks <= state.repeated_close_looks_recorded[worker])
                continue;
            state.repeated_close_looks_recorded[worker] = exit.repeated_close_looks;
            try {
                journal.Record("worker_exit_observation_index", static_cast<std::uint64_t>(worker));
                RecordRepeatedCloseLook(journal, *exit.at_repeated_close);
            } catch (...) {
                complete = false;
            }
        }
        return complete;
    }
    state.details_recorded = true;
    for (std::size_t worker = 0; worker < exits.size(); ++worker)
        state.repeated_close_looks_recorded[worker] = exits[worker].repeated_close_looks;
    if (!closed) {
        try {
            if (failure) RecordWorkerFailureToJournal(journal, *failure);
            else journal.Record("failure_observation_missing");
        } catch (...) {
            complete = false;
        }
    }
    for (std::size_t worker = 0; worker < exits.size(); ++worker) {
        try {
            RecordWorkerExitObservationToJournal(journal, worker, exits[worker]);
        } catch (...) {
            complete = false;
        }
    }
    return complete;
}

} // namespace a0::phase0::experimental
