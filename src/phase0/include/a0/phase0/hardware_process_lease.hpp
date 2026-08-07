#pragma once

#include <chrono>
#include <string_view>

namespace a0::phase0 {

// Serializes all real-camera Phase 0 commands across processes in the current
// interactive Windows logon session. Cross-session/service enforcement is an
// installation policy concern. Per-transport session guards are still needed.
class HardwareProcessLease final {
public:
    explicit HardwareProcessLease(
        std::string_view lease_name = "A0CameraStitcher.Phase0.CameraControl.v1",
        std::chrono::milliseconds wait = std::chrono::milliseconds::zero());
    ~HardwareProcessLease();

    HardwareProcessLease(const HardwareProcessLease&) = delete;
    HardwareProcessLease& operator=(const HardwareProcessLease&) = delete;
    HardwareProcessLease(HardwareProcessLease&&) = delete;
    HardwareProcessLease& operator=(HardwareProcessLease&&) = delete;

    [[nodiscard]] bool RecoveredAbandonedOwner() const noexcept;

private:
    void* handle_{nullptr};
    bool owned_{false};
    bool recovered_abandoned_owner_{false};
};

} // namespace a0::phase0
