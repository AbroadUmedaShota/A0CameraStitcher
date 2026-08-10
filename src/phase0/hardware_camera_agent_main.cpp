#include "a0/phase0/hardware_camera_agent.hpp"

#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace a0::phase0;

namespace {

fs::path DerivedWpdMap(fs::path sdk_map) {
    sdk_map.replace_filename(
        sdk_map.stem().wstring() + L"-wpd" + sdk_map.extension().wstring());
    return sdk_map;
}

std::string NarrowAscii(std::wstring_view value) {
    std::string output;
    output.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < 0x20 || character > 0x7E) {
            throw std::invalid_argument("pipe name must contain ASCII characters only");
        }
        output.push_back(static_cast<char>(character));
    }
    return output;
}

void PrintUsage() {
    std::wcerr
        << L"Usage: A0CameraStitcher.CameraAgent [--serve-once] [--pipe-name NAME] "
           L"[--camera-map SDK_MAP] [--wpd-camera-map WPD_MAP] "
           L"[--single-identity-v3 PATH] "
           L"[--artifacts-root PATH] [--reports-root PATH] "
           L"[--transaction-state-root PATH] [--approved-capture-profile PATH]\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        ProductionHardwareCameraAgentConfig config =
            ProductionHardwareCameraAgentConfig::Defaults();
        std::string pipe_name(kDefaultHardwareCameraAgentPipeName);
        bool serve_once = false;
        bool sdk_map_overridden = false;
        bool wpd_map_overridden = false;
        std::set<std::wstring> seen;

        for (int index = 1; index < argc; ++index) {
            const std::wstring argument = argv[index];
            if (argument == L"--serve-once") {
                if (serve_once) throw std::invalid_argument("--serve-once was repeated");
                serve_once = true;
                continue;
            }
            if (argument == L"--help" || argument == L"-h") {
                PrintUsage();
                return 0;
            }
            const bool known =
                argument == L"--pipe-name" || argument == L"--camera-map" ||
                argument == L"--wpd-camera-map" || argument == L"--single-identity-v3" ||
                argument == L"--artifacts-root" ||
                argument == L"--reports-root" || argument == L"--transaction-state-root" ||
                argument == L"--approved-capture-profile";
            if (!known) throw std::invalid_argument("unknown hardware Camera Agent argument");
            if (!seen.insert(argument).second) throw std::invalid_argument("hardware Camera Agent argument was repeated");
            if (++index >= argc || std::wstring_view(argv[index]).empty()) {
                throw std::invalid_argument("hardware Camera Agent argument value is missing");
            }
            const fs::path value = argv[index];
            if (argument == L"--pipe-name") pipe_name = NarrowAscii(argv[index]);
            else if (argument == L"--camera-map") {
                config.sdk_identity_map = value;
                sdk_map_overridden = true;
            } else if (argument == L"--wpd-camera-map") {
                config.wpd_identity_map = value;
                wpd_map_overridden = true;
            } else if (argument == L"--single-identity-v3") {
                config.single_identity_v3 = value;
            } else if (argument == L"--artifacts-root") config.artifacts_root = value;
            else if (argument == L"--reports-root") config.reports_root = value;
            else if (argument == L"--transaction-state-root") config.transaction_state_root = value;
            else config.approved_capture_profile = value;
        }
        if (sdk_map_overridden && !wpd_map_overridden) {
            config.wpd_identity_map = DerivedWpdMap(config.sdk_identity_map);
        }

        ProductionHardwareCameraAgentBackend backend(std::move(config));
        HardwareCameraAgentDispatcher dispatcher(backend);
        return RunHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, serve_once);
    } catch (const std::exception& error) {
        std::cerr << "hardware Camera Agent failed closed: " << error.what() << '\n';
        PrintUsage();
        return 1;
    }
}
