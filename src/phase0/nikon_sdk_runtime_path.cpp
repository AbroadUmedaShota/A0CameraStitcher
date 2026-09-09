#include "nikon_sdk_runtime_path.hpp"

#include <array>
#include <cwctype>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace a0::phase0::detail {
namespace fs = std::filesystem;
namespace {

constexpr std::wstring_view kExpectedDirectorySuffix =
    L"/module/win/binary files/x64";

std::wstring Lowercase(std::wstring value) {
    for (wchar_t& character : value) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return value;
}

bool HasExpectedLocation(const fs::path& module_path) {
    if (Lowercase(module_path.filename().wstring()) != L"type0014.md3") {
        return false;
    }
    const std::wstring directory = Lowercase(
        module_path.parent_path().generic_wstring());
    return directory.size() >= kExpectedDirectorySuffix.size() &&
        directory.compare(
            directory.size() - kExpectedDirectorySuffix.size(),
            kExpectedDirectorySuffix.size(),
            kExpectedDirectorySuffix) == 0;
}

bool IsFixedLocalPath(const fs::path& path) {
#ifdef _WIN32
    const std::wstring native = path.native();
    if (native.starts_with(L"\\\\")) {
        return false;
    }
    const std::wstring root = path.root_path().native();
    return !root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_FIXED;
#else
    (void)path;
    return false;
#endif
}

bool HasNoReparsePoint(const fs::path& path) {
#ifdef _WIN32
    fs::path current = path.root_path();
    const DWORD root_attributes = GetFileAttributesW(current.c_str());
    if (root_attributes == INVALID_FILE_ATTRIBUTES ||
        (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    for (const fs::path& component : path.lexically_relative(current)) {
        current /= component;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }
    }
    return true;
#else
    (void)path;
    return false;
#endif
}

bool IsTrustedRegularFile(const fs::path& path) {
    std::error_code error;
    return path.is_absolute() && IsFixedLocalPath(path) &&
        HasNoReparsePoint(path) && fs::is_regular_file(path, error) && !error;
}

} // namespace

std::optional<fs::path> ValidateNikonSdkRuntimeModule(
    const fs::path& module_path) noexcept {
    try {
        const fs::path normalized = module_path.lexically_normal();
        if (!HasExpectedLocation(normalized) || !IsTrustedRegularFile(normalized)) {
            return std::nullopt;
        }

        const fs::path directory = normalized.parent_path();
        constexpr std::array<std::wstring_view, 3> kCompanions{
            L"NkdPTP.dll", L"NkRoyalmile.dll", L"dnssd.dll"};
        for (const std::wstring_view name : kCompanions) {
            if (!IsTrustedRegularFile(directory / name)) {
                return std::nullopt;
            }
        }

        std::error_code error;
        const fs::path canonical_directory = fs::canonical(directory, error);
        if (error) {
            return std::nullopt;
        }
        const fs::path canonical_module = fs::canonical(normalized, error);
        if (error || canonical_module.parent_path() != canonical_directory) {
            return std::nullopt;
        }
        return canonical_module;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<fs::path> ResolveNikonSdkModuleFromEnvironment() noexcept {
#ifdef _WIN32
    constexpr wchar_t kVariableName[] = L"NIKON_D810_SDK_MODULE_PATH";
    const DWORD required = GetEnvironmentVariableW(kVariableName, nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }

    std::vector<wchar_t> value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        kVariableName, value.data(), static_cast<DWORD>(value.size()));
    if (written == 0 || written >= static_cast<DWORD>(value.size())) {
        return std::nullopt;
    }
    return ValidateNikonSdkRuntimeModule(fs::path(value.data()));
#else
    return std::nullopt;
#endif
}

} // namespace a0::phase0::detail
