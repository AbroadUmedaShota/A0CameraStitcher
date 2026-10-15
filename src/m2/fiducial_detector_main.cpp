#include "a0/m2/fiducial_detector.hpp"

#include <Windows.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    std::string output(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), size, nullptr, nullptr) != size) {
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    }
    return output;
}

std::map<std::wstring, std::wstring> ParseOptions(const int argc, wchar_t* argv[]) {
    std::map<std::wstring, std::wstring> options;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc || std::wstring_view(argv[index]).find(L"--") != 0) {
            throw std::invalid_argument("every option requires a --name and a value");
        }
        const std::wstring name = argv[index] + 2;
        if (name.empty() || !options.emplace(name, argv[index + 1]).second) {
            throw std::invalid_argument("option names must be non-empty and unique");
        }
    }
    return options;
}

std::string ReadJob(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > a0::m2::fiducial::kMaximumJobBytes) {
        throw std::invalid_argument("job file is missing, empty, or larger than 1 MiB");
    }
    std::ifstream file(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file && !file.eof()) throw std::runtime_error("job file read failed");
    return text;
}

// Creates a new file; refuses to touch an existing one.
void WriteNew(const std::filesystem::path& path, const std::string& text) {
    if (std::filesystem::exists(path)) throw std::runtime_error("output file already exists: " + path.filename().string());
    std::ofstream file(path, std::ios::binary | std::ios::out);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    if (!file) throw std::runtime_error("output file write failed: " + path.filename().string());
}

} // namespace

// Usage: A0CameraStitcher.FiducialDetector --job <job.json> --output <directory>
// Image paths in the job are relative to the job file's folder. Writes
// <directory>/session.json (only when every view resolved) and
// <directory>/detection-report.json. Development tool; not part of the product.
int wmain(const int argc, wchar_t* argv[]) {
    try {
        const auto options = ParseOptions(argc, argv);
        for (const auto& [name, value] : options) {
            (void)value;
            if (name != L"job" && name != L"output") throw std::invalid_argument("unknown option: --" + Utf8(name));
        }
        const auto job_option = options.find(L"job");
        const auto output_option = options.find(L"output");
        if (job_option == options.end() || output_option == options.end()) {
            throw std::invalid_argument("--job and --output are required");
        }
        const std::filesystem::path job_path = job_option->second;
        const std::filesystem::path output = output_option->second;
        // Refuse either collision before computing or writing anything. In
        // particular, an existing report must not leave a newly written session.
        for (const auto* name : {L"session.json", L"detection-report.json"}) {
            if (std::filesystem::exists(output / name)) {
                throw std::runtime_error("output file already exists");
            }
        }
        const auto result = a0::m2::fiducial::RunJob(ReadJob(job_path), job_path.parent_path());

        std::filesystem::create_directories(output);
        if (result.ok) WriteNew(output / "session.json", result.session_json);
        WriteNew(output / "detection-report.json", result.report_json);
        std::cout << "result=" << (result.ok ? "session-written" : "no-session") << '\n';
        return result.ok ? 0 : 3;
    } catch (const std::exception& exception) {
        std::cerr << "error=" << exception.what() << '\n';
        return 2;
    }
}
