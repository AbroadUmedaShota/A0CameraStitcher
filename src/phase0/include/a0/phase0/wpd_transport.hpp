#pragma once

#include "a0/phase0/phase0.hpp"

#include <memory>

namespace a0::phase0 {

class WpdTransport final : public ICameraTransport {
public:
    WpdTransport();
    ~WpdTransport() override;
    WpdTransport(const WpdTransport&) = delete;
    WpdTransport& operator=(const WpdTransport&) = delete;
    [[nodiscard]] std::string SdkVersion() const override;
    [[nodiscard]] std::vector<CameraInfo> Enumerate() override;
    void Open(std::string_view stable_identity, std::chrono::seconds timeout) override;
    [[nodiscard]] std::string Baseline(std::chrono::seconds timeout) override;
    [[nodiscard]] std::vector<ImageCandidate> CaptureAndDownload(
        std::string_view baseline,
        std::chrono::seconds image_event_timeout,
        std::chrono::seconds download_timeout,
        std::chrono::seconds transaction_timeout) override;
    void Close(std::chrono::seconds timeout) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace a0::phase0
