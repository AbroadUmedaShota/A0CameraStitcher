#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace a0::phase0::experimental {
// Parent-side launch/close integration. Commissioning/preview commands are not
// exposed until physical A/B binding and concurrent grants are connected.
// Thread-affine: create, close and destroy on the lease-owning thread.
// Destruction does not close workers or clear an armed quarantine marker.
class PreviewWorkerOwner final {
public:
    PreviewWorkerOwner();
    PreviewWorkerOwner(std::string_view test_lease_name, const std::filesystem::path& test_marker_root,
                       const std::filesystem::path& worker_executable, std::chrono::milliseconds lifetime);
    ~PreviewWorkerOwner();
    PreviewWorkerOwner(const PreviewWorkerOwner&) = delete;
    PreviewWorkerOwner& operator=(const PreviewWorkerOwner&) = delete;
    // Cached result on repeated calls: never resends an ambiguous close.
    bool Close() noexcept;
    std::array<std::uint32_t, 2> ProcessIds() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace a0::phase0::experimental
