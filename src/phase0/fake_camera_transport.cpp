#include "a0/phase0/fake_camera_transport.hpp"

#include <stdexcept>

namespace a0::phase0 {

FakeCameraTransport::FakeCameraTransport(FakeFailureMode mode) : mode_(mode) {}

std::string FakeCameraTransport::SdkVersion() const { return "fake-transport-1"; }

std::vector<CameraInfo> FakeCameraTransport::Enumerate() {
    return {
        {"Nikon D810", "fake-fw-a", "S", "fake-private-identity-a"},
        {"Nikon D810", "fake-fw-b", "S", "fake-private-identity-b"}
    };
}

void FakeCameraTransport::Open(std::string_view stable_identity, std::chrono::seconds) {
    ++open_attempts_;
    if (open_) throw TransportError("session_busy", "fake transport session overlap");
    if (stable_identity.empty() || mode_ == FakeFailureMode::open_failure) {
        throw TransportError("open_failed", "fake open failure");
    }
    open_ = true;
}

std::string FakeCameraTransport::Baseline(std::chrono::seconds) {
    if (!open_) throw TransportError("session_not_open", "fake baseline without session");
    return "baseline-" + std::to_string(capture_sequence_);
}

std::vector<ImageCandidate> FakeCameraTransport::CaptureAndDownload(
    std::string_view,
    std::chrono::seconds,
    std::chrono::seconds,
    std::chrono::seconds) {
    if (!open_) throw TransportError("session_not_open", "fake capture without session");
    ++capture_attempts_;
    if (mode_ == FakeFailureMode::capture_failure) {
        throw TransportError("capture_command_failed", "fake capture failure");
    }
    ++capture_sequence_;
    const std::vector<unsigned char> jpeg{0xFF, 0xD8, 0x01, static_cast<unsigned char>(capture_sequence_ & 0xFF), 0xFF, 0xD9};
    switch (mode_) {
    case FakeFailureMode::no_candidate: return {};
    case FakeFailureMode::ambiguous: return {{"first.jpg", jpeg}, {"late.jpg", jpeg}};
    case FakeFailureMode::invalid_jpeg: return {{"invalid.jpg", {0x01, 0x02, 0x03, 0x04}}};
    case FakeFailureMode::late_candidate: return {{"late.jpg", jpeg, false}};
    default: return {{"captured.jpg", jpeg}};
    }
}

void FakeCameraTransport::Close(std::chrono::seconds) {
    if (!open_) throw TransportError("session_not_open", "fake close without session");
    open_ = false;
    if (mode_ == FakeFailureMode::close_failure) {
        throw TransportError("close_failed", "fake close failure");
    }
}

int FakeCameraTransport::OpenSessions() const noexcept { return open_ ? 1 : 0; }
unsigned long long FakeCameraTransport::OpenAttempts() const noexcept { return open_attempts_; }
unsigned long long FakeCameraTransport::CaptureAttempts() const noexcept { return capture_attempts_; }

} // namespace a0::phase0
