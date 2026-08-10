#include "a0/phase0/fake_camera_transport.hpp"
#include "a0/phase0/cli_safety.hpp"
#include "a0/phase0/hardware_camera_agent.hpp"
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include "a0/phase0/phase0.hpp"
#include "a0/phase0/wpd_transport.hpp"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace a0::phase0;

namespace {

struct Options {
    std::string command;
    std::string stage{"single"};
    std::string alias{"CAM-A"};
    bool alias_explicit{false};
    std::string scenario;
    std::string run_id;
    int count{1};
    int frames{30};
    int interval_ms{100};
    int duration_seconds{0};
    int correlation_samples{3};
    int correlation_interval_ms{1500};
    bool correlation_samples_explicit{false};
    bool correlation_interval_explicit{false};
    bool save_last_frame{false};
    bool exclusive_camera_control_confirmed{false};
    bool dedicated_spool_scope_confirmed{false};
    bool dual_dedicated_spools_confirmed{false};
    bool exact_object_delete_confirmed{false};
    bool single_camera_connected_confirmed{false};
    std::string transport;
    bool transport_explicit{false};
    WpdCommandTargetPolicy wpd_command_target{WpdCommandTargetPolicy::functional};
    WpdStatusAccess wpd_status_access{WpdStatusAccess::read_only};
    bool wpd_status_access_explicit{false};
    std::optional<std::string> operator_gate;
    int operator_gate_timeout_seconds{300};
    fs::path artifacts{"artifacts/phase0"};
    fs::path reports{"docs/evidence/phase0"};
    fs::path camera_map{DefaultIdentityMapPath()};
    bool camera_map_explicit{false};
    fs::path single_identity_v3{DefaultSingleIdentityV3Path()};
    bool single_identity_v3_explicit{false};
};

fs::path WpdIdentityMapPath(const Options& options);
CameraInfo ResolveCamera(
    const std::vector<CameraInfo>& cameras,
    const fs::path& camera_map,
    std::string_view alias);
CameraInfo ResolveCamera(ICameraTransport& transport, const fs::path& camera_map, std::string_view alias);

class ProductSingleIdentityV3WpdEnumerator final
    : public ISingleIdentityV3WpdEnumerator {
public:
    explicit ProductSingleIdentityV3WpdEnumerator(
        WpdCommandTargetPolicy command_target_policy)
        : transport_(command_target_policy) {
        transport_.RequireExactlyOneD810ForProductAgent();
    }

    std::vector<CameraInfo> Enumerate() override {
        return transport_.Enumerate();
    }

private:
    WpdTransport transport_;
};

std::optional<std::string> EnvironmentValue(const char* name) {
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return std::nullopt;
    std::string value(buffer);
    std::free(buffer);
    return value;
}

std::string EscapeCliValue(std::string_view value) {
    std::ostringstream escaped;
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': escaped << "\\\\"; break;
        case '"': escaped << "\\\""; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20U || character >= 0x7FU) {
                escaped << "\\u00" << kHex[(character >> 4U) & 0x0FU] << kHex[character & 0x0FU];
            } else {
                escaped << static_cast<char>(character);
            }
        }
    }
    return escaped.str();
}

void Usage() {
    std::cout
        << "A0CameraStitcher.Phase0 commands:\n"
        << "  preflight --stage single|dual\n"
        << "  inventory [--transport sdk|wpd|fake] [--wpd-command-target functional|omit] (default: wpd, functional)\n"
        << "  bind-identity --alias CAM-A|CAM-B --transport sdk|wpd --single-camera-connected-confirmed\n"
        << "    (exactly one physical D810 connected; refuses replacement or enumeration-order assignment)\n"
        << "  bind-cross-transport-identity --alias CAM-A|CAM-B --single-camera-connected-confirmed\n"
        << "    (one lease; SDK then WPD; validates both local maps before either binding)\n"
        << "  bind-single-identity-v3 --alias CAM-A --single-camera-connected-confirmed\n"
        << "    (read-only SDK/WPD enumeration; persists only the WPD serial digest and exact-one SDK policy)\n"
        << "  verify-dual-identity (read-only; requires exactly CAM-A and CAM-B in SDK and WPD)\n"
        << "  verify-dual-spools (read-only; requires dual identity, then counts all payloads on both cards)\n"
        << "  sdk-status --alias CAM-A [--single-identity-v3 PATH]\n"
        << "    (SingleCamera identity-v3 by default; read-only; does not start Live View or change camera settings)\n"
        << "  sdk-status --alias CAM-A|CAM-B --camera-map PATH\n"
        << "    (explicit legacy/Dual identity-v2 route; never reuses SingleCamera identity-v3)\n"
        << "  wpd-status --alias CAM-A [--wpd-status-access read-only|read-write] (query-only; does not capture or execute a vendor operation)\n"
        << "  spool-status --alias CAM-A (read-only aggregate; counts all camera payload objects without capture or deletion)\n"
        << "  wpd-correlation-status --alias CAM-A [--samples 3] [--sample-interval-ms 1500] (read-only close/reopen observation)\n"
        << "  live-view --alias CAM-A [--frames 30 | --duration-seconds 300] [--interval-ms 100] [--save-last-frame]\n"
        << "  live-view-handoff --alias CAM-A --count 10 [--frames 1] [--wpd-command-target functional|omit]\n"
        << "    --exclusive-camera-control-confirmed --dedicated-spool-scope-confirmed --exact-object-delete-confirmed\n"
        << "  hybrid-capture-single --alias CAM-A --count 1|10 --exclusive-camera-control-confirmed\n"
        << "    --dedicated-spool-scope-confirmed --exact-object-delete-confirmed\n"
        << "    (one physical D810; dedicated empty card; no physical/external shutter during the active transaction)\n"
        << "  hybrid-capture-pair --count 1|10|100 --exclusive-camera-control-confirmed\n"
        << "    --dedicated-spool-scope-confirmed --dual-dedicated-spools-confirmed --exact-object-delete-confirmed\n"
        << "    (two explicitly bound D810 bodies; CAM-A then CAM-B; one dedicated empty card per body; no shutter synchronization guarantee)\n"
        << "  hybrid-fault-single --alias CAM-A --scenario usb-disconnect|power-off --operator-gate <safe-name>\n"
        << "    --exclusive-camera-control-confirmed --dedicated-spool-scope-confirmed --exact-object-delete-confirmed\n"
        << "  hybrid-fault-pair --alias CAM-A|CAM-B --scenario usb-disconnect|power-off --operator-gate <safe-name>\n"
        << "    --exclusive-camera-control-confirmed --dedicated-spool-scope-confirmed --dual-dedicated-spools-confirmed\n"
        << "    --exact-object-delete-confirmed (fault after selected body's SDK close, before its WPD recovery)\n"
        << "  hybrid-interrupt-pair --operator-gate <safe-name> --exclusive-camera-control-confirmed\n"
        << "    --dedicated-spool-scope-confirmed --dual-dedicated-spools-confirmed --exact-object-delete-confirmed\n"
        << "    (terminate this process after CAM-A completion; CAM-B can never resume from the gate)\n"
        << "  capture-single --alias CAM-A --count 10 --transport fake (contract harness only; real hardware is rejected)\n"
        << "  capture-pair --count 10 --transport fake (contract harness only; real hardware is rejected)\n"
        << "  stability --count 100 --transport fake (contract harness only; real hardware is rejected)\n"
        << "  fault-test --scenario no-candidate|ambiguous|late-candidate|invalid-jpeg|open-failure|capture-failure --transport fake\n"
        << "  report --run-id <id>\n";
}

Options Parse(int argc, char** argv) {
    if (argc < 2) throw std::runtime_error("missing command");
    Options options;
    options.command = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string arg = argv[index];
        const auto require_value = [&]() -> std::string {
            if (++index >= argc) throw std::runtime_error("missing value for " + arg);
            return argv[index];
        };
        if (arg == "--stage") options.stage = require_value();
        else if (arg == "--alias") { options.alias = require_value(); options.alias_explicit = true; }
        else if (arg == "--count") options.count = std::stoi(require_value());
        else if (arg == "--frames") options.frames = std::stoi(require_value());
        else if (arg == "--interval-ms") options.interval_ms = std::stoi(require_value());
        else if (arg == "--duration-seconds") options.duration_seconds = std::stoi(require_value());
        else if (arg == "--samples") { options.correlation_samples = std::stoi(require_value()); options.correlation_samples_explicit = true; }
        else if (arg == "--sample-interval-ms") { options.correlation_interval_ms = std::stoi(require_value()); options.correlation_interval_explicit = true; }
        else if (arg == "--save-last-frame") options.save_last_frame = true;
        else if (arg == "--exclusive-camera-control-confirmed") options.exclusive_camera_control_confirmed = true;
        else if (arg == "--dedicated-spool-scope-confirmed") options.dedicated_spool_scope_confirmed = true;
        else if (arg == "--dual-dedicated-spools-confirmed") options.dual_dedicated_spools_confirmed = true;
        else if (arg == "--exact-object-delete-confirmed") options.exact_object_delete_confirmed = true;
        else if (arg == "--single-camera-connected-confirmed") options.single_camera_connected_confirmed = true;
        else if (arg == "--scenario") options.scenario = require_value();
        else if (arg == "--run-id") options.run_id = require_value();
        else if (arg == "--transport") { options.transport = require_value(); options.transport_explicit = true; }
        else if (arg == "--wpd-command-target") {
            const auto value = require_value();
            if (value == "functional") options.wpd_command_target = WpdCommandTargetPolicy::functional;
            else if (value == "omit") options.wpd_command_target = WpdCommandTargetPolicy::omit;
            else throw std::runtime_error("wpd-command-target must be functional or omit");
        }
        else if (arg == "--wpd-status-access") {
            const auto value = require_value();
            if (value == "read-only") options.wpd_status_access = WpdStatusAccess::read_only;
            else if (value == "read-write") options.wpd_status_access = WpdStatusAccess::read_write;
            else throw std::runtime_error("wpd-status-access must be read-only or read-write");
            options.wpd_status_access_explicit = true;
        }
        else if (arg == "--operator-gate") options.operator_gate = require_value();
        else if (arg == "--operator-gate-timeout-seconds") options.operator_gate_timeout_seconds = std::stoi(require_value());
        else if (arg == "--artifacts") options.artifacts = require_value();
        else if (arg == "--reports") options.reports = require_value();
        else if (arg == "--camera-map") { options.camera_map = require_value(); options.camera_map_explicit = true; }
        else if (arg == "--single-identity-v3") {
            options.single_identity_v3 = require_value();
            options.single_identity_v3_explicit = true;
        }
        else throw std::runtime_error("unknown option: " + arg);
    }
    if (options.count < 1) throw std::runtime_error("count must be positive");
    if (const auto hybrid_error = ValidateHybridCaptureArguments(
            options.command,
            options.count,
            options.exclusive_camera_control_confirmed,
            options.dedicated_spool_scope_confirmed,
            options.exact_object_delete_confirmed,
            options.dual_dedicated_spools_confirmed)) {
        throw std::runtime_error(*hybrid_error);
    }
    if (options.frames < 1) throw std::runtime_error("frames must be positive");
    if (options.interval_ms < 0) throw std::runtime_error("interval-ms must not be negative");
    if (options.duration_seconds < 0) throw std::runtime_error("duration-seconds must not be negative");
    if (const auto correlation_error = ValidateWpdCorrelationArguments(
            options.command, options.correlation_samples_explicit, options.correlation_interval_explicit,
            options.correlation_samples, options.correlation_interval_ms)) {
        throw std::runtime_error(*correlation_error);
    }
    if (options.transport.empty()) {
        options.transport = options.command == "capture-single" || options.command == "capture-pair" ||
                options.command == "stability" || options.command == "fault-test"
            ? "fake"
            : options.command == "live-view" || options.command == "live-view-handoff" ||
                options.command == "sdk-status" || options.command == "hybrid-capture-single" ||
                options.command == "hybrid-capture-pair" ||
                options.command == "hybrid-fault-single" || options.command == "hybrid-fault-pair" ||
                options.command == "hybrid-interrupt-pair"
            ? "sdk"
            : "wpd";
    }
    if (options.alias != "CAM-A" && options.alias != "CAM-B") throw std::runtime_error("alias must be CAM-A or CAM-B");
    if (options.transport != "sdk" && options.transport != "wpd" && options.transport != "fake") {
        throw std::runtime_error("transport must be sdk, wpd, or fake");
    }
    if (const auto sdk_status_error = ValidateSdkStatusCliRouting(options.command, options.transport)) {
        throw std::runtime_error(*sdk_status_error);
    }
    if (options.single_identity_v3_explicit &&
        (options.command != "sdk-status" || options.alias != "CAM-A")) {
        throw std::runtime_error("single-identity-v3 is valid only for sdk-status --alias CAM-A");
    }
    if (options.command == "sdk-status" && options.single_identity_v3_explicit && options.camera_map_explicit) {
        throw std::runtime_error("sdk-status cannot combine SingleCamera identity-v3 with a legacy camera map");
    }
    if (const auto binding_error = ValidateIdentityBindingArguments(
            options.command,
            options.transport,
            options.single_camera_connected_confirmed,
            options.transport_explicit)) {
        throw std::runtime_error(*binding_error);
    }
    if (const auto direct_capture_error = ValidateDirectCaptureSafety(
            options.command, options.transport, options.operator_gate.has_value())) {
        throw std::runtime_error(*direct_capture_error);
    }
    if (options.wpd_status_access_explicit && options.command != "wpd-status") {
        throw std::runtime_error("wpd-status-access is valid only for wpd-status");
    }
    if (options.operator_gate) {
        const bool hybrid_fault_gate =
            (options.command == "hybrid-fault-single" || options.command == "hybrid-fault-pair" ||
             options.command == "hybrid-interrupt-pair") &&
            options.transport == "sdk" && options.count == 1;
        if (!hybrid_fault_gate) {
            throw std::runtime_error(
                "operator gate requires hybrid-fault-single, hybrid-fault-pair, or hybrid-interrupt-pair");
        }
        if (!OperatorGate::IsSafeName(*options.operator_gate)) {
            throw std::runtime_error("operator gate name must match [A-Za-z0-9_-] and be 1-64 characters");
        }
        if (options.operator_gate_timeout_seconds < 1 || options.operator_gate_timeout_seconds > 3600) {
            throw std::runtime_error("operator gate timeout must be between 1 and 3600 seconds");
        }
    }
    if (options.command == "hybrid-fault-single" || options.command == "hybrid-fault-pair") {
        if (!options.operator_gate) throw std::runtime_error(options.command + " requires --operator-gate");
        if (options.scenario != "usb-disconnect" && options.scenario != "power-off") {
            throw std::runtime_error(options.command + " scenario must be usb-disconnect or power-off");
        }
    }
    if (options.command == "hybrid-fault-pair" && !options.alias_explicit) {
        throw std::runtime_error("hybrid-fault-pair requires explicit --alias CAM-A or CAM-B");
    }
    if (options.command == "hybrid-interrupt-pair") {
        if (!options.operator_gate) throw std::runtime_error("hybrid-interrupt-pair requires --operator-gate");
        if (options.alias_explicit) throw std::runtime_error("hybrid-interrupt-pair does not accept --alias");
        if (!options.scenario.empty()) throw std::runtime_error("hybrid-interrupt-pair does not accept --scenario");
    }
    return options;
}

int RunWpdCorrelationStatus(const Options& options) {
    if (options.transport != "wpd") throw std::runtime_error("wpd-correlation-status requires --transport wpd");
    const std::string run_id = NewRunId();
    WpdTransport transport;
    WpdCorrelationRunSummary summary;
    try {
        const auto camera = ResolveCamera(transport, WpdIdentityMapPath(options), options.alias);
        summary = ExecuteWpdCorrelationSamples(
            transport, camera.stable_identity, options.correlation_samples, options.correlation_interval_ms);
    } catch (...) {
        summary.terminal_state = "Failed";
        summary.failed_stage = "resolve_camera";
    }
    const auto path = PersistWpdCorrelationSummary(options.artifacts, run_id, options.alias, summary);
    EvidenceWriter evidence(options.artifacts, run_id, transport.SdkVersion());
    evidence.GenerateRedactedReport(options.reports);
    std::cout << "RunId: " << run_id << "\nSampleCount: " << summary.sample_count
              << "\nDeviceDatetimeAvailableCount: " << summary.device_datetime_available_count
              << "\nReopenAdvanceCount: " << summary.reopen_advance_count
              << "\nReopenEqualCount: " << summary.reopen_equal_count
              << "\nReopenRegressCount: " << summary.reopen_regress_count
              << "\nJpegCount: " << summary.jpeg_count
              << "\nDatedJpegCount: " << summary.dated_jpeg_count
              << "\nWpdSessionsClosed: " << summary.wpd_sessions_closed
              << "\nTerminalState: " << summary.terminal_state
              << "\nFailedStage: " << summary.failed_stage
              << "\nSummaryPath: " << path.string()
              << "\nReadOnlyObservation: true\nCaptureCommandSent: false\nVendorOperationExecuted: false\n";
    return summary.terminal_state == "Complete" ? 0 : 5;
}

FakeFailureMode ParseFailure(std::string_view scenario) {
    if (scenario == "no-candidate") return FakeFailureMode::no_candidate;
    if (scenario == "ambiguous") return FakeFailureMode::ambiguous;
    if (scenario == "late-candidate") return FakeFailureMode::late_candidate;
    if (scenario == "invalid-jpeg") return FakeFailureMode::invalid_jpeg;
    if (scenario == "open-failure") return FakeFailureMode::open_failure;
    if (scenario == "capture-failure") return FakeFailureMode::capture_failure;
    throw std::runtime_error("unsupported fault scenario");
}

std::unique_ptr<ICameraTransport> MakeTransport(const Options& options) {
    if (options.transport == "fake") return std::make_unique<FakeCameraTransport>();
    if (options.transport == "wpd") return std::make_unique<WpdTransport>(options.wpd_command_target);
    return std::make_unique<NikonSdkTransport>();
}

fs::path IdentityMapPath(const Options& options) {
    if (options.transport != "wpd" || options.camera_map != DefaultIdentityMapPath()) return options.camera_map;
    auto path = options.camera_map;
    path.replace_filename("camera-map-wpd.json");
    return path;
}

fs::path WpdIdentityMapPath(const Options& options) {
    if (options.camera_map != DefaultIdentityMapPath()) {
        auto path = options.camera_map;
        path.replace_filename(path.stem().string() + "-wpd" + path.extension().string());
        return path;
    }
    auto path = options.camera_map;
    path.replace_filename("camera-map-wpd.json");
    return path;
}

int Preflight(const Options& options) {
    const bool stage_valid = options.stage == "single" || options.stage == "dual";
    const auto sdk_root = EnvironmentValue("NIKON_D810_SDK_ROOT");
    const bool sdk_present = sdk_root && !sdk_root->empty() && fs::exists(*sdk_root);
    std::cout << "CameraModel: Nikon D810\nStage: " << options.stage
              << "\nSdkRootPresent: " << (sdk_present ? "true" : "false")
              << "\nLicensedAdapterAvailable: " << (NikonSdkTransport::LicensedAdapterAvailable() ? "true" : "false") << '\n';
    if (!stage_valid || !sdk_present || !NikonSdkTransport::LicensedAdapterAvailable()) {
        std::cout << "Phase0CliPreflight: BLOCKED\n";
        return 2;
    }
    std::cout << "Phase0CliPreflight: READY\n";
    return 0;
}

int Inventory(const Options& options) {
    auto transport = MakeTransport(options);
    IdentityMap map(IdentityMapPath(options));
    const auto cameras = transport->Enumerate();
    const auto summary = SummarizeInventoryReadOnly(map, cameras);
    for (const auto& camera : summary.cameras) {
        std::cout << camera.alias << " model=" << camera.model << " firmware=" << camera.firmware
                  << " shootingMode=" << camera.shooting_mode << '\n';
    }
    std::cout << "CameraCount: " << summary.cameras.size()
              << "\nBoundCameraCount: " << summary.bound_camera_count
              << "\nUnboundCameraCount: " << summary.unbound_camera_count
              << "\nIdentityMapChanged: false"
              << "\nReal identifiers were not printed.\n";
    return cameras.empty() ? 3 : 0;
}

int BindIdentity(const Options& options) {
    auto transport = MakeTransport(options);
    const auto camera = SelectSingleCameraForBinding(transport->Enumerate());
    IdentityMap map(IdentityMapPath(options));
    map.Bind(options.alias, camera.stable_identity);
    std::cout << "CameraAlias: " << options.alias
              << "\nTransport: " << options.transport
              << "\nCameraModel: " << camera.model
              << "\nBindingState: bound"
              << "\nSingleCameraConnectedConfirmed: true"
              << "\nReal identifiers were not printed.\n";
    return 0;
}

int BindCrossTransportIdentityCommand(const Options& options) {
    NikonSdkTransport sdk;
    WpdTransport wpd(options.wpd_command_target);
    const auto sdk_cameras = sdk.Enumerate();
    const auto wpd_cameras = wpd.Enumerate();
    IdentityMap sdk_map(options.camera_map);
    IdentityMap wpd_map(WpdIdentityMapPath(options));
    const auto sdk_before = SummarizeInventoryReadOnly(sdk_map, sdk_cameras);
    const auto wpd_before = SummarizeInventoryReadOnly(wpd_map, wpd_cameras);
    const auto selection = BindCrossTransportIdentity(
        sdk_map, wpd_map, options.alias, sdk_cameras, wpd_cameras);
    const bool maps_changed = sdk_before.unbound_camera_count != 0 || wpd_before.unbound_camera_count != 0;
    std::cout << "CameraAlias: " << options.alias
              << "\nSdkCameraModel: " << selection.sdk_camera.model
              << "\nWpdCameraModel: " << selection.wpd_camera.model
              << "\nSdkCameraCount: " << sdk_cameras.size()
              << "\nWpdCameraCount: " << wpd_cameras.size()
              << "\nBindingState: bound-both"
              << "\nSingleCameraConnectedConfirmed: true"
              << "\nSdkSessionClosedBeforeWpdInventory: true"
              << "\nBothMapsValidatedBeforeBinding: true"
              << "\nIdentityMapsChanged: " << (maps_changed ? "true" : "false")
              << "\nCaptureCommandSent: false"
              << "\nLiveViewStarted: false"
              << "\nCameraSettingsChanged: false"
              << "\nCardAccessPerformed: false"
              << "\nReal identifiers were not printed.\n";
    return 0;
}

int BindSingleIdentityV3Command(const Options& options) {
    if (options.alias != "CAM-A") {
        throw std::runtime_error("bind-single-identity-v3 supports only product alias CAM-A");
    }
    NikonSdkTransport sdk;
    WpdTransport wpd(options.wpd_command_target);
    sdk.RequireExactlyOneD810ForProductAgent();
    wpd.RequireExactlyOneD810ForProductAgent();
    const auto sdk_cameras = sdk.Enumerate();
    const auto wpd_cameras = wpd.Enumerate();
    const auto path = DefaultSingleIdentityV3Path();
    PersistSingleIdentityV3(path, options.alias, sdk_cameras, wpd_cameras);
    std::cout << "CameraAlias: CAM-A"
              << "\nSdkCameraCount: " << sdk_cameras.size()
              << "\nWpdCameraCount: " << wpd_cameras.size()
              << "\nIdentityStrategy: wpd-serial-digest-plus-exactly-one-sdk-session"
              << "\nBindingState: identity-v3-created"
              << "\nSingleCameraConnectedConfirmed: true"
              << "\nCaptureCommandSent: false"
              << "\nLiveViewStarted: false"
              << "\nCameraSettingsChanged: false"
              << "\nCardAccessPerformed: false"
              << "\nReal identifiers were not printed.\n";
    return 0;
}

int VerifyDualIdentityCommand(const Options& options) {
    NikonSdkTransport sdk;
    WpdTransport wpd(options.wpd_command_target);
    const auto sdk_cameras = sdk.Enumerate();
    const auto wpd_cameras = wpd.Enumerate();
    IdentityMap sdk_map(options.camera_map);
    IdentityMap wpd_map(WpdIdentityMapPath(options));
    const auto summary = VerifyDualIdentityBindings(
        sdk_map, wpd_map, sdk_cameras, wpd_cameras);
    const std::string run_id = NewRunId();
    const auto summary_path = PersistDualIdentityVerificationSummary(
        options.artifacts, run_id, summary);
    EvidenceWriter evidence(
        options.artifacts, run_id, sdk.SdkVersion() + "+" + wpd.SdkVersion());
    evidence.GenerateRedactedReport(options.reports);
    std::cout << "RunId: " << run_id
              << "\nSdkCameraCount: " << summary.sdk_camera_count
              << "\nSdkCamACount: " << summary.sdk_cam_a_count
              << "\nSdkCamBCount: " << summary.sdk_cam_b_count
              << "\nSdkUnboundCount: " << summary.sdk_unbound_count
              << "\nWpdCameraCount: " << summary.wpd_camera_count
              << "\nWpdCamACount: " << summary.wpd_cam_a_count
              << "\nWpdCamBCount: " << summary.wpd_cam_b_count
              << "\nWpdUnboundCount: " << summary.wpd_unbound_count
              << "\nIdentityMapsChanged: false"
              << "\nCaptureCommandSent: false"
              << "\nLiveViewStarted: false"
              << "\nCameraSettingsChanged: false"
              << "\nCardAccessPerformed: false"
              << "\nTerminalState: " << summary.terminal_state
              << "\nFailureCategory: " << summary.failure_category
              << "\nRealIdentifiersPrinted: false"
              << "\nSummaryPath: " << summary_path.string() << '\n';
    return summary.terminal_state == "Ready" ? 0 : 5;
}

int VerifyDualSpoolsCommand(const Options& options) {
    NikonSdkTransport sdk;
    WpdTransport wpd(options.wpd_command_target);
    const auto sdk_cameras = sdk.Enumerate();
    const auto wpd_cameras = wpd.Enumerate();
    IdentityMap sdk_map(options.camera_map);
    IdentityMap wpd_map(WpdIdentityMapPath(options));
    const auto identity = VerifyDualIdentityBindings(
        sdk_map, wpd_map, sdk_cameras, wpd_cameras);
    auto summary = PrepareDualSpoolVerification(identity);
    if (summary.terminal_state == "ReadyForInspection") {
        try {
            const auto cam_a = ResolveCamera(wpd_cameras, WpdIdentityMapPath(options), "CAM-A");
            const auto cam_b = ResolveCamera(wpd_cameras, WpdIdentityMapPath(options), "CAM-B");
            summary.cam_a_payload_object_count = wpd.InspectSpoolPayloadCount(
                cam_a.stable_identity, std::chrono::seconds(30));
            ++summary.wpd_sessions_closed;
            summary.card_inspection_performed = true;
            summary.cam_b_payload_object_count = wpd.InspectSpoolPayloadCount(
                cam_b.stable_identity, std::chrono::seconds(30));
            ++summary.wpd_sessions_closed;
            FinalizeDualSpoolVerification(
                summary, summary.cam_a_payload_object_count, summary.cam_b_payload_object_count);
        } catch (const TransportError& error) {
            summary.terminal_state = "Failed";
            summary.failure_category = error.Category();
        } catch (const std::exception&) {
            summary.terminal_state = "Failed";
            summary.failure_category = "resolve_or_inspect_failed";
        }
    }
    const std::string run_id = NewRunId();
    const auto summary_path = PersistDualSpoolVerificationSummary(
        options.artifacts, run_id, summary);
    EvidenceWriter evidence(
        options.artifacts, run_id, sdk.SdkVersion() + "+" + wpd.SdkVersion());
    evidence.GenerateRedactedReport(options.reports);
    std::cout << "RunId: " << run_id
              << "\nSdkCameraCount: " << summary.identity.sdk_camera_count
              << "\nWpdCameraCount: " << summary.identity.wpd_camera_count
              << "\nCamAPayloadObjectCount: " << summary.cam_a_payload_object_count
              << "\nCamBPayloadObjectCount: " << summary.cam_b_payload_object_count
              << "\nWpdSessionsClosed: " << summary.wpd_sessions_closed
              << "\nReadOnlyObservation: true"
              << "\nCardInspectionPerformed: " << (summary.card_inspection_performed ? "true" : "false")
              << "\nCaptureCommandSent: false"
              << "\nCameraDeleteAttempted: false"
              << "\nVendorOperationExecuted: false"
              << "\nAutomaticRetry: false"
              << "\nTerminalState: " << summary.terminal_state
              << "\nFailureCategory: " << summary.failure_category
              << "\nRealIdentifiersPrinted: false"
              << "\nSummaryPath: " << summary_path.string() << '\n';
    return summary.terminal_state == "Ready" ? 0 : 5;
}

CameraInfo ResolveCamera(
    const std::vector<CameraInfo>& cameras,
    const fs::path& camera_map,
    std::string_view alias) {
    IdentityMap identity_map(camera_map);
    std::optional<CameraInfo> selected;
    for (const auto& camera : cameras) {
        const auto existing_alias = identity_map.FindAlias(camera.stable_identity);
        if (!existing_alias) {
            throw std::runtime_error(
                "enumerated D810 is not explicitly bound; connect one body at a time and run bind-cross-transport-identity");
        }
        if (*existing_alias == alias) {
            if (selected) throw std::runtime_error("multiple cameras resolved to the requested alias");
            selected = camera;
        }
    }
    if (!selected) throw std::runtime_error("requested camera alias is not available");
    return *selected;
}

CameraInfo ResolveCamera(
    ICameraTransport& transport,
    const fs::path& camera_map,
    std::string_view alias) {
    return ResolveCamera(transport.Enumerate(), camera_map, alias);
}

int RunSdkStatus(const Options& options) {
    if (const auto routing_error = ValidateSdkStatusCliRouting(options.command, options.transport)) {
        throw std::runtime_error(*routing_error);
    }
    SdkStatusProcessRouting routing;
    routing.sdk_status_executor_selected = true;
    const bool use_single_identity_v3 =
        SelectSdkStatusIdentityRoute(options.alias, options.camera_map_explicit) ==
        SdkStatusIdentityRoute::single_identity_v3;
    if (use_single_identity_v3) {
        routing.single_identity_v3_selected = true;
    }

    CameraInfo camera;
    SdkCameraStatus status;
    std::string sdk_version;
    if (use_single_identity_v3) {
        auto execution = ExecuteSingleIdentityV3SdkStatus(
            options.single_identity_v3,
            options.alias,
            [&options]() -> std::unique_ptr<ISingleIdentityV3WpdEnumerator> {
                return std::make_unique<ProductSingleIdentityV3WpdEnumerator>(
                    options.wpd_command_target);
            },
            []() -> std::unique_ptr<ISdkStatusExecutor> {
                return std::make_unique<NikonSdkStatusExecutor>();
            },
            std::chrono::seconds(10));
        camera = std::move(execution.camera);
        status = std::move(execution.status);
        routing = execution.routing;
        sdk_version = std::move(execution.sdk_version);
    } else {
        NikonSdkStatusExecutor executor;
        const auto cameras = executor.Enumerate();
        ++routing.sdk_enumeration_count;
        camera = ResolveCamera(cameras, options.camera_map, options.alias);
        status = executor.ProbeSdkStatus(
            camera.stable_identity, std::chrono::seconds(10));
        ++routing.sdk_status_probe_count;
        sdk_version = executor.SdkVersion();
    }
    if (const auto routing_failure = ValidateSdkStatusProcessRouting(routing)) {
        throw std::runtime_error("SDK status process routing failed: " + std::string(*routing_failure));
    }
    if (status.firmware == "unknown" && camera.firmware != "unknown") status.firmware = camera.firmware;

    const std::string run_id = NewRunId();
    const auto summary = PersistSdkStatusSummary(
        options.artifacts, run_id, options.alias, camera, status, routing);
    EvidenceWriter evidence(options.artifacts, run_id, sdk_version);
    evidence.GenerateRedactedReport(options.reports);

    const auto write_setting = [](std::string_view name, const SdkCameraStatus::SettingCapability& setting) {
        std::cout << '\n' << name << "Available: " << (setting.available ? "true" : "false")
                  << '\n' << name << "CapType: " << setting.cap_type
                  << '\n' << name << "ProbeState: " << setting.probe_state
                  << '\n' << name << "ValueType: " << setting.value_type
                  << '\n' << name << "CurrentValue: ";
        if (setting.current_value) std::cout << *setting.current_value;
        else std::cout << "unknown";
        std::cout << '\n' << name << "CurrentIndex: ";
        if (setting.current_index) std::cout << *setting.current_index;
        else std::cout << "unknown";
        std::cout << '\n' << name << "CurrentLabel: ";
        if (setting.current_label) std::cout << EscapeCliValue(*setting.current_label);
        else std::cout << "unknown";
        std::cout << '\n' << name << "NumericValues:";
        for (const std::uint32_t value : setting.numeric_values) std::cout << ' ' << value;
        std::cout << '\n' << name << "StringValues:";
        for (const std::string& value : setting.string_values) std::cout << ' ' << EscapeCliValue(value);
    };
    std::cout << "RunId: " << run_id
              << "\nCameraAlias: " << options.alias
              << "\nModel: " << camera.model
              << "\nFirmware: " << status.firmware
              << "\nShootingMode: " << camera.shooting_mode
              << "\nLiveViewStatus: " << status.live_view_status
              << "\nLiveViewStatusAvailable: " << (status.live_view_status_available ? "true" : "false")
              << "\nLiveViewSelector: " << status.live_view_selector
              << "\nLiveViewSelectorAvailable: " << (status.live_view_selector_available ? "true" : "false")
              << "\nLiveViewProhibitMask: ";
    if (status.live_view_prohibit_mask) {
        std::cout << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
                  << *status.live_view_prohibit_mask << std::dec;
    } else {
        std::cout << "unknown";
    }
    std::cout << "\nLiveViewProhibitAvailable: " << (status.live_view_prohibit_mask ? "true" : "false")
              << "\nSdkStatusCapGetCount: " << status.command_trace.cap_get_count
              << "\nSdkStatusCapGetArrayCount: " << status.command_trace.cap_get_array_count
              << "\nSdkStatusCapSetCount: " << status.command_trace.cap_set_count
              << "\nSdkStatusControlPlaneCapSetCount: " << status.command_trace.control_plane_cap_set_count
              << "\nSdkStatusPhotographicSettingCapSetCount: "
              << status.command_trace.photographic_setting_cap_set_count
              << "\nSdkStatusCapStartCount: " << status.command_trace.cap_start_count
              << "\nSdkStatusCaptureStartCount: " << status.command_trace.capture_start_count
              << "\nSdkStatusNonCaptureStartCount: " << status.command_trace.non_capture_start_count
              << "\nSdkStatusUnknownCapStartCount: " << status.command_trace.unknown_cap_start_count
              << "\nSdkStatusLiveViewStartCount: " << status.command_trace.live_view_start_count
              << "\nSdkStatusCommandTraceReadOnly: true"
              << "\nSdkStatusProcessRoutingContract: true"
              << "\nSdkStatusExecutorSelected: "
              << (routing.sdk_status_executor_selected ? "true" : "false")
              << "\nSingleIdentityV3Selected: "
              << (routing.single_identity_v3_selected ? "true" : "false")
              << "\nSdkStatusEnumerationCount: " << routing.sdk_enumeration_count
              << "\nSdkStatusProbeCount: " << routing.sdk_status_probe_count
              << "\nSdkStatusWpdIdentityEnumerationCount: " << routing.wpd_identity_enumeration_count
              << "\nSdkStatusWpdCallCount: " << routing.wpd_call_count
              << "\nSdkStatusCaptureCallCount: " << routing.capture_call_count
              << "\nSdkStatusDeleteCallCount: " << routing.delete_call_count
              << "\nCameraSettingReadOnlyProbe: true"
              << "\nCameraSettingWriteAttempted: "
              << (status.command_trace.photographic_setting_cap_set_count != 0 ? "true" : "false")
              << "\nSdkControlPlaneCallbackRegistrationMayUseCapSet: true"
              << "\nCameraSettingsChanged: false"
              << "\nLiveViewStarted: "
              << (status.command_trace.live_view_start_count != 0 ? "true" : "false")
              << "\nSdkSessionClosed: "
              << (status.command_trace.sdk_session_closed ? "true" : "false")
              << "\nRealIdentifiersPrinted: false"
              << "\nSummaryPath: " << summary.string();
    write_setting("FileType", status.file_type);
    write_setting("CompressionLevel", status.compression_level);
    write_setting("ImageSize", status.image_size);
    write_setting("ExposureMode", status.exposure_mode);
    write_setting("ShutterSpeed", status.shutter_speed);
    write_setting("Aperture", status.aperture);
    write_setting("Sensitivity", status.sensitivity);
    write_setting("WBMode", status.wb_mode);
    write_setting("FocusMode", status.focus_mode);
    std::cout << '\n';
    return 0;
}

int RunWpdStatus(const Options& options) {
    if (options.transport != "wpd") throw std::runtime_error("wpd-status requires --transport wpd");
    WpdTransport transport(options.wpd_command_target);
    const auto camera = ResolveCamera(transport, WpdIdentityMapPath(options), options.alias);
    const auto target = transport.ProbeCaptureTarget(camera.stable_identity);
    const auto vendor = transport.ProbeVendorOpcodes(camera.stable_identity, options.wpd_status_access);
    const auto run_id = NewRunId();
    const WpdStatusSummary status{
        target.validation_state,
        target.command_options_hresult,
        target.option_value_hresult,
        target.functional_object_count,
        target.valid_object_id_count,
        target.compatible_target_count,
        target.valid_object_ids_option_present,
        target.selected_target,
        vendor.validation_state,
        vendor.supported_commands_hresult,
        vendor.query_send_hresult,
        vendor.query_common_hresult,
        vendor.wpd_still_image_capture_command_advertised,
        vendor.vendor_opcode_query_advertised,
        vendor.read_only_command_sent,
        vendor.vendor_opcode_collection_available,
        vendor.vendor_opcode_item_count,
        vendor.vendor_opcode_unique_count,
        vendor.vendor_capture_9207_advertised,
        vendor.standard_opcode_100e_advertisement_available,
        vendor.standard_opcode_100e_advertisement_state,
        vendor.requested_access,
        vendor.read_only_access,
    };
    const auto summary = PersistWpdStatusSummary(options.artifacts, run_id, options.alias, camera, status);
    EvidenceWriter evidence(options.artifacts, run_id, transport.SdkVersion());
    evidence.GenerateRedactedReport(options.reports);
    std::cout << "RunId: " << run_id
              << "\nCameraAlias: " << options.alias
              << "\nModel: " << camera.model
              << "\nFirmware: " << camera.firmware
              << "\nTargetValidationState: " << target.validation_state
              << "\nCommandOptionsHRESULT: " << target.command_options_hresult
              << "\nOptionValueHRESULT: " << target.option_value_hresult
              << "\nFunctionalObjectCount: " << target.functional_object_count
              << "\nValidObjectIdsOptionPresent: " << (target.valid_object_ids_option_present ? "true" : "false")
              << "\nValidObjectIdCount: " << target.valid_object_id_count
              << "\nCompatibleTargetCount: " << target.compatible_target_count
              << "\nSelectedTarget: " << (target.selected_target ? "true" : "false")
              << "\nVendorOpcodeValidationState: " << vendor.validation_state
              << "\nSupportedCommandsHRESULT: " << vendor.supported_commands_hresult
              << "\nWpdStillImageCaptureCommandAdvertised: " << (vendor.wpd_still_image_capture_command_advertised ? "true" : "false")
              << "\nVendorOpcodeQueryAdvertised: " << (vendor.vendor_opcode_query_advertised ? "true" : "false")
              << "\nVendorOpcodeQuerySendHRESULT: " << vendor.query_send_hresult
              << "\nVendorOpcodeQueryCommonHRESULT: " << vendor.query_common_hresult
              << "\nVendorOpcodeCollectionAvailable: " << (vendor.vendor_opcode_collection_available ? "true" : "false")
              << "\nVendorOpcodeItemCount: " << vendor.vendor_opcode_item_count
              << "\nVendorOpcodeUniqueCount: " << vendor.vendor_opcode_unique_count
              << "\nVendorCapture9207Advertised: " << (vendor.vendor_capture_9207_advertised ? "true" : "false")
              << "\nStandardOpcode100eAdvertisementAvailable: "
              << (vendor.standard_opcode_100e_advertisement_available ? "true" : "false")
              << "\nStandardOpcode100eAdvertisementState: " << vendor.standard_opcode_100e_advertisement_state
              << "\nWpdRequestedAccess: " << vendor.requested_access
              << "\nReadOnlyAccess: " << (vendor.read_only_access ? "true" : "false")
              << "\nNonMutatingProbe: true"
              << "\nReadOnlyCommandSent: " << (vendor.read_only_command_sent ? "true" : "false")
              << "\nCaptureCommandSent: false"
              << "\nVendorOperationExecuted: false"
              << "\nRealIdentifiersPrinted: false"
              << "\nSummaryPath: " << summary.string() << '\n';
    return target.selected_target && vendor.vendor_opcode_collection_available ? 0 : 4;
}

int RunSpoolStatus(const Options& options) {
    if (options.transport != "wpd") throw std::runtime_error("spool-status requires --transport wpd");
    const std::string run_id = NewRunId();
    WpdTransport transport(options.wpd_command_target);
    WpdSpoolStatusSummary status;
    try {
        const auto camera = ResolveCamera(transport, WpdIdentityMapPath(options), options.alias);
        status.payload_object_count = transport.InspectSpoolPayloadCount(
            camera.stable_identity, std::chrono::seconds(10));
        status.wpd_sessions_closed = 1;
        status.terminal_state = "Complete";
    } catch (const TransportError& error) {
        status.terminal_state = "Failed";
        status.failed_stage = error.Category();
    } catch (const std::exception&) {
        status.terminal_state = "Failed";
        status.failed_stage = "resolve_or_inspect";
    }
    const auto summary = PersistWpdSpoolStatusSummary(
        options.artifacts, run_id, options.alias, status);
    EvidenceWriter evidence(options.artifacts, run_id, transport.SdkVersion());
    evidence.GenerateRedactedReport(options.reports);
    std::cout << "RunId: " << run_id
              << "\nCameraAlias: " << options.alias
              << "\nPayloadObjectCount: " << status.payload_object_count
              << "\nSpoolState: "
              << (status.terminal_state == "Complete"
                      ? (status.payload_object_count == 0 ? "EMPTY" : "NON_EMPTY")
                      : "UNKNOWN")
              << "\nReadOnlyObservation: true"
              << "\nCaptureCommandSent: false"
              << "\nCameraObjectDeleteAttempted: false"
              << "\nVendorOperationExecuted: false"
              << "\nWpdSessionsClosed: " << status.wpd_sessions_closed
              << "\nTerminalState: " << status.terminal_state
              << "\nFailedStage: " << status.failed_stage
              << "\nRealIdentifiersPrinted: false"
              << "\nSummaryPath: " << summary.string() << '\n';
    return status.terminal_state == "Complete" ? 0 : 5;
}

fs::path PersistLastPreview(
    const Options& options,
    std::string_view run_id,
    const std::vector<unsigned char>& frame) {
    const fs::path directory = options.artifacts / std::string(run_id) / "live-view" / options.alias;
    fs::create_directories(directory);
    const fs::path partial = directory / "last.jpg.partial";
    const fs::path final = directory / "last.jpg";
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create live view preview artifact");
    output.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
    output.close();
    if (!output) throw std::runtime_error("cannot persist live view preview artifact");
    if (fs::exists(final)) throw std::runtime_error("refusing to overwrite a live view preview artifact");
    fs::rename(partial, final);
    return final;
}

fs::path PersistLiveViewSummary(
    const Options& options,
    std::string_view run_id,
    const LiveViewProbeResult& result,
    bool preview_persisted) {
    const fs::path run_root = options.artifacts / std::string(run_id);
    fs::create_directories(run_root);
    const fs::path summary = run_root / "live-view-summary.json";
    std::ofstream output(summary, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create live view summary");
    output << "{\n"
           << "  \"schemaVersion\": \"phase0.live-view-summary.v1\",\n"
           << "  \"runId\": \"" << run_id << "\",\n"
           << "  \"cameraAlias\": \"" << options.alias << "\",\n"
           << "  \"frames\": " << result.frames << ",\n"
           << "  \"lastFrameBytes\": " << result.last_frame.size() << ",\n"
           << "  \"durationMs\": " << result.duration.count() << ",\n"
           << "  \"liveViewStopped\": true,\n"
           << "  \"sdkSessionClosed\": true,\n"
           << "  \"previewFramePersisted\": " << (preview_persisted ? "true" : "false") << "\n"
           << "}\n";
    if (!output) throw std::runtime_error("cannot persist live view summary");
    return summary;
}

int RunLiveView(const Options& options) {
    if (options.transport != "sdk") throw std::runtime_error("live-view requires --transport sdk");
    NikonSdkTransport transport;
    const auto camera = ResolveCamera(transport, options.camera_map, options.alias);
    const std::string run_id = NewRunId();
    const auto result = AcquireLiveViewFrames(
        transport, camera.stable_identity, options.frames, options.interval_ms, options.duration_seconds);

    std::optional<fs::path> saved;
    if (options.save_last_frame) saved = PersistLastPreview(options, run_id, result.last_frame);
    const auto summary = PersistLiveViewSummary(options, run_id, result, saved.has_value());
    std::cout << "RunId: " << run_id
              << "\nCameraAlias: " << options.alias
              << "\nFrames: " << result.frames
              << "\nLastFrameBytes: " << result.last_frame.size()
              << "\nDurationMs: " << result.duration.count()
              << "\nLiveViewStopped: true\nSdkSessionClosed: true"
              << "\nSummaryPath: " << summary.string() << '\n';
    if (saved) std::cout << "LastFramePath: " << saved->string() << '\n';
    return 0;
}

int RunLiveViewHandoff(const Options& options) {
    if (options.transport != "sdk") throw std::runtime_error("live-view-handoff uses SDK plus WPD and does not accept another transport");

    NikonSdkTransport sdk;
    WpdTransport wpd(options.wpd_command_target);
    const auto sdk_cameras = sdk.Enumerate();
    const auto wpd_cameras = wpd.Enumerate();
    if (sdk_cameras.size() != 1 || wpd_cameras.size() != 1) {
        throw TransportError(
            "cross_transport_binding_required",
            "live-view-handoff currently requires exactly one physical D810 connected and exactly one SDK/WPD projection; disconnect every other D810 until dual-camera cross-transport binding is registered");
    }
    const auto sdk_camera = ResolveCamera(sdk_cameras, options.camera_map, options.alias);
    const auto wpd_camera = ResolveCamera(wpd_cameras, WpdIdentityMapPath(options), options.alias);
    const std::string run_id = NewRunId();
    EvidenceWriter evidence(options.artifacts, run_id, sdk.SdkVersion() + "+" + wpd.SdkVersion());
    evidence.RecordCamera(options.alias, sdk_camera.firmware);
    LiveViewHandoffRunSummary summary;
    summary.requested = options.count;
    auto summary_path = PersistLiveViewHandoffSummary(options.artifacts, run_id, options.alias, summary);

    for (int index = 0; index < options.count; ++index) {
        ++summary.attempted;
        summary_path = PersistLiveViewHandoffSummary(options.artifacts, run_id, options.alias, summary);
        const std::string handoff_id = "handoff-" + std::to_string(index + 1);
        const auto capture_once = [&] {
            return ExecuteHybridCaptureOnce(
                wpd,
                wpd,
                sdk,
                sdk,
                evidence,
                options.alias,
                wpd_camera.stable_identity,
                sdk_camera.stable_identity,
                {});
        };
        const auto result = ExecuteLiveViewHandoffOnce(
            sdk,
            sdk_camera.stable_identity,
            capture_once,
            evidence,
            handoff_id,
            options.alias,
            options.frames,
            options.interval_ms);
        summary.spool_empty_before_count += result.capture.spool_empty_before_capture ? 1 : 0;
        summary.camera_card_delete_attempted_count += result.capture.camera_card_delete_attempted ? 1 : 0;
        summary.camera_card_delete_succeeded_count += result.capture.camera_card_delete_succeeded ? 1 : 0;
        summary.spool_empty_after_count += result.capture.spool_empty_after_cleanup ? 1 : 0;
        summary.last_handoff_state = result.terminal_state;
        summary.last_error_category = result.error_category;
        summary.last_error_detail = result.error_detail;

        if (result.terminal_state == "Complete") {
            ++summary.completed;
            summary.terminal_state = summary.completed == summary.requested ? "Complete" : "InProgress";
            summary_path = PersistLiveViewHandoffSummary(options.artifacts, run_id, options.alias, summary);
            std::cout << "Handoff " << index + 1
                      << ": beforeBytes=" << result.before.last_frame.size()
                      << " capture=" << result.capture.terminal_state
                      << " afterBytes=" << result.after.last_frame.size() << '\n';
        } else {
            ++summary.failures;
            summary.terminal_state = "FailedPartial";
            summary_path = PersistLiveViewHandoffSummary(options.artifacts, run_id, options.alias, summary);
            std::cerr << "Handoff " << index + 1
                      << " failed: state=" << result.terminal_state
                      << " category=" << result.error_category
                      << " detail=" << result.error_detail
                      << " capture=" << (result.capture.terminal_state.empty()
                              ? "NotStarted"
                              : result.capture.terminal_state)
                      << " resume=" << (result.resume_attempted ? "Attempted" : "Skipped") << '\n';
            break;
        }
    }

    if (summary.completed == summary.requested && summary.failures == 0) {
        summary.terminal_state = "Complete";
    } else if (summary.terminal_state != "FailedPartial") {
        summary.terminal_state = "FailedPartial";
    }
    summary_path = PersistLiveViewHandoffSummary(options.artifacts, run_id, options.alias, summary);

    std::cout << "RunId: " << run_id
              << "\nRequestedHandoffs: " << options.count
              << "\nAttemptedHandoffs: " << summary.attempted
              << "\nCompleteHandoffs: " << summary.completed
              << "\nFailures: " << summary.failures
              << "\nSpoolEmptyBefore: " << summary.spool_empty_before_count
              << "\nCameraCardDeleteAttempted: " << summary.camera_card_delete_attempted_count
              << "\nCameraCardDeleteSucceeded: " << summary.camera_card_delete_succeeded_count
              << "\nSpoolEmptyAfter: " << summary.spool_empty_after_count
              << "\nTerminalState: " << summary.terminal_state
              << "\nSummaryPath: " << summary_path.string()
              << "\nPreviewFramesPersisted: false\n";
    return summary.terminal_state == "Complete" ? 0 : 5;
}

int RunHybridCapture(const Options& options) {
    if (options.transport != "sdk") throw std::runtime_error("hybrid-capture-single does not accept --transport");
    NikonSdkTransport sdk;
    WpdTransport wpd(options.wpd_command_target);
    const auto sdk_cameras = sdk.Enumerate();
    const auto wpd_cameras = wpd.Enumerate();
    if (sdk_cameras.size() != 1 || wpd_cameras.size() != 1) {
        throw std::runtime_error("hybrid capture requires exactly one physical D810 in both SDK and WPD inventories");
    }
    const auto sdk_camera = ResolveCamera(sdk_cameras, options.camera_map, options.alias);
    const auto wpd_camera = ResolveCamera(wpd_cameras, WpdIdentityMapPath(options), options.alias);
    const std::string run_id = NewRunId();
    EvidenceWriter evidence(options.artifacts, run_id, sdk.SdkVersion());
    evidence.RecordCamera(options.alias, sdk_camera.firmware);
    HybridCaptureRunSummary summary;
    summary.requested = options.count;
    summary.exclusive_camera_control_confirmed = options.exclusive_camera_control_confirmed;
    summary.dedicated_spool_scope_confirmed = options.dedicated_spool_scope_confirmed;
    summary.exact_object_delete_confirmed = options.exact_object_delete_confirmed;
    for (int index = 0; index < options.count; ++index) {
        ++summary.attempted;
        const auto result = ExecuteHybridCaptureOnce(
            wpd, wpd, sdk, sdk, evidence, options.alias,
            wpd_camera.stable_identity, sdk_camera.stable_identity, {});
        summary.spool_empty_before_count += result.spool_empty_before_capture ? 1 : 0;
        summary.camera_card_delete_attempted_count += result.camera_card_delete_attempted ? 1 : 0;
        summary.camera_card_delete_succeeded_count += result.camera_card_delete_succeeded ? 1 : 0;
        summary.spool_empty_after_count += result.spool_empty_after_cleanup ? 1 : 0;
        summary.last_state = result.terminal_state;
        if (result.terminal_state == "Complete") {
            ++summary.completed;
        } else {
            ++summary.failures;
            summary.terminal_state = "FailedPartial";
            break;
        }
    }
    if (summary.completed == summary.requested && summary.failures == 0) summary.terminal_state = "Complete";
    const auto path = PersistHybridCaptureSummary(options.artifacts, run_id, options.alias, summary);
    evidence.GenerateRedactedReport(options.reports);
    std::cout << "RunId: " << run_id << "\nRequested: " << summary.requested
              << "\nAttempted: " << summary.attempted << "\nCompleted: " << summary.completed
              << "\nFailures: " << summary.failures
              << "\nSpoolEmptyBefore: " << summary.spool_empty_before_count
              << "\nCameraCardDeleteAttempted: " << summary.camera_card_delete_attempted_count
              << "\nCameraCardDeleteSucceeded: " << summary.camera_card_delete_succeeded_count
              << "\nSpoolEmptyAfter: " << summary.spool_empty_after_count
              << "\nTerminalState: " << summary.terminal_state
              << "\nSummaryPath: " << path.string() << "\n";
    return summary.terminal_state == "Complete" ? 0 : 5;
}

int RunHybridPairCapture(const Options& options) {
    if (options.transport != "sdk") throw std::runtime_error("hybrid-capture-pair does not accept --transport");
    NikonSdkTransport sdk;
    WpdTransport wpd(options.wpd_command_target);
    const auto sdk_cameras = sdk.Enumerate();
    const auto wpd_cameras = wpd.Enumerate();
    IdentityMap sdk_map(options.camera_map);
    IdentityMap wpd_identity_map(WpdIdentityMapPath(options));
    const auto identity = VerifyDualIdentityBindings(
        sdk_map, wpd_identity_map, sdk_cameras, wpd_cameras);
    const std::string run_id = NewRunId();
    const auto identity_path = PersistDualIdentityVerificationSummary(
        options.artifacts, run_id, identity);
    EvidenceWriter evidence(options.artifacts, run_id, sdk.SdkVersion() + "+" + wpd.SdkVersion());
    if (identity.terminal_state != "Ready") {
        evidence.GenerateRedactedReport(options.reports);
        std::cout << "RunId: " << run_id
                  << "\nPreflightTerminalState: " << identity.terminal_state
                  << "\nPreflightFailureCategory: " << identity.failure_category
                  << "\nIdentityMapsChanged: false"
                  << "\nCardAccessPerformed: false"
                  << "\nCaptureCommandSent: false"
                  << "\nAutomaticRetry: false"
                  << "\nActualShutterSynchronizationGuaranteed: false"
                  << "\nIdentitySummaryPath: " << identity_path.string() << "\n";
        return 5;
    }

    const auto sdk_cam_a = ResolveCamera(sdk_cameras, options.camera_map, "CAM-A");
    const auto sdk_cam_b = ResolveCamera(sdk_cameras, options.camera_map, "CAM-B");
    const auto wpd_cam_a = ResolveCamera(wpd_cameras, wpd_identity_map.Path(), "CAM-A");
    const auto wpd_cam_b = ResolveCamera(wpd_cameras, wpd_identity_map.Path(), "CAM-B");
    if (sdk_cam_a.stable_identity == sdk_cam_b.stable_identity ||
        wpd_cam_a.stable_identity == wpd_cam_b.stable_identity) {
        throw std::runtime_error("CAM-A and CAM-B must resolve to distinct physical D810 identities");
    }

    evidence.RecordCamera("CAM-A", sdk_cam_a.firmware);
    evidence.RecordCamera("CAM-B", sdk_cam_b.firmware);

    auto summary = ExecuteHybridPairRun(options.count, [&] {
        return ExecuteHybridCapturePair(
            wpd, wpd, sdk, sdk, evidence,
            wpd_cam_a.stable_identity, sdk_cam_a.stable_identity,
            wpd_cam_b.stable_identity, sdk_cam_b.stable_identity);
    });
    summary.exclusive_camera_control_confirmed = options.exclusive_camera_control_confirmed;
    summary.dedicated_spool_scope_confirmed = options.dedicated_spool_scope_confirmed;
    summary.dual_dedicated_spools_confirmed = options.dual_dedicated_spools_confirmed;
    summary.exact_object_delete_confirmed = options.exact_object_delete_confirmed;

    const auto path = PersistHybridPairSummary(options.artifacts, run_id, summary);
    const auto recovery = AssessHybridPairRecoveryEventLog(evidence.RunRoot() / "events.jsonl");
    (void)PersistHybridPairRecoverySummary(options.artifacts, run_id, recovery);
    evidence.GenerateRedactedReport(options.reports);
    std::cout << "RunId: " << run_id
              << "\nRequestedPairs: " << summary.requested_pairs
              << "\nAttemptedPairs: " << summary.attempted_pairs
              << "\nCompletedPairs: " << summary.completed_pairs
              << "\nFailures: " << summary.failures
              << "\nCamACompleted: " << summary.cam_a_completed_count
              << "\nCamBCompleted: " << summary.cam_b_completed_count
              << "\nAttemptedCameraTransactions: " << summary.attempted_camera_transactions
              << "\nCompletedCameraTransactions: " << summary.completed_camera_transactions
              << "\nDurationSampleCount: " << summary.duration_sample_count
              << "\nPairDurationP50Ms: " << summary.pair_duration_p50_ms
              << "\nPairDurationP95Ms: " << summary.pair_duration_p95_ms
              << "\nPairDurationMaxMs: " << summary.pair_duration_max_ms
              << "\nTimingUsedForPhase0PassFail: false"
              << "\nCaptureOrder: CAM-A-then-CAM-B"
              << "\nPairWatchdogSeconds: " << summary.pair_watchdog_seconds
              << "\nAutomaticRetry: false"
              << "\nActualShutterSynchronizationGuaranteed: false"
              << "\nTerminalState: " << summary.terminal_state
              << "\nSummaryPath: " << path.string() << "\n";
    return summary.terminal_state == "Complete" ? 0 : 5;
}

int RunHybridFaultPair(const Options& options) {
    if (options.transport != "sdk") throw std::runtime_error("hybrid-fault-pair does not accept --transport");
    NikonSdkTransport sdk;
    WpdTransport wpd(options.wpd_command_target);
    const auto sdk_cameras = sdk.Enumerate();
    const auto wpd_cameras = wpd.Enumerate();
    IdentityMap sdk_map(options.camera_map);
    IdentityMap wpd_identity_map(WpdIdentityMapPath(options));
    const auto identity = VerifyDualIdentityBindings(
        sdk_map, wpd_identity_map, sdk_cameras, wpd_cameras);
    const std::string run_id = NewRunId();
    const auto identity_path = PersistDualIdentityVerificationSummary(
        options.artifacts, run_id, identity);
    EvidenceWriter evidence(options.artifacts, run_id, sdk.SdkVersion() + "+" + wpd.SdkVersion());
    if (identity.terminal_state != "Ready") {
        evidence.GenerateRedactedReport(options.reports);
        std::cout << "RunId: " << run_id
                  << "\nPreflightTerminalState: " << identity.terminal_state
                  << "\nPreflightFailureCategory: " << identity.failure_category
                  << "\nCardAccessPerformed: false"
                  << "\nCaptureCommandSent: false"
                  << "\nAutomaticRetry: false"
                  << "\nIdentitySummaryPath: " << identity_path.string() << "\n";
        return 5;
    }

    const auto sdk_cam_a = ResolveCamera(sdk_cameras, options.camera_map, "CAM-A");
    const auto sdk_cam_b = ResolveCamera(sdk_cameras, options.camera_map, "CAM-B");
    const auto wpd_cam_a = ResolveCamera(wpd_cameras, wpd_identity_map.Path(), "CAM-A");
    const auto wpd_cam_b = ResolveCamera(wpd_cameras, wpd_identity_map.Path(), "CAM-B");
    evidence.RecordCamera("CAM-A", sdk_cam_a.firmware);
    evidence.RecordCamera("CAM-B", sdk_cam_b.firmware);

    constexpr std::string_view gate_stage = "after_sdk_close_before_wpd_recovery";
    OperatorGate gate(
        evidence.RunRoot(), *options.operator_gate,
        std::chrono::seconds(options.operator_gate_timeout_seconds),
        options.scenario, std::string(gate_stage));
    std::cout << "FaultCameraAlias: " << options.alias
              << "\nFaultScenario: " << options.scenario
              << "\nFaultAction: "
              << (options.scenario == "usb-disconnect"
                      ? "disconnect the selected D810 USB cable"
                      : "turn the selected D810 power off")
              << " after the operator gate becomes ready; then create the continue marker.\n";
    const auto invoke_gate = [&] { static_cast<void>(gate.AwaitContinue(std::cout)); };
    const std::function<void()> cam_a_gate = options.alias == "CAM-A" ? invoke_gate : std::function<void()>{};
    const std::function<void()> cam_b_gate = options.alias == "CAM-B" ? invoke_gate : std::function<void()>{};
    const auto pair = ExecuteHybridCapturePair(
        wpd, wpd, sdk, sdk, evidence,
        wpd_cam_a.stable_identity, sdk_cam_a.stable_identity,
        wpd_cam_b.stable_identity, sdk_cam_b.stable_identity,
        {}, {}, cam_a_gate, cam_b_gate);

    const auto original_persisted = [](const TransactionResult& transaction) {
        return !transaction.frames.empty() && transaction.frames.front().success;
    };
    HybridPairFaultRunSummary summary;
    summary.scenario = options.scenario;
    summary.fault_camera_alias = options.alias;
    summary.gate_stage = std::string(gate_stage);
    summary.pair_state = pair.terminal_state;
    summary.error_category = pair.error_category;
    summary.cam_b_started = pair.cam_b_started;
    summary.cam_a_original_persisted = original_persisted(pair.cam_a);
    summary.cam_b_original_persisted = original_persisted(pair.cam_b);
    summary.cam_a_delete_attempted = pair.cam_a.camera_card_delete_attempted;
    summary.cam_b_delete_attempted = pair.cam_b.camera_card_delete_attempted;
    const bool common_failure = pair.terminal_state == "FailedPartial" &&
        pair.error_category == "open_failed" && !summary.cam_b_original_persisted &&
        !summary.cam_b_delete_attempted;
    const bool cam_a_expected = options.alias == "CAM-A" && common_failure &&
        pair.cam_a.spool_empty_before_capture && !summary.cam_a_original_persisted &&
        !summary.cam_a_delete_attempted && !pair.cam_b_started;
    const bool cam_b_expected = options.alias == "CAM-B" && common_failure &&
        pair.cam_a.terminal_state == "Complete" && summary.cam_a_original_persisted &&
        pair.cam_a.camera_card_delete_succeeded && pair.cam_b_started &&
        pair.cam_b.spool_empty_before_capture;
    const bool expected_failure = cam_a_expected || cam_b_expected;
    summary.acceptance_state = expected_failure ? "Pass" : "Fail";
    const auto path = PersistHybridPairFaultSummary(options.artifacts, run_id, summary);
    const auto recovery = AssessHybridPairRecoveryEventLog(evidence.RunRoot() / "events.jsonl");
    (void)PersistHybridPairRecoverySummary(options.artifacts, run_id, recovery);
    evidence.GenerateRedactedReport(options.reports);
    std::cout << "RunId: " << run_id
              << "\nPairState: " << pair.terminal_state
              << "\nErrorCategory: " << pair.error_category
              << "\nCamBStarted: " << (pair.cam_b_started ? "true" : "false")
              << "\nCamAOriginalPersisted: " << (summary.cam_a_original_persisted ? "true" : "false")
              << "\nCamBOriginalPersisted: " << (summary.cam_b_original_persisted ? "true" : "false")
              << "\nCamADeleteAttempted: " << (summary.cam_a_delete_attempted ? "true" : "false")
              << "\nCamBDeleteAttempted: " << (summary.cam_b_delete_attempted ? "true" : "false")
              << "\nAutomaticRetry: false"
              << "\nRecoveryRequiresNewTransaction: true"
              << "\nActualShutterSynchronizationGuaranteed: false"
              << "\nAcceptanceState: " << summary.acceptance_state
              << "\nSummaryPath: " << path.string() << '\n';
    return expected_failure ? 0 : 5;
}

int RunHybridInterruptPair(const Options& options) {
    if (options.transport != "sdk") throw std::runtime_error("hybrid-interrupt-pair requires SDK transport");
    NikonSdkTransport sdk;
    WpdTransport wpd(options.wpd_command_target);
    const auto sdk_cameras = sdk.Enumerate();
    const auto wpd_cameras = wpd.Enumerate();
    IdentityMap sdk_map(options.camera_map);
    IdentityMap wpd_identity_map(WpdIdentityMapPath(options));
    const auto identity = VerifyDualIdentityBindings(
        sdk_map, wpd_identity_map, sdk_cameras, wpd_cameras);
    const std::string run_id = NewRunId();
    const auto identity_path = PersistDualIdentityVerificationSummary(
        options.artifacts, run_id, identity);
    EvidenceWriter evidence(options.artifacts, run_id, sdk.SdkVersion() + "+" + wpd.SdkVersion());
    if (identity.terminal_state != "Ready") {
        evidence.GenerateRedactedReport(options.reports);
        std::cout << "RunId: " << run_id
                  << "\nPreflightTerminalState: " << identity.terminal_state
                  << "\nPreflightFailureCategory: " << identity.failure_category
                  << "\nCardAccessPerformed: false"
                  << "\nCaptureCommandSent: false"
                  << "\nCamBStarted: false"
                  << "\nAutomaticRetry: false"
                  << "\nIdentitySummaryPath: " << identity_path.string() << "\n";
        return 5;
    }

    const auto sdk_cam_a = ResolveCamera(sdk_cameras, options.camera_map, "CAM-A");
    const auto sdk_cam_b = ResolveCamera(sdk_cameras, options.camera_map, "CAM-B");
    const auto wpd_cam_a = ResolveCamera(wpd_cameras, wpd_identity_map.Path(), "CAM-A");
    const auto wpd_cam_b = ResolveCamera(wpd_cameras, wpd_identity_map.Path(), "CAM-B");
    evidence.RecordCamera("CAM-A", sdk_cam_a.firmware);
    evidence.RecordCamera("CAM-B", sdk_cam_b.firmware);

    constexpr std::string_view gate_stage = "after-CAM-A-before-CAM-B";
    OperatorGate gate(
        evidence.RunRoot(), *options.operator_gate,
        std::chrono::seconds(options.operator_gate_timeout_seconds),
        "app-exit", std::string(gate_stage));
    std::cout << "RunId: " << run_id
              << "\nInterruptionStage: " << gate_stage
              << "\nRequiredAction: after the gate becomes ready, terminate this Phase 0 process; do not create a continue marker."
              << "\nRecoveryCommand: A0CameraStitcher.Phase0 report --run-id " << run_id
              << "\nCamBMayResumeFromGate: false\n" << std::flush;

    const auto pair = ExecuteHybridCapturePair(
        wpd, wpd, sdk, sdk, evidence,
        wpd_cam_a.stable_identity, sdk_cam_a.stable_identity,
        wpd_cam_b.stable_identity, sdk_cam_b.stable_identity,
        {}, [&] { gate.AwaitProcessTermination(std::cout); });

    // Reaching here means the operator did not terminate the process. The
    // boundary callback failed closed, so CAM-B was never opened.
    const auto recovery = AssessHybridPairRecoveryEventLog(evidence.RunRoot() / "events.jsonl");
    const auto recovery_path = PersistHybridPairRecoverySummary(options.artifacts, run_id, recovery);
    evidence.GenerateRedactedReport(options.reports);
    std::cout << "RunId: " << run_id
              << "\nPairState: " << pair.terminal_state
              << "\nErrorCategory: " << pair.error_category
              << "\nCamAOriginalPersisted: "
              << (!pair.cam_a.frames.empty() && pair.cam_a.frames.front().success ? "true" : "false")
              << "\nCamBStarted: " << (pair.cam_b_started ? "true" : "false")
              << "\nAutomaticRetry: false"
              << "\nAcceptanceState: FailOperatorDidNotTerminate"
              << "\nRecoverySummaryPath: " << recovery_path.string() << '\n';
    return 5;
}

int RunHybridFaultSingle(const Options& options) {
    if (options.transport != "sdk") throw std::runtime_error("hybrid-fault-single does not accept --transport");
    NikonSdkTransport sdk;
    WpdTransport wpd(options.wpd_command_target);
    const auto sdk_cameras = sdk.Enumerate();
    const auto wpd_cameras = wpd.Enumerate();
    if (sdk_cameras.size() != 1 || wpd_cameras.size() != 1) {
        throw std::runtime_error("hybrid fault test requires exactly one physical D810 in both SDK and WPD inventories");
    }
    const auto sdk_camera = ResolveCamera(sdk_cameras, options.camera_map, options.alias);
    const auto wpd_camera = ResolveCamera(wpd_cameras, WpdIdentityMapPath(options), options.alias);
    const std::string run_id = NewRunId();
    EvidenceWriter evidence(options.artifacts, run_id, sdk.SdkVersion() + "+" + wpd.SdkVersion());
    evidence.RecordCamera(options.alias, sdk_camera.firmware);
    constexpr std::string_view gate_stage = "after_sdk_close_before_wpd_recovery";
    OperatorGate gate(
        evidence.RunRoot(), *options.operator_gate,
        std::chrono::seconds(options.operator_gate_timeout_seconds),
        options.scenario, std::string(gate_stage));
    std::cout << "FaultScenario: " << options.scenario
              << "\nFaultAction: "
              << (options.scenario == "usb-disconnect"
                      ? "disconnect the D810 USB cable"
                      : "turn the D810 power off")
              << " after the operator gate becomes ready; then create the continue marker.\n";
    const auto result = ExecuteHybridCaptureOnce(
        wpd, wpd, sdk, sdk, evidence, options.alias,
        wpd_camera.stable_identity, sdk_camera.stable_identity, {},
        [&] { static_cast<void>(gate.AwaitContinue(std::cout)); });

    HybridCaptureRunSummary capture_summary;
    capture_summary.requested = 1;
    capture_summary.attempted = 1;
    capture_summary.completed = result.terminal_state == "Complete" ? 1 : 0;
    capture_summary.failures = result.terminal_state == "Complete" ? 0 : 1;
    capture_summary.terminal_state = result.terminal_state;
    capture_summary.last_state = result.terminal_state;
    capture_summary.spool_empty_before_count = result.spool_empty_before_capture ? 1 : 0;
    capture_summary.camera_card_delete_attempted_count = result.camera_card_delete_attempted ? 1 : 0;
    capture_summary.camera_card_delete_succeeded_count = result.camera_card_delete_succeeded ? 1 : 0;
    capture_summary.spool_empty_after_count = result.spool_empty_after_cleanup ? 1 : 0;
    capture_summary.exclusive_camera_control_confirmed = options.exclusive_camera_control_confirmed;
    capture_summary.dedicated_spool_scope_confirmed = options.dedicated_spool_scope_confirmed;
    capture_summary.exact_object_delete_confirmed = options.exact_object_delete_confirmed;
    (void)PersistHybridCaptureSummary(options.artifacts, run_id, options.alias, capture_summary);

    HybridFaultRunSummary fault_summary;
    fault_summary.scenario = options.scenario;
    fault_summary.gate_stage = std::string(gate_stage);
    fault_summary.transaction_state = result.terminal_state;
    fault_summary.error_category = result.error_category;
    fault_summary.spool_empty_before_capture = result.spool_empty_before_capture;
    fault_summary.pc_original_persisted = !result.frames.empty() && result.frames.front().success;
    fault_summary.camera_object_delete_attempted = result.camera_card_delete_attempted;
    const bool expected_failure = result.terminal_state == "FailedPartial" &&
        result.error_category == "open_failed" && result.spool_empty_before_capture &&
        !fault_summary.pc_original_persisted && !result.camera_card_delete_attempted;
    fault_summary.acceptance_state = expected_failure ? "Pass" : "Fail";
    const auto path = PersistHybridFaultSummary(options.artifacts, run_id, options.alias, fault_summary);
    evidence.GenerateRedactedReport(options.reports);
    std::cout << "RunId: " << run_id
              << "\nTransactionState: " << result.terminal_state
              << "\nErrorCategory: " << result.error_category
              << "\nPcOriginalPersisted: " << (fault_summary.pc_original_persisted ? "true" : "false")
              << "\nCameraObjectDeleteAttempted: " << (result.camera_card_delete_attempted ? "true" : "false")
              << "\nAutomaticRetry: false"
              << "\nRecoveryRequiresNewTransaction: true"
              << "\nAcceptanceState: " << fault_summary.acceptance_state
              << "\nSummaryPath: " << path.string() << '\n';
    return expected_failure ? 0 : 5;
}

int RunCapture(const Options& options, FakeFailureMode failure = FakeFailureMode::none) {
    std::unique_ptr<ICameraTransport> transport;
    if (options.transport == "fake") transport = std::make_unique<FakeCameraTransport>(failure);
    else if (options.transport == "wpd") {
        WpdTransport::BeforeCommandCallback before_command;
        if (options.operator_gate) {
            auto gate = std::make_shared<OperatorGate>(
                options.artifacts, *options.operator_gate, std::chrono::seconds(options.operator_gate_timeout_seconds));
            before_command = [gate] { static_cast<void>(gate->AwaitContinue(std::cout)); };
        }
        transport = std::make_unique<WpdTransport>(options.wpd_command_target, std::move(before_command));
    }
    else transport = std::make_unique<NikonSdkTransport>();
    const auto cameras = transport->Enumerate();
    const std::size_t required = options.command == "capture-single" || options.command == "fault-test" ? 1U : 2U;
    if (cameras.size() < required) throw std::runtime_error("required D810 count is not available");

    std::map<std::string, CameraInfo> cameras_by_alias;
    if (options.transport == "fake") {
        cameras_by_alias.emplace("CAM-A", cameras.at(0));
        if (cameras.size() > 1) cameras_by_alias.emplace("CAM-B", cameras.at(1));
    } else {
        IdentityMap identity_map(IdentityMapPath(options));
        for (const auto& camera : cameras) {
            const auto existing_alias = identity_map.FindAlias(camera.stable_identity);
            if (!existing_alias) {
                throw std::runtime_error(
                    "enumerated D810 is not explicitly bound; connect one body at a time and run bind-cross-transport-identity");
            }
            if (!cameras_by_alias.emplace(*existing_alias, camera).second) {
                throw std::runtime_error("multiple cameras resolved to the same alias");
            }
        }
    }
    if (required == 1 && !cameras_by_alias.contains(options.alias)) {
        throw std::runtime_error("requested camera alias is not available");
    }
    if (required == 2 && (!cameras_by_alias.contains("CAM-A") || !cameras_by_alias.contains("CAM-B"))) {
        throw std::runtime_error("both CAM-A and CAM-B aliases must be available");
    }

    const std::string run_id = NewRunId();
    EvidenceWriter evidence(options.artifacts, run_id, transport->SdkVersion());
    if (required == 1) {
        evidence.RecordCamera(options.alias, cameras_by_alias.at(options.alias).firmware);
    } else {
        evidence.RecordCamera("CAM-A", cameras_by_alias.at("CAM-A").firmware);
        evidence.RecordCamera("CAM-B", cameras_by_alias.at("CAM-B").firmware);
    }
    CaptureCoordinator coordinator(*transport, evidence);
    int failures = 0;
    int attempted_transactions = 0;
    for (int index = 0; index < options.count; ++index) {
        ++attempted_transactions;
        const auto result = required == 1
            ? coordinator.CaptureSingle(options.alias, cameras_by_alias.at(options.alias).stable_identity)
            : coordinator.CapturePair(
                cameras_by_alias.at("CAM-A").stable_identity,
                cameras_by_alias.at("CAM-B").stable_identity);
        if (result.terminal_state != "Complete") {
            ++failures;
            break;
        }
    }
    std::cout << "RunId: " << run_id
              << "\nRequestedTransactions: " << options.count
              << "\nAttemptedTransactions: " << attempted_transactions
              << "\nFailures: " << failures << '\n';
    return failures == 0 ? 0 : 4;
}

int GenerateReport(const Options& options) {
    if (options.run_id.empty()) throw std::runtime_error("report requires --run-id");
    EvidenceWriter evidence(options.artifacts, options.run_id, "recorded-in-summary");
    const auto event_log = evidence.RunRoot() / "events.jsonl";
    const auto recovery_summary = evidence.RunRoot() / "hybrid-pair-recovery-summary.json";
    if (fs::exists(event_log) && !fs::exists(recovery_summary)) {
        const auto recovery = AssessHybridPairRecoveryEventLog(event_log);
        if (recovery.pair_started_count > 0) {
            (void)PersistHybridPairRecoverySummary(options.artifacts, options.run_id, recovery);
        }
    }
    evidence.GenerateRedactedReport(options.reports);
    std::cout << "Redacted report created for " << options.run_id << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = Parse(argc, argv);
        std::optional<HardwareProcessLease> hardware_lease;
        if (RequiresHardwareProcessLease(options.command, options.transport)) {
            hardware_lease.emplace();
            if (hardware_lease->RecoveredAbandonedOwner()) {
                std::cerr << "Phase0 warning: recovered an abandoned camera-control lease; "
                             "the selected command must still perform its normal fail-closed checks\n";
            }
        }
        if (options.command == "preflight") return Preflight(options);
        if (options.command == "inventory") return Inventory(options);
        if (options.command == "bind-identity") return BindIdentity(options);
        if (options.command == "bind-cross-transport-identity") return BindCrossTransportIdentityCommand(options);
        if (options.command == "bind-single-identity-v3") return BindSingleIdentityV3Command(options);
        if (options.command == "verify-dual-identity") return VerifyDualIdentityCommand(options);
        if (options.command == "verify-dual-spools") return VerifyDualSpoolsCommand(options);
        if (options.command == "sdk-status") return RunSdkStatus(options);
        if (options.command == "wpd-status") return RunWpdStatus(options);
        if (options.command == "spool-status") return RunSpoolStatus(options);
        if (options.command == "wpd-correlation-status") return RunWpdCorrelationStatus(options);
        if (options.command == "live-view") return RunLiveView(options);
        if (options.command == "live-view-handoff") return RunLiveViewHandoff(options);
        if (options.command == "hybrid-capture-single") return RunHybridCapture(options);
        if (options.command == "hybrid-capture-pair") return RunHybridPairCapture(options);
        if (options.command == "hybrid-fault-single") return RunHybridFaultSingle(options);
        if (options.command == "hybrid-fault-pair") return RunHybridFaultPair(options);
        if (options.command == "hybrid-interrupt-pair") return RunHybridInterruptPair(options);
        if (options.command == "capture-single" || options.command == "capture-pair" || options.command == "stability") return RunCapture(options);
        if (options.command == "fault-test") {
            if (options.transport != "fake") throw std::runtime_error("fault-test requires --transport fake");
            return RunCapture(options, ParseFailure(options.scenario));
        }
        if (options.command == "report") return GenerateReport(options);
        Usage();
        return 1;
    } catch (const TransportError& error) {
        std::cerr << "Phase0 transport error [" << error.Category() << "]: " << error.what() << '\n';
        Usage();
        return 3;
    } catch (const std::exception& error) {
        std::cerr << "Phase0 error: " << error.what() << '\n';
        Usage();
        return 3;
    }
}
