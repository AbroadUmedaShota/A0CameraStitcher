#include "a0/phase0/dual_hardware_camera_agent.hpp"
#include "a0/phase0/dual_hardware_capture_backend.hpp"
#include "a0/phase0/dual_hardware_camera_agent_store.hpp"
#include "a0/phase0/dual_binding_camera_agent.hpp"
#include "a0/phase0/hardware_process_lease.hpp"
#include "a0/phase0/nikon_sdk_transport.hpp"
#include "a0/phase0/wpd_transport.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace a0::phase0;

namespace {

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

// Mirrors IsSafePipeName in hardware_camera_agent_pipe.cpp exactly (that
// internal, non-exported check remains the loop's own authoritative gate).
// This local copy exists only so a malformed --pipe-name is rejected here,
// before this process touches the filesystem for --pair-journal-root /
// --approved-capture-profile / --dual-identity-proof, instead of failing
// later inside RunDualHardwareCameraAgentNamedPipeServer.
bool IsSafePipeName(std::string_view value) noexcept {
    if (value.empty() || value.size() > 120) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 ||
            character == '.' || character == '-' || character == '_';
    });
}

void PrintUsage() {
    std::wcerr
        << L"Usage: A0CameraStitcher.DualCameraAgent [--serve-once] "
           L"[--pipe-name NAME] --pair-journal-root PATH "
           L"--approved-capture-profile PATH --dual-identity-proof PATH "
           L"[--binding-pipe-name NAME --wpd-camera-map PATH]\n"
        << L"       A0CameraStitcher.DualCameraAgent "
           L"--read-only-coexistence-probe --wpd-camera-map PATH\n"
        << L"       A0CameraStitcher.DualCameraAgent "
           L"--read-only-sdk-probe\n"
        << L"       A0CameraStitcher.DualCameraAgent "
           L"--read-only-wpd-probe --wpd-camera-map PATH\n";
}

// Legacy launch without the session-binding pair of arguments remains fail
// closed at PairDispatcherUnavailable. Supplying both --binding-pipe-name and
// --wpd-camera-map enables the production sequence: complete an operator
// binding while retaining the SDK Module, then serve the stable v2 pair pipe
// with the bound SDK/WPD backend. The private candidate tokens remain inside
// this process and never enter a request, journal, or evidence file.
//
// --approved-capture-profile and --dual-identity-proof are part of the host
// launch contract agreed with Issue #8 and are therefore accepted and
// fail-closed validated here. Neither file's content is read by this Issue:
// every start-reserved-pair request already carries its own frozen
// captureProfileSnapshot/identitySnapshot, which the existing dispatcher
// validates per request (AR-08a-2B semantic preflight). Reading these two
// files to cross-check the client-carried snapshot is future, real-backend
// work, not part of reusing the existing core.
//
// The validation itself reuses ValidateDualHardwareFixedLocalPath (the same
// fixed-local-path shape check DualHardwarePairJournalStore's constructor
// applies to its root: absolute, drive-qualified, no UNC/NT-namespace
// prefix, no traversal components, a DRIVE_FIXED volume, and no reparse
// point anywhere along the existing chain including the leaf) before
// additionally requiring the path to already exist as a plain regular file.
// A plain fs::is_regular_file check alone is not enough here, because it
// follows symlinks/junctions rather than rejecting them.
void RequireExistingFixedLocalFile(
    std::string_view argument_name,
    const fs::path& path) {
    fs::path normalized;
    try {
        normalized = ValidateDualHardwareFixedLocalPath(path, argument_name);
    } catch (const std::exception& error) {
        throw std::invalid_argument(error.what());
    }
    std::error_code status_error;
    const bool is_regular_file = fs::is_regular_file(normalized, status_error);
    if (status_error || !is_regular_file) {
        throw std::invalid_argument(
            std::string(argument_name) + " must name an existing local file");
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::string pipe_name(kDefaultDualHardwareCameraAgentPipeName);
        bool serve_once = false;
        bool read_only_coexistence_probe = false;
        bool read_only_sdk_probe = false;
        bool read_only_wpd_probe = false;
        std::optional<fs::path> pair_journal_root;
        std::optional<fs::path> approved_capture_profile;
        std::optional<fs::path> dual_identity_proof;
        std::optional<std::string> binding_pipe_name;
        std::optional<fs::path> wpd_camera_map;
        std::set<std::wstring> seen;

        for (int index = 1; index < argc; ++index) {
            const std::wstring argument = argv[index];
            if (argument == L"--serve-once") {
                if (serve_once) throw std::invalid_argument("--serve-once was repeated");
                serve_once = true;
                continue;
            }
            if (argument == L"--read-only-coexistence-probe") {
                if (read_only_coexistence_probe) {
                    throw std::invalid_argument(
                        "--read-only-coexistence-probe was repeated");
                }
                read_only_coexistence_probe = true;
                continue;
            }
            if (argument == L"--read-only-sdk-probe") {
                if (read_only_sdk_probe) {
                    throw std::invalid_argument(
                        "--read-only-sdk-probe was repeated");
                }
                read_only_sdk_probe = true;
                continue;
            }
            if (argument == L"--read-only-wpd-probe") {
                if (read_only_wpd_probe) {
                    throw std::invalid_argument(
                        "--read-only-wpd-probe was repeated");
                }
                read_only_wpd_probe = true;
                continue;
            }
            if (argument == L"--help" || argument == L"-h") {
                PrintUsage();
                return 0;
            }
            const bool known =
                argument == L"--pipe-name" || argument == L"--pair-journal-root" ||
                argument == L"--approved-capture-profile" ||
                argument == L"--dual-identity-proof" ||
                argument == L"--binding-pipe-name" ||
                argument == L"--wpd-camera-map";
            if (!known) {
                throw std::invalid_argument("unknown Dual Camera Agent argument");
            }
            if (!seen.insert(argument).second) {
                throw std::invalid_argument("Dual Camera Agent argument was repeated");
            }
            if (++index >= argc || std::wstring_view(argv[index]).empty()) {
                throw std::invalid_argument("Dual Camera Agent argument value is missing");
            }
            if (argument == L"--pipe-name") {
                pipe_name = NarrowAscii(argv[index]);
            } else if (argument == L"--binding-pipe-name") {
                binding_pipe_name = NarrowAscii(argv[index]);
            } else if (argument == L"--pair-journal-root") {
                pair_journal_root = fs::path(argv[index]);
            } else if (argument == L"--approved-capture-profile") {
                approved_capture_profile = fs::path(argv[index]);
            } else if (argument == L"--wpd-camera-map") {
                wpd_camera_map = fs::path(argv[index]);
            } else {
                dual_identity_proof = fs::path(argv[index]);
            }
        }

        // Validate --pipe-name before this process touches the filesystem at
        // all (--pair-journal-root / --approved-capture-profile /
        // --dual-identity-proof are all checked below). This is a fail-fast
        // duplicate of the loop's own authoritative IsSafePipeName check;
        // see the comment on the local IsSafePipeName above.
        if (!IsSafePipeName(pipe_name)) {
            throw std::invalid_argument(
                "--pipe-name must be 1-120 ASCII letters, digits, '.', '-', or '_'");
        }
        if (binding_pipe_name && !IsSafePipeName(*binding_pipe_name)) {
            throw std::invalid_argument(
                "--binding-pipe-name must be 1-120 ASCII letters, digits, '.', '-', or '_'");
        }
        if (!read_only_coexistence_probe && !read_only_wpd_probe &&
            binding_pipe_name.has_value() != wpd_camera_map.has_value()) {
            throw std::invalid_argument(
                "--binding-pipe-name and --wpd-camera-map must be supplied together");
        }
        if (binding_pipe_name && serve_once) {
            throw std::invalid_argument(
                "--serve-once cannot complete a multi-request binding session");
        }
        if (read_only_sdk_probe) {
            if (serve_once || read_only_coexistence_probe ||
                read_only_wpd_probe || !seen.empty()) {
                throw std::invalid_argument(
                    "--read-only-sdk-probe cannot be combined with other options");
            }
            DualSdkReadOnlyProbeResult result;
            try {
                HardwareProcessLease camera_control_lease;
                NikonSdkTransport transport;
                result = RunDualSdkReadOnlyProbe(
                    transport, std::chrono::seconds(10));
            } catch (...) {
                // The fixed default result is the only public boundary for a
                // host/lease setup failure. Never publish exception text from
                // the licensed SDK or process environment.
            }
            std::cout << SerializeDualSdkReadOnlyProbeResult(result) << '\n';
            return result.terminal_state == DualSdkReadOnlyProbeTerminalState::Pass
                ? 0 : 2;
        }
        if (read_only_wpd_probe) {
            if (serve_once || read_only_coexistence_probe || read_only_sdk_probe ||
                !wpd_camera_map || seen.size() != 1 ||
                seen.count(L"--wpd-camera-map") != 1) {
                throw std::invalid_argument(
                    "--read-only-wpd-probe requires only --wpd-camera-map");
            }
            DualWpdReadOnlyProbeResult result;
            try {
                RequireExistingFixedLocalFile(
                    "--wpd-camera-map", *wpd_camera_map);
                HardwareProcessLease camera_control_lease;
                IdentityMap map(*wpd_camera_map);
                WpdTransport transport;
                result = RunDualWpdReadOnlyProbe(
                    transport, map, std::chrono::seconds(10));
            } catch (...) {
                // Host, lease, and map setup failures stay behind the fixed
                // anonymous result. Never publish identities or WPD details.
            }
            std::cout << SerializeDualWpdReadOnlyProbeResult(result) << '\n';
            return result.terminal_state ==
                    DualWpdReadOnlyProbeTerminalState::Pass
                ? 0 : 2;
        }
        if (read_only_coexistence_probe) {
            if (!wpd_camera_map) {
                throw std::invalid_argument(
                    "--wpd-camera-map is required for the read-only coexistence probe");
            }
            RequireExistingFixedLocalFile("--wpd-camera-map", *wpd_camera_map);
            // The probe retains the Nikon SDK Module while WPD enumerates the
            // same physical cameras. Serialize that real-camera access with
            // every other Phase 0 process for the probe's complete lifetime.
            HardwareProcessLease camera_control_lease;
            NikonDualBindingSdkAdapter adapter;
            const auto candidates = adapter.EnumerateCandidates();
            WpdTransport wpd;
            const auto wpd_cameras = wpd.Enumerate();
            IdentityMap map(*wpd_camera_map);
            std::size_t bound_count = 0;
            std::size_t unbound_count = 0;
            for (const auto& camera : wpd_cameras) {
                map.FindAlias(camera.stable_identity) ? ++bound_count : ++unbound_count;
            }
            adapter.EndSession(std::chrono::seconds(10));
            std::cout
                << "{\"operation\":\"read-only-coexistence-probe\","
                   "\"sdkD810Count\":" << candidates.size()
                << ",\"wpdD810Count\":" << wpd_cameras.size()
                << ",\"wpdBoundAliasCount\":" << bound_count
                << ",\"wpdUnboundAliasCount\":" << unbound_count
                << ",\"sdkModuleRetainedDuringWpd\":true,"
                   "\"sdkSourceOpenDuringWpd\":false,"
                   "\"captureCommandSent\":false,"
                   "\"cameraSettingsChanged\":false,"
                   "\"cameraObjectDeleteAttempted\":false,"
                   "\"terminalState\":\""
                << (candidates.size() == 2 && wpd_cameras.size() == 2
                        ? "Pass" : "Blocked")
                << "\"}\n";
            return candidates.size() == 2 && wpd_cameras.size() == 2 ? 0 : 2;
        }
        if (!pair_journal_root.has_value()) {
            throw std::invalid_argument("--pair-journal-root is required");
        }
        if (!approved_capture_profile.has_value()) {
            throw std::invalid_argument("--approved-capture-profile is required");
        }
        if (!dual_identity_proof.has_value()) {
            throw std::invalid_argument("--dual-identity-proof is required");
        }
        RequireExistingFixedLocalFile(
            "--approved-capture-profile", *approved_capture_profile);
        RequireExistingFixedLocalFile(
            "--dual-identity-proof", *dual_identity_proof);
        if (wpd_camera_map) {
            RequireExistingFixedLocalFile("--wpd-camera-map", *wpd_camera_map);
        }

        auto pair_store =
            std::make_shared<DualHardwarePairJournalStore>(*pair_journal_root);
        // No utc_clock is supplied: DualHardwareCameraAgentDispatcher falls
        // back to std::chrono::system_clock::now() whenever its injected
        // clock is empty, which is exactly the real-time behavior a
        // production host needs. Session-binding mode injects the real backend;
        // legacy mode below deliberately injects none and remains fail closed.
        if (binding_pipe_name) {
            // One operator binding session owns both the SDK candidate Live
            // View phase and the subsequent bound capture pipe. Keep the
            // process-wide camera lease in this scope so no other Phase 0
            // process can open either D810 between binding and host shutdown.
            HardwareProcessLease camera_control_lease;
            auto sdk_adapter = std::make_shared<NikonDualBindingSdkAdapter>();
            DualBindingCameraAgentDispatcher binding_dispatcher(sdk_adapter, true);
            const int binding_exit = RunDualBindingCameraAgentNamedPipeServer(
                *binding_pipe_name, binding_dispatcher, false);
            if (binding_exit != 0 ||
                binding_dispatcher.BindingState() !=
                    DualIdentitySessionBindingState::Ready) {
                throw std::runtime_error(
                    "Dual binding host ended before a Ready binding was established");
            }
            auto capture_backend = std::make_shared<DualBoundPairCaptureBackend>(
                binding_dispatcher, sdk_adapter, *wpd_camera_map);
            DualHardwareCameraAgentDispatcher dispatcher(
                pair_store, [] { return std::chrono::system_clock::now(); },
                capture_backend);
            return RunDualHardwareCameraAgentNamedPipeServer(
                pipe_name, dispatcher, false);
        }
        DualHardwareCameraAgentDispatcher dispatcher(pair_store);
        return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, serve_once);
    } catch (const std::exception& error) {
        std::cerr << "Dual Camera Agent failed closed: " << error.what() << '\n';
        PrintUsage();
        return 1;
    }
}
