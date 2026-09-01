#pragma once

#include "a0/phase0/dual_binding_camera_agent.hpp"
#include "a0/phase0/dual_hardware_camera_agent.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include "a0/phase0/wpd_transport.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

namespace a0::phase0 {

enum class DualBoundPairPreflightError {
    None,
    SdkInvalidatedBeforeWpd,
    SdkBoundaryUnsafeBeforeWpd,
    WpdProbeBlocked,
    SdkBoundaryUnsafeAfterWpd,
    SdkInvalidatedAfterWpd,
};

struct DualBoundPairPreflightResult {
    NikonDualRetainedModuleState sdk_state_before_wpd;
    NikonDualRetainedModuleState sdk_state_after_wpd;
    DualWpdReadOnlyProbeResult wpd_result;
    DualIdentityInvalidationReason invalidation_reason{
        DualIdentityInvalidationReason::None};
    DualBoundPairPreflightError error{
        DualBoundPairPreflightError::SdkBoundaryUnsafeBeforeWpd};
    bool ready{};
};

// Pair-level, no-shutter gate used by both the production backend and the
// explicit coexistence probe. SDK calls occur only before and after the
// synchronous WPD read-only window; the WPD seam has no capture, delete,
// vendor-operation, settings-write, or retry capability.
[[nodiscard]] DualBoundPairPreflightResult RunDualBoundPairPreflight(
    NikonDualBindingSdkAdapter& sdk_adapter,
    IWpdDualReadOnlyProbeTransport& wpd_transport,
    const IdentityMap& identity_map,
    std::chrono::seconds timeout) noexcept;

// Applies the production fail-closed policy to one completed pair preflight.
// Keeping this separate from the concrete WPD owner makes the critical
// cleanup-unconfirmed branch directly contract-testable: that branch abandons
// the SDK owner without invoking EndSession or any other SDK API.
[[nodiscard]] DualHardwarePairPreflightOutcome ResolveDualBoundPairPreflight(
    NikonDualBindingSdkAdapter& sdk_adapter,
    const DualBoundPairPreflightResult& result) noexcept;

// Applies the post-capture WPD cleanup boundary.  An open WPD session makes
// the leg fail even when a verified canonical original exists; the SDK owner
// is abandoned without Poll/End and the Agent must terminalize the binding.
[[nodiscard]] DualIdentityInvalidationReason ResolveDualCaptureWpdCleanup(
    NikonDualBindingSdkAdapter& sdk_adapter,
    bool wpd_session_open,
    bool wpd_cleanup_confirmed = true) noexcept;

// CaptureRecoveryOnly accepts only the approved D810 JPEG Fine/L dimensions.
// This is intentionally evaluated over both the recovered PC original and its
// canonical reread before the exact WPD object becomes eligible for deletion.
[[nodiscard]] bool HasExpectedDualCaptureJpegDimensions(
    const std::vector<unsigned char>& bytes) noexcept;

// Publishes a recovered DualCamera original to its requested fixed local
// destination only after source and canonical reread validation. Callers must
// invoke this before making the exact WPD object eligible for deletion.
void PublishVerifiedDualCaptureCanonicalOriginal(
    const FrameEvidence& frame,
    const std::filesystem::path& requested_path,
    std::chrono::steady_clock::time_point deadline);

// Real DualCamera leg backend. SDK candidate identity remains inside the
// binding process; WPD uses a separately registered local alias map.
class DualBoundPairCaptureBackend final : public DualHardwarePairCaptureBackend {
public:
    DualBoundPairCaptureBackend(
        DualBindingCameraAgentDispatcher& binding_dispatcher,
        std::shared_ptr<NikonDualBindingSdkAdapter> sdk_adapter,
        std::filesystem::path wpd_camera_map);

    [[nodiscard]] DualHardwarePairPreflightOutcome PreflightPair(
        bool capture_recovery_only,
        std::int64_t watchdog_deadline_100ns) override;
    [[nodiscard]] DualHardwarePairCaptureCapabilities Capabilities() const noexcept override {
        return {false, true};
    }
    [[nodiscard]] DualIdentityInvalidationReason LastBindingInvalidationReason() const noexcept override {
        return capture_invalidation_reason_;
    }

    [[nodiscard]] DualHardwareFakeCaptureOutcome Capture(
        std::string_view alias,
        const std::filesystem::path& canonical_original_path,
        std::int64_t watchdog_deadline_100ns) override;

private:
    DualBindingCameraAgentDispatcher& binding_dispatcher_;
    std::shared_ptr<NikonDualBindingSdkAdapter> sdk_adapter_;
    std::filesystem::path wpd_camera_map_;
    DualIdentityInvalidationReason capture_invalidation_reason_{
        DualIdentityInvalidationReason::None};
};

} // namespace a0::phase0
