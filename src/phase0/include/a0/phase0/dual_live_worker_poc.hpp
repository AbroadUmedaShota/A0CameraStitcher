#pragma once

// SOFTWARE-ONLY EXPERIMENT: two worker processes may carry transient preview
// bytes, but this header deliberately has no Nikon SDK/WPD dependency and is
// not wired into any production agent. It cannot open a camera, capture, or
// release the operator-session camera-control lease.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace a0::phase0 {

inline constexpr std::size_t kDualLiveWorkerPocMaximumFrameBytes = 256U * 1024U;

struct DualLiveWorkerPocFrame {
    std::string worker;
    std::string generation;
    std::uint64_t sequence{};
    std::vector<std::uint8_t> bytes;
};

// This is an open-time filter, not an identity binding. Source IDs are local
// to one SDK process and may not be handed to another process as an identity.
// A future hardware path must derive this plan inside the worker that performed
// session-local visual assignment and then prove the physical correspondence.
[[nodiscard]] inline std::optional<std::uint32_t> SelectAssignedSourceBeforeOpen(
    const std::vector<std::uint32_t>& enumerated_source_ids,
    std::uint32_t worker_local_assigned_source_id) {
    std::size_t matches = 0;
    for (const auto id : enumerated_source_ids) {
        if (id == worker_local_assigned_source_id) ++matches;
    }
    return matches == 1 ? std::optional<std::uint32_t>(worker_local_assigned_source_id)
                        : std::nullopt;
}

// A controller-owned, fail-closed latest-frame mailbox. A worker is accepted
// only for its issued generation and capability token. Any worker exit/fault
// makes the whole controller terminal: no reconnect, worker restart, capture,
// or WPD handoff is authorized by this experiment.
class DualLiveWorkerPocCoordinator final {
public:
    explicit DualLiveWorkerPocCoordinator(
        std::string generation, std::string a_token, std::string b_token)
        : generation_(std::move(generation)), a_token_(std::move(a_token)),
          b_token_(std::move(b_token)) {
        terminal_ = generation_.empty() || a_token_.empty() || b_token_.empty() || a_token_ == b_token_;
    }

    [[nodiscard]] bool AcceptFrame(
        std::string_view worker, std::string_view generation, std::string_view token,
        std::uint64_t sequence, std::vector<std::uint8_t> bytes) {
        const auto expected_token = worker == "CAM-A" ? a_token_
            : worker == "CAM-B" ? b_token_ : std::string{};
        if (terminal_ || sequence == 0 || generation != generation_ || token != expected_token ||
            expected_token.empty() || bytes.empty() ||
            bytes.size() > kDualLiveWorkerPocMaximumFrameBytes) return false;
        auto& slot = worker == "CAM-A" ? a_ : b_;
        if (slot && sequence <= slot->sequence) return false;
        slot = DualLiveWorkerPocFrame{std::string(worker), generation_, sequence, std::move(bytes)};
        return true;
    }

    void ReportWorkerExited(std::string_view worker) noexcept {
        if (worker == "CAM-A" || worker == "CAM-B") terminal_ = true;
    }
    void ReportControllerExited() noexcept { terminal_ = true; }
    [[nodiscard]] bool CanBeginNewOperation() const noexcept { return !terminal_; }
    [[nodiscard]] bool CanBeginCaptureOrWpd() const noexcept { return false; }
    [[nodiscard]] const std::optional<DualLiveWorkerPocFrame>& LatestA() const noexcept { return a_; }
    [[nodiscard]] const std::optional<DualLiveWorkerPocFrame>& LatestB() const noexcept { return b_; }

private:
    std::string generation_;
    std::string a_token_;
    std::string b_token_;
    bool terminal_{};
    std::optional<DualLiveWorkerPocFrame> a_;
    std::optional<DualLiveWorkerPocFrame> b_;
};

} // namespace a0::phase0
