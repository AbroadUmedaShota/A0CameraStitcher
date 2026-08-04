#include "a0/phase0/fake_camera_transport.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include "a0/phase0/phase0.hpp"
#include "a0/phase0/wpd_transport.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace a0::phase0;

namespace {

struct Options {
    std::string command;
    std::string stage{"single"};
    std::string alias{"CAM-A"};
    std::string scenario;
    std::string run_id;
    int count{1};
    std::string transport{"sdk"};
    fs::path artifacts{"artifacts/phase0"};
    fs::path reports{"docs/evidence/phase0"};
    fs::path camera_map{DefaultIdentityMapPath()};
};

std::optional<std::string> EnvironmentValue(const char* name) {
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return std::nullopt;
    std::string value(buffer);
    std::free(buffer);
    return value;
}

void Usage() {
    std::cout
        << "A0CameraStitcher.Phase0 commands:\n"
        << "  preflight --stage single|dual\n"
        << "  inventory [--transport sdk|wpd|fake]\n"
        << "  capture-single --alias CAM-A --count 10 [--transport sdk|wpd|fake]\n"
        << "  capture-pair --count 10 [--transport sdk|wpd|fake]\n"
        << "  stability --count 100 [--transport sdk|wpd|fake]\n"
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
        else if (arg == "--alias") options.alias = require_value();
        else if (arg == "--count") options.count = std::stoi(require_value());
        else if (arg == "--scenario") options.scenario = require_value();
        else if (arg == "--run-id") options.run_id = require_value();
        else if (arg == "--transport") options.transport = require_value();
        else if (arg == "--artifacts") options.artifacts = require_value();
        else if (arg == "--reports") options.reports = require_value();
        else if (arg == "--camera-map") options.camera_map = require_value();
        else throw std::runtime_error("unknown option: " + arg);
    }
    if (options.count < 1) throw std::runtime_error("count must be positive");
    if (options.alias != "CAM-A" && options.alias != "CAM-B") throw std::runtime_error("alias must be CAM-A or CAM-B");
    if (options.transport != "sdk" && options.transport != "wpd" && options.transport != "fake") {
        throw std::runtime_error("transport must be sdk, wpd, or fake");
    }
    return options;
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
    if (options.transport == "wpd") return std::make_unique<WpdTransport>();
    return std::make_unique<NikonSdkTransport>();
}

fs::path IdentityMapPath(const Options& options) {
    if (options.transport != "wpd" || options.camera_map != DefaultIdentityMapPath()) return options.camera_map;
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
    for (const auto& camera : cameras) {
        const auto alias = map.AssignNext(camera.stable_identity);
        std::cout << alias << " model=" << camera.model << " firmware=" << camera.firmware
                  << " shootingMode=" << camera.shooting_mode << '\n';
    }
    std::cout << "CameraCount: " << cameras.size() << "\nReal identifiers were not printed.\n";
    return cameras.empty() ? 3 : 0;
}

int RunCapture(const Options& options, FakeFailureMode failure = FakeFailureMode::none) {
    std::unique_ptr<ICameraTransport> transport;
    if (options.transport == "fake") transport = std::make_unique<FakeCameraTransport>(failure);
    else if (options.transport == "wpd") transport = std::make_unique<WpdTransport>();
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
            const std::string alias = existing_alias
                ? *existing_alias
                : identity_map.AssignNext(camera.stable_identity);
            if (!cameras_by_alias.emplace(alias, camera).second) {
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
    for (int index = 0; index < options.count; ++index) {
        const auto result = required == 1
            ? coordinator.CaptureSingle(options.alias, cameras_by_alias.at(options.alias).stable_identity)
            : coordinator.CapturePair(
                cameras_by_alias.at("CAM-A").stable_identity,
                cameras_by_alias.at("CAM-B").stable_identity);
        if (result.terminal_state != "Complete") ++failures;
    }
    std::cout << "RunId: " << run_id << "\nTransactions: " << options.count << "\nFailures: " << failures << '\n';
    return failures == 0 ? 0 : 4;
}

int GenerateReport(const Options& options) {
    if (options.run_id.empty()) throw std::runtime_error("report requires --run-id");
    EvidenceWriter evidence(options.artifacts, options.run_id, "recorded-in-summary");
    evidence.GenerateRedactedReport(options.reports);
    std::cout << "Redacted report created for " << options.run_id << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = Parse(argc, argv);
        if (options.command == "preflight") return Preflight(options);
        if (options.command == "inventory") return Inventory(options);
        if (options.command == "capture-single" || options.command == "capture-pair" || options.command == "stability") return RunCapture(options);
        if (options.command == "fault-test") {
            if (options.transport != "fake") throw std::runtime_error("fault-test requires --transport fake");
            return RunCapture(options, ParseFailure(options.scenario));
        }
        if (options.command == "report") return GenerateReport(options);
        Usage();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Phase0 error: " << error.what() << '\n';
        Usage();
        return 3;
    }
}
