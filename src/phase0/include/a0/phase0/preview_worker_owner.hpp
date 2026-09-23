#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include "a0/phase0/preview_commissioning.hpp"
#include "a0/phase0/preview_worker_reply.hpp"

namespace a0::phase0::experimental {
struct PreviewWorkerFailureObservation {
    std::size_t worker_index{};
    std::string operation;
    std::string category; // Bounded worker code, never an SDK exception message.
    bool response_received{};
    bool response_validated{};
    bool ack_write_completed{}; // Local write only; not proof of server ACK processing.
    std::optional<PreviewWorkerReplyStatus> response_status;
    std::optional<PreviewWorkerCloseReceipt> reported_close;
};

// Fixed/bounded diagnostic fields only. Never include SDK free-form errors.
[[nodiscard]] std::wstring FormatPreviewWorkerFailure(const PreviewWorkerFailureObservation& failure);

class PreviewWorkerStartupError final : public std::runtime_error {
public:
    explicit PreviewWorkerStartupError(bool workers_may_exist)
        : std::runtime_error("preview worker startup failed"), workers_may_exist_(workers_may_exist) {}
    bool WorkersMayExist() const noexcept { return workers_may_exist_; }
private:
    bool workers_may_exist_;
};
// Internal experimental integration; hardware acceptance remains gated.
// Thread-affine: create, close and destroy on the lease-owning thread.
// Destruction does not close workers or clear an armed quarantine marker.
class PreviewWorkerOwner final {
public:
    PreviewWorkerOwner();
    PreviewWorkerOwner(std::string_view test_lease_name, const std::filesystem::path& test_marker_root,
                       const std::filesystem::path& worker_executable, std::chrono::milliseconds lifetime,
                       std::function<void(std::array<std::uint32_t, 2>)> after_spawn_for_testing = {},
                       bool inject_ack_write_failure_for_testing = false);
    ~PreviewWorkerOwner();
    PreviewWorkerOwner(const PreviewWorkerOwner&) = delete;
    PreviewWorkerOwner& operator=(const PreviewWorkerOwner&) = delete;
    // Cached result on repeated calls: never resends an ambiguous close.
    bool Close() noexcept;
    [[nodiscard]] std::optional<PreviewWorkerFailureObservation> FirstFailure() const;
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
