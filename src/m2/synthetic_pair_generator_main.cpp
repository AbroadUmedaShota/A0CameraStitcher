#include "a0/m2/synthetic_pair.hpp"

#include <Windows.h>

#include <charconv>
#include <cstdint>
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

std::string ReadWholeFile(const std::filesystem::path& path) {
    constexpr std::uintmax_t kMaximumSpecBytes = 1U << 20U;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumSpecBytes) {
        throw std::invalid_argument("spec file is missing, empty, or larger than 1 MiB");
    }
    std::ifstream file(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file && !file.eof()) throw std::runtime_error("spec file read failed");
    return text;
}

} // namespace

// Usage: A0CameraStitcher.SyntheticPairGenerator --spec <spec.json> --output <directory> [--threads <n>]
// Writes <directory>/<alias>/original.jpg for every camera and <directory>/ground-truth.json.
int wmain(const int argc, wchar_t* argv[]) {
    try {
        const auto options = ParseOptions(argc, argv);
        for (const auto& [name, value] : options) {
            if (name != L"spec" && name != L"output" && name != L"threads") {
                throw std::invalid_argument("unknown option: --" + Utf8(name));
            }
        }
        const auto spec_option = options.find(L"spec");
        const auto output_option = options.find(L"output");
        if (spec_option == options.end() || output_option == options.end()) {
            throw std::invalid_argument("--spec and --output are required");
        }
        a0::m2::synthetic::GenerateOptions generate;
        generate.output_directory = output_option->second;
        if (const auto threads = options.find(L"threads"); threads != options.end()) {
            const auto text = Utf8(threads->second);
            const auto result = std::from_chars(text.data(), text.data() + text.size(), generate.thread_count);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || generate.thread_count == 0) {
                throw std::invalid_argument("--threads must be a positive integer");
            }
        }
        const auto spec = a0::m2::synthetic::ParsePairSpec(ReadWholeFile(spec_option->second));
        const auto result = a0::m2::synthetic::GeneratePair(spec, generate);

        std::cout << "result=generated-synthetic-pair\n"
                  << "generatorVersion=" << a0::m2::synthetic::kGeneratorVersion << '\n'
                  << "seed=" << spec.seed << '\n'
                  << "width=" << spec.image_width_px << "\nheight=" << spec.image_height_px << '\n';
        for (const auto& file : result.files) {
            std::cout << "output." << file.alias << '=' << file.relative_path
                      << "\nsha256." << file.alias << '=' << file.sha256
                      << "\nsize." << file.alias << '=' << file.size_bytes << '\n';
        }
        std::cout << "groundTruth=" << result.ground_truth_relative_path
                  << "\nsha256.groundTruth=" << result.ground_truth_sha256
                  << "\nelapsedMilliseconds=" << result.elapsed_milliseconds
                  << "\npeakWorkingSetBytes=" << result.peak_working_set_bytes << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "error=" << exception.what() << '\n';
        return 2;
    }
}
