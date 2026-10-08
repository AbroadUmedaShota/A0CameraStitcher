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
// True when `marker_root` is the per-user production data root
// (`%LOCALAPPDATA%\A0CameraStitcher`, which holds the production dual-delegation
// marker root and the recovery records beside it), lies inside it, or cannot be
// shown to lie outside it. An empty path yields false: this function says
// nothing about whether an empty root is acceptable, so do not use it to decide
// that. Throws TransportError when LocalAppData cannot be resolved, so callers
// fail closed instead of treating an unverifiable root as safe. The comparison
// follows `MarkerRootMayOverlap`: names that resolve to an existing directory
// (8.3 short names, subst drives, junctions) are matched by volume/file ID and
// final path; a name that does not exist yet is refused when it contains `~`;
// and any name, existing or not, that contains `:` or ends in a dot or space is
// refused (see `MarkerRootMayOverlap`).
//
// Limits. The check is made at one point in time and the lease repeats it after
// taking the mutex. A drive letter that is re-mapped (for example by subst)
// after the last check is not detected. A swap between the last check and the
// directory or marker write can let one write land through a junction or
// symlink before the lease layer's reparse-point rejection stops it. That
// rejection is not a backstop for a root whose own name ends in a dot or space:
// Win32 trims every trailing dot and space from the last name of a path, but an
// inner name loses a trailing dot only when it ends in exactly one dot (`foo.`
// becomes `foo`, `foo .` becomes `foo `; `foo ` and `foo..` are kept), so the
// reparse check sees `foo` while the marker is written through `foo `. That spelling is closed by the name rule above, which refuses
// every name that ends in a dot or space, a single dot included, and not by the
// reparse check. Do not relax it to allow one trailing dot. A volume
// mounted on a folder inside the data root and then addressed through its own
// drive letter is not detected, because the final path comes back on the drive
// letter side; the production marker root itself refuses reparse points, so it
// cannot sit there. Only the current user's LocalAppData is checked.
[[nodiscard]] bool MarkerRootMayTouchProductionData(const std::filesystem::path &marker_root);
// Pure overlap test behind MarkerRootMayTouchProductionData, with the
// reference root supplied by the caller. It only opens directories to read
// their identity; it never creates, writes, or deletes anything. True when
// `candidate` equals `reference` or lies beneath it after normalization,
// resolves to such a directory, or cannot be verified (not an absolute
// drive-letter path, a drive or ancestor that cannot be opened, a name that
// does not exist yet and contains `~`, or any name in `candidate`, existing or
// not, that contains `:` or ends in a dot or space). An empty `candidate`
// yields false, so this must not be used to decide whether an empty root is
// acceptable; a sibling or an ancestor of `reference` yields false.
[[nodiscard]] bool MarkerRootMayOverlap(const std::filesystem::path &candidate,
                                        const std::filesystem::path &reference);
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
