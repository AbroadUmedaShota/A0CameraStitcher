#pragma once

#include "a0/phase0/dual_binding_camera_agent.hpp"
#include "a0/phase0/dual_hardware_camera_agent.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace a0::phase0 {

// CaptureRecoveryOnly accepts only the approved D810 JPEG Fine/L dimensions.
// This is intentionally evaluated over both the recovered PC original and its
// canonical reread before the exact WPD object becomes eligible for deletion.
[[nodiscard]] bool HasExpectedDualCaptureJpegDimensions(
    const std::vector<unsigned char>& bytes) noexcept;

// Real DualCamera leg backend. SDK candidate identity remains inside the
// binding process; WPD uses a separately registered local alias map.
class DualBoundPairCaptureBackend final : public DualHardwarePairCaptureBackend {
public:
    DualBoundPairCaptureBackend(
        DualBindingCameraAgentDispatcher& binding_dispatcher,
        std::shared_ptr<NikonDualBindingSdkAdapter> sdk_adapter,
        std::filesystem::path wpd_camera_map);

    [[nodiscard]] DualHardwareFakeCaptureOutcome Capture(
        std::string_view alias,
        const std::filesystem::path& canonical_original_path,
        std::int64_t watchdog_deadline_100ns) override;

private:
    DualBindingCameraAgentDispatcher& binding_dispatcher_;
    std::shared_ptr<NikonDualBindingSdkAdapter> sdk_adapter_;
    std::filesystem::path wpd_camera_map_;
};

} // namespace a0::phase0
