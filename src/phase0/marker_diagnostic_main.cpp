#include "a0/phase0/hardware_process_lease.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

int wmain(int argc, wchar_t **argv) {
    std::filesystem::path root;
    if (argc == 2 && std::wstring_view(argv[1]) == L"--read-only") {
        // The production root is resolved by the library; this command never
        // opens a camera, creates a lease, or changes the marker.
#ifdef A0_MARKER_DIAGNOSTIC_TEST_ROOT
    } else if (argc == 3 && std::wstring_view(argv[1]) == L"--test-root") {
        root = std::filesystem::path(argv[2]);
#endif
    } else {
        std::cerr << "usage: A0CameraStitcher.MarkerDiagnostic --read-only\n";
        return 2;
    }

    a0::phase0::DualDelegationMarkerDiagnostic result;
    try {
        result = a0::phase0::InspectDualDelegationMarkerReadOnly(root);
    } catch (...) {
        result.status = "marker_unavailable";
    }
    // Status values are fixed tokens from the library; no path, PID, nonce,
    // epoch, SDK message, or camera identity is printed.
    std::cout << "{\"status\":\"" << result.status << "\",\"anonymousSha256\":\""
              << result.anonymous_sha256 << "\",\"size\":" << result.size << "}\n";
    return result.status == "eligible_for_human_review" ? 0 : 2;
}
