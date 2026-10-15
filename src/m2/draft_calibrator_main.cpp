#include "a0/m2/draft_calibration_artifacts.hpp"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
std::string Read(const std::filesystem::path& path, std::uintmax_t maximum) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > maximum) throw std::invalid_argument("input file missing, empty or too large");
    std::ifstream file(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) throw std::runtime_error("input file read failed");
    return text;
}

// Exclusive file creation and durable flush inside a directory owned by this
// invocation. A failed run retains the staging directory for diagnosis.
void WriteNew(const std::filesystem::path& path, const std::string& text) {
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("new output file could not be created");
    struct Guard { HANDLE h; ~Guard() { CloseHandle(h); } } guard{handle};
    std::size_t offset = 0;
    while (offset < text.size()) {
        const DWORD size = static_cast<DWORD>(std::min<std::size_t>(1U << 20, text.size() - offset));
        DWORD written = 0;
        if (!WriteFile(handle, text.data() + offset, size, &written, nullptr) || written == 0)
            throw std::runtime_error("output file write failed; partial directory retained");
        offset += written;
    }
    if (!FlushFileBuffers(handle)) throw std::runtime_error("output file flush failed; partial directory retained");
}
} // namespace

// Development-only: --session <observations.json> --job <job.json> --output <NEW directory>
int wmain(int argc, wchar_t* argv[]) {
    try {
        std::map<std::wstring, std::wstring> options;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 >= argc || std::wstring_view(argv[i]).find(L"--") != 0) throw std::invalid_argument("options need name and value");
            const std::wstring name = argv[i] + 2;
            if ((name != L"session" && name != L"job" && name != L"output") || !options.emplace(name, argv[i + 1]).second)
                throw std::invalid_argument("unknown or repeated option");
        }
        if (options.size() != 3) throw std::invalid_argument("--session, --job and --output are required");
        const std::filesystem::path output = options.at(L"output");
        if (output.empty() || std::filesystem::exists(output)) throw std::invalid_argument("output directory must be new");
        const auto artifacts = a0::m2::calibration::RunDraftCalibrationJob(
            Read(options.at(L"session"), a0::m2::calibration::kMaximumObservationSessionJsonBytes), Read(options.at(L"job"), 65536));
        auto staging = output;
        staging += L".partial-" + std::to_wstring(GetCurrentProcessId());
        // The parent must already exist. Do not create or replace an arbitrary
        // chain of directories or reuse a previous partial directory.
        if (!std::filesystem::create_directory(staging)) throw std::runtime_error("new partial directory could not be created");
        if (artifacts.ok) WriteNew(staging / "profile.json", artifacts.profile_json);
        WriteNew(staging / "calibration-report.json", artifacts.report_json);
        if ((artifacts.ok && Read(staging / "profile.json", 1U << 20) != artifacts.profile_json)
            || Read(staging / "calibration-report.json", 16U << 20) != artifacts.report_json)
            throw std::runtime_error("output reread mismatch; partial directory retained");
        if (!MoveFileExW(staging.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("output directory publication failed; partial directory retained");
        std::cout << "result=" << (artifacts.ok ? "draft-produced" : "no-profile") << '\n';
        return artifacts.ok ? 0 : 3;
    } catch (const std::exception& error) {
        // Standard filesystem exceptions contain local paths; only expose a
        // controlled code. The artifacts have no environment-derived paths.
        (void)error;
        std::cerr << "error=draft-calibration-input-or-io-failed\n";
        return 2;
    }
}
