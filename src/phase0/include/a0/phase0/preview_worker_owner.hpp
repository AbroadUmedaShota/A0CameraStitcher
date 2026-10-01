#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include "a0/phase0/preview_commissioning.hpp"
#include "a0/phase0/preview_worker_reply.hpp"

namespace a0::phase0::experimental {
struct PreviewWorkerFailureObservation {
    std::size_t worker_index{};
    std::string operation;
    std::string category; // Bounded worker code, never an SDK exception message.
    bool response_received{};
    bool response_validated{};
    bool ack_write_completed{}; // Local write only; not proof of server ACK processing.
    std::optional<PreviewWorkerReplyStatus> response_status;
    std::optional<PreviewWorkerCloseReceipt> reported_close;
};

// One point-in-time look at a child OS process through WaitForSingleObject and
// GetExitCodeProcess only. Deliberately separate from any SDK/IPC claim.
struct PreviewWorkerProcessCheck {
    bool wait_failed{};     // WaitForSingleObject returned neither WAIT_OBJECT_0 nor WAIT_TIMEOUT.
    bool exited{};          // WaitForSingleObject returned WAIT_OBJECT_0.
    bool code_available{};  // GetExitCodeProcess succeeded while exited.
    std::uint32_t exit_code{};
};

// How PreviewWorkerExitObservation::at_close was taken.
enum class PreviewWorkerExitCheckKind {
    // Close() has not run the close step for this child yet (or was called off
    // the owner thread). There is no OS observation.
    not_checked,
    // The close exchange was answered and its ACK written: the existing
    // success-path wait of up to 5000 ms for the process to be signaled.
    waited_after_close_ack,
    // The close exchange threw. This includes a close that was never sent
    // because an earlier command left this child's delivery unconfirmed. The
    // check is a single 0 ms look at the moment of the close attempt, not a
    // wait: a worker still tearing down is reported as not yet exited.
    instant_at_close_failure,
};

// What the parent could independently confirm about one child OS process
// during Close().
struct PreviewWorkerExitObservation {
    PreviewWorkerExitCheckKind kind{PreviewWorkerExitCheckKind::not_checked};
    PreviewWorkerProcessCheck at_close;
    // Present only when kind is instant_at_close_failure and at_close did not
    // see the process exit: a second 0 ms look taken by Close() after the close
    // step of both children has finished. It adds no wait of its own. When the
    // other child went through the success path, that child's close exchange
    // and exit wait is the time this child had to finish exiting; when no child
    // did, the recheck follows the first look almost immediately.
    std::optional<PreviewWorkerProcessCheck> after_both_closes;
};

// Fixed/bounded diagnostic fields only. Never include SDK free-form errors.
[[nodiscard]] std::wstring FormatPreviewWorkerFailure(const PreviewWorkerFailureObservation& failure);

class PreviewWorkerStartupError final : public std::runtime_error {
public:
    explicit PreviewWorkerStartupError(bool workers_may_exist)
        : std::runtime_error("preview worker startup failed"), workers_may_exist_(workers_may_exist) {}
    bool WorkersMayExist() const noexcept { return workers_may_exist_; }
private:
    bool workers_may_exist_;
};
// Internal experimental integration; hardware acceptance remains gated.
// Thread-affine: create, close and destroy on the lease-owning thread.
// Destruction does not close workers or clear an armed quarantine marker.
class PreviewWorkerOwner final {
public:
    PreviewWorkerOwner();
    PreviewWorkerOwner(std::string_view test_lease_name, const std::filesystem::path& test_marker_root,
                       const std::filesystem::path& worker_executable, std::chrono::milliseconds lifetime,
                       std::function<void(std::array<std::uint32_t, 2>)> after_spawn_for_testing = {},
                       bool inject_ack_write_failure_for_testing = false);
    ~PreviewWorkerOwner();
    PreviewWorkerOwner(const PreviewWorkerOwner&) = delete;
    PreviewWorkerOwner& operator=(const PreviewWorkerOwner&) = delete;
    // Cached result on repeated calls: never resends an ambiguous close.
    bool Close() noexcept;
    [[nodiscard]] std::optional<PreviewWorkerFailureObservation> FirstFailure() const;
    // Per-child OS exit evidence gathered by Close() (kind not_checked before
    // that). Available regardless of whether Close() returned true, so a
    // failure path can still persist both workers' exit evidence.
    [[nodiscard]] std::array<PreviewWorkerExitObservation, 2> ExitObservations() const noexcept;
    std::array<std::uint32_t, 2> ProcessIds() const noexcept;
    std::array<std::string, 2> Enumerate(std::size_t worker);
    std::vector<unsigned char> Preview(std::size_t worker, std::string_view candidate);
    void ConfirmAndSuspend(std::size_t worker, ObservedPreviewBody body);
    void StartBoth();
    std::vector<unsigned char> Read(ObservedPreviewBody body);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace a0::phase0::experimental
