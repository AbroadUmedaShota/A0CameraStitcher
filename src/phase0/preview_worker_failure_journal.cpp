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
// still-running result for the check that produced it, because a 5000 ms wait
// that ran out and a 0 ms look that found the process running mean different
// things.
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
    }
    if (exit_observation.after_both_closes) {
        journal.Record("worker_exit_recheck_after_both_closes");
        RecordProcessCheck(journal, *exit_observation.after_both_closes, "worker_exit_not_observed_at_recheck");
    }
}

bool RecordCloseOutcomeToJournal(PreviewRunJournal& journal, bool closed,
                                 const std::optional<PreviewWorkerFailureObservation>& failure,
                                 const std::array<PreviewWorkerExitObservation, 2>& exits,
                                 bool& details_recorded) noexcept {
    bool complete = true;
    try {
        journal.Record(closed ? "both_workers_close_verified" : "close_unconfirmed");
    } catch (...) {
        complete = false;
    }
    if (details_recorded) return complete;
    details_recorded = true;
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
