#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include "a0/phase0/preview_commissioning.hpp"

namespace a0::phase0::experimental {
// Internal experimental integration; no application/CLI enables this yet.
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
    std::array<std::string, 2> Enumerate(std::size_t worker);
    std::vector<unsigned char> Preview(std::size_t worker, std::string_view candidate);
    void ConfirmAndSuspend(std::size_t worker, ObservedPreviewBody body);
    void StartBoth();
    std::vector<unsigned char> Read(ObservedPreviewBody body);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace a0::phase0::experimental
