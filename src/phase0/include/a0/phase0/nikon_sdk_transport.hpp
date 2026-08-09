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

} // namespace a0::phase0
