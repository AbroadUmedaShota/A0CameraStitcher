#include "a0/phase0/dual_hardware_capture_backend.hpp"

#include "a0/phase0/dual_hardware_camera_agent_store.hpp"
#include "a0/phase0/phase0.hpp"
#include "a0/phase0/wpd_transport.hpp"

#include <Windows.h>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace a0::phase0 {
namespace {

constexpr std::uint16_t kDualCaptureOriginalWidth = 7360U;
constexpr std::uint16_t kDualCaptureOriginalHeight = 4912U;

std::string NormalizeSettingLabel(std::string_view label) {
    std::string normalized;
    normalized.reserve(label.size());
    for (const unsigned char character : label) {
        if (std::isalnum(character) != 0) {
            normalized.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    return normalized;
}

bool LabelContains(
    const SdkCameraStatus::SettingCapability& setting,
    std::string_view expected) {
    if (!setting.available || !setting.current_label) return false;
    return NormalizeSettingLabel(*setting.current_label).find(expected) !=
        std::string::npos;
}

void RequireReadOnlyDualCaptureProfile(const SdkCameraStatus& status) {
    if (!status.live_view_status_available || status.live_view_status != "off") {
        throw TransportError(
            "dual_live_view_not_off",
            "DualCamera Live View OFF was not confirmed in the capture session");
    }
    if (!LabelContains(status.file_type, "jpeg") ||
        !LabelContains(status.compression_level, "fine")) {
        throw TransportError(
            "dual_jpeg_fine_not_confirmed",
            "DualCamera JPEG Fine was not confirmed in the capture session");
    }
    const bool large_label =
        LabelContains(status.image_size, "large") ||
        (status.image_size.available && status.image_size.current_label &&
         NormalizeSettingLabel(*status.image_size.current_label) == "l") ||
        (LabelContains(status.image_size, "7360") &&
         LabelContains(status.image_size, "4912"));
    if (!large_label) {
        throw TransportError(
            "dual_image_size_l_not_confirmed",
            "DualCamera image size L was not confirmed in the capture session");
    }
}

class BoundNikonCardCaptureTransport final : public ICameraTransport,
                                             public ICardCaptureTransport {
public:
    BoundNikonCardCaptureTransport(
        std::shared_ptr<NikonDualBindingSdkAdapter> adapter,
        std::string token)
        : adapter_(std::move(adapter)), token_(std::move(token)) {}

    [[nodiscard]] std::string SdkVersion() const override {
        return "Nikon-D810-licensed-dual-session";
    }
    [[nodiscard]] std::vector<CameraInfo> Enumerate() override {
        throw TransportError("dual_sdk_reenumeration_forbidden",
                             "Dual capture cannot re-enumerate SDK candidates");
    }
    void Open(std::string_view stable_identity, std::chrono::seconds timeout) override {
        if (open_ || stable_identity != token_) {
            throw TransportError("dual_sdk_token_invalid",
                                 "Dual bound capture token is invalid or already open");
        }
        adapter_->OpenBoundCapture(token_, timeout);
        open_ = true;
    }
    [[nodiscard]] std::string Baseline(std::chrono::seconds) override {
        throw TransportError("dual_sdk_baseline_unsupported",
                             "Dual bound card capture has no SDK download baseline");
    }
    [[nodiscard]] std::vector<ImageCandidate> CaptureAndDownload(
        std::string_view, std::chrono::seconds, std::chrono::seconds,
        std::chrono::seconds) override {
        throw TransportError("dual_sdk_download_forbidden",
                             "Dual bound capture recovers through WPD only");
    }
    void CaptureToCard(std::chrono::seconds image_event_timeout,
                       std::chrono::seconds transaction_timeout) override {
        if (!open_) {
            throw TransportError("dual_sdk_source_not_open",
                                 "Dual bound source is not open");
        }
        adapter_->CaptureToCard(image_event_timeout, transaction_timeout);
    }
    void Close(std::chrono::seconds timeout) override {
        if (!open_) return;
        open_ = false;
        adapter_->CloseBoundCapture(timeout);
    }

private:
    std::shared_ptr<NikonDualBindingSdkAdapter> adapter_;
    std::string token_;
    bool open_{false};
};

std::chrono::steady_clock::time_point SteadyDeadline(std::int64_t utc_100ns) {
    constexpr std::int64_t kTicksPerSecond = 10'000'000;
    const auto now_system = std::chrono::system_clock::now();
    const auto now_ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now_system.time_since_epoch()).count() / 100;
    if (utc_100ns <= now_ticks) {
        throw TransportError("transaction_watchdog",
                             "Dual pair watchdog expired before camera access");
    }
    const auto remaining_ticks = utc_100ns - now_ticks;
    if (remaining_ticks > 180 * kTicksPerSecond) {
        throw TransportError("transaction_watchdog",
                             "Dual pair watchdog exceeds the approved 180-second window");
    }
    return std::chrono::steady_clock::now() +
        std::chrono::nanoseconds(remaining_ticks * 100);
}

} // namespace

void PublishVerifiedDualCaptureCanonicalOriginal(
    const FrameEvidence& frame,
    const fs::path& requested_path,
    std::chrono::steady_clock::time_point deadline) {
    const fs::path canonical = ValidateDualHardwareFixedLocalPath(
        requested_path, "canonical original path");
    const fs::path parent = canonical.parent_path();
    std::error_code create_error;
    fs::create_directories(parent, create_error);
    if (create_error) {
        throw TransportError("canonical_directory_failed",
                             "Cannot create the canonical original directory");
    }
    (void)ValidateDualHardwareFixedLocalPath(parent, "canonical original directory");
    if (fs::exists(canonical)) {
        throw TransportError("canonical_original_exists",
                             "Refusing to overwrite an existing canonical original");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        throw TransportError("transaction_watchdog",
                             "Pair watchdog expired before canonical copy");
    }

    std::ifstream input(frame.path, std::ios::binary);
    const std::vector<unsigned char> bytes{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad() || bytes.size() != frame.bytes || !IsValidJpeg(bytes) ||
        !HasExpectedDualCaptureJpegDimensions(bytes) ||
        Sha256Hex(bytes) != frame.sha256) {
        throw TransportError("canonical_source_verification_failed",
                             "Verified recovery source could not be revalidated");
    }
    const fs::path partial = canonical.string() + ".partial";
    if (fs::exists(partial)) {
        throw TransportError("canonical_partial_exists",
                             "A prior canonical partial requires manual review");
    }
    {
        std::ofstream output(partial, std::ios::binary | std::ios::out);
        if (!output) {
            throw TransportError("canonical_write_failed",
                                 "Cannot create canonical original partial");
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            throw TransportError("canonical_write_failed",
                                 "Cannot flush canonical original partial");
        }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        throw TransportError("transaction_watchdog",
                             "Pair watchdog expired before canonical publish");
    }
    if (!MoveFileExW(partial.c_str(), canonical.c_str(), MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "Cannot atomically publish canonical original");
    }
    std::ifstream persisted(canonical, std::ios::binary);
    const std::vector<unsigned char> persisted_bytes{
        std::istreambuf_iterator<char>(persisted), std::istreambuf_iterator<char>()};
    if (persisted.bad() || persisted_bytes.size() != frame.bytes ||
        !IsValidJpeg(persisted_bytes) ||
        !HasExpectedDualCaptureJpegDimensions(persisted_bytes) ||
        Sha256Hex(persisted_bytes) != frame.sha256) {
        throw TransportError("canonical_verification_failed",
                             "Canonical original failed reread verification");
    }
}

bool HasExpectedDualCaptureJpegDimensions(
    const std::vector<unsigned char>& bytes) noexcept {
    if (!IsValidJpeg(bytes)) return false;
    std::size_t offset = 2;
    while (offset + 1 < bytes.size()) {
        if (bytes[offset++] != 0xFFU) return false;
        while (offset < bytes.size() && bytes[offset] == 0xFFU) ++offset;
        if (offset >= bytes.size()) return false;
        const unsigned char marker = bytes[offset++];
        if (marker == 0x00U || marker == 0xD9U || marker == 0xDAU) return false;
        if (marker == 0x01U || (marker >= 0xD0U && marker <= 0xD7U)) continue;
        if (offset + 2 > bytes.size()) return false;
        const std::size_t segment_length =
            (static_cast<std::size_t>(bytes[offset]) << 8U) |
            static_cast<std::size_t>(bytes[offset + 1]);
        if (segment_length < 2 || segment_length > bytes.size() - offset) return false;
        const bool is_start_of_frame =
            marker >= 0xC0U && marker <= 0xCFU &&
            marker != 0xC4U && marker != 0xC8U && marker != 0xCCU;
        if (is_start_of_frame) {
            if (segment_length < 7) return false;
            const std::uint16_t height = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(bytes[offset + 3]) << 8U) |
                bytes[offset + 4]);
            const std::uint16_t width = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(bytes[offset + 5]) << 8U) |
                bytes[offset + 6]);
            return width == kDualCaptureOriginalWidth &&
                height == kDualCaptureOriginalHeight;
        }
        offset += segment_length;
    }
    return false;
}

DualBoundPairCaptureBackend::DualBoundPairCaptureBackend(
    DualBindingCameraAgentDispatcher& binding_dispatcher,
    std::shared_ptr<NikonDualBindingSdkAdapter> sdk_adapter,
    fs::path wpd_camera_map)
    : binding_dispatcher_(binding_dispatcher),
      sdk_adapter_(std::move(sdk_adapter)),
      wpd_camera_map_(std::move(wpd_camera_map)) {
    if (!sdk_adapter_) throw std::invalid_argument("Dual SDK adapter is required");
    (void)ValidateDualHardwareFixedLocalPath(wpd_camera_map_, "WPD camera map");
    if (!fs::is_regular_file(wpd_camera_map_)) {
        throw std::invalid_argument("WPD camera map must be an existing regular file");
    }
}

DualHardwareFakeCaptureOutcome DualBoundPairCaptureBackend::Capture(
    std::string_view alias,
    const fs::path& canonical_original_path,
    std::int64_t watchdog_deadline_100ns) {
    if (alias != "CAM-A" && alias != "CAM-B") {
        throw TransportError("dual_alias_invalid", "Dual capture alias is invalid");
    }
    if (binding_dispatcher_.BindingState() != DualIdentitySessionBindingState::Ready ||
        sdk_adapter_->PollInvalidation() != DualIdentityInvalidationReason::None) {
        throw TransportError("dual_binding_invalidated",
                             "Dual binding is not Ready or was invalidated");
    }
    const std::string token =
        binding_dispatcher_.BoundSourceObjectForCapture(alias);

    IdentityMap wpd_map(wpd_camera_map_);
    const auto wpd_identity = wpd_map.FindIdentity(alias);
    const auto other_identity =
        wpd_map.FindIdentity(alias == "CAM-A" ? "CAM-B" : "CAM-A");
    if (!wpd_identity || wpd_identity->empty() || !other_identity ||
        other_identity->empty() || *wpd_identity == *other_identity) {
        throw TransportError("wpd_alias_map_incomplete",
                             "WPD CAM-A/CAM-B map is incomplete or ambiguous");
    }

    const auto deadline = SteadyDeadline(watchdog_deadline_100ns);
    const fs::path transaction_root = canonical_original_path.parent_path().parent_path();
    const std::string run_id = "dual-leg-" + std::string(alias) + "-" + NewRunId();
    EvidenceWriter evidence(transaction_root / "diagnostics", run_id,
                            "Nikon-D810-licensed-dual-session");
    WpdTransport wpd;
    BoundNikonCardCaptureTransport sdk(sdk_adapter_, token);
    const auto result = ExecuteHybridCaptureOnce(
        wpd, wpd, sdk, sdk, evidence, alias, *wpd_identity, token, {}, {}, {},
        deadline, [&](const FrameEvidence& frame) {
            // The core keeps the WPD object untouched until this callback
            // returns. Persist and verify the requested canonical original
            // (including dimensions) before making deletion eligible.
            PublishVerifiedDualCaptureCanonicalOriginal(
                frame, canonical_original_path, deadline);
        }, [&] {
            if (sdk_adapter_->PollInvalidation() != DualIdentityInvalidationReason::None) {
                throw TransportError("dual_binding_invalidated",
                                     "Dual binding invalidated before shutter command");
            }
            RequireReadOnlyDualCaptureProfile(
                sdk_adapter_->ProbeOpenCaptureSessionStatus(
                    std::chrono::seconds(5)));
        });

    DualHardwareFakeCaptureOutcome outcome;
    outcome.succeeded = result.terminal_state == "Complete" &&
        result.frames.size() == 1 && result.frames.front().success;
    outcome.exact_recovered_object_deleted = result.camera_card_delete_succeeded;
    outcome.spool_empty_after_delete = result.spool_empty_after_cleanup;
    if (!outcome.succeeded) return outcome;

    outcome.succeeded = true;
    return outcome;
}

} // namespace a0::phase0
