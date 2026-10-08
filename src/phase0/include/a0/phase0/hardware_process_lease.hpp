#pragma once

#include <chrono>
#include <filesystem>
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

// Pure predicates for callers that must refuse production lease identities
// before constructing a lease. They never relax what the constructor enforces.
//
// True when `lease_name` is a test lease name, the only kind that may be paired
// with a test marker root. The production lease name is not a test name.
[[nodiscard]] bool IsHardwareProcessTestLeaseName(std::string_view lease_name) noexcept;
// True when `marker_root` designates the per-user production dual-delegation
// marker root (compared ignoring case and separator style). Throws
// TransportError when the production root cannot be resolved, so callers fail
// closed instead of treating an unverifiable root as safe. The comparison is on
// strings: 8.3 short names, trailing dots, and subst aliases are not detected.
// Junctions and symlinks are stopped by the lease layer's reparse-point
// rejection. Alias hardening is tracked in #246.
[[nodiscard]] bool IsProductionDualDelegationMarkerRoot(const std::filesystem::path &marker_root);

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
