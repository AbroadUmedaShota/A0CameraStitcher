#include "nikon_sdk_runtime_path.hpp"

#ifdef A0_TEST_NIKON_SDK_ENABLED
#include "a0/phase0/nikon_sdk_transport.hpp"
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using a0::phase0::detail::ResolveNikonSdkModuleFromEnvironment;
using a0::phase0::detail::ValidateNikonSdkRuntimeModule;

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
            ("a0-nikon-sdk-runtime-path-" + std::to_string(nonce));
        fs::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& Path() const noexcept { return path_; }

private:
    fs::path path_;
};

fs::path AddValidModule(const fs::path& root, const fs::path& package_name) {
    const fs::path directory = root / package_name /
        "Module" / "Win" / "Binary Files" / "x64";
    fs::create_directories(directory);
    for (const auto* name : {"Type0014.md3", "NkdPTP.dll", "NkRoyalmile.dll", "dnssd.dll"}) {
        std::ofstream(directory / name, std::ios::binary).put('\0');
    }
    return directory / "Type0014.md3";
}

void TestInvalidLocationsFailClosed() {
    TemporaryDirectory temporary;
    Check(!ValidateNikonSdkRuntimeModule(temporary.Path() / "missing.md3"),
        "missing module must fail closed");
    Check(!ValidateNikonSdkRuntimeModule(fs::path("relative") / "Type0014.md3"),
        "relative module path must fail closed");
    Check(!ValidateNikonSdkRuntimeModule(
            fs::path(LR"(\\server\share\Module\Win\Binary Files\x64\Type0014.md3)")),
        "remote module path must fail closed");

    const fs::path wrong_layout = temporary.Path() / "Module" / "Win" /
        "Sample Program" / "x64" / "Release" / "Type0014.md3";
    fs::create_directories(wrong_layout.parent_path());
    std::ofstream(wrong_layout, std::ios::binary).put('\0');
    Check(!ValidateNikonSdkRuntimeModule(wrong_layout),
        "sample build module must fail closed");
}

void TestCompleteLocalModuleIsAccepted() {
    TemporaryDirectory temporary;
    const fs::path expected = AddValidModule(
        temporary.Path(), L"licensed-package-\u65E5\u672C\u8A9E");
    const auto resolved = ValidateNikonSdkRuntimeModule(expected);
    Check(resolved && *resolved == fs::canonical(expected),
        "complete local x64 module must be accepted, including Unicode paths");
}

void TestMissingCompanionFailsClosed() {
    TemporaryDirectory temporary;
    const fs::path module = AddValidModule(temporary.Path(), "incomplete");
    fs::remove(module.parent_path() / "NkdPTP.dll");
    Check(!ValidateNikonSdkRuntimeModule(module),
        "module without a runtime companion must fail closed");
}

#ifdef _WIN32
class EnvironmentRestore {
public:
    explicit EnvironmentRestore(const wchar_t* name) : name_(name) {
        const DWORD required = GetEnvironmentVariableW(name_, nullptr, 0);
        if (required != 0) {
            value_.resize(required, L'\0');
            const DWORD written = GetEnvironmentVariableW(
                name_, value_.data(), static_cast<DWORD>(value_.size()));
            if (written != 0 && written < static_cast<DWORD>(value_.size())) {
                value_.resize(written);
                existed_ = true;
            }
        }
    }

    ~EnvironmentRestore() {
        SetEnvironmentVariableW(name_, existed_ ? value_.c_str() : nullptr);
    }

private:
    const wchar_t* name_;
    std::wstring value_;
    bool existed_{};
};

void TestEnvironmentResolutionIsExplicitAndFailClosed() {
    constexpr wchar_t kVariableName[] = L"NIKON_D810_SDK_MODULE_PATH";
    EnvironmentRestore restore(kVariableName);
    Check(SetEnvironmentVariableW(kVariableName, nullptr) != FALSE,
        "test environment variable must be cleared");
    Check(!ResolveNikonSdkModuleFromEnvironment(),
        "missing runtime module environment variable must fail closed");

    TemporaryDirectory temporary;
    const fs::path expected = AddValidModule(temporary.Path(), "runtime");
    Check(SetEnvironmentVariableW(kVariableName, expected.c_str()) != FALSE,
        "test environment variable must be set");
    const auto resolved = ResolveNikonSdkModuleFromEnvironment();
    Check(resolved && *resolved == fs::canonical(expected),
        "runtime environment must resolve the explicitly configured module");
}

enum class ReparsePointTestResult {
    executed,
    unsupported,
};

ReparsePointTestResult TestDirectoryReparsePointFailsClosedWhenSupported() {
    TemporaryDirectory temporary;
    const fs::path real_root = temporary.Path() / "real";
    const fs::path module = AddValidModule(real_root, "licensed");
    const fs::path link = temporary.Path() / "linked";
    constexpr DWORD kAllowUnprivilegedCreate = 0x2;
    if (CreateSymbolicLinkW(
            link.c_str(), real_root.c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY | kAllowUnprivilegedCreate) == FALSE) {
        return ReparsePointTestResult::unsupported;
    }
    const fs::path linked_module = link / module.lexically_relative(real_root);
    Check(!ValidateNikonSdkRuntimeModule(linked_module),
        "module reached through a directory reparse point must fail closed");
    return ReparsePointTestResult::executed;
}

#ifdef A0_TEST_NIKON_SDK_ENABLED
void TestInvalidRuntimePathReleasesRealTransportSessionClaim() {
    constexpr wchar_t kVariableName[] = L"NIKON_D810_SDK_MODULE_PATH";
    EnvironmentRestore restore(kVariableName);
    Check(SetEnvironmentVariableW(kVariableName, nullptr) != FALSE,
        "test environment variable must be cleared");

    a0::phase0::NikonSdkTransport transport;
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            const auto cameras = transport.Enumerate();
            (void)cameras;
            throw std::runtime_error(
                "real transport must fail before loading the SDK when runtime configuration is absent");
        } catch (const a0::phase0::TransportError& error) {
            Check(error.Category() == "sdk_load_failed",
                "missing runtime configuration must report sdk_load_failed and release the session claim");
        }
    }
}
#endif
#endif

} // namespace

int main() {
    try {
        TestInvalidLocationsFailClosed();
        TestCompleteLocalModuleIsAccepted();
        TestMissingCompanionFailsClosed();
#ifdef _WIN32
        TestEnvironmentResolutionIsExplicitAndFailClosed();
        const auto reparse_result = TestDirectoryReparsePointFailsClosedWhenSupported();
        std::cout << "Reparse-point rejection: "
                  << (reparse_result == ReparsePointTestResult::executed
                          ? "EXECUTED_AND_PASSED"
                          : "SKIPPED_UNSUPPORTED")
                  << '\n';
#ifdef A0_TEST_NIKON_SDK_ENABLED
        TestInvalidRuntimePathReleasesRealTransportSessionClaim();
        std::cout << "Real transport missing-runtime cleanup: EXECUTED_AND_PASSED\n";
#else
        std::cout << "Real transport missing-runtime cleanup: SKIPPED_SDK_ADAPTER_NOT_BUILT\n";
#endif
#endif
        std::cout << "Nikon SDK runtime path tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
