#include "a0/phase0/hardware_process_lease.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

// Audited one-shot recovery of a stranded dual-delegation marker
// (docs/design/dual-preview-topology-diag.md section 9). This command never
// opens a camera, starts SDK/WPD, or constructs a HardwareProcessLease. The
// only change it can make is deleting the single canonical marker whose
// anonymous SHA-256 a human approved after a recorded dry run.
namespace {

int Usage() {
    std::cerr << "usage: A0CameraStitcher.MarkerRecovery --dry-run [--expect-sha256 <hex64>]\n"
                 "       A0CameraStitcher.MarkerRecovery --execute --expect-sha256 <hex64> "
                 "--confirm-cameras-disconnected [--confirm-foreign-session]\n";
#ifdef A0_MARKER_RECOVERY_TEST_ROOT
    std::cerr << "       test build only: --test-root <absolute marker root> --test-lease <test lease name>\n";
#endif
    return 2;
}

bool IsLowerHex64(std::wstring_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](wchar_t character) {
               return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f');
           });
}

// Callers pass only values already validated as ASCII.
std::string NarrowAscii(std::wstring_view value) {
    std::string narrow;
    narrow.reserve(value.size());
    for (const wchar_t character : value) narrow.push_back(static_cast<char>(character));
    return narrow;
}

#ifdef A0_MARKER_RECOVERY_TEST_ROOT
bool IsPrintableAscii(std::wstring_view value) {
    return std::all_of(value.begin(), value.end(),
                       [](wchar_t character) { return character > L' ' && character < 0x7f; });
}
#endif

} // namespace

int wmain(int argc, wchar_t **argv) {
    enum class Mode { none, dry_run, execute };
    Mode mode = Mode::none;
    std::wstring expected;
    bool has_expected = false;
    bool attested = false;
    bool foreign_session = false;
#ifdef A0_MARKER_RECOVERY_TEST_ROOT
    std::wstring test_root;
    std::wstring test_lease;
    bool has_test_root = false;
    bool has_test_lease = false;
#endif
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--dry-run" || argument == L"--execute") {
            if (mode != Mode::none) return Usage();
            mode = argument == L"--dry-run" ? Mode::dry_run : Mode::execute;
        } else if (argument == L"--expect-sha256") {
            if (has_expected || index + 1 >= argc) return Usage();
            expected = argv[++index];
            has_expected = true;
        } else if (argument == L"--confirm-cameras-disconnected") {
            if (attested) return Usage();
            attested = true;
        } else if (argument == L"--confirm-foreign-session") {
            if (foreign_session) return Usage();
            foreign_session = true;
#ifdef A0_MARKER_RECOVERY_TEST_ROOT
        } else if (argument == L"--test-root") {
            if (has_test_root || index + 1 >= argc) return Usage();
            test_root = argv[++index];
            has_test_root = true;
        } else if (argument == L"--test-lease") {
            if (has_test_lease || index + 1 >= argc) return Usage();
            test_lease = argv[++index];
            has_test_lease = true;
#endif
        } else {
            return Usage();
        }
    }
    if (mode == Mode::none || (has_expected && !IsLowerHex64(expected))) return Usage();
    if (mode == Mode::dry_run && (attested || foreign_session)) return Usage();
    if (mode == Mode::execute && !has_expected) return Usage();

    a0::phase0::DualDelegationMarkerRecoveryOptions options;
#ifdef A0_MARKER_RECOVERY_TEST_ROOT
    if (has_test_root != has_test_lease ||
        (has_test_root && (test_root.empty() || test_lease.empty() || !IsPrintableAscii(test_lease))))
        return Usage();
    if (has_test_root) {
        options.test_marker_root = std::filesystem::path(test_root);
        options.test_lease_name = NarrowAscii(test_lease);
    }
#endif
    // Validated above as lower-case hex, so the narrowing copy is exact.
    const std::string expected_sha256 = NarrowAscii(expected);

    a0::phase0::DualDelegationMarkerRecoveryResult result;
    try {
        result = mode == Mode::dry_run
            ? a0::phase0::DryRunDualDelegationMarkerRecovery(options, expected_sha256)
            : a0::phase0::ExecuteDualDelegationMarkerRecovery(options, expected_sha256, attested, foreign_session);
    } catch (...) {
        result = {};
        result.status = "marker_unavailable";
    }
    // Fixed status token, anonymous hash, size and session match only: no
    // path, PID, nonce, epoch, session number or camera identity is printed.
    std::cout << "{\"status\":\"" << result.status << "\",\"anonymousSha256\":\"" << result.anonymous_sha256
              << "\",\"size\":" << result.size << ",\"sessionMatch\":" << (result.session_match ? 1 : 0) << "}\n";
    const std::string_view success = mode == Mode::dry_run ? "recovery_eligible" : "executed_marker_absent";
    return result.status == success ? 0 : 2;
}
