#pragma once

#include <filesystem>
#include <optional>

namespace a0::phase0::detail {

// Runtime-only resolution keeps the licensed SDK location out of product
// binaries. The explicitly configured module is accepted only when it is a
// trusted local x64 SDK file with all required runtime companions colocated.
[[nodiscard]] std::optional<std::filesystem::path>
ValidateNikonSdkRuntimeModule(const std::filesystem::path& module_path) noexcept;

[[nodiscard]] std::optional<std::filesystem::path>
ResolveNikonSdkModuleFromEnvironment() noexcept;

} // namespace a0::phase0::detail
