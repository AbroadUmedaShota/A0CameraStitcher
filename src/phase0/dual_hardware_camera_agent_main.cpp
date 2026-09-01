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

const char* PreflightErrorText(
    DualBoundPairPreflightError error) noexcept {
    switch (error) {
    case DualBoundPairPreflightError::None: return "none";
    case DualBoundPairPreflightError::SdkInvalidatedBeforeWpd:
        return "sdkInvalidatedBeforeWpd";
    case DualBoundPairPreflightError::SdkBoundaryUnsafeBeforeWpd:
        return "sdkBoundaryUnsafeBeforeWpd";
    case DualBoundPairPreflightError::WpdProbeBlocked:
        return "wpdProbeBlocked";
    case DualBoundPairPreflightError::SdkBoundaryUnsafeAfterWpd:
        return "sdkBoundaryUnsafeAfterWpd";
    case DualBoundPairPreflightError::SdkInvalidatedAfterWpd:
        return "sdkInvalidatedAfterWpd";
    }
    return "sdkBoundaryUnsafeBeforeWpd";
}

const char* JsonBool(bool value) noexcept {
    return value ? "true" : "false";
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
            std::optional<std::size_t> sdk_count;
            DualBoundPairPreflightResult preflight;
            INikonDualSessionTransport::ExitState final_sdk_state;
            bool sdk_cleanup_attempted = false;
            bool sdk_cleanup_error = false;

            // The process lease remains alive through WPD destruction and the
            // one bounded SDK cleanup attempt. No cleanup retry is hidden by a
            // second owner or an outer exception handler.
            try {
                HardwareProcessLease camera_control_lease;
                std::shared_ptr<NikonDualBindingSdkAdapter> adapter;
                try {
                    IdentityMap map(*wpd_camera_map);
                    adapter = std::make_shared<NikonDualBindingSdkAdapter>();
                    sdk_count = adapter->EnumerateCandidates().size();
                    {
                        WpdTransport wpd;
                        if (*sdk_count == kDualIdentityRequiredCandidateCount) {
                            preflight = RunDualBoundPairPreflight(
                                *adapter, wpd, map, std::chrono::seconds(10));
                        }
                    }
                } catch (...) {
                    // Fixed anonymous fields below are the only public error
                    // boundary for licensed SDK, WPD, map, and host failures.
                }
                if (adapter) {
                    const auto retained_before_cleanup =
                        adapter->InspectRetainedModuleState();
                    final_sdk_state = {
                        retained_before_cleanup.process_claim_retained,
                        retained_before_cleanup.module_retained,
                        retained_before_cleanup.open_source_count != 0};
                    const bool wpd_cleanup_unconfirmed =
                        preflight.wpd_result.wpd_session_open_at_exit ||
                        preflight.wpd_result.cleanup ==
                            DualWpdReadOnlyProbeCleanup::Unconfirmed;
                    if (wpd_cleanup_unconfirmed) {
                        // No SDK cleanup is safe while WPD ownership is
                        // unconfirmed. The adapter isolates itself until this
                        // process exits instead of crossing that boundary.
                        adapter->AbandonSessionNoSdkCalls();
                    } else if (!final_sdk_state.FullyEnded()) {
                        sdk_cleanup_attempted = true;
                        try {
                            adapter->EndSession(std::chrono::seconds(10));
                        } catch (...) {
                            sdk_cleanup_error = true;
                        }
                    }
                    const auto retained = adapter->InspectRetainedModuleState();
                    final_sdk_state = {
                        retained.process_claim_retained,
                        retained.module_retained,
                        retained.open_source_count != 0};
                }
            } catch (...) {
                // Lease construction failure is also represented only by the
                // fixed Blocked result below.
            }

            const bool module_retained_during_wpd =
                preflight.wpd_result.wpd_access_attempted &&
                preflight.sdk_state_before_wpd.module_retained &&
                preflight.sdk_state_after_wpd.module_retained;
            const bool source_open_during_wpd =
                preflight.sdk_state_before_wpd.open_source_count != 0 ||
                preflight.sdk_state_after_wpd.open_source_count != 0;
            const bool live_view_active_during_wpd =
                preflight.sdk_state_before_wpd.live_view_active ||
                preflight.sdk_state_after_wpd.live_view_active;
            const bool passed = sdk_count ==
                    kDualIdentityRequiredCandidateCount &&
                preflight.ready && module_retained_during_wpd &&
                !source_open_during_wpd && !live_view_active_during_wpd &&
                final_sdk_state.FullyEnded() && !sdk_cleanup_error;

            std::cout
                << "{\"operation\":\"read-only-coexistence-probe\","
                   "\"sdkD810Count\":";
            if (sdk_count) std::cout << *sdk_count; else std::cout << "null";
            std::cout << ",\"wpdD810Count\":";
            if (preflight.wpd_result.wpd_d810_count) {
                std::cout << *preflight.wpd_result.wpd_d810_count;
            } else {
                std::cout << "null";
            }
            std::cout
                << ",\"camAMapMatchCount\":"
                << preflight.wpd_result.cam_a_match_count
                << ",\"camBMapMatchCount\":"
                << preflight.wpd_result.cam_b_match_count
                << ",\"camAPayloadObjectCount\":";
            if (preflight.wpd_result.cam_a_payload_object_count) {
                std::cout << *preflight.wpd_result.cam_a_payload_object_count;
            } else {
                std::cout << "null";
            }
            std::cout << ",\"camBPayloadObjectCount\":";
            if (preflight.wpd_result.cam_b_payload_object_count) {
                std::cout << *preflight.wpd_result.cam_b_payload_object_count;
            } else {
                std::cout << "null";
            }
            std::cout
                << ",\"topologyStable\":"
                << JsonBool(preflight.wpd_result.topology_stable)
                << ",\"wpdSessionOpenAtExit\":"
                << JsonBool(preflight.wpd_result.wpd_session_open_at_exit)
                << ",\"sdkModuleRetainedDuringWpd\":"
                << JsonBool(module_retained_during_wpd)
                << ",\"sdkSourceOpenDuringWpd\":"
                << JsonBool(source_open_during_wpd)
                << ",\"liveViewActiveDuringWpd\":"
                << JsonBool(live_view_active_during_wpd)
                << ",\"sdkOperationInvokedDuringWpd\":false,"
                   "\"sdkSessionEnded\":"
                << JsonBool(final_sdk_state.FullyEnded())
                << ",\"sdkCleanupAttempted\":"
                << JsonBool(sdk_cleanup_attempted)
                << ",\"captureCommandSent\":false,"
                   "\"cameraSettingsChanged\":false,"
                   "\"cameraObjectDeleteAttempted\":false,"
                   "\"vendorOperationExecuted\":false,"
                   "\"automaticRetryCount\":0,"
                   "\"realIdentifiersIncluded\":false,"
                   "\"errorCategory\":\""
                << PreflightErrorText(preflight.error)
                << "\",\"terminalState\":\""
                << (passed ? "Pass" : "Blocked") << "\"}\n";
            return passed ? 0 : 2;
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
            if (binding_exit != 0) {
                throw std::runtime_error(
                    "Dual binding host ended before a terminal request was delivered");
            }
            if (binding_dispatcher.CancellationRequested()) {
                return binding_dispatcher.CancellationSucceeded() ? 0 : 2;
            }
            if (!binding_dispatcher.CaptureTransitionRequested() ||
                binding_dispatcher.BindingState() !=
                    DualIdentitySessionBindingState::Ready) {
                throw std::runtime_error(
                    "Dual binding host ended before capture activation was approved");
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
