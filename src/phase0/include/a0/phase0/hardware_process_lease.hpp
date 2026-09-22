#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace a0::phase0 {
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
};

} // namespace a0::phase0
