#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace a0::phase0 {
// Read-only preflight for a stranded dual-delegation marker. A positive result
// is only a candidate for human review; it never authorizes deletion or SDK use.
struct DualDelegationMarkerDiagnostic final {
    std::string status;
    std::string anonymous_sha256;
    unsigned long long size{};
};
[[nodiscard]] DualDelegationMarkerDiagnostic InspectDualDelegationMarkerReadOnly(
    const std::filesystem::path &test_marker_root = {});

// Audited one-shot recovery of a single stranded dual-delegation marker
// (docs/design/dual-preview-topology-diag.md section 9). It never constructs a
// HardwareProcessLease: the camera-control mutex is taken only inside these
// functions, and the only change they can make is deleting the one canonical
// marker whose anonymous SHA-256 a human approved, through the same exclusive
// handle that re-verified it. Status values are fixed tokens (section 9.9).
enum class DualDelegationRecoveryProbe : unsigned char { absent, present, unavailable };
enum class DualDelegationRecoveryTestPoint : unsigned char {
    between_marker_snapshots,
    before_exclusive_open,
    before_disposition,
};
// Test seams. The layout is the same in every build so that the library and
// its callers agree on it; a library compiled with the licensed SDK rejects any
// non-default value before touching the file system or the mutex, and the
// SDK-enabled CLI has no argument that can set one.
struct DualDelegationMarkerRecoveryOptions final {
    std::filesystem::path test_marker_root;
    std::string test_lease_name;
    std::function<DualDelegationRecoveryProbe()> test_camera_probe;
    std::function<DualDelegationRecoveryProbe()> test_a0_process_probe;
    std::function<std::chrono::system_clock::time_point()> test_now;
    std::function<void(DualDelegationRecoveryTestPoint)> test_hook;
};
struct DualDelegationMarkerRecoveryResult final {
    std::string status;
    std::string anonymous_sha256; // set only once the marker was read
    unsigned long long size{};
    bool session_match{};
};
// Read-only apart from one appended audit line on success. A
// `recovery_eligible` result is material for a human decision, not a grant.
[[nodiscard]] DualDelegationMarkerRecoveryResult DryRunDualDelegationMarkerRecovery(
    const DualDelegationMarkerRecoveryOptions &options, std::string_view expected_sha256 = {});
// Deletes the marker at most once per anonymous SHA-256; never retries.
// `operator_confirmed_foreign_session` must be true exactly when the marker
// name carries a Windows session ID other than the current one.
[[nodiscard]] DualDelegationMarkerRecoveryResult ExecuteDualDelegationMarkerRecovery(
    const DualDelegationMarkerRecoveryOptions &options, std::string_view expected_sha256,
    bool operator_attested_cameras_disconnected, bool operator_confirmed_foreign_session);

struct WorkerDelegationCloseEvidence final {
    bool live_view_off{};
    bool source_closed{};
    bool module_closed{};
    // Borrowed process handle, kept open by the controller until disarm completes.
    void *worker_process{};
    [[nodiscard]] bool Complete() const noexcept {
        return live_view_off && source_closed && module_closed && worker_process != nullptr;
    }
};
// SDK close flags remain caller assertions; process termination is verified by the lease.
struct DualDelegationCloseEvidence final {
    WorkerDelegationCloseEvidence camera_a;
    WorkerDelegationCloseEvidence camera_b;
    [[nodiscard]] bool Complete() const noexcept {
        return camera_a.Complete() && camera_b.Complete();
    }
};

// Serializes all real-camera Phase 0 commands across processes in the current
// interactive Windows logon session. Cross-session/service enforcement is an
// installation policy concern. Per-transport session guards are still needed.
// The durable dual-delegation marker check at construction scans every
// Windows session sharing the current user profile's marker root, not just
// the current session; only the in-process mutex wait is scoped to one
// session at a time.
class HardwareProcessLease final {
  public:
    explicit HardwareProcessLease(std::string_view lease_name = "A0CameraStitcher.Phase0.CameraControl.v1",
                                  std::chrono::milliseconds wait = std::chrono::milliseconds::zero());
    HardwareProcessLease(std::string_view lease_name, std::chrono::milliseconds wait,
                         const std::filesystem::path &test_marker_root);
    ~HardwareProcessLease();

    HardwareProcessLease(const HardwareProcessLease &) = delete;
    HardwareProcessLease &operator=(const HardwareProcessLease &) = delete;
    HardwareProcessLease(HardwareProcessLease &&) = delete;
    HardwareProcessLease &operator=(HardwareProcessLease &&) = delete;

    [[nodiscard]] bool RecoveredAbandonedOwner() const noexcept;
    void ArmDualDelegation();
    // Call once after spawning, before granting SDK access. Keeps non-inherited duplicates.
    void RegisterDualWorkers(void *camera_a_process, void *camera_b_process);
    // Opaque, per-delegation value for the two registered workers. It is not a
    // camera identity and is unavailable until registration is durable.
    [[nodiscard]] const std::string &DelegationEpoch() const;
    // A delegated worker calls this before constructing an SDK transport and
    // again around each SDK command. It intentionally does not take the mutex:
    // it proves that the controller still owns it. Test roots are accepted only
    // with a test lease and must never be supplied by an SDK-enabled executable.
    [[nodiscard]] static bool ValidateWorkerDelegation(
        void *inherited_parent_process, std::string_view epoch,
        std::string_view lease_name = "A0CameraStitcher.Phase0.CameraControl.v1",
        const std::filesystem::path &test_marker_root = {});
    void DisarmDualDelegation(const DualDelegationCloseEvidence &evidence);
    [[nodiscard]] bool DualDelegationArmed() const noexcept;

  private:
    void *handle_{nullptr};
    bool owned_{false};
    bool recovered_abandoned_owner_{false};
    unsigned long owner_thread_id_{};
    bool durable_marker_enabled_{false}, delegation_armed_{false}, delegation_ever_armed_{false},
        delegation_disarm_failed_{false};
    std::wstring marker_path_;
    std::string marker_contents_;
    std::string delegation_epoch_;
    unsigned long owner_process_id_{};
    void *worker_a_{nullptr};
    void *worker_b_{nullptr};
};

} // namespace a0::phase0
