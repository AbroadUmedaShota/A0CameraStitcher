#include "a0/phase0/wpd_transport.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace a0::phase0;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kCamA = "fixture-wpd-identity-a";
constexpr std::string_view kCamB = "fixture-wpd-identity-b";

void Check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

CameraInfo Camera(std::string_view identity) {
    return {"D810", "fixture-firmware", "photo", std::string(identity)};
}

IdentityMap Map(
    std::optional<std::string> cam_a = std::string(kCamA),
    std::optional<std::string> cam_b = std::string(kCamB)) {
    return IdentityMap("fixture-wpd-map.json", std::move(cam_a), std::move(cam_b));
}

class RecordingWpdProbeTransport final : public IWpdDualReadOnlyProbeTransport {
public:
    std::vector<std::vector<CameraInfo>> inventories{{Camera(kCamA), Camera(kCamB)}};
    std::vector<std::string> events;
    std::size_t inventory_index{};
    std::size_t cam_a_payload_count{};
    std::size_t cam_b_payload_count{};
    std::optional<std::size_t> fail_inventory_at;
    bool fail_inventory_as_close{};
    bool inventory_failure_releases_session{};
    std::optional<std::size_t> inventory_leaves_session_open_at;
    bool fail_inspect{};
    std::optional<std::string> fail_inspect_identity;
    bool fail_inspect_as_close{};
    bool close_failure_releases_session{};
    bool inspect_leaves_session_open{};
    bool fail_cleanup{};
    bool cleanup_leaves_session_open{};
    bool session_open{};

    [[nodiscard]] std::vector<CameraInfo> EnumerateForDualReadOnlyProbe() override {
        events.emplace_back("inventory");
        if (fail_inventory_at && inventory_index == *fail_inventory_at) {
            session_open = !inventory_failure_releases_session;
            throw TransportError(
                fail_inventory_as_close
                    ? "inventory_close_failed" : "fixture_inventory_failed",
                "fixture-sensitive-inventory-detail");
        }
        if (inventory_index >= inventories.size()) {
            throw std::runtime_error("unexpected extra inventory");
        }
        const auto current_index = inventory_index++;
        if (inventory_leaves_session_open_at &&
            current_index == *inventory_leaves_session_open_at) {
            session_open = true;
        }
        return inventories[current_index];
    }

    [[nodiscard]] std::size_t InspectDualReadOnlySpoolPayloadCount(
        std::string_view stable_identity, std::chrono::seconds) override {
        events.emplace_back("inspect:" + std::string(stable_identity));
        session_open = true;
        if (fail_inspect &&
            (!fail_inspect_identity || stable_identity == *fail_inspect_identity)) {
            if (fail_inspect_as_close && close_failure_releases_session) {
                session_open = false;
            }
            throw TransportError(fail_inspect_as_close ? "close_failed" : "fixture_inspect_failed",
                "fixture-sensitive-inspection-detail");
        }
        session_open = inspect_leaves_session_open;
        if (stable_identity == kCamA) return cam_a_payload_count;
        if (stable_identity == kCamB) return cam_b_payload_count;
        throw std::runtime_error("unexpected identity");
    }

    void CloseDualReadOnlyProbeSession(std::chrono::seconds) override {
        events.emplace_back("cleanup");
        if (!cleanup_leaves_session_open) session_open = false;
        if (fail_cleanup) {
            throw TransportError("close_failed", "fixture-sensitive-cleanup-detail");
        }
    }

    [[nodiscard]] bool DualReadOnlyProbeSessionOpen() const noexcept override {
        return session_open;
    }
};

void CheckNoIdentifiersOrRetries(const std::string& json) {
    Check(json.find(kCamA) == std::string::npos && json.find(kCamB) == std::string::npos,
        "serialized WPD probe output must not include fixture identities");
    Check(json.find("fixture-sensitive-") == std::string::npos,
        "serialized WPD probe output must not include transport details");
    Check(json.find("\"automaticRetryCount\":0") != std::string::npos &&
          json.find("\"realIdentifiersIncluded\":false") != std::string::npos,
        "serialized WPD probe output must explicitly report no retries or identifiers");
}

void TestPassUsesStrictReadOnlySequenceAndFixedAnonymousJson() {
    RecordingWpdProbeTransport transport;
    transport.inventories = {
        {Camera(kCamA), Camera(kCamB)},
        {Camera(kCamB), Camera(kCamA)},
        {Camera(kCamA), Camera(kCamB)},
    };

    const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
    Check(result.error == DualWpdReadOnlyProbeError::None &&
          result.cleanup == DualWpdReadOnlyProbeCleanup::Ended &&
          result.terminal_state == DualWpdReadOnlyProbeTerminalState::Pass &&
          result.wpd_d810_count == 2 && result.inventory_checks_completed == 3 &&
          result.cam_a_payload_object_count == 0 && result.cam_b_payload_object_count == 0 &&
          result.cam_a_session_closed && result.cam_b_session_closed &&
          result.wpd_spool_sessions_closed == 2 && result.topology_stable &&
          !result.wpd_session_open_at_exit,
        "empty two-camera WPD probe must pass after all three inventory checks");
    Check(transport.events == std::vector<std::string>{
        "inventory", "inspect:" + std::string(kCamA), "inventory",
        "inspect:" + std::string(kCamB), "inventory"},
        "WPD probe must inspect CAM-A before CAM-B and recheck topology between them");

    const auto json = SerializeDualWpdReadOnlyProbeResult(result);
    Check(json ==
        "{\"operation\":\"read-only-wpd-probe\",\"wpdD810Count\":2,"
        "\"wpdInventoryChecksCompleted\":3,\"camAMapMatchCount\":1,"
        "\"camBMapMatchCount\":1,\"wpdUnboundCameraCount\":0,"
        "\"aliasIdentitiesDistinct\":true,\"camAPayloadObjectCount\":0,"
        "\"camBPayloadObjectCount\":0,\"camASessionClosed\":true,"
        "\"camBSessionClosed\":true,\"wpdSpoolSessionsClosed\":2,"
        "\"topologyStable\":true,\"wpdAccessAttempted\":true,"
        "\"wpdSessionOpenAtExit\":false,\"readOnlyAccessOnly\":true,"
        "\"sdkAccessed\":false,\"captureCommandSent\":false,"
        "\"cameraSettingsChanged\":false,\"cameraObjectDeleteAttempted\":false,"
        "\"vendorOperationExecuted\":false,\"automaticRetryCount\":0,"
        "\"realIdentifiersIncluded\":false,\"errorCategory\":\"none\","
        "\"cleanupState\":\"ended\",\"terminalState\":\"Pass\"}",
        "WPD probe pass output must use the exact anonymous fixed schema");
    CheckNoIdentifiersOrRetries(json);
}

void TestCountsAndMapFailuresStopBeforeCardAccess() {
    for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
        RecordingWpdProbeTransport transport;
        std::vector<CameraInfo> inventory;
        for (std::size_t index = 0; index < count; ++index) {
            inventory.push_back(Camera(index == 0 ? kCamA : kCamB));
        }
        transport.inventories = {inventory};
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error == DualWpdReadOnlyProbeError::CameraCountMismatch &&
              result.terminal_state == DualWpdReadOnlyProbeTerminalState::Blocked &&
              transport.events == std::vector<std::string>{"inventory"},
            "zero, one, or three D810s must block before spool inspection");
        CheckNoIdentifiersOrRetries(SerializeDualWpdReadOnlyProbeResult(result));
    }

    for (const auto& map : std::vector<IdentityMap>{
             Map(std::nullopt, std::string(kCamB)),
             Map(std::string(kCamA), std::string(kCamA))}) {
        RecordingWpdProbeTransport transport;
        const auto result = RunDualWpdReadOnlyProbe(transport, map, 5s);
        Check(result.error == DualWpdReadOnlyProbeError::AliasMapInvalid &&
              transport.events.empty(),
            "missing or duplicate aliases must block before WPD access");
    }

    for (const auto& inventory : std::vector<std::vector<CameraInfo>>{
             std::vector<CameraInfo>{Camera(kCamA), Camera(kCamA)},
             std::vector<CameraInfo>{Camera(kCamA), Camera("fixture-wpd-unbound")}}) {
        RecordingWpdProbeTransport transport;
        transport.inventories = {inventory};
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error == DualWpdReadOnlyProbeError::AliasMatchMismatch &&
              transport.events == std::vector<std::string>{"inventory"},
            "alias mismatch or unbound WPD device must block before spool inspection");
    }
}

void TestNonEmptyCardsAndTopologyChangesStopWithoutRetry() {
    {
        RecordingWpdProbeTransport transport;
        transport.cam_a_payload_count = 1;
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error == DualWpdReadOnlyProbeError::SpoolNotEmpty &&
              transport.events == std::vector<std::string>{"inventory", "inspect:" + std::string(kCamA)} &&
              !result.cam_b_payload_object_count,
            "a nonempty CAM-A card must prevent CAM-B access");
    }
    {
        RecordingWpdProbeTransport transport;
        transport.inventories = {{Camera(kCamA), Camera(kCamB)}, {Camera(kCamA), Camera(kCamB)}};
        transport.cam_b_payload_count = 1;
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error == DualWpdReadOnlyProbeError::SpoolNotEmpty &&
              transport.events == std::vector<std::string>{
                  "inventory", "inspect:" + std::string(kCamA), "inventory", "inspect:" + std::string(kCamB)},
            "a nonempty CAM-B card must block before final inventory retry");
    }
    {
        RecordingWpdProbeTransport transport;
        transport.inventories = {
            {Camera(kCamA), Camera(kCamB)}, {Camera(kCamA), Camera("fixture-wpd-replaced")}};
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error == DualWpdReadOnlyProbeError::TopologyChanged &&
              transport.events == std::vector<std::string>{
                  "inventory", "inspect:" + std::string(kCamA), "inventory"},
            "topology change after CAM-A must prevent CAM-B access");
    }
    {
        RecordingWpdProbeTransport transport;
        transport.inventories = {
            {Camera(kCamA), Camera(kCamB)}, {Camera(kCamA), Camera(kCamB)},
            {Camera(kCamA), Camera("fixture-wpd-replaced")}};
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error == DualWpdReadOnlyProbeError::TopologyChanged &&
              transport.events == std::vector<std::string>{
                  "inventory", "inspect:" + std::string(kCamA), "inventory",
                  "inspect:" + std::string(kCamB), "inventory"},
            "topology change after CAM-B must block rather than accept the pair");
    }
}

void TestFailuresCloseOrReportUnconfirmedWithoutRetry() {
    {
        RecordingWpdProbeTransport transport;
        transport.fail_inventory_at = 0;
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error == DualWpdReadOnlyProbeError::InventoryFailed &&
              result.cleanup == DualWpdReadOnlyProbeCleanup::EndedAfterError &&
              transport.events == std::vector<std::string>{"inventory", "cleanup"},
            "inventory failure must attempt one read-only cleanup and block");
        CheckNoIdentifiersOrRetries(SerializeDualWpdReadOnlyProbeResult(result));
    }
    {
        RecordingWpdProbeTransport transport;
        transport.fail_inspect = true;
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error == DualWpdReadOnlyProbeError::SpoolInspectionFailed &&
              result.cleanup == DualWpdReadOnlyProbeCleanup::EndedAfterError &&
              transport.events == std::vector<std::string>{
                  "inventory", "inspect:" + std::string(kCamA), "cleanup"},
            "spool inspection failure must close before returning Blocked");
    }
    {
        RecordingWpdProbeTransport transport;
        transport.inventories = {
            {Camera(kCamA), Camera(kCamB)}, {Camera(kCamA), Camera(kCamB)}};
        transport.fail_inspect = true;
        transport.fail_inspect_identity = std::string(kCamB);
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error == DualWpdReadOnlyProbeError::SpoolInspectionFailed &&
              result.wpd_spool_sessions_closed == 1 &&
              transport.events == std::vector<std::string>{
                  "inventory", "inspect:" + std::string(kCamA), "inventory",
                  "inspect:" + std::string(kCamB), "cleanup"},
            "CAM-B inspection failure must preserve CAM-A completion and stop once");
    }
    {
        RecordingWpdProbeTransport transport;
        transport.fail_inventory_at = 1;
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error == DualWpdReadOnlyProbeError::InventoryFailed &&
              transport.events == std::vector<std::string>{
                  "inventory", "inspect:" + std::string(kCamA), "inventory", "cleanup"},
            "topology inventory failure after CAM-A must stop before CAM-B");
    }
    {
        RecordingWpdProbeTransport transport;
        transport.fail_inventory_at = 0;
        transport.fail_inventory_as_close = true;
        transport.inventory_failure_releases_session = true;
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error ==
                  DualWpdReadOnlyProbeError::SessionCleanupUnconfirmed &&
              result.cleanup == DualWpdReadOnlyProbeCleanup::Unconfirmed &&
              transport.events == std::vector<std::string>{"inventory"},
            "an inventory Close failure must remain unconfirmed after handle release");
    }
    {
        RecordingWpdProbeTransport transport;
        transport.fail_inspect = true;
        transport.fail_inspect_as_close = true;
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error == DualWpdReadOnlyProbeError::SessionCloseFailed &&
              result.cleanup == DualWpdReadOnlyProbeCleanup::EndedAfterError,
            "a close error with confirmed cleanup must remain blocked");
    }
    {
        RecordingWpdProbeTransport transport;
        transport.fail_inspect = true;
        transport.fail_inspect_as_close = true;
        transport.close_failure_releases_session = true;
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error ==
                  DualWpdReadOnlyProbeError::SessionCleanupUnconfirmed &&
              result.cleanup == DualWpdReadOnlyProbeCleanup::Unconfirmed &&
              transport.events == std::vector<std::string>{
                  "inventory", "inspect:" + std::string(kCamA)},
            "a scan cleanup Close failure must stay unconfirmed after handle release");
    }
    {
        RecordingWpdProbeTransport transport;
        transport.inventory_leaves_session_open_at = 0;
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error == DualWpdReadOnlyProbeError::SessionCloseFailed &&
              result.cleanup == DualWpdReadOnlyProbeCleanup::EndedAfterError &&
              transport.events == std::vector<std::string>{"inventory", "cleanup"},
            "a successful inventory return with an open session must be cleaned and blocked");
    }
    {
        RecordingWpdProbeTransport transport;
        transport.inspect_leaves_session_open = true;
        transport.fail_cleanup = true;
        transport.cleanup_leaves_session_open = true;
        const auto result = RunDualWpdReadOnlyProbe(transport, Map(), 5s);
        Check(result.error == DualWpdReadOnlyProbeError::SessionCleanupUnconfirmed &&
              result.cleanup == DualWpdReadOnlyProbeCleanup::Unconfirmed &&
              result.wpd_session_open_at_exit &&
              transport.events == std::vector<std::string>{
                  "inventory", "inspect:" + std::string(kCamA), "cleanup"},
            "unconfirmed cleanup must fail closed without a retry");
        CheckNoIdentifiersOrRetries(SerializeDualWpdReadOnlyProbeResult(result));
    }
}

} // namespace

int main() {
    try {
        TestPassUsesStrictReadOnlySequenceAndFixedAnonymousJson();
        TestCountsAndMapFailuresStopBeforeCardAccess();
        TestNonEmptyCardsAndTopologyChangesStopWithoutRetry();
        TestFailuresCloseOrReportUnconfirmedWithoutRetry();
        std::cout << "WPD Dual read-only probe contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WPD Dual read-only probe contract failed: " << error.what() << '\n';
        return 1;
    }
}
