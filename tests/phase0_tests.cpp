#include "a0/phase0/fake_camera_transport.hpp"
#include "a0/phase0/phase0.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace a0::phase0;

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string ReadAll(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

fs::path NewTestRoot(const std::string& name) {
    const auto id = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto root = fs::temp_directory_path() / ("a0-phase0-test-" + name + '-' + std::to_string(id));
    fs::create_directories(root);
    return root;
}

void TestHashAndJpeg() {
    Check(Sha256Hex({'a', 'b', 'c'}) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 must match the independent abc vector");
    Check(IsValidJpeg({0xFF, 0xD8, 0x00, 0xFF, 0xD9}), "JPEG markers should be accepted");
    Check(!IsValidJpeg({0xFF, 0xD8, 0x00}), "truncated JPEG should be rejected");
}

void TestIdentityMap() {
    const auto root = NewTestRoot("identity");
    IdentityMap map(root / "camera-map.json");
    Check(map.AssignNext("private-a") == "CAM-A", "first camera should become CAM-A");
    Check(map.AssignNext("private-b") == "CAM-B", "second camera should become CAM-B");
    IdentityMap reloaded(root / "camera-map.json");
    Check(reloaded.FindAlias("private-a") == "CAM-A", "CAM-A should survive reload");
    Check(reloaded.FindAlias("private-b") == "CAM-B", "CAM-B should survive reload");
    fs::remove_all(root);
}

void TestSuccessfulPairAndRedaction() {
    const auto root = NewTestRoot("pair");
    FakeCameraTransport transport;
    EvidenceWriter evidence(root / "artifacts", "run-pair", transport.SdkVersion());
    evidence.RecordCamera("CAM-A", "fake-fw-a");
    evidence.RecordCamera("CAM-B", "fake-fw-b");
    CaptureCoordinator coordinator(transport, evidence);
    const auto cameras = transport.Enumerate();
    const auto result = coordinator.CapturePair(cameras[0].stable_identity, cameras[1].stable_identity);
    Check(result.terminal_state == "Complete", "pair should complete");
    Check(result.frames.size() == 2 && result.frames[0].success && result.frames[1].success, "both originals should persist");
    Check(transport.OpenSessions() == 0, "session should be closed");
    Check(transport.OpenAttempts() == 2 && transport.CaptureAttempts() == 2, "CAM-A and CAM-B should each run once without retry");
    const auto events = ReadAll(evidence.RunRoot() / "events.jsonl");
    Check(events.find("private-a") == std::string::npos && events.find("private-b") == std::string::npos, "event log must redact stable identities");
    Check(events.find("CaptureA") < events.find("CaptureB"), "CAM-A must precede CAM-B");
    Check(fs::exists(evidence.RunRoot() / result.transaction_id / "CAM-A" / "original.jpg"), "CAM-A original should exist");
    Check(fs::exists(evidence.RunRoot() / result.transaction_id / "CAM-B" / "original.jpg"), "CAM-B original should exist");
    evidence.GenerateRedactedReport(root / "reports");
    const auto report = ReadAll(root / "reports" / "run-pair" / "report.md");
    const auto redacted_events = ReadAll(root / "reports" / "run-pair" / "transaction-events.jsonl");
    Check(report.find("private-") == std::string::npos, "report must not expose stable identity");
    Check(redacted_events.find(result.transaction_id) != std::string::npos, "redacted events should include transaction ID");
    Check(redacted_events.find("CAM-A") != std::string::npos && redacted_events.find("fake-fw-a") != std::string::npos,
        "redacted events should include alias and firmware");
    Check(redacted_events.find(result.frames[0].sha256) != std::string::npos,
        "redacted events should include original hash");
    Check(redacted_events.find("private-") == std::string::npos, "redacted events must not expose stable identity");
    fs::remove_all(root);
}

void TestAmbiguousCandidateFailsAndQuarantines() {
    const auto root = NewTestRoot("ambiguous");
    FakeCameraTransport transport(FakeFailureMode::ambiguous);
    EvidenceWriter evidence(root / "artifacts", "run-ambiguous", transport.SdkVersion());
    CaptureCoordinator coordinator(transport, evidence);
    const auto camera = transport.Enumerate().front();
    const auto result = coordinator.CaptureSingle("CAM-A", camera.stable_identity);
    Check(result.terminal_state == "FailedPartial", "ambiguity should fail the transaction");
    Check(result.error_category == "ambiguous_candidates", "ambiguity should be categorized");
    Check(transport.CaptureAttempts() == 1, "ambiguity must not auto-retry");
    const auto quarantine = root / "artifacts" / "quarantine" / "run-ambiguous" / result.transaction_id / "CAM-A";
    Check(fs::exists(quarantine), "ambiguous candidates should be quarantined");
    Check(std::distance(fs::directory_iterator(quarantine), fs::directory_iterator{}) == 2, "both ambiguous candidates should be retained");
    fs::remove_all(root);
}

void TestTransportFailureDoesNotRetry() {
    const auto root = NewTestRoot("failure");
    FakeCameraTransport transport(FakeFailureMode::capture_failure);
    EvidenceWriter evidence(root / "artifacts", "run-failure", transport.SdkVersion());
    CaptureCoordinator coordinator(transport, evidence);
    const auto camera = transport.Enumerate().front();
    const auto result = coordinator.CaptureSingle("CAM-A", camera.stable_identity);
    Check(result.terminal_state == "FailedPartial", "transport error should fail the transaction");
    Check(result.error_category == "capture_command_failed", "typed transport category should be retained");
    Check(transport.OpenAttempts() == 1 && transport.CaptureAttempts() == 1, "transport failure must not retry");
    Check(transport.OpenSessions() == 0, "transport session should close after failure");
    fs::remove_all(root);
}

void TestLateCandidateFailsAndQuarantines() {
    const auto root = NewTestRoot("late");
    FakeCameraTransport transport(FakeFailureMode::late_candidate);
    EvidenceWriter evidence(root / "artifacts", "run-late", transport.SdkVersion());
    CaptureCoordinator coordinator(transport, evidence);
    const auto camera = transport.Enumerate().front();
    const auto result = coordinator.CaptureSingle("CAM-A", camera.stable_identity);
    Check(result.terminal_state == "FailedPartial", "late candidate should fail the transaction");
    Check(result.error_category == "late_candidate", "late candidate should retain its category");
    Check(transport.CaptureAttempts() == 1, "late candidate must not auto-retry");
    const auto quarantine = root / "artifacts" / "quarantine" / "run-late" / result.transaction_id / "CAM-A";
    Check(fs::exists(quarantine), "late candidate should be quarantined");
    Check(!fs::exists(root / "artifacts" / "run-late" / result.transaction_id / "CAM-A" / "original.jpg"),
        "late candidate must not become an original");
    fs::remove_all(root);
}

void TestPairWatchdogStopsBeforeOpen() {
    const auto root = NewTestRoot("watchdog");
    FakeCameraTransport transport;
    EvidenceWriter evidence(root / "artifacts", "run-watchdog", transport.SdkVersion());
    Timeouts timeouts;
    timeouts.pair_watchdog = std::chrono::seconds::zero();
    CaptureCoordinator coordinator(transport, evidence, timeouts);
    const auto cameras = transport.Enumerate();
    const auto result = coordinator.CapturePair(cameras[0].stable_identity, cameras[1].stable_identity);
    Check(result.terminal_state == "FailedPartial", "expired pair watchdog should fail the transaction");
    Check(result.error_category == "transaction_watchdog", "watchdog failure should be categorized");
    Check(transport.OpenAttempts() == 0 && transport.CaptureAttempts() == 0,
        "expired watchdog must stop before opening CAM-A");
    fs::remove_all(root);
}

void TestCloseFailureRetainsOriginalAndStopsPair() {
    const auto root = NewTestRoot("close-failure");
    FakeCameraTransport transport(FakeFailureMode::close_failure);
    EvidenceWriter evidence(root / "artifacts", "run-close-failure", transport.SdkVersion());
    CaptureCoordinator coordinator(transport, evidence);
    const auto cameras = transport.Enumerate();
    const auto result = coordinator.CapturePair(cameras[0].stable_identity, cameras[1].stable_identity);
    Check(result.terminal_state == "FailedPartial", "close failure should fail the transaction");
    Check(result.error_category == "close_failed", "close failure category should be retained");
    Check(result.frames.size() == 1, "CAM-B must not start after CAM-A close failure");
    Check(result.frames.front().success, "a persisted original remains an acquired frame");
    Check(fs::exists(result.frames.front().path), "persisted original must survive close failure");
    Check(transport.OpenAttempts() == 1 && transport.CaptureAttempts() == 1, "close failure must not retry");
    Check(transport.OpenSessions() == 0, "fake close failure should still release its session");
    fs::remove_all(root);
}

} // namespace

int main() {
    try {
        TestHashAndJpeg();
        TestIdentityMap();
        TestSuccessfulPairAndRedaction();
        TestAmbiguousCandidateFailsAndQuarantines();
        TestTransportFailureDoesNotRetry();
        TestLateCandidateFailsAndQuarantines();
        TestPairWatchdogStopsBeforeOpen();
        TestCloseFailureRetainsOriginalAndStopsPair();
    } catch (const std::exception& error) {
        std::cerr << "UNEXPECTED: " << error.what() << '\n';
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All Phase 0 contract tests passed\n";
    return 0;
}
