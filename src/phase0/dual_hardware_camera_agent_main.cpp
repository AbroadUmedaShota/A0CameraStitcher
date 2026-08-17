#include "a0/phase0/dual_hardware_camera_agent.hpp"
#include "a0/phase0/dual_hardware_camera_agent_store.hpp"

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
           L"--approved-capture-profile PATH --dual-identity-proof PATH\n";
}

// This host intentionally never constructs a DualHardwareFakePairCaptureBackend.
// AR-08a-2B's fake orchestrator exists for contract tests only; wiring a real
// camera backend (or the fake one) into start-reserved-pair here is out of
// this Issue's scope ("real SDK/WPD/camera backend" is explicitly excluded).
// Without a fake_backend_, DualHardwareCameraAgentDispatcher::Handle already
// answers every start-reserved-pair request with the typed PairDispatcherUnavailable
// rejection *after* running its full identity/capture-profile/rig-profile/
// confirmation preflight (see dual_hardware_camera_agent.cpp), so this
// production host still exercises get-dual-capabilities, reserve-pair-
// transaction, the complete start-reserved-pair preflight, and get-pair-
// transaction-result end to end through the durable pair journal store.
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
        std::optional<fs::path> pair_journal_root;
        std::optional<fs::path> approved_capture_profile;
        std::optional<fs::path> dual_identity_proof;
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
                argument == L"--pipe-name" || argument == L"--pair-journal-root" ||
                argument == L"--approved-capture-profile" ||
                argument == L"--dual-identity-proof";
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
            } else if (argument == L"--pair-journal-root") {
                pair_journal_root = fs::path(argv[index]);
            } else if (argument == L"--approved-capture-profile") {
                approved_capture_profile = fs::path(argv[index]);
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

        auto pair_store =
            std::make_shared<DualHardwarePairJournalStore>(*pair_journal_root);
        // No utc_clock is supplied: DualHardwareCameraAgentDispatcher falls
        // back to std::chrono::system_clock::now() whenever its injected
        // clock is empty, which is exactly the real-time behavior a
        // production host needs. No fake_backend is supplied: see the
        // comment above RequireExistingFixedLocalFile for why that is the
        // documented, fail-closed choice for this Issue.
        DualHardwareCameraAgentDispatcher dispatcher(pair_store);
        return RunDualHardwareCameraAgentNamedPipeServer(pipe_name, dispatcher, serve_once);
    } catch (const std::exception& error) {
        std::cerr << "Dual Camera Agent failed closed: " << error.what() << '\n';
        PrintUsage();
        return 1;
    }
}
