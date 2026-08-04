#pragma once

#include "a0/phase0/phase0.hpp"

namespace a0::phase0 {

enum class FakeFailureMode {
    none,
    no_candidate,
    ambiguous,
    invalid_jpeg,
    late_candidate,
    open_failure,
    capture_failure,
    close_failure
};

class FakeCameraTransport final : public ICameraTransport {
public:
    explicit FakeCameraTransport(FakeFailureMode mode = FakeFailureMode::none);
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
    [[nodiscard]] int OpenSessions() const noexcept;
    [[nodiscard]] unsigned long long OpenAttempts() const noexcept;
    [[nodiscard]] unsigned long long CaptureAttempts() const noexcept;

private:
    FakeFailureMode mode_;
    bool open_{false};
    unsigned long long capture_sequence_{0};
    unsigned long long open_attempts_{0};
    unsigned long long capture_attempts_{0};
};

} // namespace a0::phase0
