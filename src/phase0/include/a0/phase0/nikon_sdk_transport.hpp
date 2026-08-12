#pragma once

#include "a0/phase0/phase0.hpp"

#include <memory>

namespace a0::phase0 {

// Derives the private SDK-side identity from documented, source-level MAID
// strings. The returned digest is local-only and must never be committed.
// Source object IDs are deliberately excluded because they are ephemeral.
[[nodiscard]] std::string DeriveNikonSdkStableIdentity(
    std::string_view source_name,
    std::string_view source_interface);

// Small SDK-independent model of the source callback window used by a
// SaveMedia=Card capture. It keeps callback/pump/session ordering testable
// without loading licensed Nikon binaries or issuing a camera command.
enum class NikonCardCaptureEvent {
    capture_complete,
    add_child_in_card,
    other,
};

struct NikonCardCaptureEventSnapshot {
    bool callback_registered{false};
    bool capture_command_started{false};
    bool capture_command_accepted{false};
    bool event_pump_started{false};
    bool event_pump_stopped{false};
    bool session_closed{false};
    bool session_closed_while_pumping{false};
    std::size_t capture_complete_events{};
    std::size_t add_child_in_card_events{};
    std::size_t ignored_events{};
};

class NikonCardCaptureEventWindow final {
public:
    void ResetForSession() noexcept;
    void CallbackRegistered() noexcept;
    [[nodiscard]] bool BeginCaptureCommand() noexcept;
    [[nodiscard]] bool BeginEventPump() noexcept;
    void CaptureCommandAccepted() noexcept;
    void Observe(NikonCardCaptureEvent event) noexcept;
    void EndEventPump() noexcept;
    void SessionClosed() noexcept;
    [[nodiscard]] bool CaptureCompleted() const noexcept;
    [[nodiscard]] NikonCardCaptureEventSnapshot Snapshot() const noexcept;

private:
    NikonCardCaptureEventSnapshot snapshot_{};
};

class NikonSdkTransport final : public ICameraTransport, public ILiveViewTransport, public ICardCaptureTransport {
public:
    NikonSdkTransport();
    ~NikonSdkTransport() override;
    NikonSdkTransport(const NikonSdkTransport&) = delete;
    NikonSdkTransport& operator=(const NikonSdkTransport&) = delete;
    [[nodiscard]] std::string SdkVersion() const override;
    [[nodiscard]] std::vector<CameraInfo> Enumerate() override;
    [[nodiscard]] SdkCameraStatus ProbeSdkStatus(
        std::string_view stable_identity,
        std::chrono::seconds timeout);
    // Reads settings from the already-open capture session. Camera Agent uses
    // this immediately before the shutter command so the approved profile is
    // checked in the same SDK session that performs the capture.
    [[nodiscard]] SdkCameraStatus ProbeOpenCaptureSessionStatus(
        std::chrono::seconds timeout);
    // Product Camera Agent only: every subsequent SDK source open rejects
    // unless the open-time inventory contains exactly one D810. Legacy pair
    // experiments leave this disabled.
    void RequireExactlyOneD810ForProductAgent();
    void Open(std::string_view stable_identity, std::chrono::seconds timeout) override;
    [[nodiscard]] std::string Baseline(std::chrono::seconds timeout) override;
    [[nodiscard]] std::vector<ImageCandidate> CaptureAndDownload(
        std::string_view baseline,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) override;
    void CaptureToCard(std::chrono::seconds image_event_timeout,
                       std::chrono::seconds transaction_timeout) override;
    void OpenLiveView(std::string_view stable_identity, std::chrono::seconds timeout) override;
    void StartLiveView(std::chrono::seconds timeout) override;
    [[nodiscard]] std::vector<unsigned char> ReadLiveViewFrame(std::chrono::seconds timeout) override;
    void StopLiveView(std::chrono::seconds timeout) override;
    void Close(std::chrono::seconds timeout) override;
    [[nodiscard]] static bool LicensedAdapterAvailable() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class NikonSdkStatusExecutor final : public ISdkStatusExecutor {
public:
    NikonSdkStatusExecutor();
    ~NikonSdkStatusExecutor() override;
    NikonSdkStatusExecutor(const NikonSdkStatusExecutor&) = delete;
    NikonSdkStatusExecutor& operator=(const NikonSdkStatusExecutor&) = delete;
    [[nodiscard]] std::string SdkVersion() const override;
    [[nodiscard]] std::vector<CameraInfo> Enumerate() override;
    // SingleCamera identity-v3 only: require the status probe's SDK open-time
    // inventory to remain exactly one D810. This does not add capture, Live
    // View, WPD, or delete capabilities to the status-only executor.
    void RequireExactlyOneD810ForSingleStatus() override;
    [[nodiscard]] SdkCameraStatus ProbeSdkStatus(
        std::string_view stable_identity,
        std::chrono::seconds timeout) override;

private:
    NikonSdkTransport transport_;
};

} // namespace a0::phase0
